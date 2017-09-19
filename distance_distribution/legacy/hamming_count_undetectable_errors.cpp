// Copyright 2015,2016 Till Kolditz
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <chrono>
#include <array>
#include <utility>
#include <vector>
#include <type_traits>
#include <omp.h>
#include <immintrin.h>

#include "Hamming/Scalar.hpp"
#include "Hamming/SSE.hpp"
#include "Hamming/AVX2.hpp"
#include "Util/Statistics.hpp"
#include "Util/Binom.hpp"
#include "Util/StopWatch.hpp"

/*
 * Count undetectable bit flips for Hamming encoded data words.
 *
 * Extended Hamming cannot detect bit flips which lead
 *  * either exactly to another codeword,
 *  * or to any other word in the 1-distance sphere around another codeword.
 *
 * We essentially only count all transitions between codewords and other codewords,
 * as well as codewords and non-codewords which are 1 bit away from the codewords (1-distance sphere),
 * because we know that these transistions cannot be detected.
 *
 * Therefore, we model the space of possible messages as a graph. The edges are the transitions / bit flips between words,
 * while the weights are the number of bits required to flip to get to the other message.
 * Since we start from codewords only, the graph is directed, from all codewords to all other codewords an all words in the
 * corresponding 1-distance sphears.
 *
 * The 1-distance sphere is only virtual in this implementation, realized by an inner for-loop over all 22 possible 1-bitflips
 *
 * The weights (W) are counted, grouped by the weight. This gives the corresponding number of undetectable W-bitflips.
 */

