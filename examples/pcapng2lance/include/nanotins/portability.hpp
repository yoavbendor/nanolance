#pragma once

// Single place to annotate functions that must compile for both the CPU reference impl and a future
// CUDA (`ex::bulk`) device path. Host-only today, so the macro is empty unless NVCC is driving.
#if defined(__CUDACC__)
#define NANOTINS_HD __host__ __device__
#else
#define NANOTINS_HD
#endif
