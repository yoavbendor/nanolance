// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Storage options (nanolance/object_store.hpp), with no network and no credentials: object_store_options_from
// reads every spelling pylance's object_store (0.12.4) accepts for the fields nanolance implements, with and
// without the aws_ prefix. Where two spellings of one field are given the aws_ one wins, whatever the order;
// two of the same spelling: the first. aws_default_region / default_region give the region only when no region
// is given. allow_http takes object_store's booleans (case-insensitive); a value that does not parse is an
// error, as pylance raises it. Unknown keys are ignored. set_object_store_options refuses a URI with no bucket in
// every build, and for s3://bucket either keeps the options (a build with S3) or refuses by saying the build has
// no S3 support. Nothing here opens a connection: options are only stored.

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

// The region, endpoint and credential a single key sets (the others stay empty).
std::string region_of(const Pairs& pairs) { return nano_lance::io::object_store_options_from(pairs).region; }
std::string endpoint_of(const Pairs& pairs) { return nano_lance::io::object_store_options_from(pairs).endpoint; }

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
    // Every alias object_store takes, each alone.
    require(region_of(Pairs{{"aws_default_region", "ap-south-1"}}) == "ap-south-1", "aws_default_region");
    require(region_of(Pairs{{"default_region", "ap-east-1"}}) == "ap-east-1", "default_region");
    require(endpoint_of(Pairs{{"aws_endpoint_url", "http://a:1"}}) == "http://a:1", "aws_endpoint_url");
    require(endpoint_of(Pairs{{"endpoint_url", "http://b:2"}}) == "http://b:2", "endpoint_url");
    {
        const auto o = object_store_options_from(Pairs{{"aws_token", "T1"}});
        require(o.session_token == "T1", "aws_token");
        require(object_store_options_from(Pairs{{"token", "T2"}}).session_token == "T2", "token");
        require(object_store_options_from(Pairs{{"aws_allow_http", "true"}}).allow_http, "aws_allow_http");
    }
    // default_region is the region only when no region is given; region wins over it either way.
    require(region_of(Pairs{{"default_region", "d"}, {"region", "r"}}) == "r", "region beats default_region");
    require(region_of(Pairs{{"region", "r"}, {"default_region", "d"}}) == "r", "region beats default_region (order)");
    require(region_of(Pairs{{"aws_default_region", "a"}, {"default_region", "d"}}) == "a",
            "aws_default_region beats default_region");
    require(region_of(Pairs{{"default_region", "d"}, {"aws_default_region", "a"}}) == "a",
            "aws_default_region beats default_region (order)");
    require(region_of(Pairs{{"aws_default_region", "d"}, {"region", "r"}}) == "r",
            "region beats aws_default_region");
    // Precedence between the spellings of one field: the aws_ one, whatever the order of the pairs.
    require(region_of(Pairs{{"region", "a"}, {"aws_region", "b"}}) == "b", "aws_region beats region");
    require(region_of(Pairs{{"aws_region", "b"}, {"region", "a"}}) == "b", "aws_region beats region (order)");
    require(endpoint_of(Pairs{{"endpoint", "e"}, {"endpoint_url", "u"}, {"aws_endpoint", "a"}, {"aws_endpoint_url", "w"}}) ==
                "w",
            "aws_endpoint_url beats the others");
    require(endpoint_of(Pairs{{"endpoint", "e"}, {"aws_endpoint", "a"}}) == "a", "aws_endpoint beats endpoint");
    require(endpoint_of(Pairs{{"endpoint_url", "u"}, {"endpoint", "e"}}) == "u", "endpoint_url beats endpoint");
    require(object_store_options_from(Pairs{{"session_token", "s"}, {"aws_session_token", "a"}}).session_token == "a",
            "aws_session_token beats session_token");
    // The same spelling twice: the first one given.
    require(region_of(Pairs{{"region", "first"}, {"region", "second"}}) == "first", "same spelling twice: first");
    // Booleans: object_store's spellings, case-insensitive.
    for (const char* yes : {"1", "true", "True", "TRUE", "on", "ON", "yes", "Yes", "y", "Y"}) {
        require(object_store_options_from(Pairs{{"allow_http", yes}}).allow_http, std::string("allow_http=") + yes);
        require(object_store_options_from(Pairs{{"aws_allow_http", yes}}).allow_http,
                std::string("aws_allow_http=") + yes);
    }
    for (const char* no : {"0", "false", "False", "FALSE", "off", "OFF", "no", "No", "n", "N"}) {
        require(!object_store_options_from(Pairs{{"allow_http", no}}).allow_http, std::string("allow_http=") + no);
    }
    // A boolean that does not parse is an error, as in pylance: named, and the options are the defaults.
    for (const char* bad : {"", "maybe", "2", "t", "f", "truee", " true"}) {
        std::string error;
        const auto o = object_store_options_from(Pairs{{"endpoint", "http://x:1"}, {"allow_http", bad}}, &error);
        require(error == std::string("failed to parse \"") + bad + "\" as boolean", "bad boolean named: " + error);
        require(o.endpoint.empty() && !o.allow_http, std::string("bad boolean gives default options: ") + bad);
    }
    // The same error when the bad value sits under the spelling that does not win.
    {
        std::string error;
        object_store_options_from(Pairs{{"aws_allow_http", "true"}, {"allow_http", "maybe"}}, &error);
        require(error == "failed to parse \"maybe\" as boolean", "a bad value under the losing spelling is an error");
    }
    // Without an error pointer nothing throws and the result is the defaults.
    require(!object_store_options_from(Pairs{{"allow_http", "maybe"}}).allow_http, "no error pointer: defaults");
    // A good call clears the error the caller passed in.
    {
        std::string error = "stale";
        object_store_options_from(Pairs{{"region", "r"}}, &error);
        require(error.empty(), "a good call clears the error");
    }
    // Unknown keys are ignored, values included.
    {
        std::string error;
        const auto o = object_store_options_from(
            Pairs{{"timeout", "30s"},
                  {"virtual_hosted_style_request", "maybe"},
                  {"skip_signature", "maybe"},
                  {"allow_invalid_certificates", "maybe"},
                  {"aws_server_side_encryption", "AES256"}},
            &error);
        require(error.empty(), "unknown keys, even with a non-boolean value, are not errors");
        require(o.access_key_id.empty() && o.region.empty() && o.endpoint.empty() && !o.allow_http,
                "unknown keys change nothing");
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
