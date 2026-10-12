/**
 * @file jtty_tbcc_test.cpp
 * @brief Bit-exact gate for the JTTY TBCC FEC port against reference vectors.
 *
 * vectors/jtty_tbcc_vectors.txt is written by the reference Fortran
 * (lib/jtty, tag v3.3.0-beta1) compiled standalone -- never by this project's
 * build. This program replays it through JS8_Mode/jtty_tbcc.cpp and demands:
 *   ENC   encoder tones and raw coded bits identical
 *   LIST  list-decoder candidate lists identical for coherent lengths 1/2/4:
 *         count, pool count, bits, identity, both metrics to the last IEEE
 *         bit, start state, CRC flag
 *   OUTER outer-ladder success, payload and every result field identical
 *   WAVA  the simple energy decoder identical (independent cross-check)
 * plus an encode -> noiseless decode round trip that needs no vectors.
 *
 * Build: tests_jtty/build.sh
 */

#include "JS8_Mode/jtty_tbcc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace JTTY;

namespace {

int g_failures = 0;
int g_checks = 0;

void fail(const std::string &what) {
    ++g_failures;
    if (g_failures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

void check(bool ok, const std::string &what) {
    ++g_checks;
    if (!ok) fail(what);
}

std::string bitsToString(const std::int8_t *bits, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += bits[i] ? '1' : '0';
    return s;
}

void parseBits(const std::string &tok, std::int8_t *bits, int n) {
    if (static_cast<int>(tok.size()) != n) {
        fail("bit field of wrong length: " + tok);
        return;
    }
    for (int i = 0; i < n; ++i) bits[i] = tok[i] == '1' ? 1 : 0;
}

// Tone fields carry digits 0..3, not bits.
std::string tonesToString(const std::int8_t *tones, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += static_cast<char>('0' + tones[i]);
    return s;
}

void parseTones(const std::string &tok, std::int8_t *tones, int n) {
    if (static_cast<int>(tok.size()) != n) {
        fail("tone field of wrong length: " + tok);
        return;
    }
    for (int i = 0; i < n; ++i) tones[i] = static_cast<std::int8_t>(tok[i] - '0');
}

float hexToFloat(const std::string &tok) {
    std::uint32_t u = static_cast<std::uint32_t>(std::strtoul(tok.c_str(), nullptr, 16));
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

std::uint64_t doubleBits(double d) {
    std::uint64_t u;
    std::memcpy(&u, &d, sizeof u);
    return u;
}

std::string hex16(std::uint64_t u) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(u));
    return buf;
}

bool readCorr(std::istream &in, const char *tag, CorrelationFrame &frame) {
    std::string line;
    if (!std::getline(in, line) || line != tag) {
        fail(std::string("expected ") + tag + " got '" + line + "'");
        return false;
    }
    for (int s = 0; s < INFO_BITS; ++s) {
        if (!std::getline(in, line)) { fail("short CORR block"); return false; }
        std::istringstream ls(line);
        for (int tone = 0; tone < 4; ++tone) {
            std::string re, im;
            ls >> re >> im;
            frame.c[s][tone] = std::complex<float>(hexToFloat(re), hexToFloat(im));
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "vectors/jtty_tbcc_vectors.txt";
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }

    std::string line, tag;
    std::getline(in, line);
    check(line == "JTTY_TBCC_VECTORS 1", "header: " + line);

    // ---- encoder vectors -------------------------------------------------
    int nenc = 0;
    in >> tag >> nenc;
    check(tag == "ENC", "ENC header");
    std::getline(in, line);
    for (int i = 0; i < nenc; ++i) {
        std::string p, t, e;
        in >> p >> t >> e;
        std::int8_t payload[PAYLOAD_BITS], wantTones[INFO_BITS], wantBits[INFO_BITS * 2];
        parseBits(p, payload, PAYLOAD_BITS);
        parseTones(t, wantTones, INFO_BITS);
        parseBits(e, wantBits, INFO_BITS * 2);
        std::int8_t tones[INFO_BITS], bits[INFO_BITS * 2];
        tbccEncode(payload, tones, PROFILE_1167_1545_80F, bits);
        check(std::memcmp(tones, wantTones, sizeof tones) == 0,
              "ENC " + std::to_string(i + 1) + " tones: got " + tonesToString(tones, INFO_BITS) +
                  " want " + t);
        check(std::memcmp(bits, wantBits, sizeof bits) == 0,
              "ENC " + std::to_string(i + 1) + " coded bits");
        std::int8_t info[INFO_BITS];
        encodeCrc12(payload, info, PROFILE_1167_1545_80F);
        check(crcValid(info, PROFILE_1167_1545_80F), "ENC " + std::to_string(i + 1) + " CRC self-check");
    }
    std::getline(in, line);

    // ---- decoder vectors -------------------------------------------------
    int ndec = 0;
    in >> tag >> ndec;
    check(tag == "DEC", "DEC header");
    std::getline(in, line);

    ListDecoder list1(PROFILE_1167_1545_80F, 1, 4, 2, MAX_HYPOTHESES);
    ListDecoder list2(PROFILE_1167_1545_80F, 2, 4, 2, MAX_HYPOTHESES);
    ListDecoder list4(PROFILE_1167_1545_80F, 4, 4, 2, MAX_HYPOTHESES);
    ListDecoder *lists[3] = {&list1, &list2, &list4};
    Decoder outer;

    int outerSuccesses = 0, listCandidates = 0;
    for (int f = 0; f < ndec; ++f) {
        int frameNo = 0;
        std::string p;
        in >> tag >> frameNo >> p;
        check(tag == "FRAME", "FRAME tag at frame " + std::to_string(f + 1));
        std::int8_t payload[PAYLOAD_BITS];
        parseBits(p, payload, PAYLOAD_BITS);
        std::string t;
        in >> tag >> t;
        std::int8_t tones[INFO_BITS];
        parseTones(t, tones, INFO_BITS);
        std::getline(in, line);
        std::int8_t myTones[INFO_BITS];
        tbccEncode(payload, myTones);
        check(std::memcmp(myTones, tones, sizeof tones) == 0,
              "frame " + std::to_string(frameNo) + " tones");

        CorrelationFrame corr, hcorr;
        if (!readCorr(in, "CORR", corr)) return 1;
        if (!readCorr(in, "HCORR", hcorr)) return 1;

        for (int li = 0; li < 3; ++li) {
            int L = 0, count = 0, pool = 0;
            in >> tag >> L >> count >> pool;
            check(tag == "LIST" && L == lists[li]->coherentLength(),
                  "LIST tag/length at frame " + std::to_string(frameNo));
            ListDecoder::Output out;
            lists[li]->decode(corr, out, true);
            std::string where = "frame " + std::to_string(frameNo) + " L=" + std::to_string(L);
            check(out.candidateCount == count,
                  where + " candidateCount got " + std::to_string(out.candidateCount) +
                      " want " + std::to_string(count));
            check(out.poolCount == pool, where + " poolCount got " + std::to_string(out.poolCount) +
                                             " want " + std::to_string(pool));
            for (int k = 0; k < count; ++k) {
                std::string bits, cm, wm, crc;
                unsigned long long identity = 0;
                int startState = 0;
                in >> tag >> bits >> identity >> cm >> wm >> startState >> crc;
                check(tag == "CAND", where + " CAND tag");
                if (k >= out.candidateCount) continue;
                const ListDecoder::Candidate &c = out.candidates[k];
                std::string rank = where + " cand " + std::to_string(k + 1);
                check(bitsToString(c.bits, INFO_BITS) == bits, rank + " bits");
                check(c.identity == identity, rank + " identity");
                check(hex16(doubleBits(c.cleanMetric)) == cm,
                      rank + " cleanMetric got " + hex16(doubleBits(c.cleanMetric)) + " want " + cm);
                check(hex16(doubleBits(c.wavaMetric)) == wm,
                      rank + " wavaMetric got " + hex16(doubleBits(c.wavaMetric)) + " want " + wm);
                check(c.startState == startState, rank + " startState");
                check(c.crcValid == (crc == "T"), rank + " crcValid");
                ++listCandidates;
            }
        }

        {
            std::string ok, pay, metric, half;
            int rank = 0, exported = 0, circular = 0, cbl = 0, erc = 0;
            unsigned long long identity = 0;
            in >> tag >> ok >> pay >> rank >> exported >> circular >> cbl >> erc >> identity >>
                metric >> half;
            check(tag == "OUTER", "OUTER tag at frame " + std::to_string(frameNo));
            std::int8_t want[PAYLOAD_BITS], got[PAYLOAD_BITS];
            parseBits(pay, want, PAYLOAD_BITS);
            DecodeResult r;
            bool success = outer.decode(corr, hcorr, got, &r);
            std::string where = "frame " + std::to_string(frameNo) + " OUTER";
            check(success == (ok == "T"), where + " success");
            check(std::memcmp(got, want, sizeof got) == 0, where + " payload");
            check(r.acceptedHypothesisRank == rank, where + " rank");
            check(r.exportedCandidateCount == exported, where + " exported");
            check(r.circularCandidateCount == circular, where + " circular");
            check(r.coherentBlockLength == cbl, where + " coherentBlockLength");
            check(r.evaluatedRungCount == erc, where + " evaluatedRungCount");
            check(r.acceptedIdentity == identity, where + " identity");
            check(hex16(doubleBits(r.acceptedMetric)) == metric, where + " metric");
            check(r.usedHalfSymbolObservation == (half == "T"), where + " half");
            if (success) ++outerSuccesses;
            if (success) check(std::memcmp(got, payload, sizeof got) == 0,
                               where + " decoded payload equals the transmitted one");
        }

        {
            std::string ok, pay;
            in >> tag >> ok >> pay;
            check(tag == "WAVA", "WAVA tag at frame " + std::to_string(frameNo));
            std::int8_t want[PAYLOAD_BITS], got[PAYLOAD_BITS];
            parseBits(pay, want, PAYLOAD_BITS);
            EnergyFrame energies;
            for (int s = 0; s < INFO_BITS; ++s)
                for (int tone = 0; tone < 4; ++tone) {
                    float re = corr.c[s][tone].real(), im = corr.c[s][tone].imag();
                    energies.e[s][tone] = re * re + im * im;
                }
            bool success = tbccWavaFskDecode(energies, 4, 2, got, RESERVED_BIT);
            std::string where = "frame " + std::to_string(frameNo) + " WAVA";
            check(success == (ok == "T"), where + " success");
            check(std::memcmp(got, want, sizeof got) == 0, where + " payload");
        }
        std::getline(in, line);
    }

    // ---- vector-free round trip ------------------------------------------
    {
        std::uint32_t x = 12345;
        int roundTrips = 0;
        for (int i = 0; i < 256; ++i) {
            std::int8_t payload[PAYLOAD_BITS];
            for (int b = 0; b < PAYLOAD_BITS; ++b) {
                x = x * 1664525u + 1013904223u;
                payload[b] = static_cast<std::int8_t>((x >> 24) & 1u);
            }
            payload[RESERVED_BIT - 1] = 0;
            bool anySet = false;
            for (int b = 0; b < PAYLOAD_BITS; ++b) anySet = anySet || payload[b];
            if (!anySet) payload[0] = 1;
            std::int8_t tones[INFO_BITS];
            tbccEncode(payload, tones);
            CorrelationFrame corr{};
            for (int s = 0; s < INFO_BITS; ++s) corr.c[s][tones[s]] = std::complex<float>(1.0f, 0.0f);
            std::int8_t got[PAYLOAD_BITS];
            DecodeResult r;
            bool ok = outer.decode(corr, corr, got, &r);
            check(ok && std::memcmp(got, payload, sizeof got) == 0 && r.coherentBlockLength == 1,
                  "round trip " + std::to_string(i));
            if (ok) ++roundTrips;
        }
        std::printf("round trips: %d/256\n", roundTrips);
    }

    std::printf("vectors: %d encoder frames, %d decoder frames, %d list candidates, "
                "%d outer successes\n",
                nenc, ndec, listCandidates, outerSuccesses);
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
