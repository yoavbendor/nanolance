//! Index workloads for the lance crate: scalar, vector and full-text indexes built and then queried,
//! timed the way tools/nlbench_index.cpp times nanolance -- the same workload file, the same protocol,
//! so the two engines' numbers compare directly. Both run on every core by default.
//!
//!   lance_rs_bench index <workload.txt> <scalar|vector|fts> <work_dir> <base.lance> <out.json>
//!
//! The workload is a tab-separated text file, read the same way by tools/nlbench_index.cpp:
//!
//!   min_seconds    SECONDS
//!   scalar_build   COLUMN   BTREE|BITMAP|LABEL_LIST
//!   scalar_query   NAME     FILTER     COLUMNS (comma-separated, or *)
//!   vector_config  COLUMN   PARTITIONS  DIM  BUILDS (default 5)
//!   vector_kind    NAME     IVF_FLAT|IVF_PQ|IVF_HNSW_SQ
//!   vector_query   COMMA-SEPARATED FLOATS            (one line per query)
//!   vector_search  NAME     K  NPROBES  REFINE_FACTOR  (0: unset)
//!   fts_config     COLUMN
//!   fts_search     NAME     LIMIT (0: all)  AND (0|1)
//!   fts_text       TEXT                               (one line per query, after its fts_search)
//!
//! The base dataset is unindexed and is never changed: each build works on a fresh copy made outside
//! the timed region. Builds are timed until their timed total reaches the workload's min_seconds (and
//! at least three builds ran). Queries repeat until their timed total reaches min_seconds: a scalar
//! query is one sample per call; a vector or full-text search is one sample per pass over its query
//! set, divided by the number of queries. Medians are reported with the minimum and the sample count.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::Instant;

use arrow_array::{Array, Float32Array, RecordBatch, UInt64Array};
use lance::dataset::Dataset;
use lance::index::DatasetIndexExt;
use lance::index::vector::VectorIndexParams;
use lance_index::scalar::inverted::query::{FtsQuery, MatchQuery, Operator};
use lance_index::scalar::{BuiltinIndexType, FullTextSearchQuery, InvertedIndexParams, ScalarIndexParams};
use lance_index::{IndexParams, IndexType};
use lance_linalg::distance::MetricType;
use serde_json::{json, Value};

pub struct Workload {
    pub min_seconds: f64,
    pub scalar: Option<Scalar>,
    pub vector: Option<Vector>,
    pub fts: Option<Fts>,
}

pub struct Scalar {
    pub builds: Vec<(String, String)>,
    pub queries: Vec<ScalarQuery>,
}

pub struct ScalarQuery {
    pub name: String,
    pub filter: String,
    pub columns: Option<Vec<String>>,
}

pub struct Vector {
    pub column: String,
    pub partitions: usize,
    pub dim: usize,
    /// Builds per index kind (each on its own copy), averaged over.
    pub builds: usize,
    pub queries: Vec<Vec<f32>>,
    pub kinds: Vec<VectorKind>,
    pub searches: Vec<VectorSearch>,
}

pub struct VectorKind {
    pub name: String,
    /// IVF_FLAT, IVF_PQ or IVF_HNSW_SQ.
    pub kind: String,
}

pub struct VectorSearch {
    pub name: String,
    pub k: usize,
    pub nprobes: Option<usize>,
    pub refine_factor: Option<u32>,
}

pub struct Fts {
    pub column: String,
    pub searches: Vec<FtsSearch>,
}

pub struct FtsSearch {
    pub name: String,
    pub texts: Vec<String>,
    pub limit: Option<i64>,
    pub and: bool,
}

/// Timed samples in milliseconds: the median, the minimum, and how many were taken.
fn summary(mut xs: Vec<f64>) -> Value {
    xs.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let median = xs[xs.len() / 2];
    json!({ "median_ms": median, "min_ms": xs[0], "samples": xs.len() })
}

/// Recursive copy of a dataset directory (untimed by the callers).
fn copy_dir(from: &Path, to: &Path) -> std::io::Result<()> {
    let _ = fs::remove_dir_all(to);
    fs::create_dir_all(to)?;
    for entry in fs::read_dir(from)? {
        let entry = entry?;
        let target = to.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            copy_dir(&entry.path(), &target)?;
        } else {
            fs::copy(entry.path(), target)?;
        }
    }
    Ok(())
}

