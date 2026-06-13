// GPU/CPU equivalence + benchmark harness for the spec/DAG decode.
//
// Builds N synthetic Ethernet packets (a round-robin of eth/ipv4/udp, eth/ipv6/tcp, eth/vlan/ipv4/udp, and
// eth/gPTP-Sync, with per-packet-varying fields) into one contiguous window, then decodes them into the
// per-node DAG tables THREE ways and checks they are BIT-EXACT equal:
//
//     serial   nanotins::serial_for_each            (in-thread reference)
//     bulk     nanotins::bulk_for_each on a pool     (CPU thread pool)
//     gpu      nanotins::gpu::dag_decode_window_gpu  (nvexec; only under NANOTINS_ENABLE_CUDA)
//
// and prints per-mode timing (ns/packet + Mpps) so gpu ~= cpu is easy to read off after building on a CUDA
// host. The count/scan/scatter writes are pure functions of each packet's prefix-summed base, so all three
// MUST agree to the byte — any difference is a real bug.
//
// ============================ RUN ON THE GPU HOST ============================
// Build the example with -DNANOTINS_ENABLE_CUDA=ON, then:
//     ./dag_gpu_equiv_bench --packets 32000000 --bench
// Expect: "EQUIVALENCE: serial == bulk == gpu (bit-exact)" and a timing table. The CTest entry runs a
// small N automatically (serial==bulk, plus ==gpu on a CUDA build). 32M needs a few GB RAM (the window +
// the per-mode output tables); drop --packets if memory-bound.
// ============================================================================

#include "nanotins/bulk.hpp"
#include "nanotins/dag_bulk.hpp"
#include "nanotins/dag_decode.hpp"
#include "nanotins/pcap_blocks.hpp"  // pcapblocks::Bytes
#include "nanotins/spec_dag.hpp"

#include <exec/static_thread_pool.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifdef NANOTINS_ENABLE_CUDA
#include "gputins/dag_decode_gpu.hpp"
#include "gputins/gpu.hpp"
#endif

using G = nanotins::L2L3Graph;

namespace {

void p16(std::uint8_t* b, std::uint16_t v) { b[0] = std::uint8_t(v >> 8); b[1] = std::uint8_t(v); }

// Append one synthetic Ethernet packet (template t, index i) to `win`; return its byte size. All packets
// are Ethernet (DAG root); fields vary with i so the scattered columns are non-trivial.
std::size_t emit_packet(std::vector<std::uint8_t>& win, int t, std::uint64_t i) {
    const std::size_t base = win.size();
    auto put = [&](std::size_t off, std::uint16_t v) { p16(&win[base + off], v); };
    const std::uint16_t vi = static_cast<std::uint16_t>(i);
    if (t == 0) {  // eth / ipv4 / udp (42)
        win.resize(base + 42, 0);
        put(12, 0x0800);
        win[base + 14] = 0x45;          // ipv4 ver/ihl
        win[base + 14 + 9] = 17;        // udp
        put(14 + 4, vi);                // ipv4 identification (varies)
        put(34, vi);                    // udp src_port (varies)
        put(36, static_cast<std::uint16_t>(vi ^ 0xABCD));
        return 42;
    }
    if (t == 1) {  // eth / ipv6 / tcp (74)
        win.resize(base + 74, 0);
        put(12, 0x86DD);
        win[base + 14] = 0x60;          // ipv6 version
        win[base + 14 + 6] = 6;         // next_header tcp
        win[base + 54 + 12] = 0x50;     // tcp data_offset 5
        put(54, vi);                    // tcp src_port (varies)
        return 74;
    }
    if (t == 2) {  // eth / vlan / ipv4 / udp (46)
        win.resize(base + 46, 0);
        put(12, 0x8100);
        put(16, 0x0800);                // inner ethertype ipv4
        win[base + 18] = 0x45;
        win[base + 18 + 9] = 17;        // udp
        put(38, vi);                    // udp src_port (varies)
        return 46;
    }
    // t == 3: eth / gPTP Sync (58) -> common header + PtpTimestampBody
    win.resize(base + 58, 0);
    put(12, 0x88F7);
    win[base + 14] = nanotins::kPtpMsgSync;  // message_type 0 (low nibble)
    p16(&win[base + 14 + 30], vi);           // ptp sequence_id @ header offset 30 (varies)
    return 58;
}

struct Synth {
    std::vector<std::uint8_t> window;
    std::vector<std::uint64_t> poff;
    std::vector<std::uint32_t> psize;
    std::vector<std::uint16_t> link_type;
    std::vector<nanotins::dag_packet> pkts;
};

Synth make_synth(std::uint64_t n) {
    Synth s;
    s.poff.reserve(n);
    s.psize.reserve(n);
    s.link_type.assign(n, 1 /*LINKTYPE_ETHERNET*/);
    s.window.reserve(n * 56);
    for (std::uint64_t i = 0; i < n; ++i) {
        const std::size_t off = s.window.size();
        const std::size_t sz = emit_packet(s.window, static_cast<int>(i & 3), i);
        s.poff.push_back(off);
        s.psize.push_back(static_cast<std::uint32_t>(sz));
    }
    s.pkts.reserve(n);
    for (std::uint64_t i = 0; i < n; ++i) {
        s.pkts.push_back({s.window.data() + s.poff[i], s.psize[i], i});
    }
    return s;
}

// std::vector and std::tuple<std::vector...> both have operator==, so a node table compares by its
// packet_id vector + its columns tuple; fold that over every node.
template <class Tables>
bool tables_equal(const Tables& a, const Tables& b) {
    bool ok = true;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((ok = ok && std::get<I>(a).packet_id == std::get<I>(b).packet_id &&
                std::get<I>(a).columns == std::get<I>(b).columns),
         ...);
    }(std::make_index_sequence<std::tuple_size_v<Tables>>{});
    return ok;
}

