// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "cli_subcommands.hpp"

#include "nanolance/nano_lance_reader.h"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/version.hpp"

#include <CLI/CLI.hpp>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void signal_handler(int /*signal*/) {
    g_stop_requested = 1;
}

// Portable handler install. SIGINT exists on every platform; SIGPIPE is POSIX-only
// (on Windows a broken pipe surfaces as a write error, which the IPC loop already handles).
bool install_signal_handlers() {
    bool ok = std::signal(SIGINT, signal_handler) != SIG_ERR;
#ifdef SIGPIPE
    ok = ok && std::signal(SIGPIPE, signal_handler) != SIG_ERR;
#endif
    return ok;
}

// stdin defaults to text mode on Windows, which mangles binary Arrow IPC bytes
// (CRLF translation, Ctrl-Z as EOF). No-op elsewhere.
void set_stdin_binary() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
}

}  // namespace

int nanolance_cli_import(int argc, char** argv) {
    CLI::App app{"Convert Arrow IPC streams (from a file or stdin) to Lance v2.2 datasets"};
    app.set_version_flag("--version", std::string(argv[0]) + " (nanolance " + nanolance::library_version() + ")");

    std::string output_path;
    std::string input_path;
    std::string inspect_path;
    bool create = false;
    bool append = false;
    bool ignore_nullability = false;
    bool compress = false;
    bool no_structural = false;
    int compression_level = 3;
    std::uint64_t max_pending_bytes = 0;

    app.add_option("-o,--output", output_path, "Output Lance dataset path");
    app.add_option("-i,--input", input_path,
                   "Read the Arrow IPC stream from this file instead of stdin");
    app.add_option("--inspect", inspect_path, "Print latest manifest metadata as JSON and exit (no IPC conversion)");
    app.add_flag("-c,--create", create, "Create a new dataset");
    app.add_flag("-a,--append", append, "Append to an existing dataset");
    app.add_flag("--ignore-nullability",
                 ignore_nullability,
                 "No-op, accepted for compatibility: nullable-flagged fields are accepted by default now, "
                 "and nulls are written through Lance's definition levels. A null struct is still refused.");
    app.add_flag("--compress", compress,
                 "zstd-compress variable-width (string/binary) columns (Lance-compatible)");
    app.add_flag("--no-structural", no_structural,
                 "Disable structural encodings (bitpacking/constant/RLE/dictionary); emit plain pages");
    app.add_option("-l,--compression-level", compression_level, "Zstd compression level")->default_val(3);
    app.add_option("--max-pending-bytes", max_pending_bytes,
                   "Flush a fragment whenever the writer holds this many bytes of unwritten rows, bounding "
                   "memory (0 = off: one fragment for the whole stream). A batch is never split, so peak "
                   "memory is about this plus one IPC batch.")
        ->default_val(0);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }

    if (!inspect_path.empty()) {
        if (create || append) {
            std::cerr << "--inspect cannot be combined with --create or --append\n";
            return 2;
        }
        if (!output_path.empty()) {
            std::cerr << "omit -o/--output when using --inspect\n";
            return 2;
        }
        NanoLanceDatasetMetadata meta{};
        char err[512]{};
        if (nano_lance_dataset_read_latest(inspect_path.c_str(), &meta, err, sizeof err) != NANO_LANCE_READER_OK) {
            std::cerr << err << '\n';
            return 1;
        }
        std::cout << "{\n";
        std::cout << "  \"manifest_version\": " << meta.manifest_version << ",\n";
        std::cout << "  \"total_physical_rows\": " << meta.total_physical_rows << ",\n";
        std::cout << "  \"file_format\": \"" << (meta.file_format ? meta.file_format : "") << "\",\n";
        std::cout << "  \"format_version\": \"" << (meta.format_version ? meta.format_version : "") << "\",\n";
        std::cout << "  \"has_max_fragment_id\": " << (meta.has_max_fragment_id ? "true" : "false") << ",\n";
        std::cout << "  \"max_fragment_id\": " << meta.max_fragment_id << ",\n";
        std::cout << "  \"fields\": [\n";
        for (size_t i = 0; i < meta.fields_len; ++i) {
            const auto& f = meta.fields[i];
            std::cout << "    {\"id\": " << f.id << ", \"parent_id\": " << f.parent_id << ", \"name\": \""
                      << (f.name ? f.name : "") << "\", \"logical_type\": \"" << (f.logical_type ? f.logical_type : "")
                      << "\"}";
            if (i + 1 < meta.fields_len) {
                std::cout << ',';
            }
            std::cout << '\n';
        }
        std::cout << "  ],\n";
        std::cout << "  \"fragments\": [\n";
        for (size_t fi = 0; fi < meta.fragments_len; ++fi) {
            const auto& fr = meta.fragments[fi];
            std::cout << "    {\"id\": " << fr.id << ", \"physical_rows\": " << fr.physical_rows << ", \"files\": [\n";
            for (size_t di = 0; di < fr.files_len; ++di) {
                const auto& df = fr.files[di];
                std::cout << "      {\"path\": \"" << (df.path ? df.path : "") << "\", \"file_size_bytes\": "
                          << df.file_size_bytes << "}";
                if (di + 1 < fr.files_len) {
                    std::cout << ',';
                }
                std::cout << '\n';
            }
            std::cout << "    ]}";
            if (fi + 1 < meta.fragments_len) {
                std::cout << ',';
            }
            std::cout << '\n';
        }
        std::cout << "  ]\n}\n";
        nano_lance_dataset_metadata_free(&meta);
        return 0;
    }

    if (create == append) {
        std::cerr << "exactly one of --create or --append is required\n";
        return 2;
    }
    if (output_path.empty()) {
        std::cerr << "-o/--output is required for conversion mode\n";
        return 2;
    }

    // Reading only from stdin is a surprising default for a converter: `arrowipc2lance -i in.arrow`
    // is what anyone tries first, and `< in.arrow` is not available at all in a subprocess call that
    // does not go through a shell. Opening the file here (rather than requiring a redirect) also
    // gives a real error message for a missing or unreadable input.
    FILE* input_file = stdin;
    bool close_input = false;
    if (!input_path.empty()) {
        input_file = std::fopen(input_path.c_str(), "rb");
        if (input_file == nullptr) {
            std::cerr << "cannot open input file: " << input_path << '\n';
            return 1;
        }
        close_input = true;
    }

    if (!install_signal_handlers()) {
        std::cerr << "failed to install signal handlers\n";
        return 1;
    }
    if (!close_input) {
        set_stdin_binary();
    }

    // Every early return between here and ArrowIpcInputStreamInitFile has to close the input; after
    // that call the stream owns it (close_on_release below).
    const auto close_input_if_owned = [&]() {
        if (close_input && input_file != nullptr) {
            std::fclose(input_file);
            input_file = nullptr;
        }
    };

    NanoLanceWriter writer{};
    const int init_status = nano_lance_writer_init(&writer, output_path.c_str(), compression_level);
    if (init_status != NANO_LANCE_OK) {
        std::cerr << nano_lance_writer_last_error(&writer) << '\n';
        close_input_if_owned();
        return init_status;
    }
    const int nullability_status = nano_lance_writer_set_ignore_nullability(&writer, ignore_nullability);
    if (nullability_status != NANO_LANCE_OK) {
        std::cerr << nano_lance_writer_last_error(&writer) << '\n';
        nano_lance_writer_close(&writer);
        close_input_if_owned();
        return nullability_status;
    }
    const int compression_status = nano_lance_writer_set_compression(&writer, compress);
    if (compression_status != NANO_LANCE_OK) {
        std::cerr << nano_lance_writer_last_error(&writer) << '\n';
        nano_lance_writer_close(&writer);
        close_input_if_owned();
        return compression_status;
    }
    const int structural_status = nano_lance_writer_set_structural_encoding(&writer, !no_structural);
    if (structural_status != NANO_LANCE_OK) {
        std::cerr << nano_lance_writer_last_error(&writer) << '\n';
        nano_lance_writer_close(&writer);
        close_input_if_owned();
        return structural_status;
    }
    const int budget_status = nano_lance_writer_set_max_pending_bytes(&writer, max_pending_bytes);
    if (budget_status != NANO_LANCE_OK) {
        std::cerr << nano_lance_writer_last_error(&writer) << '\n';
        nano_lance_writer_close(&writer);
        close_input_if_owned();
        return budget_status;
    }

    ArrowIpcInputStream ipc_input{};
    int err = ArrowIpcInputStreamInitFile(&ipc_input, input_file, close_input ? 1 : 0);
    if (err != NANOARROW_OK) {
        std::cerr << "ArrowIpcInputStreamInitFile failed\n";
        nano_lance_writer_close(&writer);
        close_input_if_owned();
        return 1;
    }
    close_input = false;  // the IPC stream owns the FILE* now and closes it on release

    ArrowArrayStream ipc_stream{};
    ArrowIpcArrayStreamReaderOptions options{};
    std::memset(&options, 0, sizeof(options));
    options.field_index = -1;
    err = ArrowIpcArrayStreamReaderInit(&ipc_stream, &ipc_input, &options);
    if (err != NANOARROW_OK) {
        std::cerr << "ArrowIpcArrayStreamReaderInit failed\n";
        ipc_input.release(&ipc_input);
        nano_lance_writer_close(&writer);
        return 1;
    }

    ArrowSchema schema{};
    err = ipc_stream.get_schema(&ipc_stream, &schema);
    if (err != NANOARROW_OK || schema.release == nullptr) {
        std::cerr << "IPC stream schema read failed\n";
        if (ipc_stream.release != nullptr) {
            ipc_stream.release(&ipc_stream);
        }
        nano_lance_writer_close(&writer);
        return 1;
    }

    // Accumulate ONLY the core write work (ingest + encode + commit), excluding IPC parse and process
    // startup, so it can be compared apples-to-apples with an in-process writer like lance.write_dataset.
    double core_write_ms = 0.0;
    using clock = std::chrono::steady_clock;

    std::uint64_t batches = 0;
    while (g_stop_requested == 0) {
        ArrowArray batch{};
        err = ipc_stream.get_next(&ipc_stream, &batch);
        if (err != NANOARROW_OK) {
            std::cerr << "IPC stream batch read failed\n";
            schema.release(&schema);
            if (ipc_stream.release != nullptr) {
                ipc_stream.release(&ipc_stream);
            }
            nano_lance_writer_close(&writer);
            return 1;
        }
        if (batch.release == nullptr) {
            break;
        }
        const auto write_t0 = clock::now();
        const int write_status = nano_lance_write_batch(&writer, &batch, &schema);
        core_write_ms += std::chrono::duration<double, std::milli>(clock::now() - write_t0).count();
        batch.release(&batch);
        if (write_status != NANO_LANCE_OK) {
            std::cerr << nano_lance_writer_last_error(&writer) << '\n';
            schema.release(&schema);
            if (ipc_stream.release != nullptr) {
                ipc_stream.release(&ipc_stream);
            }
            nano_lance_writer_close(&writer);
            return write_status;
        }
        ++batches;
    }

    schema.release(&schema);
    if (ipc_stream.release != nullptr) {
        ipc_stream.release(&ipc_stream);
    }

    if (batches > 0) {
        const auto commit_t0 = clock::now();
        const int commit_status = nano_lance_writer_commit(&writer, append);
        core_write_ms += std::chrono::duration<double, std::milli>(clock::now() - commit_t0).count();
        if (commit_status != NANO_LANCE_OK) {
            std::cerr << nano_lance_writer_last_error(&writer) << '\n';
            nano_lance_writer_close(&writer);
            return commit_status;
        }
    }

    const int close_status = nano_lance_writer_close(&writer);
    if (close_status != NANO_LANCE_OK) {
        std::cerr << nano_lance_writer_last_error(&writer) << '\n';
        return close_status;
    }
    std::cerr << argv[0] << ": committed " << batches << " complete IPC batches to " << output_path << '\n';
    std::cerr << "nl_write_ms=" << core_write_ms << '\n';  // core ingest+encode+commit only (machine-readable)
    return 0;
}

#ifndef NANOLANCE_CLI_SUBCOMMAND
// Standalone build of this tool. The `nanolance` binary compiles the same file with
// NANOLANCE_CLI_SUBCOMMAND defined and calls nanolance_cli_import from its dispatcher instead.
int main(int argc, char** argv) { return nanolance_cli_import(argc, argv); }
#endif
