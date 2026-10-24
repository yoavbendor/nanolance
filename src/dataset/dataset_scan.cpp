// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// lance_dataset_scan and LanceTableStream::open_request: the scan layer's reads with the dataset's scalar
// indices answering a filter. The scan layer cannot name the index code (it sits below it, and a build without
// indices must not link it), so a request carries the index selector as a pointer; these fill it in when the
// caller left it null, so a C++ caller gets indexed filters by default, as before the layers were split.

#include "nanolance/index_search.hpp"
#include "nanolance/lance_table_reader.hpp"

namespace nano_lance {

namespace {

/// `request` with the index layer's selector when it names none.
LanceScanRequest with_indices(const LanceScanRequest& request) {
    LanceScanRequest out = request;
    if (out.index_candidates == nullptr) {
        out.index_candidates = index_candidates;
    }
    return out;
}

}  // namespace

bool lance_dataset_scan(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                        ArrowSchema& out_schema, std::vector<ArrowArray>& out_batches, std::string& error,
                        bool trusted_input) {
    return lance_dataset_scan_core(dataset_path, with_indices(request), out_schema, out_batches, error, trusted_input);
}

bool LanceTableStream::open_request(const std::filesystem::path& dataset_path, const LanceScanRequest& request,
                                    ArrowSchema& out_schema, LanceTableStream& out, std::string& error,
                                    bool trusted_input) {
    return open_request_core(dataset_path, with_indices(request), out_schema, out, error, trusted_input);
}

}  // namespace nano_lance
