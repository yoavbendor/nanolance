# gputins

The **CUDA (nvexec) executors** for [`nanotins`](../nanotins), split out so every GPU/CUDA dependency is
isolated in one library. It is **header-only** and entirely behind `NANOTINS_ENABLE_CUDA` — link it on a
CPU-only build and you pull in *nothing* (the headers compile to empty without the macro). Namespace lives
under the `gpu` namespaces; include prefix `gputins/`.

## The one idea

`nanotins::bulk_for_each(scheduler, num_tasks, n, kernel)` is **scheduler-agnostic**. The CPU path passes
`exec::static_thread_pool::scheduler`; the GPU path passes `nvexec::stream_context::scheduler` and the
**same kernel lambda runs on the device**. gputins supplies only what the device path needs around that:

- **`gputins/gpu.hpp`** — `gpu::context` (owns an `nvexec::stream_context`; `.scheduler()` feeds
  `bulk_for_each` unchanged), `gpu::device_buffer<T>` (RAII `cudaMalloc`/`cudaFree` + H2D/D2H), and
  `gpu::free_vram_bytes()` / `gpu::vram_budget(bytes, pct)` for sizing a per-window VRAM budget.
- **`gputins/protocol_decode_gpu.hpp`** — `decode_window_gpu`: the on-device
  count → `thrust::exclusive_scan` → scatter (the variable-outputs-per-input pattern), using the **same**
  `count_packet` / `scatter_packet` / `walk_packet` primitives as the CPU path. Only the scheduler, the
  pointers (device), and the scan (thrust vs. a host loop) differ.

## Layout

```
include/gputins/   gpu.hpp                  gpu::context, device_buffer<T>, vram_budget
                   protocol_decode_gpu.hpp  decode_window_gpu (count -> thrust scan -> scatter)
```

## CMake

`gputins` is an INTERFACE target that pulls `nanotins`. The CUDA toolkit + arch wiring (clang `-x cuda`
vs. nvcc, the gpu arch, `CUDA::cudart`) is the **consumer's** job — see
[`../nanotins/docs/GPU_BULK_INTEGRATION.md`](../nanotins/docs/GPU_BULK_INTEGRATION.md), which the
`pcapng2lance` example follows behind its `NANOTINS_ENABLE_CUDA` block.

```cmake
add_subdirectory(gputins)
target_link_libraries(my_app PRIVATE gputins)   # inert unless NANOTINS_ENABLE_CUDA is defined
```

## Status

The GPU path is a **near-complete scaffold written on a CUDA-less host** — structure, device buffers,
thrust calls, and D2H append are all in place. The acceptance bar before trusting any timing is that the
GPU output is **byte-identical to the CPU output** (see GPU_BULK_INTEGRATION.md Step 3). Parked
optimizations: the per-window double H2D, pinned host memory, and keeping PDUs on-device across windows.
