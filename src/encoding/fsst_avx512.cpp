// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The AVX-512 batch FSST compressor. The kernel (the unroll-4 loop and the scalar tail that finishes
// its unfinished jobs) is adapted from cwida/fsst's fsst_avx512.cpp, libfsst.cpp and libfsst.hpp. Their
// MIT notice follows, as the license requires:
//
// this software is distributed under the MIT License (http://www.opensource.org/licenses/MIT):
//
// Copyright 2018-2020, CWI, TU Munich, FSU Jena
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files
// (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify,
// merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// - The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// The FSST source repository: https://github.com/cwida/fsst
//
// What nanolance changed: the kernel takes its table from kernel_build_table (codes in nanom's order, no
// regrouping by length); its jobs are 511-byte pieces of one value, restarted where a piece's parse stops
// matching the whole value's (kernel_compress); and only the unroll-4 variant is kept.

#include "nanolance/fsst_kernel.hpp"

// The kernel needs AVX-512F (gathers, scatters, masked compress and expand), AVX-512DQ (64-bit multiply) and
// POPCNT. The build compiles this file with exactly those flags (CMakeLists.txt); anywhere else it is empty.
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && defined(__AVX512F__) && \
    defined(__AVX512DQ__) && defined(__POPCNT__)

#include <immintrin.h>

#include <cstdlib>
#include <cstring>
#include <new>

