// nlbench: time nanolance's native full-scan read of a dataset it wrote.
// Usage: nlbench <dataset.lance> [iters]   -> prints rows and avg read milliseconds (JSON).
#include "nanolance/lance_table_reader.hpp"

#include <nanoarrow/nanoarrow.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: nlbench <dataset.lance> [iters]\n";
        return 2;
    }
    const std::string path = argv[1];
    const int iters = argc >= 3 ? std::atoi(argv[2]) : 5;

    std::int64_t rows = 0;
    double best_ms = 1e30;
    double total_ms = 0.0;
    for (int it = 0; it < iters; ++it) {
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string error;
        const auto t0 = std::chrono::steady_clock::now();
        if (!nano_lance::lance_table_read_dataset(path, schema, batches, error)) {
            std::cerr << "read failed: " << error << '\n';
            return 1;
        }
        const auto t1 = std::chrono::steady_clock::now();
        rows = 0;
        for (auto& b : batches) {
            rows += b.length;
        }
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        best_ms = ms < best_ms ? ms : best_ms;
        total_ms += ms;
        ArrowSchemaRelease(&schema);
        for (auto& b : batches) {
            ArrowArrayRelease(&b);
        }
    }
    std::cout << "{\"rows\": " << rows << ", \"best_ms\": " << best_ms
              << ", \"avg_ms\": " << (total_ms / iters) << "}\n";
    return 0;
}
