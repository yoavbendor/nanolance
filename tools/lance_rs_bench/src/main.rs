//! lance_rs_bench: the pure-Rust side of tools/bench_matrix.py -- the lance crate (the same version
//! pylance is built from) writing and reading a dataset, with no Python in the process.
//!
//!   lance_rs_bench write <in.arrow> <out.lance> [iters]
//!   lance_rs_bench read <dataset.lance> [iters] [--dump out.arrow]
//!
//! Prints one JSON line in tools/nlbench's shape: rows, per-run times and warm_median_ms (the median
//! without the first, warm-up, run), plus peak_rss_mb: the most memory any one run added over what
//! the process held just before it (the input batches, for a write), measured by resetting the
//! kernel's high-water mark (/proc/self/clear_refs) before each run and reading VmHWM after it. A
//! read's peak includes the table it returns, as nlbench's does.
//!
//! Threads: tokio's worker count is TOKIO_WORKER_THREADS (default: one per core); lance's own CPU
//! and I/O pools follow LANCE_CPU_THREADS and LANCE_IO_THREADS, as they do under pylance.
//! `--dump` writes what the first read returned as an Arrow IPC stream, so the harness can check
//! it against the source before trusting a time.

use std::fs::File;
use std::sync::Arc;
use std::time::Instant;

use arrow_array::{RecordBatch, RecordBatchIterator};
use arrow_ipc::reader::StreamReader;
use arrow_ipc::writer::StreamWriter;
use arrow_schema::Schema;
use futures::TryStreamExt;
use lance::dataset::{Dataset, WriteMode, WriteParams};
use lance_file::version::LanceFileVersion;

fn status_kb(field: &str) -> u64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| {
            s.lines()
                .find(|l| l.starts_with(field))
                .and_then(|l| l.split_whitespace().nth(1).and_then(|v| v.parse().ok()))
        })
        .unwrap_or(0)
}

/// Reset VmHWM to the current RSS (Linux: writing 5 to clear_refs), returning that baseline in KiB.
fn reset_peak() -> u64 {
    let _ = std::fs::write("/proc/self/clear_refs", "5");
    status_kb("VmRSS:")
}

fn peak_added_kb(baseline: u64) -> u64 {
    status_kb("VmHWM:").saturating_sub(baseline)
}

fn report(rows: usize, ms: &[f64], peak_kb: u64) {
    let mut warm: Vec<f64> = ms.iter().skip(if ms.len() > 1 { 1 } else { 0 }).copied().collect();
    warm.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let median = warm.get(warm.len() / 2).copied().unwrap_or(0.0);
    let best = warm.first().copied().unwrap_or(0.0);
    let all: Vec<String> = ms.iter().map(|v| format!("{v:.4}")).collect();
    println!(
        "{{\"rows\": {rows}, \"best_ms\": {best:.4}, \"warm_median_ms\": {median:.4}, \"all_ms\": [{}], \"peak_rss_mb\": {:.2}}}",
        all.join(", "),
        peak_kb as f64 / 1024.0
    );
}

fn load_ipc(path: &str) -> (Arc<Schema>, Vec<RecordBatch>) {
    let reader = StreamReader::try_new(File::open(path).expect("open input"), None).expect("Arrow IPC stream");
    let schema = reader.schema();
    let batches = reader.collect::<Result<Vec<_>, _>>().expect("read batches");
    (schema, batches)
}

async fn bench_write(input: &str, out: &str, iters: usize) {
    let (schema, batches) = load_ipc(input);
    let rows = batches.iter().map(|b| b.num_rows()).sum();
    let mut ms = Vec::new();
    let mut peak = 0;
    for _ in 0..iters {
        let _ = std::fs::remove_dir_all(out);
        let baseline = reset_peak();
        let t0 = Instant::now();
        let reader = RecordBatchIterator::new(batches.clone().into_iter().map(Ok), schema.clone());
        let params = WriteParams {
            mode: WriteMode::Create,
            data_storage_version: Some(LanceFileVersion::V2_2),
            ..Default::default()
        };
        Dataset::write(reader, out, Some(params)).await.expect("write");
        ms.push(t0.elapsed().as_secs_f64() * 1e3);
        peak = peak.max(peak_added_kb(baseline));
    }
    report(rows, &ms, peak);
}

async fn bench_read(path: &str, iters: usize, dump: Option<&str>) {
    let mut ms = Vec::new();
    let mut peak = 0;
    let mut rows = 0;
    for it in 0..iters {
        let baseline = reset_peak();
        let t0 = Instant::now();
        let dataset = Dataset::open(path).await.expect("open");
        let batches: Vec<RecordBatch> = dataset
            .scan()
            .try_into_stream()
            .await
            .expect("scan")
            .try_collect()
            .await
            .expect("read");
        ms.push(t0.elapsed().as_secs_f64() * 1e3);
        peak = peak.max(peak_added_kb(baseline));
        rows = batches.iter().map(|b| b.num_rows()).sum();
        if it == 0 {
            if let Some(dump) = dump {
                let schema = batches.first().map(|b| b.schema()).unwrap_or_else(|| Arc::new(dataset.schema().into()));
                let mut w = StreamWriter::try_new(File::create(dump).expect("create dump"), &schema).expect("dump");
                for b in &batches {
                    w.write(b).expect("dump batch");
                }
                w.finish().expect("dump finish");
            }
        }
    }
    report(rows, &ms, peak);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let usage = "usage: lance_rs_bench write <in.arrow> <out.lance> [iters]\n       lance_rs_bench read <dataset.lance> [iters] [--dump out.arrow]";
    if args.len() < 3 {
        eprintln!("{usage}");
        std::process::exit(2);
    }
    let workers = std::env::var("TOKIO_WORKER_THREADS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or_else(|| std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1));
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(workers)
        .enable_all()
        .build()
        .expect("tokio runtime");
    match args[1].as_str() {
        "write" if args.len() >= 4 => {
            let iters = args.get(4).and_then(|v| v.parse().ok()).unwrap_or(5);
            runtime.block_on(bench_write(&args[2], &args[3], iters));
        }
        "read" => {
            let mut iters = 5;
            let mut dump = None;
            let mut i = 3;
            while i < args.len() {
                if args[i] == "--dump" && i + 1 < args.len() {
                    dump = Some(args[i + 1].clone());
                    i += 2;
                } else {
                    iters = args[i].parse().unwrap_or(iters);
                    i += 1;
                }
            }
            runtime.block_on(bench_read(&args[2], iters, dump.as_deref()));
        }
        _ => {
            eprintln!("{usage}");
            std::process::exit(2);
        }
    }
}
