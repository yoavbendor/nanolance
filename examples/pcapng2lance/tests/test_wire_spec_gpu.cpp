// wire_spec GPU smoke (T5): parse headers on the GPU via scatter_spec_pod and assert the columns are
// byte-identical to the CPU spec_soa — for UDP (scalars), IPv4 (bit-fields), and IPv6 (byte-crossing
// bit-fields + 16-byte fixed-size-binary device columns). Behind NANOTINS_ENABLE_CUDA; a CPU build is a
// trivial pass (the gputins header compiles to nothing), so the normal suite stays green; the CUDA host
// runs the real GPU==CPU check. See nanotins/docs/GPU_BULK_INTEGRATION.md.

#include "gputins/wire_spec_gpu.hpp"

#include "nanotins/wire_spec.hpp"
#include "nanotins/wire_spec_soa.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using namespace nanotins::literals;
using nanotins::wire_endian;

namespace {

using UdpHdrSpec = nanotins::WireSpec<
    nanotins::named_field<decltype("src_port"_fld), 0, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("dst_port"_fld), 2, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("length"_fld), 4, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("checksum"_fld), 6, std::uint16_t, wire_endian::big>>;

using Ipv4Spec = nanotins::WireSpec<
    nanotins::named_bit_field<decltype("version"_fld), 0, std::uint8_t, 0, 4, wire_endian::big>,
    nanotins::named_bit_field<decltype("ihl"_fld), 0, std::uint8_t, 4, 4, wire_endian::big>,
    nanotins::named_bit_field<decltype("dscp"_fld), 1, std::uint8_t, 0, 6, wire_endian::big>,
    nanotins::named_bit_field<decltype("ecn"_fld), 1, std::uint8_t, 6, 2, wire_endian::big>,
    nanotins::named_field<decltype("total_length"_fld), 2, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("identification"_fld), 4, std::uint16_t, wire_endian::big>,
    nanotins::named_bit_field<decltype("flags"_fld), 6, std::uint16_t, 0, 3, wire_endian::big>,
    nanotins::named_bit_field<decltype("frag_offset"_fld), 6, std::uint16_t, 3, 13, wire_endian::big>,
    nanotins::named_field<decltype("ttl"_fld), 8, std::uint8_t, wire_endian::big>,
    nanotins::named_field<decltype("protocol"_fld), 9, std::uint8_t, wire_endian::big>,
    nanotins::named_field<decltype("checksum"_fld), 10, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("src_addr"_fld), 12, std::uint32_t, wire_endian::big>,
    nanotins::named_field<decltype("dst_addr"_fld), 16, std::uint32_t, wire_endian::big>>;

using Ipv6Spec = nanotins::WireSpec<
    nanotins::named_bit_field<decltype("version"_fld), 0, std::uint32_t, 0, 4, wire_endian::big>,
    nanotins::named_bit_field<decltype("traffic_class"_fld), 0, std::uint32_t, 4, 8, wire_endian::big>,
    nanotins::named_bit_field<decltype("flow_label"_fld), 0, std::uint32_t, 12, 20, wire_endian::big>,
    nanotins::named_field<decltype("payload_length"_fld), 4, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("next_header"_fld), 6, std::uint8_t, wire_endian::big>,
    nanotins::named_field<decltype("hop_limit"_fld), 7, std::uint8_t, wire_endian::big>,
    nanotins::named_bytes_field<decltype("src_addr"_fld), 8, 16>,
    nanotins::named_bytes_field<decltype("dst_addr"_fld), 24, 16>>;

// Deterministic bytes — the actual values don't matter, only that CPU and GPU read the same bytes.
std::vector<std::uint8_t> fill_bytes(std::size_t n, std::size_t stride) {
    std::vector<std::uint8_t> v(n * stride);
    for (std::size_t k = 0; k < v.size(); ++k) {
        v[k] = static_cast<std::uint8_t>((k * 131u + 7u) & 0xFFu);
    }
    return v;
}

}  // namespace

#ifdef NANOTINS_ENABLE_CUDA

namespace {

// Parse the same bytes on CPU (spec_soa) and GPU (parse_spec_gpu) and compare every column.
template <class Spec, std::size_t N>
bool gpu_matches_cpu(std::size_t stride) {
    const std::vector<std::uint8_t> hdrs = fill_bytes(N, stride);

    nanotins::spec_soa<Spec, N> cpu;
    for (std::size_t i = 0; i < N; ++i) {
        cpu.append(&hdrs[i * stride]);
    }

    nanotins::gpu::context ctx(0);
    nanotins::spec_soa<Spec, N> gpu_out;
    nanotins::gpu::parse_spec_gpu<Spec>(ctx, 256, hdrs.data(), stride, N, gpu_out.raw());

    bool ok = true;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((ok = ok &&
               [&] {
                   for (std::size_t i = 0; i < N; ++i) {
                       if (!(cpu.template column<I>()[i] == gpu_out.template column<I>()[i])) {
                           return false;
                       }
                   }
                   return true;
               }()),
         ...);
    }(std::make_index_sequence<nanotins::spec_soa<Spec, N>::ncols>{});
    return ok;
}

}  // namespace

int main() {
    constexpr std::size_t N = 1024;
    struct Case {
        const char* name;
        bool ok;
    };
    const Case cases[] = {
        {"udp (4 scalar cols)", gpu_matches_cpu<UdpHdrSpec, N>(8)},
        {"ipv4 (bit-fields + scalars, 13 cols)", gpu_matches_cpu<Ipv4Spec, N>(20)},
        {"ipv6 (byte-crossing bit-fields + 16B fixed-binary, 8 cols)", gpu_matches_cpu<Ipv6Spec, N>(40)},
    };
    bool all = true;
    for (const Case& c : cases) {
        std::fprintf(stderr, "  %-58s : %s\n", c.name, c.ok ? "GPU == CPU" : "MISMATCH");
        all = all && c.ok;
    }
    if (!all) {
        std::fprintf(stderr, "wire_spec_gpu: FAIL — a protocol mismatched\n");
        return 1;
    }
    std::printf("wire_spec_gpu: GPU == CPU over %zu headers each for udp / ipv4 / ipv6\n", N);
    return 0;
}

#else

int main() {
    std::printf("wire_spec_gpu: skipped (build with -DNANOTINS_ENABLE_CUDA on a CUDA host)\n");
    return 0;
}

#endif
