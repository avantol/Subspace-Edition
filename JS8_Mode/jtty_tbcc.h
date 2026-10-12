#pragma once
#ifdef JS8_ENABLE_JTTY
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

// JTTY forward error correction: the tail-biting rate-1/2 convolutional code
// with a 12-bit outer check, as shipped in WSJT-X lib/jtty (tag
// v3.3.0-beta1). Direct C++ port of jtty_tbcc_code_profile.f90, tbcc.f90,
// jtty_tbcc_list_decoder.f90 and jtty_tbcc_decoder.f90; the Fortran stays
// vendored under lib/jtty as the read-only reference and is never built.
//
// Frame: 34 payload bits (32 source + reserved-zero bit 33 + EOM bit 34),
// CRC-12 appended -> 46 information bits -> rate-1/2 TBCC -> 92 coded bits
// -> 46 Gray-mapped 4-FSK tones. Memory nu = 9 (512 states), generators
// octal 1167/1545, outer polynomial 0x80F.

namespace JTTY {

constexpr int PAYLOAD_BITS   = 34;
constexpr int CRC_BITS       = 12;
constexpr int INFO_BITS      = PAYLOAD_BITS + CRC_BITS;  // 46 = data symbols per frame
constexpr int RESERVED_BIT   = 33;   // 1-based payload bit, always transmitted as 0
constexpr int MAX_HYPOTHESES = 4;    // closed codewords handed to CRC admission

struct CodeProfile {
    int profileId;
    int memoryNu;
    int constraintLength;
    int stateCount;
    int stateMask;
    int registerMask;
    std::uint32_t generator0;
    std::uint32_t generator1;
    std::uint32_t outerPolynomial;
    std::uint32_t outerTopBitMask;
    std::uint32_t outerRegisterMask;
    int payloadBits;
    int outerCheckBits;
    int informationBits;

    bool isValid() const;
    bool isSupported() const;   // valid AND equal to the one shipped profile
};

constexpr CodeProfile PROFILE_1167_1545_80F{
    1, 9, 10, 512, 0x1FF, 0x3FF, 01167u, 01545u, 0x80Fu, 0x800u, 0xFFFu,
    PAYLOAD_BITS, CRC_BITS, INFO_BITS};

// One complex correlation per tone per data symbol. Symbol-major so a
// decoder walks a frame contiguously; Fortran's correlations(0:3, K) is the
// same data with the tone index innermost.
struct CorrelationFrame {
    std::complex<float> c[INFO_BITS][4];
};

// |correlation|^2 per tone per symbol, for the simple energy-only decoder.
struct EnergyFrame {
    float e[INFO_BITS][4];
};

// Single trellis step: output register, two generator parities, Gray tone.
void tbccTransition(const CodeProfile &code, int state, int inputBit,
                    int &nextState, int &tone);

// payload[34] -> info[46]: payload followed by its 12-bit outer check.
void encodeCrc12(const std::int8_t payload[PAYLOAD_BITS],
                 std::int8_t info[INFO_BITS],
                 const CodeProfile &code = PROFILE_1167_1545_80F);

// Polynomial remainder over all 46 bits is zero.
bool crcValid(const std::int8_t bits[INFO_BITS],
              const CodeProfile &code = PROFILE_1167_1545_80F);

// payload[34] -> tones[46] (values 0..3). encodedBits, if given, receives
// the raw coded bit pairs as [INFO_BITS][2].
void tbccEncode(const std::int8_t payload[PAYLOAD_BITS],
                std::int8_t tones[INFO_BITS],
                const CodeProfile &code = PROFILE_1167_1545_80F,
                std::int8_t *encodedBits = nullptr);

// Trellis state implied by the last nu information bits (tail-biting start).
int tailbitingState(const CodeProfile &code, const std::int8_t bits[INFO_BITS]);

// 46 bits packed little-endian: bit i (1-based) -> identity bit i-1.
std::uint64_t candidateIdentity(const std::int8_t bits[INFO_BITS]);

// The simpler energy-only WAVA list decoder from tbcc.f90. Kept as an
// independent cross-check of the list decoder below; production uses the
// coherent list decoder. reservedZeroBit is 1-based, 0 = not applied.
bool tbccWavaFskDecode(const EnergyFrame &energies, int listSize,
                       int maxWavaIters, std::int8_t payload[PAYLOAD_BITS],
                       int reservedZeroBit = 0,
                       const CodeProfile &code = PROFILE_1167_1545_80F);

// Coherent list WAVA decoder (jtty_tbcc_list_wava_optimized). One instance
// holds the immutable plan for a coherent block length plus reusable
// scratch, so the receiver hot path allocates nothing.
class ListDecoder {
  public:
    struct Candidate {
        std::int8_t bits[INFO_BITS];
        std::uint64_t identity;
        double cleanMetric;   // single-frame score, used for admission order
        double wavaMetric;    // survivor metric including earlier wraps
        int startState;
        bool crcValid;        // polynomial remainder only; all-zero admission is the caller's
    };
    struct Output {
        Candidate candidates[MAX_HYPOTHESES];
        int candidateCount = 0;   // <= hypothesisCount
        int poolCount = 0;        // distinct closed codewords before the limit
    };

