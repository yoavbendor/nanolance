# GPU bulk integration (nvexec) — instructions for the CUDA-capable build

**Audience:** a model/engineer on the **Linux host with a GPU and a CUDA/clang-cuda toolchain**. This
machine (the one that wrote the scaffold) has **no CUDA**, so it could *write* the GPU path but not
*compile* it. Everything GPU is behind `#ifdef NANOTINS_ENABLE_CUDA`, so the normal CPU build is
unaffected and **all 21 tests pass without CUDA**. Your job is to finish the toolchain wiring, compile,
and verify. The code is intentionally minimal and mirrors the reference experiment
`stdexec_gpu_experiment/main_stdexec_gpu_unified.cpp` (its `run_bulk_cpu` / `run_bulk_gpu` differ **only**
by scheduler).

## The one idea

`nanotins::bulk_for_each(scheduler, num_tasks, n, kernel)` is **scheduler-agnostic**. The CPU path passes
`exec::static_thread_pool::scheduler`; the GPU path passes `nvexec::stream_context::scheduler` and the
**same kernel lambda runs on the GPU**. The only real differences:

1. the pointers the kernel captures must be **device** pointers (use `gpu::device_buffer`), and
2. you bracket the call with **H2D / D2H** copies.

The per-element functions it calls (`parse_epb`, `overlay`, `pack_ports`, …) are already `NANOTINS_HD`
(`__host__ __device__` under nvcc/clang-cuda), so they compile for device unchanged.

## What is already wired (scaffold)

| File | What |
|---|---|
| `gputins/include/gputins/gpu.hpp` | `gpu::context` (owns `nvexec::stream_context`), `gpu::device_buffer<T>` (RAII cudaMalloc/Free + H2D/D2H), `gpu::free_vram_bytes()`, `gpu::vram_budget(bytes,pct)`. All behind `NANOTINS_ENABLE_CUDA`. (All CUDA/nvexec code lives in the `gputins` library — a sibling of `nanotins`/`soatins` — so the CUDA dependency is isolated.) |
| `nanotins/include/nanotins/bulk.hpp` | `bulk_for_each` (used as-is with the GPU scheduler) + `serial_for_each`. (Stays in `nanotins`; reused by both CPU and GPU.) |
| `examples/pcapng2lance/src/pcapng2lance_main.cpp` | `L1Converter::parse_packets_gpu()` — the **GPU L1 parse**: H2D(window+BlockRefs) → `bulk_for_each(gpu_ctx_->scheduler(), …, parse_epb kernel)` → D2H(EpbView). Under `--decode-l2l3` it also calls `decode_window_gpu`. Selected by `--gpu`; window capped to the VRAM budget. CPU path unchanged. |
| `gputins/include/gputins/dag_decode_gpu.hpp` | **GPU L2/L3/L4 decode via spec_dag** — on-device count → `thrust::exclusive_scan` → scatter (the variable-output pattern), driven by the **spec_dag** DAG/FSM. Same `count_packet`/`scatter_packet` DAG-walk kernels as CPU (now `NANOTINS_HD`); only the scan (thrust) + device buffers + D2H append differ. Produces byte-identical output to the CPU DAG path. |
| `examples/pcapng2lance/CMakeLists.txt` | `option(NANOTINS_ENABLE_CUDA …)` + the documented build hook. |
| CLI | `--gpu`, `--cuda-device D`, `--vram-pct P`, `--vram-bytes B`. Without a CUDA build, `--gpu` exits with a clear error. |

Both Phase-B halves are now scaffolded on GPU: the **L1 `parse_epb` scatter** (one `EpbView` per packet,
AoS) and the **L2/L3/L4 spec_dag decode** (`dag_decode_gpu`, count→scan→scatter). The decode is where the GPU
actually pays off (heavier per-packet work). All of it is **written-not-compiled** — your job is to compile,
fix any toolchain syntax, and **verify byte-identical to CPU**.

## Step 1 — build with CUDA

The exact incantation depends on your toolchain. Two options; pick one and finish the `if(NANOTINS_ENABLE_CUDA)`
block in `examples/pcapng2lance/CMakeLists.txt`.

**A. clang-cuda (what the experiment used).** Configure with clang as the C++ compiler and compile the
driver TU as CUDA:
```bash
cmake -S . -B build-gpu -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DNANOLANCE_BUILD_EXAMPLES=ON -DNANOTINS_ENABLE_CUDA=ON
# In the CMake block, uncomment + set the arch for your GPU:
#   target_compile_options(pcapng2lance PRIVATE -x cuda --cuda-gpu-arch=sm_80 --expt-relaxed-constexpr)
cmake --build build-gpu --target pcapng2lance
```

