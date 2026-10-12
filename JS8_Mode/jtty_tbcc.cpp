/**
 * @file jtty_tbcc.cpp
 * @brief JTTY tail-biting convolutional FEC: encoder, CRC-12, coherent list
 *        WAVA decoder and the outer decode ladder.
 *
 * Direct port of WSJT-X lib/jtty (tag v3.3.0-beta1):
 *   jtty_tbcc_code_profile.f90  -> CodeProfile
 *   tbcc.f90                    -> encodeCrc12, tbccEncode, tbccWavaFskDecode
 *   jtty_tbcc_list_decoder.f90  -> ListDecoder
 *   jtty_tbcc_decoder.f90       -> Decoder, tbccDecode
 *
 * Loop order, tie-breaking and float/double promotion follow the reference
 * exactly so that tests_jtty/jtty_tbcc_test.cpp can hold this port to
 * bit-identical candidate lists and metrics against vectors produced by the
 * reference Fortran.
 */

#ifdef JS8_ENABLE_JTTY

#include "jtty_tbcc.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace JTTY {

namespace {

constexpr double NEGATIVE_METRIC = -std::numeric_limits<double>::max();
constexpr int PACKED_ORIGIN_BITS = 11;
constexpr int PACKED_VALID_BIT = INFO_BITS + PACKED_ORIGIN_BITS;   // 57
constexpr std::uint64_t VALID_MASK = std::uint64_t{1} << PACKED_VALID_BIT;
constexpr std::uint64_t ORIGIN_MASK = (std::uint64_t{1} << PACKED_ORIGIN_BITS) - 1;

inline int parityBit(std::uint32_t v) { return std::popcount(v) & 1; }

inline bool testBit(std::uint64_t v, int bit) { return (v >> bit) & 1u; }

inline bool blockContainsReserved(int blockStart, int blockLength) {
    return blockStart <= RESERVED_BIT && blockStart + blockLength > RESERVED_BIT;
}

// input_bits(offset) = ibits(branch_word, block_length - offset, 1): MSB first.
inline void unpackBranchWord(int branchWord, int blockLength, int inputBits[4]) {
    for (int offset = 0; offset < 4; ++offset) inputBits[offset] = 0;
    for (int offset = 1; offset <= blockLength; ++offset)
        inputBits[offset - 1] = (branchWord >> (blockLength - offset)) & 1;
}

// Gray tone order of tbcc.f90's build_incoming_tones: tones(dropped, state).
void buildIncomingTones(const CodeProfile &code, std::vector<int> &tones) {
    static constexpr int grayTones[4] = {0, 1, 3, 2};
    tones.assign(static_cast<std::size_t>(code.stateCount) * 2, 0);
    for (int state = 0; state < code.stateCount; ++state) {
        for (int dropped = 0; dropped < 2; ++dropped) {
            std::uint32_t reg = static_cast<std::uint32_t>(state) |
                                (static_cast<std::uint32_t>(dropped) << code.memoryNu);
            int b0 = parityBit(reg & code.generator0);
            int b1 = parityBit(reg & code.generator1);
            tones[static_cast<std::size_t>(state) * 2 + dropped] = grayTones[2 * b0 + b1];
        }
    }
}

std::uint32_t crcRegisterOver(const std::int8_t *bits, int count, const CodeProfile &code) {
    std::uint32_t crc = 0;
    for (int i = 0; i < count; ++i) {
        crc ^= static_cast<std::uint32_t>(bits[i]) << (code.outerCheckBits - 1);
        if (crc & code.outerTopBitMask)
            crc = (crc << 1) ^ code.outerPolynomial;
        else
            crc <<= 1;
        crc &= code.outerRegisterMask;
    }
    return crc;
}

} // namespace

/******************************************************************************/
// CodeProfile
/******************************************************************************/

bool CodeProfile::isValid() const {
    if (memoryNu < 1 || memoryNu > 20) return false;
    if (constraintLength != memoryNu + 1) return false;
    int expectedStateCount = 1 << memoryNu;
    int expectedRegisterMask = (1 << constraintLength) - 1;
    if (stateCount != expectedStateCount) return false;
    if (stateMask != expectedStateCount - 1) return false;
    if (registerMask != expectedRegisterMask) return false;
    std::uint32_t rmask = static_cast<std::uint32_t>(registerMask);
    if (generator0 == 0 || (generator0 & ~rmask) != 0) return false;
    if (generator1 == 0 || (generator1 & ~rmask) != 0) return false;
    if (payloadBits != PAYLOAD_BITS) return false;
    if (outerCheckBits != CRC_BITS) return false;
    if (informationBits != payloadBits + outerCheckBits) return false;
    if (outerTopBitMask != (1u << (outerCheckBits - 1))) return false;
    if (outerRegisterMask != (1u << outerCheckBits) - 1) return false;
    if (outerPolynomial == 0 || (outerPolynomial & ~outerRegisterMask) != 0) return false;
    return true;
}

