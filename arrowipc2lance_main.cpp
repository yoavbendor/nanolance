#include "nanolance/nano_lance_reader.h"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/version.hpp"

#include <CLI/CLI.hpp>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

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

int main(int argc, char** argv) {
    CLI::App app{"Convert Arrow IPC streams from stdin to Lance v2.2 datasets"};
    app.set_version_flag("--version", std::string("arrowipc2lance (nanolance ") + nanolance::library_version() + ")");

    std::string output_path;
    std::string inspect_path;
    bool create = false;
    bool append = false;
    bool ignore_nullability = false;
    int compression_level = 3;

    app.add_option("-o,--output", output_path, "Output Lance dataset path");
    app.add_option("--inspect", inspect_path, "Print latest manifest metadata as JSON and exit (no IPC conversion)");
    app.add_flag("-c,--create", create, "Create a new dataset");
    app.add_flag("-a,--append", append, "Append to an existing dataset");
    app.add_flag("--ignore-nullability",
                 ignore_nullability,
                 "Treat fixed-width nullable Arrow fields as non-null Lance fields and copy null slot bytes as-is");
    app.add_option("-l,--compression-level", compression_level, "Zstd compression level")->default_val(3);

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

    if (!install_signal_handlers()) {
        std::cerr << "failed to install signal handlers\n";
        return 1;
    }
    set_stdin_binary();

    NanoLanceWriter writer{};
    const int init_status = nano_lance_writer_init(&writer, output_path.c_str(), compression_level);
    if (init_status != NANO_LANCE_OK) {
        std::cerr << nano_lance_writer_last_error(&writer) << '\n';
        return init_status;
    }
    const int nullability_status = nano_lance_writer_set_ignore_nullability(&writer, ignore_nullability);
    if (nullability_status != NANO_LANCE_OK) {
        std::cerr << nano_lance_writer_last_error(&writer) << '\n';
        nano_lance_writer_close(&writer);
        return nullability_status;
    }

    ArrowIpcInputStream ipc_input{};
    int err = ArrowIpcInputStreamInitFile(&ipc_input, stdin, 0);
    if (err != NANOARROW_OK) {
        std::cerr << "ArrowIpcInputStreamInitFile failed\n";
        nano_lance_writer_close(&writer);
        return 1;
    }

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
        const int write_status = nano_lance_write_batch(&writer, &batch, &schema);
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
        const int commit_status = nano_lance_writer_commit(&writer, append);
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
    std::cerr << "arrowipc2lance: committed " << batches << " complete IPC batches to " << output_path << '\n';
    return 0;
}
