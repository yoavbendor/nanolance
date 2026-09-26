// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Added to lance-encoding's `testing` module by tools/rust_suite.py: every round trip a lance-encoding
// test checks is also read back by nanolance. The pages the Rust encoder produced become a Lance 2.2
// file (the same bytes, with the file's schema and column metadata around them), nanolance reads it
// whole, by the test's row ranges and by its row indices, and the result is compared with what the
// test expects -- the comparison the Rust decoder is held to.

use std::ffi::{CString, c_char};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering as AtomicOrdering};

use arrow_array::{Array, ArrayRef, UInt64Array};
use arrow_schema::ffi::FFI_ArrowSchema;
use arrow_schema::{Field, Schema};
use prost::Message;

use super::nanolance_hook::{Outcome, compare, message, nanolance_rust_write_file, read};
use crate::decoder::{ColumnInfo, PageEncoding};

#[derive(Clone, PartialEq, prost::Message)]
struct AnyMessage {
    #[prost(string, tag = "1")]
    type_url: String,
    #[prost(bytes = "vec", tag = "2")]
    value: Vec<u8>,
}

fn any_bytes(type_url: &str, value: Vec<u8>) -> Vec<u8> {
    AnyMessage { type_url: type_url.to_string(), value }.encode_to_vec()
}

fn put_u32(out: &mut Vec<u8>, v: u32) {
    out.extend_from_slice(&v.to_le_bytes());
}

fn put_u64(out: &mut Vec<u8>, v: u64) {
    out.extend_from_slice(&v.to_le_bytes());
}

fn put_bytes(out: &mut Vec<u8>, bytes: &[u8]) {
    put_u32(out, bytes.len() as u32);
    out.extend_from_slice(bytes);
}

/// The columns' page tables, framed for nanolance_rust_write_file (little endian):
/// u32 columns; per column: u32 pages, per page (u64 rows, u64 priority, u32 buffers, (u64 offset,
/// u64 size) per buffer, u32 + bytes of the page encoding as a protobuf Any); u32 column buffers,
/// (u64, u64) each; u32 + bytes of the column encoding as an Any.
fn frame_columns(column_infos: &[Arc<ColumnInfo>]) -> Vec<u8> {
    let mut out = Vec::new();
    put_u32(&mut out, column_infos.len() as u32);
    for column in column_infos {
        put_u32(&mut out, column.page_infos.len() as u32);
        for page in column.page_infos.iter() {
            put_u64(&mut out, page.num_rows);
            put_u64(&mut out, page.priority);
            put_u32(&mut out, page.buffer_offsets_and_sizes.len() as u32);
            for (offset, size) in page.buffer_offsets_and_sizes.iter() {
                put_u64(&mut out, *offset);
                put_u64(&mut out, *size);
            }
            let encoding = match &page.encoding {
                PageEncoding::Legacy(e) => any_bytes("/lance.encodings.ArrayEncoding", e.encode_to_vec()),
                PageEncoding::Structural(e) => any_bytes("/lance.encodings21.PageLayout", e.encode_to_vec()),
            };
            put_bytes(&mut out, &encoding);
        }
        put_u32(&mut out, column.buffer_offsets_and_sizes.len() as u32);
        for (offset, size) in column.buffer_offsets_and_sizes.iter() {
            put_u64(&mut out, *offset);
            put_u64(&mut out, *size);
        }
        put_bytes(
            &mut out,
            &any_bytes("/lance.encodings.ColumnEncoding", column.encoding.encode_to_vec()),
        );
    }
    out
}

static CASE: AtomicU64 = AtomicU64::new(0);