namespace nano_lance::fsst {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr unsigned FSST_LEN_BITS = 12;          // a table entry's length sits above the 9-bit code
constexpr unsigned FSST_SHIFT = 15;
constexpr unsigned FSST_HASH_LOG2SIZE = 10;     // the long symbols' hash table has 1024 slots
constexpr u64 FSST_HASH_PRIME = 2971215073ULL;
constexpr u64 FSST_ICL_FREE = (u64{15} << 28) | (u64{511} << 16);  // an empty hash slot
constexpr u8 kEscape = 255;
constexpr std::size_t kChunk = 511;     // bytes of one job
constexpr std::size_t kCut = 503;       // a code starting past this byte may differ from the whole value's parse
constexpr std::size_t kJobs = 512;      // jobs per kernel call
constexpr std::size_t kStaged = std::size_t{1} << 18;  // input bytes staged per call (each job plus a terminator)
constexpr std::size_t kCoded = std::size_t{1} << 19;   // output bytes reserved per call (7 + 2 per input byte)
constexpr u16 kEscapeEntry = u16((1U << FSST_LEN_BITS) | 511U);  // a shortCodes entry for an escaped byte

/// A hash-table slot: the symbol's bytes at +0, its icl (ignored bits, code, length) at +8, as the kernel reads them.
namespace detail {
struct Symbol {
    u64 val;
    u64 icl;
};
}  // namespace detail

/// The table in the kernel's layout. `shortCodes` holds, for each two-byte prefix, the code of the two-byte symbol
/// there, else of the one-byte symbol of its first byte, else the escape; the entries carry the length (1 or 2) above
/// bit 12. `hashTab` holds the symbols of three to eight bytes, by their first three bytes.
struct BatchTable {
    std::uint16_t shortCodes[65536];
    detail::Symbol hashTab[1U << FSST_HASH_LOG2SIZE];
    std::uint8_t in_step[256];   ///< input bytes a code covers (1 for the escape)
    std::uint8_t terminator;
};


/// A job's state, bit-packed as the kernel expects: output offset, job index, end and current input offsets.
struct SIMDjob {
    u64 out : 19, pos : 9, end : 18, cur : 18;
};
static_assert(sizeof(SIMDjob) == 8, "the kernel's job layout");

namespace {

/// The kernel's hash of a value's first three bytes: the slot in hashTab, as the kernel computes it.
u64 slot_of(u64 word) {
    const u64 m = (word & 0xFFFFFF) * FSST_HASH_PRIME;
    return (m ^ (m >> FSST_SHIFT)) & ((u64{1} << FSST_HASH_LOG2SIZE) - 1);
}

/// The staging and output buffers one thread's calls use: allocated once, kept.
struct Scratch {
    u8* staged = nullptr;
    u8* coded = nullptr;
    Scratch() {
        staged = static_cast<u8*>(std::malloc(kStaged + 64));
        coded = static_cast<u8*>(std::malloc(kCoded + 64));
        if (staged == nullptr || coded == nullptr) {
            std::free(staged);
            std::free(coded);
            throw std::bad_alloc();
        }
    }
    ~Scratch() {
        std::free(staged);
        std::free(coded);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

Scratch& scratch() {
    thread_local Scratch s;
    return s;
}

/// The kernel itself (cwida's unroll-4 variant): processes up to n jobs (n between 32 and 512) and returns how many it
/// started; the rest are copied to `output` unchanged. Jobs it finishes and jobs it stopped part way are both in
/// output[0..started), with the state the scalar tail completes.
std::size_t kernel_unroll4(const BatchTable& symbolTable, u8* codeBase, u8* symbolBase, SIMDjob* input,
                           SIMDjob* output, std::size_t n) {
    const __m512i all_MASK = _mm512_set1_epi64(-1);
    const __m512i all_PRIME = _mm512_set1_epi64(static_cast<long long>(FSST_HASH_PRIME));
    const __m512i all_ICL_FREE = _mm512_set1_epi64(static_cast<long long>(FSST_ICL_FREE));
    const __m512i all_HASH = _mm512_srli_epi64(all_MASK, 64 - FSST_HASH_LOG2SIZE);
    const __m512i all_ONE = _mm512_srli_epi64(all_MASK, 63);
    const __m512i all_M19 = _mm512_srli_epi64(all_MASK, 45);
    const __m512i all_M18 = _mm512_srli_epi64(all_MASK, 46);
    const __m512i all_FFFFFF = _mm512_srli_epi64(all_MASK, 40);
    const __m512i all_FFFF = _mm512_srli_epi64(all_MASK, 48);
    const __m512i all_FF = _mm512_srli_epi64(all_MASK, 56);

    SIMDjob* inputEnd = input + n;
    __m512i job1 = _mm512_setzero_si512(), job2 = _mm512_setzero_si512(), job3 = _mm512_setzero_si512();
    __m512i job4 = _mm512_setzero_si512();
    __mmask8 loadmask1 = 255, loadmask2 = 255, loadmask3 = 255, loadmask4 = 255;
    u32 delta1 = 8, delta2 = 8, delta3 = 8, delta4 = 8;

    while (input + delta1 + delta2 + delta3 + delta4 < inputEnd) {
            job1      = _mm512_mask_expandloadu_epi64(job1, loadmask1, input); input += delta1;
            job2      = _mm512_mask_expandloadu_epi64(job2, loadmask2, input); input += delta2;
            job3      = _mm512_mask_expandloadu_epi64(job3, loadmask3, input); input += delta3;
            job4      = _mm512_mask_expandloadu_epi64(job4, loadmask4, input); input += delta4;
   __m512i  word1     = _mm512_i64gather_epi64(_mm512_srli_epi64(job1, 46), symbolBase, 1);
   __m512i  word2     = _mm512_i64gather_epi64(_mm512_srli_epi64(job2, 46), symbolBase, 1);
   __m512i  word3     = _mm512_i64gather_epi64(_mm512_srli_epi64(job3, 46), symbolBase, 1);
   __m512i  word4     = _mm512_i64gather_epi64(_mm512_srli_epi64(job4, 46), symbolBase, 1);
   __m512i  code1     = _mm512_i64gather_epi64(_mm512_and_epi64(word1, all_FFFF), symbolTable.shortCodes, sizeof(u16));
   __m512i  code2     = _mm512_i64gather_epi64(_mm512_and_epi64(word2, all_FFFF), symbolTable.shortCodes, sizeof(u16));
   __m512i  code3     = _mm512_i64gather_epi64(_mm512_and_epi64(word3, all_FFFF), symbolTable.shortCodes, sizeof(u16));
   __m512i  code4     = _mm512_i64gather_epi64(_mm512_and_epi64(word4, all_FFFF), symbolTable.shortCodes, sizeof(u16));
   __m512i  pos1      = _mm512_mullo_epi64(_mm512_and_epi64(word1, all_FFFFFF), all_PRIME);
   __m512i  pos2      = _mm512_mullo_epi64(_mm512_and_epi64(word2, all_FFFFFF), all_PRIME);
   __m512i  pos3      = _mm512_mullo_epi64(_mm512_and_epi64(word3, all_FFFFFF), all_PRIME);
   __m512i  pos4      = _mm512_mullo_epi64(_mm512_and_epi64(word4, all_FFFFFF), all_PRIME);
            pos1      = _mm512_slli_epi64(_mm512_and_epi64(_mm512_xor_epi64(pos1,_mm512_srli_epi64(pos1,FSST_SHIFT)), all_HASH), 4);
            pos2      = _mm512_slli_epi64(_mm512_and_epi64(_mm512_xor_epi64(pos2,_mm512_srli_epi64(pos2,FSST_SHIFT)), all_HASH), 4);
            pos3      = _mm512_slli_epi64(_mm512_and_epi64(_mm512_xor_epi64(pos3,_mm512_srli_epi64(pos3,FSST_SHIFT)), all_HASH), 4);
            pos4      = _mm512_slli_epi64(_mm512_and_epi64(_mm512_xor_epi64(pos4,_mm512_srli_epi64(pos4,FSST_SHIFT)), all_HASH), 4);
   __m512i  icl1      = _mm512_i64gather_epi64(pos1, (((char*) symbolTable.hashTab) + 8), 1);
   __m512i  icl2      = _mm512_i64gather_epi64(pos2, (((char*) symbolTable.hashTab) + 8), 1);
   __m512i  icl3      = _mm512_i64gather_epi64(pos3, (((char*) symbolTable.hashTab) + 8), 1);
   __m512i  icl4      = _mm512_i64gather_epi64(pos4, (((char*) symbolTable.hashTab) + 8), 1);
   __m512i  write1    = _mm512_slli_epi64(_mm512_and_epi64(word1, all_FF), 8);
   __m512i  write2    = _mm512_slli_epi64(_mm512_and_epi64(word2, all_FF), 8);
   __m512i  write3    = _mm512_slli_epi64(_mm512_and_epi64(word3, all_FF), 8);
   __m512i  write4    = _mm512_slli_epi64(_mm512_and_epi64(word4, all_FF), 8);
   __m512i  symb1     = _mm512_i64gather_epi64(pos1, (((char*) symbolTable.hashTab) + 0), 1);
   __m512i  symb2     = _mm512_i64gather_epi64(pos2, (((char*) symbolTable.hashTab) + 0), 1);
   __m512i  symb3     = _mm512_i64gather_epi64(pos3, (((char*) symbolTable.hashTab) + 0), 1);
   __m512i  symb4     = _mm512_i64gather_epi64(pos4, (((char*) symbolTable.hashTab) + 0), 1);
            pos1      = _mm512_srlv_epi64(all_MASK, _mm512_and_epi64(icl1, all_FF));
            pos2      = _mm512_srlv_epi64(all_MASK, _mm512_and_epi64(icl2, all_FF));
            pos3      = _mm512_srlv_epi64(all_MASK, _mm512_and_epi64(icl3, all_FF));
            pos4      = _mm512_srlv_epi64(all_MASK, _mm512_and_epi64(icl4, all_FF));
   __mmask8 match1    = _mm512_cmpeq_epi64_mask(symb1, _mm512_and_epi64(word1, pos1)) & _mm512_cmplt_epi64_mask(icl1, all_ICL_FREE);
   __mmask8 match2    = _mm512_cmpeq_epi64_mask(symb2, _mm512_and_epi64(word2, pos2)) & _mm512_cmplt_epi64_mask(icl2, all_ICL_FREE);
   __mmask8 match3    = _mm512_cmpeq_epi64_mask(symb3, _mm512_and_epi64(word3, pos3)) & _mm512_cmplt_epi64_mask(icl3, all_ICL_FREE);
   __mmask8 match4    = _mm512_cmpeq_epi64_mask(symb4, _mm512_and_epi64(word4, pos4)) & _mm512_cmplt_epi64_mask(icl4, all_ICL_FREE);
            code1     = _mm512_mask_mov_epi64(code1, match1, _mm512_srli_epi64(icl1, 16));
            code2     = _mm512_mask_mov_epi64(code2, match2, _mm512_srli_epi64(icl2, 16));
            code3     = _mm512_mask_mov_epi64(code3, match3, _mm512_srli_epi64(icl3, 16));
            code4     = _mm512_mask_mov_epi64(code4, match4, _mm512_srli_epi64(icl4, 16));
            write1    = _mm512_or_epi64(write1, _mm512_and_epi64(code1, all_FF));
            write2    = _mm512_or_epi64(write2, _mm512_and_epi64(code2, all_FF));
            write3    = _mm512_or_epi64(write3, _mm512_and_epi64(code3, all_FF));
            write4    = _mm512_or_epi64(write4, _mm512_and_epi64(code4, all_FF));
            code1     = _mm512_and_epi64(code1, all_FFFF);
            code2     = _mm512_and_epi64(code2, all_FFFF);
            code3     = _mm512_and_epi64(code3, all_FFFF);
            code4     = _mm512_and_epi64(code4, all_FFFF);
                        _mm512_i64scatter_epi64(codeBase, _mm512_and_epi64(job1, all_M19), write1, 1);
                        _mm512_i64scatter_epi64(codeBase, _mm512_and_epi64(job2, all_M19), write2, 1);
                        _mm512_i64scatter_epi64(codeBase, _mm512_and_epi64(job3, all_M19), write3, 1);
                        _mm512_i64scatter_epi64(codeBase, _mm512_and_epi64(job4, all_M19), write4, 1);
            job1      = _mm512_add_epi64(job1, _mm512_slli_epi64(_mm512_srli_epi64(code1, FSST_LEN_BITS), 46));
            job2      = _mm512_add_epi64(job2, _mm512_slli_epi64(_mm512_srli_epi64(code2, FSST_LEN_BITS), 46));
            job3      = _mm512_add_epi64(job3, _mm512_slli_epi64(_mm512_srli_epi64(code3, FSST_LEN_BITS), 46));
            job4      = _mm512_add_epi64(job4, _mm512_slli_epi64(_mm512_srli_epi64(code4, FSST_LEN_BITS), 46));
            job1      = _mm512_add_epi64(job1, _mm512_add_epi64(all_ONE, _mm512_and_epi64(_mm512_srli_epi64(code1, 8), all_ONE)));
            job2      = _mm512_add_epi64(job2, _mm512_add_epi64(all_ONE, _mm512_and_epi64(_mm512_srli_epi64(code2, 8), all_ONE)));
            job3      = _mm512_add_epi64(job3, _mm512_add_epi64(all_ONE, _mm512_and_epi64(_mm512_srli_epi64(code3, 8), all_ONE)));
            job4      = _mm512_add_epi64(job4, _mm512_add_epi64(all_ONE, _mm512_and_epi64(_mm512_srli_epi64(code4, 8), all_ONE)));
            loadmask1 = _mm512_cmpeq_epi64_mask(_mm512_srli_epi64(job1, 46), _mm512_and_epi64(_mm512_srli_epi64(job1, 28), all_M18));
            loadmask2 = _mm512_cmpeq_epi64_mask(_mm512_srli_epi64(job2, 46), _mm512_and_epi64(_mm512_srli_epi64(job2, 28), all_M18));
            loadmask3 = _mm512_cmpeq_epi64_mask(_mm512_srli_epi64(job3, 46), _mm512_and_epi64(_mm512_srli_epi64(job3, 28), all_M18));
            loadmask4 = _mm512_cmpeq_epi64_mask(_mm512_srli_epi64(job4, 46), _mm512_and_epi64(_mm512_srli_epi64(job4, 28), all_M18));
            delta1    = _mm_popcnt_u32((int) loadmask1);
            delta2    = _mm_popcnt_u32((int) loadmask2);
            delta3    = _mm_popcnt_u32((int) loadmask3);
            delta4    = _mm_popcnt_u32((int) loadmask4);
                        _mm512_mask_compressstoreu_epi64(output, loadmask1, job1); output += delta1;
                        _mm512_mask_compressstoreu_epi64(output, loadmask2, job2); output += delta2;
                        _mm512_mask_compressstoreu_epi64(output, loadmask3, job3); output += delta3;
                        _mm512_mask_compressstoreu_epi64(output, loadmask4, job4); output += delta4;
    }

    // Flush the jobs still in flight (the lanes not done), as the last of the output.
    const std::size_t started = n - static_cast<std::size_t>(inputEnd - input);
    u32 unfinished = 0;
    loadmask4 = static_cast<__mmask8>(~loadmask4);
    _mm512_mask_compressstoreu_epi64(output + unfinished, loadmask4, job4);
    unfinished += _mm_popcnt_u32(loadmask4);
    loadmask3 = static_cast<__mmask8>(~loadmask3);
    _mm512_mask_compressstoreu_epi64(output + unfinished, loadmask3, job3);
    unfinished += _mm_popcnt_u32(loadmask3);
    loadmask2 = static_cast<__mmask8>(~loadmask2);
    _mm512_mask_compressstoreu_epi64(output + unfinished, loadmask2, job2);
    unfinished += _mm_popcnt_u32(loadmask2);
    loadmask1 = static_cast<__mmask8>(~loadmask1);
    _mm512_mask_compressstoreu_epi64(output + unfinished, loadmask1, job1);
    return started;
}

/// Finishes one job's remaining input with the scalar code (cwida's tail of compressSIMD).
void finish_scalar(const BatchTable& t, const u8* symbolBase, u8* codeBase, SIMDjob& job) {
    const u8* cur = symbolBase + job.cur;
    const u8* end = symbolBase + job.end;
    u8* out = codeBase + job.out;
    while (cur < end) {
        u64 word;
        std::memcpy(&word, cur, 8);
        const std::size_t code = t.shortCodes[word & 0xFFFF];
        const std::size_t pos = word & 0xFFFFFF;
        const detail::Symbol s = t.hashTab[slot_of(pos)];
        out[1] = static_cast<u8>(word);  // speculatively the escaped byte
        word &= (~u64{0} >> static_cast<u8>(s.icl));
        if (s.icl < FSST_ICL_FREE && s.val == word) {
            *out++ = static_cast<u8>((s.icl >> 16) & 511);
            cur += s.icl >> 28;
        } else {
            *out = static_cast<u8>(code);
            out += 1 + ((code & 256) >> 8);
            cur += code >> FSST_LEN_BITS;
        }
    }
    job.out = static_cast<u64>(out - codeBase);
    job.cur = job.end;
}

/// One value's job for this call: which item, where its bytes are, how many (at most 511).
struct Job {
    std::size_t item;
    const u8* src;
    std::size_t len;
};

/// Compresses one batch of jobs (one per item) and then, per item, appends the part of its job whose
/// parse matches the whole value's: all of it for the value's last job, else the codes that start by byte 503.
void run_batch(const BatchTable& t, Scratch& s, Item* items, const Job* jobs, std::size_t k, std::size_t* taken,
               std::size_t* in_pos) {
    SIMDjob input[kJobs];
    SIMDjob output[kJobs];
    std::size_t staged = 0, coded = 0;
    for (std::size_t j = 0; j < k; ++j) {
        const Job& jb = jobs[j];
        std::memcpy(s.staged + staged, jb.src, jb.len);
        s.staged[staged + jb.len] = t.terminator;  // the kernel never encodes it: no symbol holds it
        SIMDjob sj{};
        sj.cur = staged;
        sj.end = staged + jb.len;
        sj.pos = j;
        sj.out = coded;
        input[j] = sj;
        staged += jb.len + 1;
        coded += 7 + 2 * jb.len;  // the worst case, plus the kernel's 8-byte scatter slack
    }

    std::size_t started = 0;
    if (k >= 32) {
        started = kernel_unroll4(t, s.coded, s.staged, input, output, k);
    }
    for (std::size_t d = started; d < k; ++d) {
        output[d] = input[d];
    }

    std::size_t end_of[kJobs];
    for (std::size_t i = 0; i < k; ++i) {
        SIMDjob job = output[i];
        if (job.cur < job.end) {
            finish_scalar(t, s.staged, s.coded, job);
        }
        end_of[job.pos] = job.out;
    }

    for (std::size_t j = 0; j < k; ++j) {
        const std::size_t item = jobs[j].item;
        const std::size_t start = input[j].out;
        const u8* code = s.coded + start;
        const std::size_t total = end_of[j] - start;
        const bool last = items[item].size - in_pos[item] <= kChunk;  // the job covers the rest of the value
        std::size_t out_n = 0, in_n = 0;
        while (out_n < total) {
            if (!last && in_n > kCut) {
                break;
            }
            const u8 c = code[out_n];
            if (c == kEscape) {
                out_n += 2;
                in_n += 1;
            } else {
                out_n += 1;
                in_n += t.in_step[c];
            }
        }
        std::memcpy(items[item].dst + taken[item], code, out_n);
        taken[item] += out_n;
        in_pos[item] += in_n;
    }
}

}  // namespace

BatchTable* kernel_build_table(const std::uint64_t* symbols, const std::uint8_t* lengths, unsigned count,
                               std::uint8_t terminator) {
    if (count > 255) {
        return nullptr;
    }
    u16 byte_code[256];
    for (auto& entry : byte_code) {
        entry = kEscapeEntry;
    }
    for (unsigned c = 0; c < count; ++c) {
        if (lengths[c] == 1) {
            byte_code[symbols[c] & 0xFF] = static_cast<u16>(c | (1U << FSST_LEN_BITS));
        }
    }
    auto* t = new (std::nothrow) BatchTable;
    if (t == nullptr) {
        return nullptr;
    }
    for (u32 w = 0; w < 65536; ++w) {
        t->shortCodes[w] = byte_code[w & 0xFF];
    }
    for (unsigned c = 0; c < count; ++c) {
        if (lengths[c] == 2) {
            t->shortCodes[symbols[c] & 0xFFFF] = static_cast<u16>(c | (2U << FSST_LEN_BITS));
        }
    }
    for (auto& slot : t->hashTab) {
        slot = detail::Symbol{0, FSST_ICL_FREE};
    }
    for (unsigned c = 0; c < count; ++c) {
        if (lengths[c] >= 3) {
            auto& slot = t->hashTab[slot_of(symbols[c])];
            if (slot.icl < FSST_ICL_FREE) {  // taken: nanom's table never has this, so refuse it
                delete t;
                return nullptr;
            }
            slot = detail::Symbol{symbols[c], (u64{lengths[c]} << 28) | (u64{c} << 16) | (u64{8U - lengths[c]} * 8U)};
        }
    }
    for (unsigned c = 0; c < 256; ++c) {
        t->in_step[c] = c < count ? lengths[c] : 1;
    }
    t->terminator = terminator;
    return t;
}

void kernel_destroy_table(const BatchTable* table) {
    delete table;
}

void kernel_compress(const BatchTable& table, Item* items, std::size_t count) {
    Scratch& s = scratch();
    auto* taken = static_cast<std::size_t*>(std::calloc(count + 1, sizeof(std::size_t)));
    auto* in_pos = static_cast<std::size_t*>(std::calloc(count + 1, sizeof(std::size_t)));
    if (taken == nullptr || in_pos == nullptr) {
        std::free(taken);
        std::free(in_pos);
        throw std::bad_alloc();
    }
    Job jobs[kJobs];
    // A round gives every value with bytes left one job (its next 511 bytes); a value's next round starts where its
    // parse stopped, so the rounds repeat until every value is done.
    for (bool more = true; more;) {
        more = false;
        std::size_t k = 0, staged = 0, coded = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t left = items[i].size - in_pos[i];
            if (left == 0) {
                continue;
            }
            more = true;
            const std::size_t len = left < kChunk ? left : kChunk;
            if (k == kJobs || staged + len + 1 > kStaged || coded + 7 + 2 * len > kCoded) {
                run_batch(table, s, items, jobs, k, taken, in_pos);
                k = staged = coded = 0;
            }
            jobs[k++] = Job{i, items[i].data + in_pos[i], len};
            staged += len + 1;
            coded += 7 + 2 * len;
        }
        if (k > 0) {
            run_batch(table, s, items, jobs, k, taken, in_pos);
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        items[i].written = taken[i];
    }
    std::free(taken);
    std::free(in_pos);
}

}  // namespace nano_lance::fsst
#endif  // AVX-512 build
