// Lance's RaBitQ rotation, for the IVF_RQ builder (vector_index_build.cpp). The rotation is the one
// vector_search.cpp applies to queries, exported for the build.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nano_lance::rq_build {

/// The fast rotation of `in` into `out` (dim values, a multiple of 8): 4 rounds of sign flips and
/// Walsh-Hadamard transforms, with the sign bytes of the model (4 rounds of dim / 8 bytes).
void rotate(const float* in, std::size_t dim, const std::uint8_t* signs, float* out);

}  // namespace nano_lance::rq_build
