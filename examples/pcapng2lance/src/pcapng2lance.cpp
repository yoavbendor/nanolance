// SPDX-License-Identifier: Apache-2.0
// pcapng2lance entry point. The packet table and the --decode-l2l3 protocol tables are written by the nanom
// converter by default. The nanotins converter stays for the options nanom does not implement yet (staged
// runs for l2-l4, windowed streaming, threads, --gpu) and is chosen automatically when one of them is
// given. --backend nanom|nanotins forces one.
#include "nanom_convert.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int nanotins_convert_main(int argc, char** argv);

namespace {

// Options only the nanotins converter implements.
bool nanotins_only(const std::string& s) {
    static const char* const kOptions[] = {"--sequential", "--threads", "--window-bytes", "--mem-bytes",
                                           "--read-tile-bytes", "--gpu"};
    for (const char* o : kOptions) {
        if (s == o) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::string backend;  // "", "nanom" or "nanotins"; empty chooses by the options given
    bool needs_nanotins = false;
    std::vector<char*> forwarded{argv[0]};  // argv without --backend, which the converters do not know
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--backend" && i + 1 < argc) {
            backend = argv[++i];
        } else if (s.rfind("--backend=", 0) == 0) {
            backend = s.substr(std::strlen("--backend="));
        } else {
            // --stage l1 and l2 are the nanom converter's; l3 and l4 need nanotins.
            if (nanotins_only(s)) needs_nanotins = true;
            if (s == "--stage" && i + 1 < argc && std::string(argv[i + 1]) != "l1" &&
                std::string(argv[i + 1]) != "l2") {
                needs_nanotins = true;
            }
            forwarded.push_back(argv[i]);
        }
    }
    if (backend.empty()) backend = needs_nanotins ? "nanotins" : "nanom";
    const int n = static_cast<int>(forwarded.size());
    if (backend == "nanom") {
        if (needs_nanotins) {
            std::fprintf(stderr, "pcapng2lance: the nanom backend does not implement the option given; use --backend nanotins\n");
            return 2;
        }
        return nanom_convert_main(n, forwarded.data());
    }
    if (backend == "nanotins") return nanotins_convert_main(n, forwarded.data());
    std::fprintf(stderr, "pcapng2lance: unknown --backend '%s' (nanom or nanotins)\n", backend.c_str());
    return 2;
}
