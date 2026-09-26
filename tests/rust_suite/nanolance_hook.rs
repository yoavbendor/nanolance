// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Added to the Lance crates' test modules by tools/rust_suite.py, never shipped with them: what the
// hooks (nanolance_encoding_hook.rs, nanolance_file_hook.rs) share -- the calls into nanolance, the
// comparison the Rust decoder is held to, and the log. One JSON line per check goes to
// $NANOLANCE_RUST_LOG; without it the hooks do nothing. The tests' own assertions are untouched, so a
// test's Rust result and nanolance's are separate.

#![allow(dead_code)]

use std::ffi::{CStr, c_char, c_int};
use std::io::Write;

use arrow_array::ffi_stream::{ArrowArrayStreamReader, FFI_ArrowArrayStream};
use arrow_array::{Array, ArrayRef, RecordBatchReader};
use arrow_schema::ffi::FFI_ArrowSchema;

// libnanolance_rust_shim (tests/rust_suite/nanolance_rust_shim.cpp); tools/rust_suite.py puts it on
// the link path.
#[link(name = "nanolance_rust_shim")]
unsafe extern "C" {
    pub(crate) fn nanolance_rust_write_file(
        path: *const c_char,
        data: *const u8,
        data_len: usize,
        columns: *const u8,
        columns_len: usize,
        schema: *const FFI_ArrowSchema,
        num_rows: u64,
        msg: *mut c_char,
        msg_cap: usize,
    ) -> c_int;
    pub(crate) fn nanolance_rust_read(
        path: *const c_char,
        offset: u64,
        length: u64,
        indices: *const u64,
        n_indices: usize,
        take: c_int,
        out: *mut FFI_ArrowArrayStream,
        msg: *mut c_char,
        msg_cap: usize,
    ) -> c_int;
}

pub(crate) fn message(buf: &[u8]) -> String {
    CStr::from_bytes_until_nul(buf)
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default()
}

/// One read by nanolance: each column's rows as one array, or why not.
pub(crate) fn read_columns(
    path: &CStr,
    offset: u64,
    length: u64,
    indices: Option<&[u64]>,
) -> Result<Vec<ArrayRef>, String> {
    let mut msg = vec![0u8; 4096];
    let mut stream = FFI_ArrowArrayStream::empty();
    let (ptr, n, take) = match indices {
        Some(idx) => (idx.as_ptr(), idx.len(), 1),
        None => (std::ptr::null(), 0, 0),
    };
    let rc = unsafe {
        nanolance_rust_read(
            path.as_ptr(), offset, length, ptr, n, take, &mut stream,
            msg.as_mut_ptr() as *mut c_char, msg.len(),
        )
    };
    if rc != 0 {
        return Err(message(&msg));
    }
    let reader = ArrowArrayStreamReader::try_new(stream).map_err(|e| e.to_string())?;
    let schema = reader.schema();
    let mut parts: Vec<Vec<ArrayRef>> = vec![Vec::new(); schema.fields().len()];
    for batch in reader {
        let batch = batch.map_err(|e| e.to_string())?;
        for (c, column) in batch.columns().iter().enumerate() {
            parts[c].push(column.clone());
        }
    }
    parts
        .into_iter()
        .zip(schema.fields().iter())
        .map(|(p, f)| {
            if p.is_empty() {
                return Ok(arrow_array::new_empty_array(f.data_type()));
            }
            let refs = p.iter().map(|a| a.as_ref()).collect::<Vec<_>>();
            arrow_select::concat::concat(&refs).map_err(|e| e.to_string())
        })
        .collect()
}

/// One read of a one-column file: its rows as one array, or why not.
pub(crate) fn read(path: &CStr, offset: u64, length: u64, indices: Option<&[u64]>) -> Result<ArrayRef, String> {
    read_columns(path, offset, length, indices)?
        .into_iter()
        .next()
        .ok_or_else(|| "the file read back has no column".to_string())
}

/// "pass", "type" (the values are equal but the Arrow type is not) or "mismatch".
pub(crate) fn compare(expected: &ArrayRef, actual: &ArrayRef) -> (&'static str, String) {
    if expected.len() != actual.len() {
        return ("mismatch", format!("{} rows, expected {}", actual.len(), expected.len()));
    }
    if expected.data_type() == actual.data_type() {
        if expected.as_ref() == actual.as_ref() {
            return ("pass", String::new());
        }
        return ("mismatch", first_difference(expected, actual));
    }
    match arrow_cast::cast(actual, expected.data_type()) {
        Ok(cast) if cast.as_ref() == expected.as_ref() => (
            "type",
            format!("read as {} (Rust: {})", actual.data_type(), expected.data_type()),
        ),
        Ok(cast) => ("mismatch", first_difference(expected, &cast)),
        Err(_) => (
            "mismatch",
            format!("read as {} (Rust: {})", actual.data_type(), expected.data_type()),
        ),
    }
}

fn first_difference(expected: &ArrayRef, actual: &ArrayRef) -> String {
    for i in 0..expected.len() {
        if expected.slice(i, 1).as_ref() != actual.slice(i, 1).as_ref() {
            let show = |a: &ArrayRef| {
                let s = format!("{:?}", a.slice(i, 1));
                s.chars().take(300).collect::<String>()
            };
            return format!("row {i}: expected {} got {}", show(expected), show(actual));
        }
    }
    "arrays differ".to_string()
}

pub(crate) fn json_string(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// The worst outcome of one check's reads, and what it was.
pub(crate) struct Outcome {
    pub status: &'static str,
    pub detail: String,
}

impl Outcome {
    pub(crate) fn new() -> Self {
        Outcome { status: "pass", detail: String::new() }
    }

    pub(crate) fn note(&mut self, status: &'static str, detail: String, what: &str) {
        let rank = |s: &str| match s {
            "pass" => 0,
            "type" => 1,
            "refused" => 2,
            _ => 3,
        };
        if rank(status) > rank(self.status) {
            self.status = status;
            self.detail = format!("{what}: {detail}");
        }
    }

    /// Append the outcome to $NANOLANCE_RUST_LOG, named after the running test.
    pub(crate) fn log(&self, log_path: &str, kind: &str, data_type: &str, rows: u64) {
        let krate = std::env::var("NANOLANCE_RUST_CRATE").unwrap_or_default();
        let test = format!("{krate}::{}", std::thread::current().name().unwrap_or("?"));
        let line = format!(
            "{{\"test\":{},\"encoding\":{},\"type\":{},\"rows\":{},\"status\":{},\"detail\":{}}}\n",
            json_string(&test),
            json_string(kind),
            json_string(data_type),
            rows,
            json_string(self.status),
            json_string(&self.detail),
        );
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(log_path) {
            let _ = f.write_all(line.as_bytes());
        }
    }
}