**B. nvcc.** `enable_language(CUDA)`, then
`set_source_files_properties(src/pcapng2lance_main.cpp PROPERTIES LANGUAGE CUDA)`, set
`CMAKE_CUDA_ARCHITECTURES`, and add `--expt-relaxed-constexpr --extended-lambda`.

Either way: `nvexec` headers are already on the include path (they ship inside the stdexec repo that
`nanotins_stdexec` pulls — `#include <nvexec/stream_context.cuh>`), and `CUDA::cudart` is linked by the block
when `find_package(CUDAToolkit)` succeeds.

Sanity smoke (mirrors the experiment's `--smoke`): a one-line GPU schedule. If you want it, add a
`--gpu-smoke` that does `ex::sync_wait(ex::schedule(gpu_ctx_->scheduler()) | ex::then([]{return 0;}))`.

## Step 2 — run

```bash
# GPU L1 parse, VRAM budget = 50% of free VRAM (window auto-capped to fit):
build-gpu/examples/pcapng2lance/pcapng2lance --gpu --vram-pct 50 capture.pcapng out.lance
# or an absolute budget, and a specific device:
build-gpu/.../pcapng2lance --gpu --vram-bytes 8000000000 --cuda-device 1 capture.pcapng out.lance
```
It prints `Phase B = gpu (L1 parse on device, tasks=N)` and the VRAM-capped window size.

## Step 3 — VERIFY correctness (do this before trusting any timing)

The GPU output must be **byte-identical** to the CPU output. Convert both ways and diff:
```bash
EXE=build-gpu/examples/pcapng2lance/pcapng2lance
$EXE          capture.pcapng /tmp/cpu.lance
$EXE --gpu    capture.pcapng /tmp/gpu.lance
# dump packets.lance both ways and diff (nlance2table is built under NANOLANCE_BUILD_TOOLS):
build-gpu/nlance2table -f ndjson /tmp/cpu.lance | sort > /tmp/cpu.ndjson
build-gpu/nlance2table -f ndjson /tmp/gpu.lance | sort > /tmp/gpu.ndjson
diff /tmp/cpu.ndjson /tmp/gpu.ndjson && echo "GPU == CPU"
```
If they differ, the usual suspects are below.

## Step 4 — benchmark (selectable at runtime)

`bench/decode_bench.sh` already sweeps `--sequential` + a `--threads` list. To compare against GPU, run the
converter directly, e.g.:
```bash
for mode in "--sequential" "--threads 32" "--gpu --vram-pct 50"; do
  /usr/bin/time -v $EXE $mode --no-write capture.pcapng /tmp/o.lance 2>&1 | grep Elapsed
done
```
(Use `--no-write` to isolate Phase B from the Lance write, as on CPU.) Note the L1 parse alone is
memory-bound and the **H2D copy of the window will dominate** — the GPU only pays off once the *decode*
runs on-device too (Next steps) or the window is large and the kernel heavier.

## Gotchas (most likely failure points)

1. **`std::span` over device memory.** `parse_packets_gpu` builds `pcapblocks::Bytes wb(win, wsize)` where
   `win` is a **device** pointer; the span is constructed *inside the device lambda*, so it's fine. Do NOT
   pass a host-constructed span of device memory in and deref on host.
2. **`std::tuple` on device** (only relevant if you use the `soa` device-view `scatter()` on GPU, not the
   L1 parse which uses a plain `EpbView out[i] = v`). `nanotins::scatter` uses `std::get` in a fold; under
   clang-cuda/`--expt-relaxed-constexpr` this is usually fine, but if the device compile chokes, replace
   `soa_ptrs<T>` (a `std::tuple` of pointers) with a generated struct of named pointers. The L1 MVP avoids
   this entirely (AoS `EpbView` scatter).
3. **`EpbView` carries an `Options` (ptr+size) member** that points into the *host* window. On device it's
   written but never dereferenced (we only read scalar fields after D2H), so it's harmless — but don't try
   to follow it on the host afterward.
4. **Window must fit VRAM.** `vram_budget` + the 0.7 cap size the window to `bytes(window)+refs+EpbView*n`.
   If you still OOM, lower `--vram-pct` or `--vram-bytes`. Multi-window streaming already handles a capped
   window (one fragment per window).
5. **nvexec lambda annotations.** The reference passed a *plain* `[=]` lambda to `ex::bulk` on the stream
   scheduler (nvexec compiles it for device); we do the same. Do **not** hand-annotate the `bulk_for_each`
   kernel `__device__` — let nvexec handle it. (The functions it *calls* are `NANOTINS_HD`.)

## Compiling the DAG decode (`dag_decode_gpu`)

It lives in `gputins/include/gputins/dag_decode_gpu.hpp` and needs **thrust** (ships with the CUDA
toolkit, header-only): the includes are already there. The single `thrust::exclusive_scan` / `thrust::reduce`
with `thrust::plus<node_counts<NN>>{}` does the per-node prefix-sum for **all DAG nodes at once** —
the `node_counts` struct's component-wise `operator+` makes that work.

Extra flags likely needed (on top of the L1 ones): **`--extended-lambda`** (nvcc) or the clang-cuda
equivalent — because the DAG walkers (`count_packet`/`scatter_packet`) create lambdas *inside*
`NANOTINS_HD` functions, and those must be device-callable. If the device compile complains about the
in-function lambdas, that flag is the fix.

Verify the decode the same way as L1, but diff the **PDU tables**:
```bash
$EXE --decode-l2l3       capture.pcapng /tmp/cpu.lance
$EXE --decode-l2l3 --gpu capture.pcapng /tmp/gpu.lance
for t in ethernet vlan ipv4 ipv6 tcp udp remainder_after_l4; do
  diff <(nlance2table -f ndjson /tmp/cpu_$t.lance | sort) \
       <(nlance2table -f ndjson /tmp/gpu_$t.lance | sort) && echo "$t OK"
done
```
Row **order** within a table is by packet index (the scan sums in index order), so the tables must come out
identical to CPU — the `test_pdu_table_interop` unit test and `pcapng2lance_frag_harmony` integration test
already encode this contract for the CPU paths (and the GPU scaffold inherits the same structure).

## Next steps (after GPU matches CPU)

1. **Coalesced SoA output.** Instead of the AoS `std::vector<EpbView>` + a host `assemble`, fill the final
   columns directly on device using the **soa device-view**: allocate one `device_buffer` per column,
   build a `soa_ptrs<EpbRow>`-shaped pack of device pointers, and `nanotins::scatter(pack, i, row)` in the
   kernel (coalesced per-column writes). See `nanotins::soa<T>::raw()` / `nanotins::scatter` and
   `test_soa_scatter.cpp`. (Watch gotcha #2.)
2. **Overlap copies with compute** (CUDA streams / pinned host memory) so H2D of window N+1 overlaps the
   kernel of window N — the streaming loop already produces one window at a time.
3. **Keep PDUs on-device across windows** (skip the per-window D2H) and only copy back at the end, if VRAM
   for the accumulated tables allows; otherwise the per-window D2H append (current `append_column`) is the
   bounded-memory choice.

## wire_spec GPU smoke (T5) — do this FIRST, it is the smallest device check

Before the full pipeline, validate the new `wire_spec` device path on a 4×u16 UDP header. This is the
cheapest way to learn whether the `wire_spec` `NANOTINS_HD` primitives compile under clang-cuda/nvcc and
match the CPU — in particular whether `scatter_spec` capturing a `std::tuple` of device pointers
(`soatins::soa_ptrs`) into the nvexec bulk lambda survives the device compile (gotcha #2). If it chokes
here, the fix is a generated struct-of-named-pointers, and you found it on a UDP header instead of a sensor.

- Files: `gputins/include/gputins/wire_spec_gpu.hpp` (`nanotins::gpu::parse_spec_gpu<Spec,N>` — H2D,
  device columns, `bulk_for_each(ctx.scheduler(), …, scatter_spec)`, D2H) and the test
  `examples/pcapng2lance/tests/test_wire_spec_gpu.cpp`.
- The test target `wire_spec_gpu_smoke` is wired in `examples/pcapng2lance/CMakeLists.txt`: with
  `-DNANOTINS_ENABLE_CUDA=ON` under **clang-cuda** it is already compiled as CUDA (`-x cuda`, the same
  `_NT_CUDA_OPTS` as the bridge). For **nvcc**, set the source LANGUAGE to CUDA the same way you do the
  bridge (`set_source_files_properties(tests/test_wire_spec_gpu.cpp PROPERTIES LANGUAGE CUDA)` +
  `--extended-lambda`).
- Run: `ctest -R wire_spec_gpu_smoke` — it prints `GPU == CPU over 1024 UDP headers (4 cols)` on success
  (without CUDA it prints `skipped`). That is the T5 gate; once green, the IPv4/IPv6 specs (bit-fields,
  fixed-size-binary) use the identical path.

## Acceptance

- `cmake … -DNANOTINS_ENABLE_CUDA=ON` builds `pcapng2lance` clean.
- **`wire_spec_gpu_smoke` prints `GPU == CPU`** (the smallest device gate; do this first).
- `--gpu` runs and prints the GPU Phase-B line.
- **GPU `packets.lance` diffs identical to CPU** (Step 3). That is the bar; timing is secondary until the
  decode runs on-device.
