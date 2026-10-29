// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace nano_lance::io {

/// Connection settings for the objects of one bucket (`s3://`). An empty field falls back to the
/// environment chain (AWS_ACCESS_KEY_ID, AWS_REGION, AWS_ENDPOINT_URL, ...), as nanos3reader does.
struct ObjectStoreOptions {
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;
    std::string region;
    std::string endpoint;      // e.g. "http://127.0.0.1:9000"; selects path-style addressing
    bool allow_http = false;   // an http:// endpoint is refused unless set
};

/// Sets the options for every object in the bucket `uri` names (any `s3://bucket/...` URI). They are
/// process-wide and per bucket: each open of a dataset in that bucket sets them again, and default
/// options clear them (the environment chain applies). False and `error` for a URI with no bucket, or
/// in a build without S3 support.
/// Options from the storage-option keys pylance (object_store 0.12.4) takes for these fields: the
/// credentials, `region` (`default_region` when no region is given), `endpoint` (`endpoint_url`) and
/// `allow_http`, each under every spelling object_store accepts (`aws_`-prefixed or bare). When two
/// spellings of one field are given, the `aws_`-prefixed one wins; `allow_http` booleans are
/// object_store's ("1", "true", "on", "yes", "y", and the false ones; case-insensitive). Other keys are
/// ignored. A boolean that does not parse is an error, as in pylance: `error` (when given) names it and
/// the result holds default options. Without `error` the failure is not reported, so callers that take
/// user input should pass one.
ObjectStoreOptions object_store_options_from(const std::vector<std::pair<std::string, std::string>>& pairs,
                                             std::string* error = nullptr);

bool set_object_store_options(const std::string& uri, const ObjectStoreOptions& options, std::string& error);

}  // namespace nano_lance::io