template<size_t ACTUAL_BITCNT_DATA>
Statistics countHammingUndetectableErrors(
        Statistics & former_stats) {
    typedef typename ExtHamming<ACTUAL_BITCNT_DATA>::hamming_impl_t T;
    typedef typename T::accumulator_t accumulator_t;
    typedef typename T::data_t data_t;

    const constexpr size_t BITCNT_HAMMINGBITS = ExtHamming<ACTUAL_BITCNT_DATA>::BITCNT_HAMMING;
    const constexpr size_t BITCNT_CODEWORD = ACTUAL_BITCNT_DATA + BITCNT_HAMMINGBITS;
    const constexpr accumulator_t CNT_CODEWORDS = 1ull << ACTUAL_BITCNT_DATA;
    const constexpr size_t CNT_COUNTS = BITCNT_CODEWORD + 1ull;

#if defined (_MSC_VER) and not defined (size_t)
    typedef __int64 size_t;
#endif

    StopWatch sw;
    sw.start();

    Statistics stats(ACTUAL_BITCNT_DATA, BITCNT_HAMMINGBITS, BITCNT_CODEWORD, CNT_CODEWORDS, CNT_COUNTS);
    // we reuse the former stats to half the number of code words we actually need to count
#ifndef NDEBUG
    std::cout << "# former stats given (" << former_stats.numCounts << " counts, " << former_stats.numCodeWords << " codewords):\n";
#endif
    for (size_t i = 0; i < former_stats.numCounts; ++i) {
        stats.counts[i] = former_stats.counts[i];
    }
    std::cout << std::flush;

#ifndef NDEBUG
    std::cout << "BITCNT_HAMMINGBITS=" << BITCNT_HAMMINGBITS << "\n";
    std::cout << "BITCNT_CODEWORD=" << BITCNT_CODEWORD << "\n";
    std::cout << "CNT_CODEWORDS=" << CNT_CODEWORDS << "\n";
    std::cout << "CNT_COUNTS=" << CNT_COUNTS << "\n";
#pragma omp parallel
    {
#pragma omp master
        {
            std::cout << "OpenMP using " << omp_get_num_threads() << " threads." << std::endl;
        }
    }
    size_t bucket_size_tmp;
    size_t remaining_tmp;
#pragma omp parallel shared(stats,former_stats,bucket_size_tmp,remaining_tmp)
#else
#pragma omp parallel shared(stats,former_stats)
#endif
    {
        const size_t num_threads = omp_get_num_threads();
        const size_t numCodeWordsToEnumerate = CNT_CODEWORDS - former_stats.numCodeWords;
        const size_t bucket_size = numCodeWordsToEnumerate / num_threads;
        const size_t remaining = numCodeWordsToEnumerate % num_threads;

#ifndef NDEBUG
#pragma omp master
        {
            bucket_size_tmp = bucket_size;
            remaining_tmp = remaining;
        }
#endif

#pragma omp for schedule(dynamic,1)
        for (size_t thread = 0; thread < num_threads; ++thread) {
            std::array<accumulator_t, CNT_COUNTS> counts_local {0};
            size_t x = former_stats.numCodeWords + bucket_size * thread;
            const size_t max = x + bucket_size + (thread == (num_threads - 1) ? remaining : 0); // the last thread does the few additional remaining code words

#ifdef __AVX2__
            const constexpr size_t values_per_mm256 = sizeof(__m256i) / sizeof(data_t);
            auto mm256 = SIMD<__m256i, data_t>::set_inc(x, 1);
            auto mm256inc = SIMD<__m256i, data_t>::set1(values_per_mm256);

            if (max >= values_per_mm256) {
                for (; x <= (max - values_per_mm256); x += values_per_mm256) {
                    auto popcount = SIMD<__m256i, data_t>::popcount(mm256);
                    auto hamming = SIMD<__m256i, data_t>::hamming(mm256);
                    auto hammingPopcount = SIMD<__m256i, data_t>::popcount(hamming);
                    popcount = SIMD<__m256i, data_t>::add(popcount, hammingPopcount);
                    auto * pPopcount = reinterpret_cast<data_t*>(&popcount);
                    for (size_t i = 0; i < values_per_mm256; ++i) {
                        counts_local[pPopcount[i]]++;
                    }
                    mm256 = SIMD<__m256i, data_t>::add(mm256, mm256inc);
                }
            }
#endif /* __AVX2__ */

#ifdef __SSE4_2__
            const constexpr size_t values_per_mm128 = sizeof(__m128i) / sizeof(data_t);
            auto mm128 = SIMD<__m128i, data_t>::set_inc(x, 1);
            auto mm128inc = SIMD<__m128i, data_t>::set1(values_per_mm128);

            if (max >= values_per_mm128) {
                for (; x <= (max - values_per_mm128); x += values_per_mm128) {
                    auto popcount = SIMD<__m128i, data_t>::popcount(mm128);
                    auto hamming = SIMD<__m128i, data_t>::hamming(mm128);
                    auto hammingPopcount = SIMD<__m128i, data_t>::popcount(hamming);
                    popcount = SIMD<__m128i, data_t>::add(popcount, hammingPopcount);
                    auto * pPopcount = reinterpret_cast<data_t*>(&popcount);
                    for (size_t i = 0; i < values_per_mm128; ++i) {
                        counts_local[pPopcount[i]]++;
                    }
                    mm128 = SIMD<__m128i, data_t>::add(mm128, mm128inc);
                }
            }
#endif /* __SSE4_2__ */

            for (; x < max; ++x) {
                counts_local[Scalar<data_t>::popcount(x) + Scalar<data_t>::popcount(T::compute(x))]++;
            }

            // 4) Sum the counts
#pragma omp critical
            for (size_t i = 0; i < CNT_COUNTS; ++i) {
                stats.counts[i] += counts_local[i];
            }
        }
    }
    sw.stop();

    typedef typename Statistics::counts_t counts_t;
    std::vector<counts_t> ext_counts(stats.numCounts);
    std::vector<counts_t> cor_counts(stats.numCounts);
    std::vector<counts_t> ext_total_counts(stats.numCounts);
    std::vector<counts_t> cor_total_counts(stats.numCounts);

    // extend the basic counts to extended Hamming
    counts_t maxCountsRaw = 0;
    counts_t maxCountsExt = 0;
    ext_counts[0] = stats.counts[0];
    for (size_t i = 1; i < CNT_COUNTS; ++i) {
        if (i & 0x1) {
            ext_counts[i] = 0;
        } else {
            ext_counts[i] = stats.counts[i] + stats.counts[i - 1];
        }
        if (stats.counts[i] > maxCountsRaw) {
            maxCountsRaw = static_cast<counts_t>(stats.counts[i]);
        }
        if (ext_counts[i] > maxCountsExt) {
            maxCountsExt = static_cast<counts_t>(ext_counts[i]);
        }
    }
    size_t maxWidthRaw = 0;
    do {
        maxCountsRaw /= 10;
        ++maxWidthRaw;
    } while (maxCountsRaw);
    size_t maxWidthExt = 0;
    do {
        maxCountsExt /= 10;
        ++maxWidthExt;
    } while (maxCountsExt);

    // the 1-bit sphere transitions
    counts_t maxCountsCor = 0;
    cor_counts[0] = ext_counts[0];
    for (size_t i = 1; i < CNT_COUNTS; ++i) {
        if (i & 0x1) {
            cor_counts[i] = (ext_counts[i - 1] * (BITCNT_CODEWORD - (i - 1))) + ((i + 1) < CNT_COUNTS ? (ext_counts[i + 1] * (i + 1)) : 0);
        } else {
            cor_counts[i] = ext_counts[i];
        }
        if (cor_counts[i] > maxCountsCor) {
            maxCountsCor = static_cast<counts_t>(cor_counts[i]);
        }
    }
    size_t maxWidthCor = 0;
    do {
        maxCountsCor /= 10;
        ++maxWidthCor;
    } while (maxCountsCor);

    // the transitions apply to all code words
    counts_t maxCountExtTotal = 0;
    counts_t maxCountCorTotal = 0;
    for (size_t i = 0; i < CNT_COUNTS; ++i) {
        ext_total_counts[i] = ext_counts[i] * CNT_CODEWORDS;
        cor_total_counts[i] = cor_counts[i] * CNT_CODEWORDS;
        if (ext_total_counts[i] > maxCountExtTotal) {
            maxCountExtTotal = static_cast<counts_t>(ext_total_counts[i]);
        }
        if (cor_total_counts[i] > maxCountCorTotal) {
            maxCountCorTotal = static_cast<counts_t>(cor_total_counts[i]);
        }
    }
    size_t maxWidthExtTotal = 0;
    do {
        maxCountExtTotal /= 10;
        ++maxWidthExtTotal;
    } while (maxCountExtTotal);
    size_t maxWidthCorTotal = 0;
    do {
        maxCountCorTotal /= 10;
        ++maxWidthCorTotal;
    } while (maxCountCorTotal);

    counts_t maxTransitions2 = CNT_CODEWORDS * binomial_coefficient<counts_t>(BITCNT_CODEWORD, BITCNT_CODEWORD / 2);
    size_t maxWidthTra = 0;
    do {
        maxTransitions2 /= 10;
        ++maxWidthTra;
    } while (maxTransitions2 > 0);

    counts_t numTotal = 0;
    counts_t numTransitions = 0;
#ifndef NDEBUG
    std::cout << '(' << BITCNT_CODEWORD << '/' << ACTUAL_BITCNT_DATA << ") : bucketsize(" << bucket_size_tmp << ") remaining(" << remaining_tmp << ")" << std::endl;
#endif
    const constexpr bool isCodeShortened = (BITCNT_CODEWORD & (~BITCNT_CODEWORD + 1)) == BITCNT_CODEWORD;
    std::cout << "# SDC probabilities for a (n,k) = (" << BITCNT_CODEWORD << ',' << ACTUAL_BITCNT_DATA << ") " << (isCodeShortened ? "shortened " : "") << "correcting Extended Hamming code\n";
    std::cout << "# The reported colunms are:\n";
    std::cout << "#   1) bit width\n";
    std::cout << "#   2) weight distribution of " << (isCodeShortened ? "shortened " : "") << "detecting-only Hamming\n";
    std::cout << "#   3) weight distribution of " << (isCodeShortened ? "shortened " : "") << "detecting-only Extended Hamming\n";
    std::cout << "#   4) weight distribution of " << (isCodeShortened ? "shortened " : "") << "correcting Extended Hamming\n";
    std::cout << "#   5) error pattern weight distribution of " << (isCodeShortened ? "shortened " : "") << "detecting-only Extended Hamming\n";
    std::cout << "#   6) error pattern weight distribution of " << (isCodeShortened ? "shortened " : "") << "correcting Extended Hamming\n";
    std::cout << "#   7) maximum # transitions of " << (isCodeShortened ? "shortened " : "") << "extended Hamming (2^k * (n over col 1))\n";
    std::cout << "#   8) probability of " << (isCodeShortened ? "shortened " : "") << "detecting-only Extended Hamming (col 5 / col 7)\n";
    std::cout << "#   9) probability of " << (isCodeShortened ? "shortened " : "") << "correcting Extended Hamming (col 6 / col 7)\n";
    std::cout << std::dec << "#Distances:\n" << std::fixed << std::setprecision(10);
    for (size_t i = 0; i < CNT_COUNTS; ++i) {
        counts_t maxTransitions = CNT_CODEWORDS * binomial_coefficient<counts_t>(BITCNT_CODEWORD, i);
        numTotal += stats.counts[i];
        numTransitions += cor_total_counts[i];

        std::cout << std::right << std::setw(4) << i;                                           // bit width
        std::cout << ',' << std::setw(maxWidthRaw) << stats.counts[i];                          // weight distribution detecting Hamming
        std::cout << ',' << std::setw(maxWidthExt) << ext_counts[i];                            // weight distribution detecting Extended Hamming
        std::cout << ',' << std::setw(maxWidthCor) << cor_counts[i];                            // weight distribution correcting Extended Hamming
        std::cout << ',' << std::setw(maxWidthExtTotal) << ext_total_counts[i];                 // error weight pattern distribution detecting Extended Hamming
        std::cout << ',' << std::setw(maxWidthCorTotal) << cor_total_counts[i];                 // error weight pattern distribution correcting Extended Hamming
        std::cout << ',' << std::setw(maxWidthTra) << maxTransitions;                           // maximum # transitions
        std::cout << ',' << (double(ext_total_counts[i]) / double(maxTransitions));             // SDC probability of detecting Extended Hamming
        std::cout << ',' << (double(cor_total_counts[i]) / double(maxTransitions)) << '\n';     // SDC probability of correcting Extended Hamming
    }
    if (numTotal != CNT_CODEWORDS) {
        std::cerr << '(' << BITCNT_CODEWORD << '/' << ACTUAL_BITCNT_DATA << ") : numTotal (" << numTotal << " != numCodeWords (" << CNT_CODEWORDS << ')' << std::endl;
    }

    std::cout << "# (" << BITCNT_CODEWORD << '/' << ACTUAL_BITCNT_DATA << ") : Computation took " << sw << "ns for " << CNT_CODEWORDS << " code words and " << numTransitions << " transitions.\n\n"
            << std::endl;

    return stats;
}