fn path_str(p: &Path) -> &str {
    p.to_str().expect("utf-8 path")
}

/// The workload file's records, in order, as the sections above describe them.
pub fn parse_workload(text: &str) -> Workload {
    let mut w = Workload { min_seconds: 10.0, scalar: None, vector: None, fts: None };
    for line in text.lines().filter(|l| !l.trim().is_empty() && !l.starts_with('#')) {
        let f: Vec<&str> = line.split('\t').collect();
        match f[0] {
            "min_seconds" => w.min_seconds = f[1].parse().expect("min_seconds"),
            "scalar_build" => scalar_of(&mut w).builds.push((f[1].into(), f[2].into())),
            "scalar_query" => scalar_of(&mut w).queries.push(ScalarQuery {
                name: f[1].into(),
                filter: f[2].into(),
                columns: if f[3] == "*" { None } else { Some(f[3].split(',').map(String::from).collect()) },
            }),
            "vector_config" => {
                let v = vector_of(&mut w);
                v.column = f[1].into();
                v.partitions = f[2].parse().expect("partitions");
                v.dim = f[3].parse().expect("dim");
                v.builds = f.get(4).map(|b| b.parse().expect("builds")).unwrap_or(5);
            }
            "vector_kind" => vector_of(&mut w).kinds.push(VectorKind { name: f[1].into(), kind: f[2].into() }),
            "vector_query" => vector_of(&mut w)
                .queries
                .push(f[1].split(',').map(|x| x.parse().expect("float")).collect()),
            "vector_search" => vector_of(&mut w).searches.push(VectorSearch {
                name: f[1].into(),
                k: f[2].parse().expect("k"),
                nprobes: opt(f[3]),
                refine_factor: opt(f[4]).map(|r| r as u32),
            }),
            "fts_config" => fts_of(&mut w).column = f[1].into(),
            "fts_search" => fts_of(&mut w).searches.push(FtsSearch {
                name: f[1].into(),
                texts: Vec::new(),
                limit: opt(f[2]).map(|l| l as i64),
                and: f[3] == "1",
            }),
            "fts_text" => fts_of(&mut w).searches.last_mut().expect("fts_text after fts_search").texts.push(f[1].into()),
            other => panic!("unknown workload record {other}"),
        }
    }
    w
}

fn opt(s: &str) -> Option<usize> {
    match s.parse::<usize>().expect("number") {
        0 => None,
        n => Some(n),
    }
}

fn scalar_of(w: &mut Workload) -> &mut Scalar {
    w.scalar.get_or_insert_with(|| Scalar { builds: Vec::new(), queries: Vec::new() })
}

fn vector_of(w: &mut Workload) -> &mut Vector {
    w.vector.get_or_insert_with(|| Vector {
        column: String::new(),
        partitions: 0,
        dim: 0,
        builds: 5,
        queries: Vec::new(),
        kinds: Vec::new(),
        searches: Vec::new(),
    })
}

fn fts_of(w: &mut Workload) -> &mut Fts {
    w.fts.get_or_insert_with(|| Fts { column: String::new(), searches: Vec::new() })
}

pub async fn run(workload: &Workload, section: &str, work: &Path, base: &Path, out: &Path) {
    let mut result = serde_json::Map::new();
    match section {
        "scalar" => {
            let spec = workload.scalar.as_ref().expect("workload has no scalar section");
            result.insert("scalar".into(), scalar(spec, workload.min_seconds, work, base).await);
        }
        "vector" => {
            let spec = workload.vector.as_ref().expect("workload has no vector section");
            result.insert("vector".into(), vector(spec, workload.min_seconds, work, base).await);
        }
        "fts" => {
            let spec = workload.fts.as_ref().expect("workload has no fts section");
            result.insert("fts".into(), fts(spec, workload.min_seconds, work, base).await);
        }
        other => panic!("unknown section {other}"),
    }
    let threads = std::env::var("TOKIO_WORKER_THREADS")
        .ok()
        .and_then(|v| v.parse::<usize>().ok())
        .unwrap_or_else(|| std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1));
    result.insert("engine".into(), json!("rust-lance 12.0.0"));
    result.insert("threads".into(), json!(threads));
    fs::write(out, serde_json::to_string_pretty(&Value::Object(result)).unwrap()).expect("write output");
}