bool CodeProfile::isSupported() const {
    const CodeProfile &r = PROFILE_1167_1545_80F;
    return isValid() && profileId == r.profileId && memoryNu == r.memoryNu &&
           constraintLength == r.constraintLength && stateCount == r.stateCount &&
           stateMask == r.stateMask && registerMask == r.registerMask &&
           generator0 == r.generator0 && generator1 == r.generator1 &&
           outerPolynomial == r.outerPolynomial &&
           outerTopBitMask == r.outerTopBitMask &&
           outerRegisterMask == r.outerRegisterMask && payloadBits == r.payloadBits &&
           outerCheckBits == r.outerCheckBits && informationBits == r.informationBits;
}

/******************************************************************************/
// Trellis primitives, CRC, encoder
/******************************************************************************/

void tbccTransition(const CodeProfile &code, int state, int inputBit,
                    int &nextState, int &tone) {
    std::uint32_t reg = (static_cast<std::uint32_t>(state << 1) |
                         static_cast<std::uint32_t>(inputBit)) &
                        static_cast<std::uint32_t>(code.registerMask);
    int b0 = parityBit(reg & code.generator0);
    int b1 = parityBit(reg & code.generator1);
    tone = 2 * b0 + (b0 ^ b1);
    nextState = static_cast<int>(reg & static_cast<std::uint32_t>(code.stateMask));
}

void encodeCrc12(const std::int8_t payload[PAYLOAD_BITS], std::int8_t info[INFO_BITS],
                 const CodeProfile &code) {
    if (!code.isSupported()) throw std::invalid_argument("unsupported JTTY TBCC code profile");
    for (int i = 0; i < PAYLOAD_BITS; ++i) info[i] = payload[i];
    std::uint32_t crc = crcRegisterOver(payload, PAYLOAD_BITS, code);
    for (int i = 1; i <= code.outerCheckBits; ++i)
        info[PAYLOAD_BITS + i - 1] =
            static_cast<std::int8_t>((crc >> (code.outerCheckBits - i)) & 1u);
}

bool crcValid(const std::int8_t bits[INFO_BITS], const CodeProfile &code) {
    return crcRegisterOver(bits, INFO_BITS, code) == 0;
}

void tbccEncode(const std::int8_t payload[PAYLOAD_BITS], std::int8_t tones[INFO_BITS],
                const CodeProfile &code, std::int8_t *encodedBits) {
    std::int8_t info[INFO_BITS];
    encodeCrc12(payload, info, code);

    // Tail-biting: preload the register with the message's own last nu bits.
    int state = 0;
    for (int t = 0; t < code.memoryNu; ++t) {
        int bit = info[INFO_BITS - code.memoryNu + t];
        state = ((state << 1) | bit) & (code.stateCount - 1);
    }

    for (int t = 0; t < INFO_BITS; ++t) {
        int bit = info[t];
        std::uint32_t reg = (static_cast<std::uint32_t>(state << 1) |
                             static_cast<std::uint32_t>(bit)) &
                            static_cast<std::uint32_t>(code.registerMask);
        int b0 = parityBit(reg & code.generator0);
        int b1 = parityBit(reg & code.generator1);
        state = ((state << 1) | bit) & (code.stateCount - 1);
        if (encodedBits) {
            encodedBits[t * 2] = static_cast<std::int8_t>(b0);
            encodedBits[t * 2 + 1] = static_cast<std::int8_t>(b1);
        }
        if (b0 == 0 && b1 == 0) tones[t] = 0;
        if (b0 == 0 && b1 == 1) tones[t] = 1;
        if (b0 == 1 && b1 == 1) tones[t] = 2;
        if (b0 == 1 && b1 == 0) tones[t] = 3;
    }
}

int tailbitingState(const CodeProfile &code, const std::int8_t bits[INFO_BITS]) {
    int state = 0;
    for (int bitIndex = INFO_BITS - code.memoryNu + 1; bitIndex <= INFO_BITS; ++bitIndex)
        state = ((state << 1) | bits[bitIndex - 1]) & code.stateMask;
    return state;
}