/// Read the round trip back with nanolance and log how it compares.
#[allow(clippy::too_many_arguments)]
pub(crate) fn check(
    field: &Field,
    encoded: &[u8],
    column_infos: &[Arc<ColumnInfo>],
    num_rows: u64,
    expected: Option<&ArrayRef>,
    concat: Option<&ArrayRef>,
    ranges: &[std::ops::Range<u64>],
    indices: &[Vec<u64>],
    encoding: &str,
) {
    let Ok(log_path) = std::env::var("NANOLANCE_RUST_LOG") else {
        return;
    };
    let case = CASE.fetch_add(1, AtomicOrdering::Relaxed);
    let dir = std::env::var("NANOLANCE_RUST_TMP").unwrap_or_else(|_| std::env::temp_dir().display().to_string());
    let path = format!("{dir}/nanolance-rust-{}-{case}.lance", std::process::id());
    let c_path = CString::new(path.clone()).unwrap();
    let mut outcome = Outcome::new();

    let schema = Schema::new(vec![field.clone()]);
    let written = match FFI_ArrowSchema::try_from(&schema) {
        Err(e) => Err(format!("schema export: {e}")),
        Ok(ffi_schema) => {
            let columns = frame_columns(column_infos);
            let mut msg = vec![0u8; 4096];
            let rc = unsafe {
                nanolance_rust_write_file(
                    c_path.as_ptr(), encoded.as_ptr(), encoded.len(), columns.as_ptr(), columns.len(),
                    &ffi_schema, num_rows, msg.as_mut_ptr() as *mut c_char, msg.len(),
                )
            };
            if rc == 0 { Ok(()) } else { Err(message(&msg)) }
        }
    };
    match written {
        Err(e) => outcome.note("refused", e, "file"),
        Ok(()) => {
            match read(&c_path, 0, num_rows, None) {
                Err(e) => outcome.note("refused", e, "read"),
                Ok(actual) => {
                    if let Some(expected) = expected {
                        let (s, d) = compare(expected, &actual);
                        outcome.note(s, d, "read");
                    } else if actual.len() as u64 != num_rows {
                        outcome.note("mismatch", format!("{} rows, expected {num_rows}", actual.len()), "read");
                    }
                }
            }
            for range in ranges {
                let what = format!("range {}..{}", range.start, range.end);
                match read(&c_path, range.start, range.end - range.start, None) {
                    Err(e) => outcome.note("refused", e, &what),
                    Ok(actual) => {
                        if let Some(expected) = expected {
                            let want = expected.slice(range.start as usize, (range.end - range.start) as usize);
                            let (s, d) = compare(&want, &actual);
                            outcome.note(s, d, &what);
                        }
                    }
                }
            }
            for idx in indices {
                // nanolance takes ascending, distinct rows; the test's order is restored here.
                let mut sorted = idx.clone();
                sorted.sort_unstable();
                sorted.dedup();
                let what = format!("take {} rows", idx.len());
                match read(&c_path, 0, 0, Some(&sorted)) {
                    Err(e) => outcome.note("refused", e, &what),
                    Ok(actual) => {
                        let positions = UInt64Array::from(
                            idx.iter().map(|i| sorted.binary_search(i).unwrap() as u64).collect::<Vec<_>>(),
                        );
                        let actual = arrow_select::take::take(&actual, &positions, None).unwrap();
                        if let Some(concat) = concat {
                            let want = arrow_select::take::take(concat, &UInt64Array::from(idx.clone()), None).unwrap();
                            let (s, d) = compare(&want, &actual);
                            outcome.note(s, d, &what);
                        }
                    }
                }
            }
        }
    }
    // NANOLANCE_RUST_KEEP=<dir> keeps the file of every mismatch there, to reproduce it outside Rust.
    match std::env::var("NANOLANCE_RUST_KEEP") {
        Ok(keep) if outcome.status == "mismatch" => {
            let _ = std::fs::rename(&path, format!("{keep}/{}-{case}.lance", std::process::id()));
        }
        _ => {
            let _ = std::fs::remove_file(&path);
        }
    }
    outcome.log(&log_path, encoding, &field.data_type().to_string(), num_rows);
}