async fn create_index(ds: &mut Dataset, column: &str, index: IndexType, params: &dyn IndexParams) {
    ds.create_index(&[column], index, None, params, true)
        .await
        .expect("create_index");
}

/// Builds the index on a fresh copy of the base dataset until the timed total reaches min_seconds and at least
/// `min_builds` builds ran. Build i's copy is `<prefix>_<i>.lance`, indexed. With `keep_all` every copy is kept
/// (the vector indexes, which are searched on each build: their training is random); otherwise only the last.
/// Returns each build's ms and the copies.
async fn timed_builds(
    base: &Path,
    prefix: &Path,
    min_seconds: f64,
    min_builds: usize,
    keep_all: bool,
    column: &str,
    index: IndexType,
    params: &dyn IndexParams,
) -> (Vec<f64>, Vec<PathBuf>) {
    let mut samples = Vec::new();
    let mut copies: Vec<PathBuf> = Vec::new();
    let mut total = 0.0;
    while samples.len() < min_builds || total < min_seconds {
        let copy = PathBuf::from(format!("{}_{}.lance", prefix.display(), samples.len()));
        copy_dir(base, &copy).expect("copy base");
        let mut ds = Dataset::open(path_str(&copy)).await.expect("open copy");
        let t0 = Instant::now();
        create_index(&mut ds, column, index.clone(), params).await;
        let secs = t0.elapsed().as_secs_f64();
        total += secs;
        samples.push(secs * 1000.0);
        if !keep_all {
            copies.iter().for_each(|old| {
                let _ = fs::remove_dir_all(old);
            });
            copies.clear();
        }
        copies.push(copy);
    }
    (samples, copies)
}

async fn scalar(spec: &Scalar, min_seconds: f64, work: &Path, base: &Path) -> Value {
    let mut builds = serde_json::Map::new();
    for (column, kind) in &spec.builds {
        let index = match kind.as_str() {
            "BTREE" => IndexType::BTree,
            "BITMAP" => IndexType::Bitmap,
            "LABEL_LIST" => IndexType::LabelList,
            other => panic!("unsupported scalar index {other}"),
        };
        let builtin = match kind.as_str() {
            "BTREE" => BuiltinIndexType::BTree,
            "BITMAP" => BuiltinIndexType::Bitmap,
            _ => BuiltinIndexType::LabelList,
        };
        let params = ScalarIndexParams::for_builtin(builtin);
        let prefix = work.join(format!("rust_build_{column}_{kind}"));
        let (samples, _) = timed_builds(base, &prefix, min_seconds, 3, false, column, index, &params).await;
        builds.insert(format!("{column} {kind}"), summary(samples));
    }

    // One dataset with every scalar index, for the queries.
    let serving = work.join("rust_scalar.lance");
    copy_dir(base, &serving).expect("copy base");
    let mut ds = Dataset::open(path_str(&serving)).await.expect("open");
    for (column, kind) in &spec.builds {
        let index = match kind.as_str() {
            "BTREE" => IndexType::BTree,
            "BITMAP" => IndexType::Bitmap,
            _ => IndexType::LabelList,
        };
        let builtin = match kind.as_str() {
            "BTREE" => BuiltinIndexType::BTree,
            "BITMAP" => BuiltinIndexType::Bitmap,
            _ => BuiltinIndexType::LabelList,
        };
        create_index(&mut ds, column, index, &ScalarIndexParams::for_builtin(builtin)).await;
    }

    let mut queries = serde_json::Map::new();
    for q in &spec.queries {
        let rows = scan_rows(&ds, &q.filter, q.columns.as_deref()).await;
        let mut samples = Vec::new();
        let mut total = 0.0;
        while samples.len() < 20 || total < min_seconds {
            let t0 = Instant::now();
            scan_rows(&ds, &q.filter, q.columns.as_deref()).await;
            let secs = t0.elapsed().as_secs_f64();
            total += secs;
            samples.push(secs * 1000.0);
        }
        let mut row = summary(samples);
        row["rows"] = json!(rows);
        queries.insert(q.name.clone(), row);
    }
    json!({ "builds": builds, "queries": queries })
}

