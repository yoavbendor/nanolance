// Gather/stitch test: merging N free-standing single-fragment datasets into one master (Option A: relocate
// fragments into one data/ + one manifest, no data re-encode) must produce a dataset byte-identical — read
// back through the full Lance reader — to a single dataset written with the same rows in the same order via
// successive appends. Also: the move leaves the sources hollow (deleted), data-file name collisions across
// sources are resolved, and a schema-mismatched source is rejected.
//
// argv[1] = path to the nlance2table executable (for the dump-and-diff, no Python).

#include "nanolance/dataset_stitcher.hpp"
#include "nanolance/nano_lance_reader.h"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

void write_batch_i64(NanoLanceWriter& w, const std::vector<std::int64_t>& vals) {
    ArrowArray batch{};
    batch.length = static_cast<std::int64_t>(vals.size());
    const void* buffers[] = {nullptr, vals.data()};
    batch.n_buffers = 2;
    batch.buffers = buffers;
    ArrowSchema field{};
    field.format = "l";  // int64
    field.name = "id";
    field.flags = 0;
    CHECK(nano_lance_write_batch(&w, &batch, &field) == NANO_LANCE_OK);
}

void write_batch_f64(NanoLanceWriter& w, const std::vector<double>& vals) {
    ArrowArray batch{};
    batch.length = static_cast<std::int64_t>(vals.size());
    const void* buffers[] = {nullptr, vals.data()};
    batch.n_buffers = 2;
    batch.buffers = buffers;
    ArrowSchema field{};
    field.format = "g";  // float64 — a different schema than "id:int64"
    field.name = "id";
    field.flags = 0;
    CHECK(nano_lance_write_batch(&w, &batch, &field) == NANO_LANCE_OK);
}

// A fresh single-fragment dataset of int64 ids.
void new_dataset(const fs::path& path, const std::vector<std::int64_t>& vals) {
    std::error_code ec;
    fs::remove_all(path, ec);
    NanoLanceWriter w{};
    CHECK(nano_lance_writer_init(&w, path.string().c_str(), 3) == NANO_LANCE_OK);
    write_batch_i64(w, vals);
    CHECK(nano_lance_writer_commit(&w, /*is_append=*/false) == NANO_LANCE_OK);
    CHECK(nano_lance_writer_close(&w) == NANO_LANCE_OK);
}

// Append one more fragment of int64 ids to an existing dataset.
void append_dataset(const fs::path& path, const std::vector<std::int64_t>& vals) {
    NanoLanceWriter w{};
    CHECK(nano_lance_writer_init_append(&w, path.string().c_str(), 3) == NANO_LANCE_OK);
    write_batch_i64(w, vals);
    CHECK(nano_lance_writer_commit(&w, /*is_append=*/true) == NANO_LANCE_OK);
    CHECK(nano_lance_writer_close(&w) == NANO_LANCE_OK);
}

std::string g_tool;

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string dump_csv(const fs::path& dataset, const fs::path& out_csv) {
    const std::string inner =
        "\"" + g_tool + "\" \"" + dataset.string() + "\" -f csv -o \"" + out_csv.string() + "\"";
#ifdef _WIN32
    const std::string cmd = "\"" + inner + "\"";
#else
    const std::string cmd = inner;
#endif
    CHECK(std::system(cmd.c_str()) == 0);
    return read_file(out_csv);
}

}  // namespace

int main(int argc, char** argv) {
    CHECK(argc >= 2);
    g_tool = argv[1];

    const fs::path tmp = fs::temp_directory_path() / "nano_lance_stitch_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    // Three worker outputs with SHUFFLED item numbers in their folder names; ids chosen so order is visible.
    const fs::path s2 = tmp / "results_a_2.lance";  // item 2 -> ids 4,5
    const fs::path s0 = tmp / "results_b_0.lance";  // item 0 -> ids 0,1
    const fs::path s1 = tmp / "results_c_1.lance";  // item 1 -> ids 2,3
    new_dataset(s2, {4, 5});
    new_dataset(s0, {0, 1});
    new_dataset(s1, {2, 3});

    // Stitch in ITEM order (0,1,2) — the order nlance-stitch derives from the names.
    const fs::path master = tmp / "master.lance";
    nano_lance::StitchSummary summary;
    std::string err;
    CHECK(nano_lance::stitch_datasets(master, {s0, s1, s2}, nano_lance::StitchOptions{}, summary, err));
    CHECK(summary.source_count == 3);
    CHECK(summary.fragment_count == 3);
    CHECK(summary.total_rows == 6);

    // Move + cleanup: the source folders are gone, and the master has 3 collision-free fragment files.
    CHECK(!fs::exists(s0) && !fs::exists(s1) && !fs::exists(s2));
    CHECK(fs::exists(master / "data" / "fragment-0.lance"));
    CHECK(fs::exists(master / "data" / "fragment-1.lance"));
    CHECK(fs::exists(master / "data" / "fragment-2.lance"));

    // Manifest sanity via the reader.
    {
        NanoLanceDatasetMetadata meta{};
        char merr[512]{};
        CHECK(nano_lance_dataset_read_latest(master.string().c_str(), &meta, merr, sizeof merr) ==
              NANO_LANCE_READER_OK);
        CHECK(meta.fragments_len == 3);
        CHECK(meta.total_physical_rows == 6);
        CHECK(meta.max_fragment_id == 2);
        nano_lance_dataset_metadata_free(&meta);
    }

    // The golden: a single dataset written with the same rows in item order via successive appends has the
    // same 3-fragment layout. The stitched master must dump identically (values + order + schema).
    const fs::path ref = tmp / "reference.lance";
    new_dataset(ref, {0, 1});
    append_dataset(ref, {2, 3});
    append_dataset(ref, {4, 5});

    const std::string master_csv = dump_csv(master, tmp / "master.csv");
    const std::string ref_csv = dump_csv(ref, tmp / "ref.csv");
    if (master_csv != ref_csv) {
        std::fprintf(stderr, "FAIL stitched master != in-order reference\n--- master ---\n%s\n--- ref ---\n%s\n",
                     master_csv.c_str(), ref_csv.c_str());
        std::exit(1);
    }
    CHECK(!master_csv.empty());

    // Negative: a source whose schema differs from the first is rejected.
    {
        const fs::path a = tmp / "results_x_0.lance";  // int64 id
        const fs::path b = tmp / "results_y_1.lance";  // float64 id (mismatch)
        new_dataset(a, {7, 8});
        {
            std::error_code rec;
            fs::remove_all(b, rec);
            NanoLanceWriter w{};
            CHECK(nano_lance_writer_init(&w, b.string().c_str(), 3) == NANO_LANCE_OK);
            write_batch_f64(w, {1.0, 2.0});
            CHECK(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK);
            CHECK(nano_lance_writer_close(&w) == NANO_LANCE_OK);
        }
        nano_lance::StitchSummary s2sum;
        std::string e2;
        const bool ok = nano_lance::stitch_datasets(tmp / "master_bad.lance", {a, b},
                                                    nano_lance::StitchOptions{}, s2sum, e2);
        CHECK(!ok);
        CHECK(!e2.empty());
    }

    fs::remove_all(tmp, ec);
    std::printf("dataset_stitch: ok (stitched master == in-order reference via nlance2table; move+cleanup; "
                "schema-mismatch rejected)\n");
    return 0;
}
