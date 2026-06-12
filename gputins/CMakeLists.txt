# gputins — the CUDA (nvexec) executors for nanotins, split out so all GPU/CUDA dependencies are isolated
# in one place. Everything here is header-only and guarded by NANOTINS_ENABLE_CUDA, so linking gputins on a
# CPU-only build pulls in *nothing* (the headers compile to empty without the macro). When a consumer turns
# NANOTINS_ENABLE_CUDA on and compiles the relevant TU as CUDA (clang -x cuda or nvcc — see
# nanotins/docs/GPU_BULK_INTEGRATION.md), these headers provide:
#   - gputins/gpu.hpp                : gpu::context (nvexec stream_context), device_buffer<T>, vram_budget.
#   - gputins/protocol_decode_gpu.hpp: decode_window_gpu (on-device count -> thrust scan -> scatter).
# The kernels are the SAME NANOTINS_HD primitives the CPU path uses; only the scheduler/pointers/scan
# differ. gputins depends on nanotins (for bulk_for_each + the protocols decode primitives); the CUDA
# toolkit + arch wiring is the consumer's job (the pcapng2lance example does it behind NANOTINS_ENABLE_CUDA).

cmake_minimum_required(VERSION 3.20)
project(gputins CXX)

# --- soatins / nanotins (the layers below) --------------------------------------------------------
if(NOT TARGET nanotins)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../nanotins" "${CMAKE_CURRENT_BINARY_DIR}/nanotins")
endif()

# --- the library target (header-only) -------------------------------------------------------------
# INTERFACE: just the headers + the include path. nvexec/cudart are pulled in by the consumer's CUDA
# build block (they are only referenced inside #ifdef NANOTINS_ENABLE_CUDA).
add_library(gputins INTERFACE)
add_library(gputins::gputins ALIAS gputins)
target_include_directories(gputins INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(gputins INTERFACE cxx_std_20)
target_link_libraries(gputins INTERFACE nanotins)