int main() {
    std::vector<Statistics> all_stats;
    all_stats.reserve(64);
    all_stats.push_back(Statistics(0, 0, 0, 0, 1));
    all_stats.emplace_back(countHammingUndetectableErrors<1>(all_stats[0])); // all stats are zero-initialized
    all_stats.emplace_back(countHammingUndetectableErrors<2>(all_stats[1])); // now stats[0] is all set-up
    all_stats.emplace_back(countHammingUndetectableErrors<3>(all_stats[2]));
    all_stats.emplace_back(countHammingUndetectableErrors<4>(all_stats[3]));
    all_stats.emplace_back(countHammingUndetectableErrors<5>(all_stats[4]));
    all_stats.emplace_back(countHammingUndetectableErrors<6>(all_stats[5]));
    all_stats.emplace_back(countHammingUndetectableErrors<7>(all_stats[6]));
    all_stats.emplace_back(countHammingUndetectableErrors<8>(all_stats[7]));
    all_stats.emplace_back(countHammingUndetectableErrors<9>(all_stats[8]));
    all_stats.emplace_back(countHammingUndetectableErrors<10>(all_stats[9]));
    all_stats.emplace_back(countHammingUndetectableErrors<11>(all_stats[10]));
    all_stats.emplace_back(countHammingUndetectableErrors<12>(all_stats[11]));
    all_stats.emplace_back(countHammingUndetectableErrors<13>(all_stats[12]));
    all_stats.emplace_back(countHammingUndetectableErrors<14>(all_stats[13]));
    all_stats.emplace_back(countHammingUndetectableErrors<15>(all_stats[14]));
    all_stats.emplace_back(countHammingUndetectableErrors<16>(all_stats[15]));
    all_stats.emplace_back(countHammingUndetectableErrors<17>(all_stats[16]));
    all_stats.emplace_back(countHammingUndetectableErrors<18>(all_stats[17]));
    all_stats.emplace_back(countHammingUndetectableErrors<19>(all_stats[18]));
    all_stats.emplace_back(countHammingUndetectableErrors<20>(all_stats[19]));
    all_stats.emplace_back(countHammingUndetectableErrors<21>(all_stats[20]));
    all_stats.emplace_back(countHammingUndetectableErrors<22>(all_stats[21]));
    all_stats.emplace_back(countHammingUndetectableErrors<23>(all_stats[22]));
    all_stats.emplace_back(countHammingUndetectableErrors<24>(all_stats[23]));
    all_stats.emplace_back(countHammingUndetectableErrors<25>(all_stats[24]));
    all_stats.emplace_back(countHammingUndetectableErrors<26>(all_stats[25]));
    all_stats.emplace_back(countHammingUndetectableErrors<27>(all_stats[26]));
    all_stats.emplace_back(countHammingUndetectableErrors<28>(all_stats[27]));
    all_stats.emplace_back(countHammingUndetectableErrors<29>(all_stats[28]));
    all_stats.emplace_back(countHammingUndetectableErrors<30>(all_stats[29]));
    all_stats.emplace_back(countHammingUndetectableErrors<31>(all_stats[30]));
    all_stats.emplace_back(countHammingUndetectableErrors<32>(all_stats[31]));
    all_stats.emplace_back(countHammingUndetectableErrors<33>(all_stats[32]));
    all_stats.emplace_back(countHammingUndetectableErrors<34>(all_stats[33]));
    all_stats.emplace_back(countHammingUndetectableErrors<35>(all_stats[34]));
    all_stats.emplace_back(countHammingUndetectableErrors<36>(all_stats[35]));
    all_stats.emplace_back(countHammingUndetectableErrors<37>(all_stats[36]));
    all_stats.emplace_back(countHammingUndetectableErrors<38>(all_stats[37]));
    all_stats.emplace_back(countHammingUndetectableErrors<39>(all_stats[38]));
    all_stats.emplace_back(countHammingUndetectableErrors<40>(all_stats[39]));

    return 0;
}
