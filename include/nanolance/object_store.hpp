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
/// Options from Lance's storage-option keys (`access_key_id`, `secret_access_key`, `session_token`,
/// `region`, `endpoint`, each also with an `aws_` prefix; `allow_http` "true"/"1"), as pylance and
/// lance-c take them. Other keys are ignored.
ObjectStoreOptions object_store_options_from(const std::vector<std::pair<std::string, std::string>>& pairs);

bool set_object_store_options(const std::string& uri, const ObjectStoreOptions& options, std::string& error);

}  // namespace nano_lance::io