std::uint64_t candidateIdentity(const std::int8_t bits[INFO_BITS]) {
    std::uint64_t identity = 0;
    for (int bitIndex = 1; bitIndex <= INFO_BITS; ++bitIndex)
        if (bits[bitIndex - 1] != 0) identity |= std::uint64_t{1} << (bitIndex - 1);
    return identity;
}

/******************************************************************************/
// Simple energy-only WAVA (tbcc_wava_fsk_decode)
/******************************************************************************/

bool tbccWavaFskDecode(const EnergyFrame &energies, int listSize, int maxWavaIters,
                       std::int8_t payload[PAYLOAD_BITS], int reservedZeroBit,
                       const CodeProfile &code) {
    for (int i = 0; i < PAYLOAD_BITS; ++i) payload[i] = 0;
    if (!code.isSupported()) throw std::invalid_argument("unsupported JTTY TBCC code profile");
    if (maxWavaIters < 1) return false;

    const int nu = code.memoryNu;
    const int states = code.stateCount;
    std::vector<float> prevM(states, 0.0f), currM(states);
    std::vector<std::int16_t> traceback(static_cast<std::size_t>(states) * INFO_BITS);
    std::vector<int> tones;
    buildIncomingTones(code, tones);

    struct Candidate {
        float metric;
        std::int8_t bits[INFO_BITS];
    };
    std::vector<Candidate> sorted(listSize);
    for (int l = 0; l < listSize; ++l) sorted[l].metric = -1.0e30f;

    for (int iter = 1; iter <= maxWavaIters; ++iter) {
        for (int t = 0; t < INFO_BITS; ++t) {
            std::fill(currM.begin(), currM.end(), -1.0e30f);
            for (int s = 0; s < states; ++s) {
                int prevS = (s >> 1) & (states - 1);
                float m0 = prevM[prevS] + energies.e[t][tones[static_cast<std::size_t>(s) * 2 + 0]];
                prevS |= 1 << (nu - 1);
                float m1 = prevM[prevS] + energies.e[t][tones[static_cast<std::size_t>(s) * 2 + 1]];
                if (m0 > m1) {
                    currM[s] = m0;
                    traceback[static_cast<std::size_t>(s) * INFO_BITS + t] = 0;
                } else {
                    currM[s] = m1;
                    traceback[static_cast<std::size_t>(s) * INFO_BITS + t] = 1;
                }
            }
            prevM = currM;
        }
    }

    std::int8_t tmpBits[INFO_BITS];
    for (int s = 0; s < states; ++s) {
        int currS = s;
        for (int t = INFO_BITS - 1; t >= 0; --t) {
            tmpBits[t] = static_cast<std::int8_t>(currS & 1);
            int prevS = (currS >> 1) & (states - 1);
            if (traceback[static_cast<std::size_t>(currS) * INFO_BITS + t] == 1)
                prevS |= 1 << (nu - 1);
            currS = prevS;
        }
        if (currS == s) {
            for (int l = 0; l < listSize; ++l) {
                if (currM[s] > sorted[l].metric) {
                    for (int i = listSize - 1; i >= l + 1; --i) sorted[i] = sorted[i - 1];
                    sorted[l].metric = currM[s];
                    std::memcpy(sorted[l].bits, tmpBits, sizeof tmpBits);
                    break;
                }
            }
        }
    }

    for (int l = 0; l < listSize; ++l) {
        if (sorted[l].metric <= -1.0e29f) continue;
        if (reservedZeroBit > 0 && sorted[l].bits[reservedZeroBit - 1] != 0) continue;
        if (crcRegisterOver(sorted[l].bits, INFO_BITS, code) == 0) {
            for (int i = 0; i < PAYLOAD_BITS; ++i) payload[i] = sorted[l].bits[i];
            return true;
        }
    }
    return false;
}

/******************************************************************************/
// ListDecoder: plan construction
/******************************************************************************/