    ListDecoder(const CodeProfile &code, int coherentLength, int perStateWidth,
                int wraps, int hypothesisCount);

    void decode(const CorrelationFrame &correlations, Output &out,
                bool pruneReservedZero = true);

    int coherentLength() const { return coherentLength_; }

  private:
    struct PackedSurvivor {
        double metric;
        std::uint64_t key;   // input word (lexical), origin state, validity
    };
    struct PoolCandidate {
        std::int8_t bits[INFO_BITS];
        int startState;
        std::uint64_t identity;
        double cleanMetric;
        double wavaMetric;
        bool crcValid;
    };

    // plan (immutable after construction)
    CodeProfile code_;
    int coherentLength_;
    int perStateWidth_;
    int wraps_;
    int hypothesisCount_;
    int stateCount_;
    int blockCount_;
    int maxBranchCount_;
    int poolCapacity_;
    int hashCapacity_;
    int energyCount_;
    std::vector<int> nextState_;          // [state*2 + bit]
    std::vector<int> tone_;               // [state*2 + bit]
    std::vector<int> blockStarts_;        // 1-based, as in the reference
    std::vector<int> blockLengths_;
    std::vector<int> branchCounts_;
    std::vector<int> energyOffsets_;      // 0-based into sequenceEnergies_
    std::vector<std::uint64_t> branchIdentities_;   // [block*maxBranch + word]
    std::vector<int> branchEndStates_;    // [(block*maxBranch + word)*states + state]
    std::vector<int> branchSequenceIndices_;

    // workspace (reused per decode)
    std::vector<PackedSurvivor> previous_;
    std::vector<PackedSurvivor> current_;
    std::vector<PoolCandidate> pool_;
    std::vector<int> order_;
    std::vector<int> sortScratch_;
    std::vector<std::uint64_t> hashKeys_;
    std::vector<int> hashIndices_;
    std::vector<char> hashUsed_;
    std::vector<double> sequenceEnergies_;

    void plannedSupertransition(int startState, const int inputBits[4],
                                int blockLength, int &endState,
                                int &toneWord) const;
    void precomputeSequenceEnergies(const CorrelationFrame &correlations);
    void scorePlannedIdentity(std::uint64_t identity, double &metric,
                              bool &closed) const;
    void advancePackedSurvivors(int blockIndex, bool prune, bool orderedMetrics);
    static void insertPackedSurvivor(PackedSurvivor survivors[4], int width,
                                     const PackedSurvivor &candidate,
                                     bool deduplicate);
    void locateIdentity(std::uint64_t identity, int &hashSlot, int &poolIndex,
                        bool &found) const;
    void sortCandidateOrder(int count);
};

struct DecodeResult {
    int acceptedHypothesisRank = 0;
    int exportedCandidateCount = 0;
    int circularCandidateCount = 0;
    int coherentBlockLength = 0;
    int evaluatedRungCount = 0;
    std::uint64_t acceptedIdentity = 0;
    double acceptedMetric = -std::numeric_limits<double>::max();
    bool usedHalfSymbolObservation = false;
};

// The outer ladder (jtty_tbcc_decode): coherent block lengths 1, 2, 4 on
// the full-symbol correlations, then length 1 on the half-symbol
// correlations; first CRC-valid candidate that is not the all-zero payload.
class Decoder {
  public:
    Decoder();
    bool decode(const CorrelationFrame &correlations,
                const CorrelationFrame &halfCorrelations,
                std::int8_t payload[PAYLOAD_BITS],
                DecodeResult *result = nullptr);

  private:
    struct Rung {
        ListDecoder::Output output;
        DecodeResult result;
        bool accepted = false;
    };
    ListDecoder l1_;
    ListDecoder l2_;
    ListDecoder l4_;
    void decodeRung(const CorrelationFrame &correlations, int rungIndex, Rung &rung);
};

// Process-wide convenience, serialised exactly as the Fortran omp critical.
bool tbccDecode(const CorrelationFrame &correlations,
                const CorrelationFrame &halfCorrelations,
                std::int8_t payload[PAYLOAD_BITS],
                DecodeResult *result = nullptr);

} // namespace JTTY
#endif // JS8_ENABLE_JTTY