async fn scan_rows(ds: &Dataset, filter: &str, columns: Option<&[String]>) -> usize {
    let mut scanner = ds.scan();
    scanner.filter(filter).expect("filter");
    if let Some(columns) = columns {
        scanner.project(columns).expect("project");
    }
    let batch: RecordBatch = scanner.try_into_batch().await.expect("scan");
    batch.num_rows()
}

/// Every vector index is built `spec.builds` times, each on its own copy, because its training is random:
/// recall is measured on every build and averaged, and each search's time is pooled over the builds.
async fn vector(spec: &Vector, min_seconds: f64, work: &Path, base: &Path) -> Value {
    let mut builds = serde_json::Map::new();
    let mut searches = serde_json::Map::new();
    let mut hits = serde_json::Map::new();
    let mut distances = serde_json::Map::new();
    for kind in &spec.kinds {
        let params = vector_params(&kind.kind, spec.partitions, spec.dim);
        let prefix = work.join(format!("rust_{}", kind.name));
        let (samples, copies) = timed_builds(
            base,
            &prefix,
            min_seconds,
            spec.builds,
            true,
            &spec.column,
            IndexType::Vector,
            &params,
        )
        .await;
        builds.insert(kind.name.clone(), summary(samples));
        let mut datasets = Vec::new();
        for copy in &copies {
            datasets.push(Dataset::open(path_str(copy)).await.expect("open index copy"));
        }
        for s in &spec.searches {
            let key = format!("{}|{}", kind.name, s.name);
            // One pass per build: each build's answers, for recall and for the correctness checks.
            let mut per_build_ids = Vec::new();
            let mut per_build_dists = Vec::new();
            for ds in &datasets {
                let (ids, dists) = vector_pass(ds, spec, s).await;
                per_build_ids.push(ids);
                per_build_dists.push(dists);
            }
            hits.insert(key.clone(), json!(per_build_ids));
            distances.insert(key.clone(), json!(per_build_dists));
            // Timed passes, round-robin over the builds, until min_seconds of search (and a pass per build).
            let passes = datasets.len().max(5);
            let mut samples = Vec::new();
            let mut total = 0.0;
            let mut turn = 0;
            while samples.len() < passes || total < min_seconds {
                let ds = &datasets[turn % datasets.len()];
                turn += 1;
                let t0 = Instant::now();
                vector_pass(ds, spec, s).await;
                let secs = t0.elapsed().as_secs_f64();
                total += secs;
                samples.push(secs * 1000.0 / spec.queries.len() as f64);
            }
            searches.insert(key, summary(samples));
        }
        for copy in &copies {
            let _ = fs::remove_dir_all(copy);
        }
    }
    json!({ "builds": builds, "searches": searches, "hits": hits, "distances": distances })
}

fn vector_params(kind: &str, partitions: usize, dim: usize) -> VectorIndexParams {
    match kind {
        "IVF_FLAT" => VectorIndexParams::ivf_flat(partitions, MetricType::L2),
        // pylance's default for IVF_PQ on a 128-dimension column: dim / 8 sub-vectors, 8 bits.
        "IVF_PQ" => VectorIndexParams::ivf_pq(partitions, 8, dim / 8, MetricType::L2, 50),
        "IVF_HNSW_SQ" => VectorIndexParams::with_ivf_hnsw_sq_params(
            MetricType::L2,
            lance_index::vector::ivf::IvfBuildParams::new(partitions),
            lance_index::vector::hnsw::builder::HnswBuildParams::default(),
            lance_index::vector::sq::builder::SQBuildParams::default(),
        ),
        other => panic!("unsupported vector index {other}"),
    }
}