ListDecoder::ListDecoder(const CodeProfile &code, int coherentLength, int perStateWidth,
                         int wraps, int hypothesisCount)
    : code_(code), coherentLength_(coherentLength), perStateWidth_(perStateWidth),
      wraps_(wraps), hypothesisCount_(hypothesisCount) {
    if (!code.isSupported())
        throw std::invalid_argument("TBCC list decoder requires a supported code profile");
    if (coherentLength != 1 && coherentLength != 2 && coherentLength != 4)
        throw std::invalid_argument("TBCC list decoder supports coherent lengths 1, 2, and 4");
    if (perStateWidth < 1 || perStateWidth > 4)
        throw std::invalid_argument("TBCC list decoder per-state width must be between 1 and 4");
    if (wraps < 1 || wraps > 2)
        throw std::invalid_argument("TBCC list decoder supports one or two wraps");
    if (hypothesisCount < 1 || hypothesisCount > MAX_HYPOTHESES)
        throw std::invalid_argument("TBCC list decoder hypothesis count must be between 1 and 4");
    if (code.memoryNu > PACKED_ORIGIN_BITS)
        throw std::invalid_argument("TBCC state does not fit packed survivor");

    stateCount_ = code.stateCount;
    blockCount_ = (INFO_BITS + coherentLength - 1) / coherentLength;
    maxBranchCount_ = 1 << coherentLength;
    poolCapacity_ = stateCount_ * perStateWidth;
    hashCapacity_ = 1;
    while (hashCapacity_ < 2 * poolCapacity_) hashCapacity_ *= 2;

    nextState_.assign(static_cast<std::size_t>(stateCount_) * 2, 0);
    tone_.assign(static_cast<std::size_t>(stateCount_) * 2, 0);
    for (int inputBit = 0; inputBit < 2; ++inputBit)
        for (int state = 0; state < stateCount_; ++state)
            tbccTransition(code, state, inputBit,
                           nextState_[static_cast<std::size_t>(state) * 2 + inputBit],
                           tone_[static_cast<std::size_t>(state) * 2 + inputBit]);

    blockStarts_.assign(blockCount_, 0);
    blockLengths_.assign(blockCount_, 0);
    branchCounts_.assign(blockCount_, 0);
    energyOffsets_.assign(blockCount_, 0);
    branchIdentities_.assign(static_cast<std::size_t>(blockCount_) * maxBranchCount_, 0);
    branchEndStates_.assign(
        static_cast<std::size_t>(blockCount_) * maxBranchCount_ * stateCount_, 0);
    branchSequenceIndices_.assign(
        static_cast<std::size_t>(blockCount_) * maxBranchCount_ * stateCount_, 0);
    energyCount_ = 0;
    for (int blockIndex = 0; blockIndex < blockCount_; ++blockIndex) {
        int blockStart = 1 + blockIndex * coherentLength;   // 1-based
        int blockLength = std::min(coherentLength, INFO_BITS - blockStart + 1);
        blockStarts_[blockIndex] = blockStart;
        blockLengths_[blockIndex] = blockLength;
        branchCounts_[blockIndex] = 1 << blockLength;
        energyOffsets_[blockIndex] = energyCount_;
        energyCount_ += 1 << (2 * blockLength);
        for (int branchWord = 0; branchWord < branchCounts_[blockIndex]; ++branchWord) {
            int inputBits[4];
            unpackBranchWord(branchWord, blockLength, inputBits);
            std::uint64_t branchIdentity = 0;
            for (int offset = 1; offset <= blockLength; ++offset)
                if (inputBits[offset - 1] != 0)
                    branchIdentity |= std::uint64_t{1} << (INFO_BITS - blockStart - offset + 1);
            std::size_t bw = static_cast<std::size_t>(blockIndex) * maxBranchCount_ + branchWord;
            branchIdentities_[bw] = branchIdentity;
            for (int state = 0; state < stateCount_; ++state) {
                int endState, toneWord;
                plannedSupertransition(state, inputBits, blockLength, endState, toneWord);
                branchEndStates_[bw * stateCount_ + state] = endState;
                branchSequenceIndices_[bw * stateCount_ + state] =
                    energyOffsets_[blockIndex] + toneWord;
            }
        }
    }

    previous_.assign(static_cast<std::size_t>(stateCount_) * perStateWidth, {NEGATIVE_METRIC, 0});
    current_.assign(static_cast<std::size_t>(stateCount_) * perStateWidth, {NEGATIVE_METRIC, 0});
    pool_.resize(std::max(1, poolCapacity_));
    order_.assign(std::max(1, poolCapacity_), 0);
    sortScratch_.assign(std::max(1, poolCapacity_), 0);
    hashKeys_.assign(hashCapacity_, 0);
    hashIndices_.assign(hashCapacity_, 0);
    hashUsed_.assign(hashCapacity_, 0);
    sequenceEnergies_.assign(energyCount_, 0.0);
}

