// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// `nanolance` -- one CLI for the tools that used to be four separate binaries.
//
// The dispatch is deliberately thin: it picks a subcommand, rewrites argv[0] so the subcommand's own
// help text names itself correctly, and hands the rest over untouched. Every flag each tool accepted
// as a standalone binary it still accepts here, because it is the same code parsing the same
// arguments -- there is no second argument grammar to drift out of step.

#include "cli_subcommands.hpp"

#include "nanolance/version.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Subcommand {
    const char* name;
    const char* summary;
    int (*run)(int argc, char** argv);
    const char* standalone;  ///< the binary this used to be, still built under that name
};

constexpr Subcommand kSubcommands[] = {
    {"import", "Convert an Arrow IPC stream (file or stdin) into a Lance dataset", &nanolance_cli_import,
     "arrowipc2lance"},
    {"info", "Print a dataset's fragments, rows per fragment and data file sizes", &nanolance_cli_info,
     "nlance_info"},
    {"cat", "Dump a dataset's rows as CSV or NDJSON", &nanolance_cli_cat, "nlance2table"},
    {"stitch", "Combine several free-standing datasets into one", &nanolance_cli_stitch, "nlance_stitch"},
};

void print_usage(std::ostream& out) {
    out << "nanolance " << nanolance::library_version() << " -- Lance v2.2 datasets, without the stack\n\n"
        << "Usage: nanolance <command> [options]\n"
           "       nanolance <command> --help\n\n"
           "Commands:\n";
    for (const auto& sub : kSubcommands) {
        out << "  " << sub.name;
        for (std::size_t pad = std::strlen(sub.name); pad < 8U; ++pad) {
            out << ' ';
        }
        out << sub.summary << '\n';
    }
    out << "\nOptions:\n"
           "  -h, --help     Show this message\n"
           "  --version      Print the nanolance version\n\n"
           "Each command is also installed under its original name (";
    for (std::size_t i = 0; i < std::size(kSubcommands); ++i) {
        out << (i == 0 ? "" : ", ") << kSubcommands[i].standalone;
    }
    out << "),\nwhich behaves identically -- it is the same code.\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(std::cerr);
        return 1;
    }

    const std::string command = argv[1];
    if (command == "-h" || command == "--help" || command == "help") {
        print_usage(std::cout);
        return 0;
    }
    if (command == "--version" || command == "-V") {
        std::cout << "nanolance " << nanolance::library_version() << '\n';
        return 0;
    }

    for (const auto& sub : kSubcommands) {
        if (command != sub.name) {
            continue;
        }
        // The subcommand parses argv itself, and prints argv[0] in its usage. Give it a name a reader
        // can retype: "nanolance import", not "nanolance" and not "arrowipc2lance".
        std::string program = std::string("nanolance ") + sub.name;
        std::vector<char*> forwarded;
        forwarded.reserve(static_cast<std::size_t>(argc));
        forwarded.push_back(program.data());
        for (int i = 2; i < argc; ++i) {
            forwarded.push_back(argv[i]);
        }
        forwarded.push_back(nullptr);
        return sub.run(static_cast<int>(forwarded.size()) - 1, forwarded.data());
    }

    std::cerr << "nanolance: unknown command '" << command << "'\n\n";
    print_usage(std::cerr);
    return 1;
}