/// One pass over the query set: the row ids and distances of each query's top k, in order.
async fn vector_pass(ds: &Dataset, spec: &Vector, s: &VectorSearch) -> (Vec<Vec<u64>>, Vec<Vec<f32>>) {
    let mut all_ids = Vec::with_capacity(spec.queries.len());
    let mut all_dists = Vec::with_capacity(spec.queries.len());
    for q in &spec.queries {
        let mut scanner = ds.scan();
        let query = Float32Array::from(q.clone());
        scanner.nearest(&spec.column, &query, s.k).expect("nearest");
        if let Some(n) = s.nprobes {
            scanner.nprobes(n);
        }
        if let Some(r) = s.refine_factor {
            scanner.refine(r);
        }
        scanner.project(&["id"]).expect("project");
        let batch = scanner.try_into_batch().await.expect("search");
        let ids = batch
            .column_by_name("id")
            .expect("id column")
            .as_any()
            .downcast_ref::<arrow_array::Int64Array>()
            .map(|a| a.values().iter().map(|v| *v as u64).collect::<Vec<u64>>())
            .or_else(|| {
                batch
                    .column_by_name("id")
                    .and_then(|c| c.as_any().downcast_ref::<UInt64Array>())
                    .map(|a| a.values().to_vec())
            })
            .unwrap_or_default();
        let dists = batch
            .column_by_name("_distance")
            .and_then(|c| c.as_any().downcast_ref::<Float32Array>())
            .map(|a| a.values().to_vec())
            .unwrap_or_default();
        all_ids.push(ids);
        all_dists.push(dists);
    }
    (all_ids, all_dists)
}

async fn fts(spec: &Fts, min_seconds: f64, work: &Path, base: &Path) -> Value {
    let prefix = work.join("rust_fts");
    let (samples, copies) = timed_builds(
        base,
        &prefix,
        min_seconds,
        3,
        false,
        &spec.column,
        IndexType::Inverted,
        &InvertedIndexParams::default(),
    )
    .await;
    let keep = copies.last().expect("a build").clone();
    let mut builds = serde_json::Map::new();
    builds.insert("INVERTED".into(), summary(samples));

    let ds = Dataset::open(path_str(&keep)).await.expect("open index copy");
    let mut searches = serde_json::Map::new();
    let mut hits = serde_json::Map::new();
    let mut scores = serde_json::Map::new();
    for s in &spec.searches {
        let (ids, sc) = fts_pass(&ds, s).await;
        hits.insert(s.name.clone(), json!(ids));
        scores.insert(s.name.clone(), json!(sc));
        let mut samples = Vec::new();
        let mut total = 0.0;
        while samples.len() < 5 || total < min_seconds {
            let t0 = Instant::now();
            fts_pass(&ds, s).await;
            let secs = t0.elapsed().as_secs_f64();
            total += secs;
            samples.push(secs * 1000.0 / s.texts.len() as f64);
        }
        searches.insert(s.name.clone(), summary(samples));
    }
    json!({ "builds": builds, "searches": searches, "hits": hits, "scores": scores })
}

fn fts_query(text: &str, and: bool) -> FtsQuery {
    if and {
        MatchQuery::new(text.to_owned()).with_operator(Operator::And).into()
    } else {
        MatchQuery::new(text.to_owned()).into()
    }
}

/// One pass over the search's texts: the row ids and BM25 scores of each text's hits, best first.
async fn fts_pass(ds: &Dataset, s: &FtsSearch) -> (Vec<Vec<u64>>, Vec<Vec<f32>>) {
    let mut all_ids = Vec::with_capacity(s.texts.len());
    let mut all_scores = Vec::with_capacity(s.texts.len());
    for text in &s.texts {
        let mut query = FullTextSearchQuery::new_query(fts_query(text, s.and));
        query.limit = s.limit;
        let mut scanner = ds.scan();
        scanner.full_text_search(query).expect("full_text_search");
        scanner.project(&["id", "_score"]).expect("project");
        let batch = scanner.try_into_batch().await.expect("fts search");
        let ids = batch
            .column_by_name("id")
            .and_then(|c| c.as_any().downcast_ref::<arrow_array::Int64Array>())
            .map(|a| a.values().iter().map(|v| *v as u64).collect())
            .unwrap_or_default();
        let scores = batch
            .column_by_name("_score")
            .and_then(|c| c.as_any().downcast_ref::<Float32Array>())
            .map(|a| a.values().to_vec())
            .unwrap_or_default();
        all_ids.push(ids);
        all_scores.push(scores);
    }
    (all_ids, all_scores)
}