void ListDecoder::plannedSupertransition(int startState, const int inputBits[4],
                                         int blockLength, int &endState,
                                         int &toneWord) const {
    int state = startState;
    toneWord = 0;
    for (int offset = 0; offset < blockLength; ++offset) {
        std::size_t idx = static_cast<std::size_t>(state) * 2 + inputBits[offset];
        toneWord = 4 * toneWord + tone_[idx];
        state = nextState_[idx];
    }
    endState = state;
}

/******************************************************************************/
// ListDecoder: per-decode helpers
/******************************************************************************/

void ListDecoder::precomputeSequenceEnergies(const CorrelationFrame &correlations) {
    for (int blockIndex = 0; blockIndex < blockCount_; ++blockIndex) {
        int blockStart = blockStarts_[blockIndex];
        int blockLength = blockLengths_[blockIndex];
        int sequenceCount = 1 << (2 * blockLength);
        for (int sequenceWord = 0; sequenceWord < sequenceCount; ++sequenceWord) {
            std::complex<float> coherentSum(0.0f, 0.0f);
            for (int offset = 1; offset <= blockLength; ++offset) {
                int tone = (sequenceWord >> (2 * (blockLength - offset))) & 3;
                coherentSum += correlations.c[blockStart + offset - 2][tone];
            }
            // real(coherent_sum*conjg(coherent_sum), real64) / real(block_length, real64)
            float re = coherentSum.real(), im = coherentSum.imag();
            float power = re * re + im * im;
            sequenceEnergies_[energyOffsets_[blockIndex] + sequenceWord] =
                static_cast<double>(power) / static_cast<double>(blockLength);
        }
    }
}

void ListDecoder::scorePlannedIdentity(std::uint64_t identity, double &metric,
                                       bool &closed) const {
    int startState = 0;
    for (int bitIndex = INFO_BITS - code_.memoryNu + 1; bitIndex <= INFO_BITS; ++bitIndex)
        startState = ((startState << 1) | (testBit(identity, bitIndex - 1) ? 1 : 0)) &
                     (stateCount_ - 1);
    int state = startState;
    metric = 0.0;
    for (int blockIndex = 0; blockIndex < blockCount_; ++blockIndex) {
        int branchWord = 0;
        for (int offset = 0; offset < blockLengths_[blockIndex]; ++offset)
            branchWord = 2 * branchWord +
                         (testBit(identity, blockStarts_[blockIndex] + offset - 1) ? 1 : 0);
        std::size_t bw = static_cast<std::size_t>(blockIndex) * maxBranchCount_ + branchWord;
        metric += sequenceEnergies_[branchSequenceIndices_[bw * stateCount_ + state]];
        state = branchEndStates_[bw * stateCount_ + state];
    }
    closed = state == startState;
}

void ListDecoder::insertPackedSurvivor(PackedSurvivor survivors[4], int width,
                                       const PackedSurvivor &candidate, bool deduplicate) {
    // Resetting a wrap's origins and identities can duplicate a state's ranks.
    // After the first block, an origin and prefix determine exactly one path,
    // so extending distinct retained paths cannot create another duplicate.
    if (deduplicate) {
        for (int slot = 0; slot < width; ++slot) {
            if (!(survivors[slot].key & VALID_MASK)) continue;
            if (survivors[slot].key == candidate.key) {
                if (candidate.metric <= survivors[slot].metric) return;
                for (int ins = 0; ins < 3; ++ins)
                    if (ins >= slot && ins < width - 1) survivors[ins] = survivors[ins + 1];
                survivors[width - 1].key = 0;
                break;
            }
        }
    }

    auto precedes = [](const PackedSurvivor &l, const PackedSurvivor &r) {
        if (l.metric > r.metric) return true;
        if (l.metric < r.metric) return false;
        return l.key < r.key;
    };

    int insertionSlot = width;
    for (int slot = 0; slot < width; ++slot) {
        if (!(survivors[slot].key & VALID_MASK) || precedes(candidate, survivors[slot])) {
            insertionSlot = slot;
            break;
        }
    }
    if (insertionSlot >= width) return;
    for (int slot = 3; slot >= 1; --slot)
        if (slot <= width - 1 && slot > insertionSlot) survivors[slot] = survivors[slot - 1];
    survivors[insertionSlot] = candidate;
}

