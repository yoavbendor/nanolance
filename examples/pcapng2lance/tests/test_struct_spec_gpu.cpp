// struct_spec GPU smoke (T5): parse N UDP headers on the GPU via scatter_spec and assert the columns are
// byte-identical to the CPU spec_soa. Behind NANOTINS_ENABLE_CUDA — on a CPU build it is a trivial pass
// (the gputins header compiles to nothing), so the normal suite stays green; the CUDA host runs the real
// GPU==CPU check. See nanotins/docs/GPU_BULK_INTEGRATION.md for how to compile this TU as CUDA.

#include "gputins/struct_spec_gpu.hpp"

#include "nanotins/struct_spec.hpp"
#include "nanotins/struct_spec_soa.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace nanotins::literals;
using nanotins::wire_endian;

namespace {
using UdpHdrSpec = nanotins::StructSpec<
    nanotins::named_field<decltype("src_port"_fld), 0, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("dst_port"_fld), 2, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("length"_fld), 4, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("checksum"_fld), 6, std::uint16_t, wire_endian::big>>;

void put_udp(std::uint8_t* b, std::uint16_t s, std::uint16_t d, std::uint16_t l, std::uint16_t c) {
    b[0] = static_cast<std::uint8_t>(s >> 8); b[1] = static_cast<std::uint8_t>(s);
    b[2] = static_cast<std::uint8_t>(d >> 8); b[3] = static_cast<std::uint8_t>(d);
    b[4] = static_cast<std::uint8_t>(l >> 8); b[5] = static_cast<std::uint8_t>(l);
    b[6] = static_cast<std::uint8_t>(c >> 8); b[7] = static_cast<std::uint8_t>(c);
}
}  // namespace

#ifdef NANOTINS_ENABLE_CUDA

int main() {
    constexpr std::size_t N = 1024;
    std::vector<std::uint8_t> hdrs(N * 8);
    for (std::size_t i = 0; i < N; ++i) {
        put_udp(&hdrs[i * 8], static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(i * 3 + 1),
                static_cast<std::uint16_t>(8 + i), static_cast<std::uint16_t>(i ^ 0xFFFF));
    }

    nanotins::spec_soa<UdpHdrSpec, N> cpu;
    for (std::size_t i = 0; i < N; ++i) cpu.append(&hdrs[i * 8]);

    nanotins::gpu::context ctx(0);
    nanotins::spec_soa<UdpHdrSpec, N> gpu_out;
    nanotins::gpu::parse_spec_gpu<UdpHdrSpec>(ctx, 256, hdrs.data(), 8, N, gpu_out.raw());

    for (std::size_t i = 0; i < N; ++i) {
        if (cpu.column<0>()[i] != gpu_out.column<0>()[i] || cpu.column<1>()[i] != gpu_out.column<1>()[i] ||
            cpu.column<2>()[i] != gpu_out.column<2>()[i] || cpu.column<3>()[i] != gpu_out.column<3>()[i]) {
            std::fprintf(stderr, "FAIL: GPU != CPU at row %zu\n", i);
            std::exit(1);
        }
    }
    std::printf("struct_spec_gpu: GPU == CPU over %zu UDP headers (4 cols)\n", N);
    return 0;
}

#else

int main() {
    std::printf("struct_spec_gpu: skipped (build with -DNANOTINS_ENABLE_CUDA on a CUDA host)\n");
    return 0;
}

#endif