double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

void report(const char* mode, std::uint64_t n, double dt) {
    std::printf("  %-8s %9.3f ms   %7.2f ns/pkt   %8.2f Mpps\n", mode, dt * 1e3,
                dt * 1e9 / static_cast<double>(n), static_cast<double>(n) / dt / 1e6);
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t n = 200000;
    unsigned threads = 0;
    std::size_t tasks = 256;
    bool bench = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--packets" && i + 1 < argc) n = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--threads" && i + 1 < argc) threads = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (a == "--tasks" && i + 1 < argc) tasks = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (a == "--bench") bench = true;
    }

    std::printf("dag_gpu_equiv_bench: %llu packets (eth/ipv4-udp, eth/ipv6-tcp, eth/vlan/ipv4-udp, eth/gptp)\n",
                static_cast<unsigned long long>(n));
    const Synth s = make_synth(n);
    const int root = nanotins::kEthRoot;
    const pcapblocks::Bytes win(s.window.data(), s.window.size());

    // serial
    nanotins::dag_tables<G> serial;
    auto t0 = std::chrono::steady_clock::now();
    nanotins::dag_decode_bulk<G>(s.pkts, serial, root,
                                 [](std::size_t nt, std::size_t m, auto k) { nanotins::serial_for_each(nt, m, k); });
    auto t1 = std::chrono::steady_clock::now();

    // bulk (CPU pool)
    const unsigned nthr = threads ? threads : std::max(1u, std::thread::hardware_concurrency());
    exec::static_thread_pool pool{nthr};
    auto sched = pool.get_scheduler();
    nanotins::dag_tables<G> bulk;
    auto t2 = std::chrono::steady_clock::now();
    nanotins::dag_decode_bulk<G>(s.pkts, bulk, root, [&](std::size_t nt, std::size_t m, auto k) {
        nanotins::bulk_for_each(sched, nt, m, k);
    });
    auto t3 = std::chrono::steady_clock::now();

    if (!tables_equal(serial, bulk)) {
        std::fprintf(stderr, "FAIL: serial != bulk (bit-exact mismatch)\n");
        return 1;
    }

    bool gpu_ran = false;
    double gpu_dt = 0;
#ifdef NANOTINS_ENABLE_CUDA
    nanotins::gpu::context ctx(0);
    nanotins::dag_tables<G> gpu;
    auto g0 = std::chrono::steady_clock::now();
    nanotins::gpu::dag_decode_window_gpu<G>(ctx.scheduler(), tasks, /*pid_base=*/0, s.link_type.data(),
                                            s.poff.data(), s.psize.data(), win, n, gpu);
    auto g1 = std::chrono::steady_clock::now();
    gpu_dt = secs(g0, g1);
    gpu_ran = true;
    if (!tables_equal(serial, gpu)) {
        std::fprintf(stderr, "FAIL: serial != gpu (bit-exact mismatch)\n");
        return 1;
    }
#else
    (void)win;
    (void)tasks;
#endif

    // row-count summary (proves the synth exercised every node)
    constexpr std::size_t kEth = nanotins::node_id_v<nanotins::EthNode, G>;
    constexpr std::size_t kUdp = nanotins::node_id_v<nanotins::UdpNode, G>;
    constexpr std::size_t kTcp = nanotins::node_id_v<nanotins::TcpNode, G>;
    constexpr std::size_t kGptp = nanotins::node_id_v<nanotins::GptpNode, G>;
    std::printf("  rows: eth=%zu udp=%zu tcp=%zu gptp=%zu\n", std::get<kEth>(serial).size(),
                std::get<kUdp>(serial).size(), std::get<kTcp>(serial).size(), std::get<kGptp>(serial).size());

    std::printf("EQUIVALENCE: serial == bulk%s (bit-exact)\n", gpu_ran ? " == gpu" : " (gpu: CPU build, skipped)");

    if (bench) {
        std::printf("timing:\n");
        report("serial", n, secs(t0, t1));
        report("bulk", n, secs(t2, t3));
        if (gpu_ran) {
            report("gpu", n, gpu_dt);
            std::printf("  gpu/cpu(bulk) speedup: %.2fx\n", secs(t2, t3) / gpu_dt);
        }
    }
    return 0;
}