void ListDecoder::advancePackedSurvivors(int blockIndex, bool prune, bool orderedMetrics) {
    const int width = perStateWidth_;
    const int branchCount = branchCounts_[blockIndex];
    const int stateStride = stateCount_ / branchCount;
    const bool pruneBlock =
        prune && blockContainsReserved(blockStarts_[blockIndex], blockLengths_[blockIndex]);
    PackedSurvivor selected[4] = {{NEGATIVE_METRIC, 0}, {NEGATIVE_METRIC, 0},
                                  {NEGATIVE_METRIC, 0}, {NEGATIVE_METRIC, 0}};
    for (int endState = 0; endState < stateCount_; ++endState) {
        for (auto &s : selected) s.key = 0;   // reference clears validity only
        const int branchWord = endState & (branchCount - 1);
        const std::size_t bw = static_cast<std::size_t>(blockIndex) * maxBranchCount_ + branchWord;
        const std::uint64_t branchIdentity = branchIdentities_[bw];
        if (!(pruneBlock && testBit(branchIdentity, INFO_BITS - RESERVED_BIT))) {
            // The low L destination bits are the input word; the discarded high
            // L source bits enumerate every predecessor in the original order.
            for (int state = endState / branchCount; state < stateCount_; state += stateStride) {
                const double branchMetric =
                    sequenceEnergies_[branchSequenceIndices_[bw * stateCount_ + state]];
                for (int rank = 0; rank < width; ++rank) {
                    const PackedSurvivor &prev = previous_[static_cast<std::size_t>(state) * width + rank];
                    if (!(prev.key & VALID_MASK)) continue;
                    const double metric = prev.metric + branchMetric;
                    if (selected[width - 1].key & VALID_MASK) {
                        if (metric < selected[width - 1].metric) {
                            // Lower ranks receive the same branch metric and cannot recover.
                            if (orderedMetrics) break;
                            continue;
                        }
                    }
                    PackedSurvivor extension = prev;
                    extension.metric = metric;
                    extension.key |= branchIdentity;
                    insertPackedSurvivor(selected, width, extension, blockIndex == 0);
                }
            }
        }
        for (int rank = 0; rank < width; ++rank)
            current_[static_cast<std::size_t>(endState) * width + rank] = selected[rank];
    }
}

void ListDecoder::locateIdentity(std::uint64_t identity, int &hashSlot, int &poolIndex,
                                 bool &found) const {
    hashSlot = static_cast<int>(identity % static_cast<std::uint64_t>(hashCapacity_));
    for (;;) {
        if (!hashUsed_[hashSlot]) {
            poolIndex = -1;
            found = false;
            return;
        }
        if (hashKeys_[hashSlot] == identity) {
            poolIndex = hashIndices_[hashSlot];
            found = true;
            return;
        }
        if (++hashSlot >= hashCapacity_) hashSlot = 0;
    }
}

void ListDecoder::sortCandidateOrder(int count) {
    if (count <= 1) return;
    auto precedes = [this](int li, int ri) {
        const PoolCandidate &l = pool_[li], &r = pool_[ri];
        if (l.cleanMetric > r.cleanMetric) return true;
        if (l.cleanMetric < r.cleanMetric) return false;
        if (l.identity != r.identity) return l.identity < r.identity;
        if (l.startState != r.startState) return l.startState < r.startState;
        return l.wavaMetric > r.wavaMetric;
    };
    // Bottom-up merge, as the reference (1-based bounds translated verbatim).
    int width = 1;
    while (width < count) {
        int left = 1;
        while (left <= count) {
            int middle = std::min(left + width, count + 1);
            int right = std::min(left + 2 * width - 1, count);
            int leftCursor = left, rightCursor = middle;
            for (int output = left; output <= right; ++output) {
                if (leftCursor >= middle) {
                    sortScratch_[output - 1] = order_[rightCursor++ - 1];
                } else if (rightCursor > right) {
                    sortScratch_[output - 1] = order_[leftCursor++ - 1];
                } else if (precedes(order_[leftCursor - 1], order_[rightCursor - 1])) {
                    sortScratch_[output - 1] = order_[leftCursor++ - 1];
                } else {
                    sortScratch_[output - 1] = order_[rightCursor++ - 1];
                }
            }
            left += 2 * width;
        }
        for (int i = 0; i < count; ++i) order_[i] = sortScratch_[i];
        width *= 2;
    }
}

/******************************************************************************/
// ListDecoder: decode (jtty_tbcc_list_wava_optimized)
/******************************************************************************/

