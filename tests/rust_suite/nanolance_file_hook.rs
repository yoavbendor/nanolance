// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Added to lance-file's `testing` module by tools/rust_suite.py: every file a lance-file test writes
// through `write_lance_file` -- a real Lance file from Lance's own FileWriter -- is also read by
// nanolance, whole, and compared with the batches the test wrote.

use std::ffi::CString;

use arrow_array::{Array, ArrayRef, RecordBatch};

use super::nanolance_hook::{Outcome, compare, read_columns};
use crate::version::ConcreteFileVersion;

pub(crate) fn check(path: &object_store::path::Path, data: &[RecordBatch], version: ConcreteFileVersion) {
    let Ok(log_path) = std::env::var("NANOLANCE_RUST_LOG") else {
        return;
    };
    let rows: usize = data.iter().map(|b| b.num_rows()).sum();
    let data_type = data
        .first()
        .map(|b| {
            b.schema().fields().iter().map(|f| f.data_type().to_string()).collect::<Vec<_>>().join(", ")
        })
        .unwrap_or_default();
    let kind = format!("file {version:?}");
    let mut outcome = Outcome::new();
    let Some(first) = data.first() else {
        return;  // nothing written, nothing to compare
    };
    if !matches!(version, ConcreteFileVersion::V2_1 | ConcreteFileVersion::V2_2) {
        // nanolance reads formats 2.1 and 2.2 (pylance 12 writes 2.2 by default).
        outcome.note("refused", format!("format {version:?} (nanolance reads 2.1 and 2.2)"), "file");
        outcome.log(&log_path, &kind, &data_type, rows as u64);
        return;
    }
    let c_path = CString::new(format!("/{}", path.as_ref())).unwrap();
    match read_columns(&c_path, 0, rows as u64, None) {
        Err(e) => outcome.note("refused", e, "read"),
        Ok(columns) => {
            if columns.len() != first.num_columns() {
                outcome.note(
                    "mismatch",
                    format!("{} columns, expected {}", columns.len(), first.num_columns()),
                    "read",
                );
            }
            for (i, actual) in columns.iter().enumerate().take(first.num_columns()) {
                let parts = data.iter().map(|b| b.column(i).as_ref()).collect::<Vec<_>>();
                let expected = arrow_select::concat::concat(&parts).unwrap();
                let (s, d) = compare(&expected, actual);
                outcome.note(s, d, &format!("column {}", first.schema().field(i).name()));
            }
        }
    }
    outcome.log(&log_path, &kind, &data_type, rows as u64);
}
