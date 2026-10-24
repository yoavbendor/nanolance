// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Lance's RaBitQ fast rotation (index/vector/rabitq_build.hpp), which IVF_RQ applies to vectors at build time
// and to queries at search time: 4 rounds of sign flips and normalized Walsh-Hadamard transforms (with Kac
// mixing for a dimension that is not a power of two). It is orthogonal, so for dimensions 8, 16, 24, 64 and
// 96 and random signs it keeps norms and inner products, is linear, is deterministic, and differs from the
// identity; flipping every sign of the first round negates the output.

#include "index/vector/rabitq_build.hpp"

#include "index_test_util.hpp"

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using index_test::require;

namespace {

double dot(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        s += static_cast<double>(a[i]) * b[i];
    }
    return s;
}

std::vector<float> rotated(const std::vector<float>& v, const std::vector<std::uint8_t>& signs) {
    std::vector<float> out(v.size());
    nano_lance::rq_build::rotate(v.data(), v.size(), signs.data(), out.data());
    return out;
}

}  // namespace

int main() {
    std::mt19937 rng(5);
    std::normal_distribution<float> normal(0.0F, 1.0F);
    for (const std::size_t dim : {8U, 16U, 24U, 64U, 96U}) {
        const std::string at = " (dim " + std::to_string(dim) + ")";
        std::vector<std::uint8_t> signs(4 * dim / 8);
        for (auto& s : signs) {
            s = static_cast<std::uint8_t>(rng());
        }
        std::vector<float> a(dim);
        std::vector<float> b(dim);
        for (std::size_t i = 0; i < dim; ++i) {
            a[i] = normal(rng);
            b[i] = normal(rng);
        }
        const auto ra = rotated(a, signs);
        const auto rb = rotated(b, signs);
        const double tol = 1e-3 * (1.0 + dot(a, a) + dot(b, b));
        require(std::fabs(dot(ra, ra) - dot(a, a)) < tol, "the norm is kept" + at);
        require(std::fabs(dot(ra, rb) - dot(a, b)) < tol, "inner products are kept" + at);
        require(rotated(a, signs) == ra, "deterministic" + at);
        double moved = 0;
        for (std::size_t i = 0; i < dim; ++i) {
            moved += std::fabs(ra[i] - a[i]);
        }
        require(moved > 1e-3, "not the identity" + at);
        std::vector<float> sum(dim);
        for (std::size_t i = 0; i < dim; ++i) {
            sum[i] = a[i] + 2.0F * b[i];
        }
        const auto rs = rotated(sum, signs);
        for (std::size_t i = 0; i < dim; ++i) {
            require(std::fabs(rs[i] - (ra[i] + 2.0F * rb[i])) < 1e-3F, "linear" + at);
        }
        auto flipped = signs;
        for (std::size_t i = 0; i < dim / 8; ++i) {
            flipped[i] = static_cast<std::uint8_t>(~flipped[i]);
        }
        const auto rn = rotated(a, flipped);
        for (std::size_t i = 0; i < dim; ++i) {
            require(std::fabs(rn[i] + ra[i]) < 1e-4F, "flipping the first round's signs negates" + at);
        }
    }
    std::cout << "rabitq_build: ok (orthogonal, linear, deterministic fast rotation)\n";
    return 0;
}