void ListDecoder::decode(const CorrelationFrame &correlations, Output &out,
                         bool pruneReservedZero) {
    precomputeSequenceEnergies(correlations);
    // Nonfinite metrics do not provide the monotone rank order needed by the cutoff.
    bool orderedMetrics = true;
    for (double e : sequenceEnergies_)
        if (!std::isfinite(e)) { orderedMetrics = false; break; }

    for (auto &s : previous_) { s.key = 0; s.metric = NEGATIVE_METRIC; }
    for (int state = 0; state < stateCount_; ++state) {
        PackedSurvivor &s = previous_[static_cast<std::size_t>(state) * perStateWidth_];
        s.key = (static_cast<std::uint64_t>(state) << INFO_BITS) | VALID_MASK;
        s.metric = 0.0;
    }

    for (int wrap = 1; wrap <= wraps_; ++wrap) {
        // reset_packed_frame_identity: keep validity, re-origin at the current state
        for (int state = 0; state < stateCount_; ++state)
            for (int rank = 0; rank < perStateWidth_; ++rank) {
                PackedSurvivor &s = previous_[static_cast<std::size_t>(state) * perStateWidth_ + rank];
                if (!(s.key & VALID_MASK)) continue;
                s.key = (static_cast<std::uint64_t>(state) << INFO_BITS) | VALID_MASK;
            }
        for (int blockIndex = 0; blockIndex < blockCount_; ++blockIndex) {
            advancePackedSurvivors(blockIndex, pruneReservedZero, orderedMetrics);
            std::swap(previous_, current_);
        }
    }

    int poolCount = 0;
    std::fill(hashUsed_.begin(), hashUsed_.end(), 0);
    for (int state = 0; state < stateCount_; ++state) {
        for (int rank = 0; rank < perStateWidth_; ++rank) {
            const PackedSurvivor &s = previous_[static_cast<std::size_t>(state) * perStateWidth_ + rank];
            if (!(s.key & VALID_MASK)) continue;
            // Tail-biting paths are eligible only when their final and remembered
            // initial states agree after the last wrap.
            if (static_cast<int>((s.key >> INFO_BITS) & ORIGIN_MASK) != state) continue;
            std::uint64_t identity = 0;
            for (int bitIndex = 0; bitIndex < INFO_BITS; ++bitIndex)
                if (testBit(s.key, bitIndex))
                    identity |= std::uint64_t{1} << (INFO_BITS - bitIndex - 1);
            // The same complete word can close through more than one retained path;
            // hash by all 46 bits before applying the global hypothesis limit.
            int hashSlot, poolIndex;
            bool found;
            locateIdentity(identity, hashSlot, poolIndex, found);
            if (found) {
                PoolCandidate &p = pool_[poolIndex];
                if (s.metric > p.wavaMetric ||
                    (!(s.metric < p.wavaMetric) && state < p.startState)) {
                    p.wavaMetric = s.metric;
                    p.startState = state;
                }
                continue;
            }
            poolIndex = poolCount++;
            hashUsed_[hashSlot] = 1;
            hashKeys_[hashSlot] = identity;
            hashIndices_[hashSlot] = poolIndex;
            PoolCandidate &p = pool_[poolIndex];
            p.identity = identity;
            p.startState = state;
            p.wavaMetric = s.metric;
            for (int bitIndex = 1; bitIndex <= INFO_BITS; ++bitIndex)
                p.bits[bitIndex - 1] = testBit(identity, bitIndex - 1) ? 1 : 0;
            // WAVA survivor metrics include history from earlier wraps.  Admission
            // order is based on a clean, single-frame score of each complete word.
            double cleanMetric;
            bool closed;
            scorePlannedIdentity(identity, cleanMetric, closed);
            if (!closed) throw std::logic_error("closed TBCC survivor failed clean scoring closure");
            p.cleanMetric = cleanMetric;
            // This flag reports the polynomial remainder only.  Message admission,
            // including the protocol's all-zero rejection, remains with the caller.
            p.crcValid = crcValid(p.bits, code_);
        }
    }

    for (int i = 0; i < poolCount; ++i) order_[i] = i;
    sortCandidateOrder(poolCount);

    for (auto &c : out.candidates) {
        std::memset(c.bits, 0, sizeof c.bits);
        c.identity = 0;
        c.cleanMetric = NEGATIVE_METRIC;
        c.wavaMetric = NEGATIVE_METRIC;
        c.startState = -1;
        c.crcValid = false;
    }
    out.poolCount = poolCount;
    out.candidateCount = std::min(hypothesisCount_, poolCount);
    for (int rank = 0; rank < out.candidateCount; ++rank) {
        const PoolCandidate &p = pool_[order_[rank]];
        Candidate &c = out.candidates[rank];
        std::memcpy(c.bits, p.bits, sizeof c.bits);
        c.identity = p.identity;
        c.cleanMetric = p.cleanMetric;
        c.wavaMetric = p.wavaMetric;
        c.startState = p.startState;
        c.crcValid = p.crcValid;
    }
}

