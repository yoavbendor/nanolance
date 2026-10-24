// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Storage options (nanolance/object_store.hpp), with no network and no credentials: object_store_options_from
// reads Lance's storage-option keys, each with and without the aws_ prefix, as pylance and lance-c pass them;
// allow_http takes "true" / "True" / "1"; unknown keys are ignored and a later key wins. set_object_store_options
// refuses a URI with no bucket in every build, and for s3://bucket either keeps the options (a build with S3) or
// refuses by saying the build has no S3 support. Nothing here opens a connection: options are only stored.

#include "nanolance/object_store.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

using Pairs = std::vector<std::pair<std::string, std::string>>;

}  // namespace

int main() {
    using nano_lance::io::object_store_options_from;
    using nano_lance::io::ObjectStoreOptions;

    // The plain keys.
    {
        const auto o = object_store_options_from(Pairs{{"access_key_id", "AK"},
                                                       {"secret_access_key", "SK"},
                                                       {"session_token", "ST"},
                                                       {"region", "eu-west-1"},
                                                       {"endpoint", "http://127.0.0.1:9000"},
                                                       {"allow_http", "true"}});
        require(o.access_key_id == "AK" && o.secret_access_key == "SK" && o.session_token == "ST", "credentials");
        require(o.region == "eu-west-1" && o.endpoint == "http://127.0.0.1:9000", "region and endpoint");
        require(o.allow_http, "allow_http");
    }
    // The aws_ spellings (pylance's examples use these).
    {
        const auto o = object_store_options_from(Pairs{{"aws_access_key_id", "AK2"},
                                                       {"aws_secret_access_key", "SK2"},
                                                       {"aws_session_token", "ST2"},
                                                       {"aws_region", "us-east-1"},
                                                       {"aws_endpoint", "https://s3.example"}});
        require(o.access_key_id == "AK2" && o.secret_access_key == "SK2" && o.session_token == "ST2", "aws_ credentials");
        require(o.region == "us-east-1" && o.endpoint == "https://s3.example", "aws_ region and endpoint");
        require(!o.allow_http, "allow_http is off by default");
    }
    // allow_http's spellings.
    for (const char* yes : {"true", "True", "1"}) {
        require(object_store_options_from(Pairs{{"allow_http", yes}}).allow_http, std::string("allow_http=") + yes);
    }
    for (const char* no : {"false", "0", ""}) {
        require(!object_store_options_from(Pairs{{"allow_http", no}}).allow_http, std::string("allow_http=") + no);
    }
    // Unknown keys are ignored; the last of two spellings wins.
    {
        const auto o = object_store_options_from(
            Pairs{{"timeout", "30s"}, {"region", "a"}, {"aws_region", "b"}, {"virtual_hosted_style_request", "true"}});
        require(o.region == "b", "the later key wins");
        require(o.access_key_id.empty() && o.endpoint.empty() && !o.allow_http, "unknown keys change nothing");
    }
    require(object_store_options_from({}).region.empty(), "no pairs: default options");

    // set_object_store_options.
    {
        std::string error;
        require(!nano_lance::io::set_object_store_options("s3://", ObjectStoreOptions{}, error) && !error.empty(),
                "a URI with no bucket is refused with a message");
        error.clear();
        ObjectStoreOptions o;
        o.region = "us-east-1";
        o.endpoint = "http://127.0.0.1:1";
        o.allow_http = true;
        const bool kept = nano_lance::io::set_object_store_options("s3://bucket/ds.lance", o, error);
        require(kept || error.find("without S3 support") != std::string::npos,
                "s3://bucket: kept (S3 build) or refused as a build without S3 (" + error + ")");
        if (kept) {
            // Default options clear the bucket's again.
            require(nano_lance::io::set_object_store_options("s3://bucket", ObjectStoreOptions{}, error),
                    "default options clear them");
        }
    }

    std::cout << "object_store: all checks passed\n";
    return 0;
}
