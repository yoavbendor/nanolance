// SPDX-License-Identifier: Apache-2.0
// pcapng2lance: the pcap / pcapng -> Lance converter. The work is in nanom_convert.cpp (the packet table, the staged
// layers, the one-shot protocol tables, and the streaming, chunking and slicing options); this file only answers
// --gpu, which is a planned feature rather than something this build has.
#include "nanom_convert.hpp"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gpu") == 0) {
            std::fprintf(stderr, "pcapng2lance: --gpu (GPU bulk decode) is a planned future feature, not supported in this build\n");
            return 2;
        }
    }
    return nanom_convert_main(argc, argv);
}