/******************************************************************************/
// Decoder: the outer ladder (jtty_tbcc_decoder.f90)
/******************************************************************************/

namespace {
constexpr int COHERENT_LENGTHS[3] = {1, 2, 4};
constexpr int CIRCULAR_PASSES = 2;
constexpr int PER_STATE_WIDTH = 4;
} // namespace

Decoder::Decoder()
    : l1_(PROFILE_1167_1545_80F, 1, PER_STATE_WIDTH, CIRCULAR_PASSES, MAX_HYPOTHESES),
      l2_(PROFILE_1167_1545_80F, 2, PER_STATE_WIDTH, CIRCULAR_PASSES, MAX_HYPOTHESES),
      l4_(PROFILE_1167_1545_80F, 4, PER_STATE_WIDTH, CIRCULAR_PASSES, MAX_HYPOTHESES) {}

void Decoder::decodeRung(const CorrelationFrame &correlations, int rungIndex, Rung &rung) {
    ListDecoder *decoders[3] = {&l1_, &l2_, &l4_};
    rung = Rung{};
    decoders[rungIndex]->decode(correlations, rung.output, true);
    rung.result = DecodeResult{};
    rung.result.exportedCandidateCount = rung.output.candidateCount;
    rung.result.circularCandidateCount = rung.output.poolCount;
    rung.result.coherentBlockLength = COHERENT_LENGTHS[rungIndex];
    for (int rank = 0; rank < rung.output.candidateCount; ++rank) {
        const ListDecoder::Candidate &c = rung.output.candidates[rank];
        if (!c.crcValid) continue;
        // Do not expand the fixed false-accept budget past the all-zero sentinel.
        bool allZero = true;
        for (int i = 0; i < PAYLOAD_BITS; ++i)
            if (c.bits[i] != 0) { allZero = false; break; }
        if (allZero) break;
        rung.accepted = true;
        rung.result.acceptedHypothesisRank = rank + 1;
        rung.result.acceptedIdentity = c.identity;
        rung.result.acceptedMetric = c.cleanMetric;
        break;
    }
}

bool Decoder::decode(const CorrelationFrame &correlations,
                     const CorrelationFrame &halfCorrelations,
                     std::int8_t payload[PAYLOAD_BITS], DecodeResult *result) {
    for (int i = 0; i < PAYLOAD_BITS; ++i) payload[i] = 0;
    DecodeResult local;
    Rung rung;
    auto accept = [&](int evaluatedRungCount, bool halfSymbol) {
        const ListDecoder::Candidate &c =
            rung.output.candidates[rung.result.acceptedHypothesisRank - 1];
        for (int i = 0; i < PAYLOAD_BITS; ++i) payload[i] = c.bits[i];
        local = rung.result;
        local.evaluatedRungCount = evaluatedRungCount;
        local.usedHalfSymbolObservation = halfSymbol;
    };

    bool success = false;
    for (int rungIndex = 0; rungIndex < 3 && !success; ++rungIndex) {
        decodeRung(correlations, rungIndex, rung);
        if (!rung.accepted) continue;
        accept(rungIndex + 1, false);
        success = true;
    }
    if (!success) {
        // Half-symbol energies discard phase, so only L1 is meaningful here.
        decodeRung(halfCorrelations, 0, rung);
        if (rung.accepted) {
            accept(4, true);
            success = true;
        } else {
            local.evaluatedRungCount = 4;
        }
    }
    if (result) *result = local;
    return success;
}

bool tbccDecode(const CorrelationFrame &correlations,
                const CorrelationFrame &halfCorrelations,
                std::int8_t payload[PAYLOAD_BITS], DecodeResult *result) {
    static Decoder decoder;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    return decoder.decode(correlations, halfCorrelations, payload, result);
}

} // namespace JTTY

#endif // JS8_ENABLE_JTTY
