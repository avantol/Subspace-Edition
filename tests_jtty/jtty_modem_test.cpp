/**
 * @file jtty_modem_test.cpp
 * @brief Gate for the JTTY modem / sync layer port against reference vectors.
 *
 * vectors/jtty_modem_vectors.txt is written by the reference Fortran
 * compiled standalone (see vectors/README.md). Bit-exact sections:
 *   WAVE6C WAVE6R WAVE12R  genJttyWave, complex and real, two sample rates
 *   SYNC                   genSyncWave
 *   ZSYM ZHALF             PayloadCorrelator on the dumped CORRIN buffer
 *   BLOCKPOW2 BLOCKPOW4    jttyBlockPow on the reference ZSYM
 *   TWKA TWKB              twkfreq, constant-shift fast path and polynomial path
 *   PEAKUP PEAKUP2         jttyPeakup on the dumped PEAKIN buffer
 * Tolerance sections (FFTW plan choice is not guaranteed identical):
 *   ANA                    ana64a on the dumped IWAVE audio
 *   SUB                    subtractJtty on the ANA result
 * plus a vector-free end-to-end: waveform -> correlate -> FEC -> tones.
 *
 * Build: tests_jtty/build.sh
 */

#include "JS8_Mode/jtty_modem.h"
#include "JS8_Mode/jtty_tbcc.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// The application defines this (JS8_Include/commons.h declares it); the
// standalone test supplies its own.
std::mutex fftw_mutex;

using namespace JTTY;
using cfloat = std::complex<float>;

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

float hexToFloat(const std::string &tok) {
    std::uint32_t u = static_cast<std::uint32_t>(std::strtoul(tok.c_str(), nullptr, 16));
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

std::string floatHex(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", u);
    return buf;
}

bool sameBits(float a, float b) {
    std::uint32_t ua, ub;
    std::memcpy(&ua, &a, sizeof ua);
    std::memcpy(&ub, &b, sizeof ub);
    return ua == ub;
}

struct Reader {
    std::ifstream in;
    explicit Reader(const char *path) : in(path) {}

    bool expectTag(const std::string &want, int &count) {
        std::string tag;
        in >> tag >> count;
        if (tag != want) {
            fail("expected section " + want + " got '" + tag + "'");
            return false;
        }
        return true;
    }
    std::vector<cfloat> complexSection(const std::string &tag) {
        int n = 0;
        std::vector<cfloat> v;
        if (!expectTag(tag, n)) return v;
        v.resize(n);
        for (int i = 0; i < n; ++i) {
            std::string re, im;
            in >> re >> im;
            v[i] = cfloat(hexToFloat(re), hexToFloat(im));
        }
        return v;
    }
    std::vector<float> realSection(const std::string &tag) {
        int n = 0;
        std::vector<float> v;
        if (!expectTag(tag, n)) return v;
        v.resize(n);
        for (int i = 0; i < n; ++i) {
            std::string t;
            in >> t;
            v[i] = hexToFloat(t);
        }
        return v;
    }
    std::vector<std::int16_t> int16Section(const std::string &tag) {
        int n = 0;
        std::vector<std::int16_t> v;
        if (!expectTag(tag, n)) return v;
        v.resize(n);
        for (int i = 0; i < n; ++i) {
            int x;
            in >> x;
            v[i] = static_cast<std::int16_t>(x);
        }
        return v;
    }
    bool triple(const std::string &tag, float &p, float &q, float &r) {
        std::string t, a, b, c;
        in >> t >> a >> b >> c;
        if (t != tag) {
            fail("expected " + tag + " got '" + t + "'");
            return false;
        }
        p = hexToFloat(a);
        q = hexToFloat(b);
        r = hexToFloat(c);
        return true;
    }
};

int compareComplexExact(const std::string &what, const std::vector<cfloat> &got,
                        const std::vector<cfloat> &want) {
    int bad = 0;
    if (got.size() != want.size()) {
        fail(what + ": size " + std::to_string(got.size()) + " vs " + std::to_string(want.size()));
        return 1;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (!sameBits(got[i].real(), want[i].real()) || !sameBits(got[i].imag(), want[i].imag())) {
            if (bad < 3)
                fail(what + "[" + std::to_string(i) + "] got " + floatHex(got[i].real()) + " " +
                     floatHex(got[i].imag()) + " want " + floatHex(want[i].real()) + " " +
                     floatHex(want[i].imag()));
            ++bad;
        }
    }
    ++g_checks;
    if (bad) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s: %d of %zu samples differ\n", what.c_str(), bad, got.size());
    }
    return bad;
}

int compareRealExact(const std::string &what, const std::vector<float> &got,
                     const std::vector<float> &want) {
    int bad = 0;
    if (got.size() != want.size()) {
        fail(what + ": size " + std::to_string(got.size()) + " vs " + std::to_string(want.size()));
        return 1;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (!sameBits(got[i], want[i])) {
            if (bad < 3)
                fail(what + "[" + std::to_string(i) + "] got " + floatHex(got[i]) + " want " +
                     floatHex(want[i]));
            ++bad;
        }
    }
    ++g_checks;
    if (bad) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s: %d of %zu samples differ\n", what.c_str(), bad, got.size());
    }
    return bad;
}

// FFT-based results: plan selection may legitimately differ, so compare to a
// relative tolerance against the section's own peak magnitude.
void compareComplexTolerance(const std::string &what, const std::vector<cfloat> &got,
                             const std::vector<cfloat> &want, double relTol) {
    if (got.size() != want.size()) {
        fail(what + ": size mismatch");
        return;
    }
    double peak = 0.0, worst = 0.0;
    for (std::size_t i = 0; i < want.size(); ++i) peak = std::max(peak, static_cast<double>(std::abs(want[i])));
    for (std::size_t i = 0; i < want.size(); ++i)
        worst = std::max(worst, static_cast<double>(std::abs(got[i] - want[i])));
    double rel = peak > 0.0 ? worst / peak : worst;
    std::printf("%s: peak %.6g, worst abs diff %.3g, relative %.3g (tolerance %.1g)\n",
                what.c_str(), peak, worst, rel, relTol);
    check(rel <= relTol, what + " within tolerance");
}

} // namespace

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "vectors/jtty_modem_vectors.txt";
    Reader rd(path);
    if (!rd.in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    std::string line;
    std::getline(rd.in, line);
    check(line == "JTTY_MODEM_VECTORS 1", "header: " + line);

    std::string tag, tonesTok;
    rd.in >> tag >> tonesTok;
    check(tag == "TONES" && tonesTok.size() == FRAME_SYMBOLS, "TONES line");
    int tones[FRAME_SYMBOLS];
    for (int i = 0; i < FRAME_SYMBOLS; ++i) tones[i] = tonesTok[i] - '0';
    for (int i = 0; i < SYNC_SYMBOLS; ++i)
        check(tones[i] == SYNC_TONES[i], "sync tone " + std::to_string(i));

    const int NSS = 192;

    // ---- waveforms ---------------------------------------------------------
    {
        auto want = rd.complexSection("WAVE6C");
        std::vector<cfloat> got(FRAME_SYMBOLS * NSS);
        genJttyWave(tones, FRAME_SYMBOLS, NSS, 2.0f, 6000.0f, 1500.0f, got.data(), nullptr, 1,
                    static_cast<int>(got.size()));
        compareComplexExact("WAVE6C", got, want);
    }
    {
        auto want = rd.realSection("WAVE6R");
        std::vector<float> got(FRAME_SYMBOLS * NSS);
        genJttyWave(tones, FRAME_SYMBOLS, NSS, 2.0f, 6000.0f, 1500.0f, nullptr, got.data(), 0,
                    static_cast<int>(got.size()));
        compareRealExact("WAVE6R", got, want);
    }
    {
        auto want = rd.realSection("WAVE12R");
        std::vector<float> got(FRAME_SYMBOLS * 384);
        genJttyWave(tones, FRAME_SYMBOLS, 384, 2.0f, 12000.0f, 1500.0f, nullptr, got.data(), 0,
                    static_cast<int>(got.size()));
        compareRealExact("WAVE12R", got, want);
    }
    {
        auto want = rd.complexSection("SYNC");
        std::vector<cfloat> got(SYNC_SYMBOLS * NSS);
        genSyncWave(got.data(), NSS);
        compareComplexExact("SYNC", got, want);
    }

    // ---- correlators and block powers -------------------------------------
    auto corrIn = rd.complexSection("CORRIN");
    auto zsymWant = rd.complexSection("ZSYM");
    auto zhalfWant = rd.complexSection("ZHALF");
    CorrelationFrame zsymRef{}, zhalfRef{};
    for (int s = 0; s < INFO_BITS; ++s)
        for (int t = 0; t < 4; ++t) {
            zsymRef.c[s][t] = zsymWant[static_cast<std::size_t>(s) * 4 + t];
            zhalfRef.c[s][t] = zhalfWant[static_cast<std::size_t>(s) * 4 + t];
        }
    {
        PayloadCorrelator pc(NSS);
        CorrelationFrame zsym{}, zhalf{};
        pc.correlate(corrIn.data(), static_cast<int>(corrIn.size()), 0, zsym, zhalf);
        std::vector<cfloat> gotSym, gotHalf;
        for (int s = 0; s < INFO_BITS; ++s)
            for (int t = 0; t < 4; ++t) {
                gotSym.push_back(zsym.c[s][t]);
                gotHalf.push_back(zhalf.c[s][t]);
            }
        compareComplexExact("ZSYM", gotSym, zsymWant);
        compareComplexExact("ZHALF", gotHalf, zhalfWant);
    }
    for (int iblock : {2, 4}) {
        auto want = rd.realSection("BLOCKPOW" + std::to_string(iblock));
        EnergyFrame pow{};
        jttyBlockPow(zsymRef, iblock, pow);
        std::vector<float> got;
        for (int s = 0; s < INFO_BITS; ++s)
            for (int t = 0; t < 4; ++t) got.push_back(pow.e[s][t]);
        compareRealExact("BLOCKPOW" + std::to_string(iblock), got, want);
    }

    // ---- twkfreq ----------------------------------------------------------
    {
        auto in = rd.complexSection("TWKIN");
        auto wantA = rd.complexSection("TWKA");
        auto wantB = rd.complexSection("TWKB");
        std::vector<cfloat> got(in.size());
        float a[3] = {-1500.0f, 0.0f, 0.0f};
        twkfreq(in.data(), got.data(), static_cast<int>(in.size()), 6000.0f, a);
        compareComplexExact("TWKA", got, wantA);
        float b[3] = {3.7f, 0.2f, -0.1f};
        twkfreq(in.data(), got.data(), static_cast<int>(in.size()), 6000.0f, b);
        compareComplexExact("TWKB", got, wantB);
    }

    // ---- peakup -----------------------------------------------------------
    {
        auto c0 = rd.complexSection("PEAKIN");
        std::vector<cfloat> c1(c0.size()), csync(SYNC_SYMBOLS * NSS);
        genSyncWave(csync.data(), NSS);
        float xdtW, f1W, snrW, xdt, f1, snr;
        rd.triple("PEAKUP", xdtW, f1W, snrW);
        jttyPeakup(c0.data(), c1.data(), csync.data(), static_cast<int>(c0.size()), NSS, 0.335f,
                   1512.0f, xdt, f1, snr);
        std::printf("PEAKUP: xdt %.6f f1 %.4f snr %.6g (want %.6f %.4f %.6g)\n", xdt, f1, snr, xdtW,
                    f1W, snrW);
        check(sameBits(xdt, xdtW), "PEAKUP xdt");
        check(sameBits(f1, f1W), "PEAKUP f1");
        check(sameBits(snr, snrW), "PEAKUP snr");
        rd.triple("PEAKUP2", xdtW, f1W, snrW);
        jttyPeakup(c0.data(), c1.data(), csync.data(), static_cast<int>(c0.size()), NSS,
                   2000.0f / 6000.0f, 1512.3f, xdt, f1, snr);
        std::printf("PEAKUP2: xdt %.6f f1 %.4f snr %.6g (want %.6f %.4f %.6g)\n", xdt, f1, snr,
                    xdtW, f1W, snrW);
        check(sameBits(xdt, xdtW), "PEAKUP2 xdt");
        check(sameBits(f1, f1W), "PEAKUP2 f1");
        check(sameBits(snr, snrW), "PEAKUP2 snr");
    }

    // ---- ana64a and subtraction (tolerance) --------------------------------
    {
        auto iwave = rd.int16Section("IWAVE");
        auto anaWant = rd.complexSection("ANA");
        auto subWant = rd.complexSection("SUB");
        const int nana = static_cast<int>(iwave.size());
        std::vector<cfloat> c0(nana);
        ana64a(iwave.data(), nana, c0.data(), nana);
        std::vector<cfloat> anaGot(c0.begin(), c0.begin() + static_cast<long>(anaWant.size()));
        compareComplexTolerance("ANA", anaGot, anaWant, 1e-5);

        double before = 0.0, after = 0.0;
        const int nframe = FRAME_SYMBOLS * NSS;
        for (int i = 1000; i < 1000 + nframe && i < nana / 2; ++i) before += std::norm(c0[i]);
        subtractJtty(c0.data(), nana, nana / 2, tones, FRAME_SYMBOLS, NSS, 1500.0f,
                     2000.0f / 12000.0f);
        for (int i = 1000; i < 1000 + nframe && i < nana / 2; ++i) after += std::norm(c0[i]);
        std::printf("SUB: frame-span energy before %.4g after %.4g (ratio %.3g)\n", before, after,
                    before > 0 ? after / before : 0.0);
        check(after < 0.2 * before, "subtraction removes most of the frame energy");
        std::vector<cfloat> subGot(c0.begin(), c0.begin() + static_cast<long>(subWant.size()));
        compareComplexTolerance("SUB", subGot, subWant, 1e-5);
    }

    // ---- vector-free end to end: waveform -> correlate -> FEC -> tones ----
    {
        std::vector<cfloat> frame(FRAME_SYMBOLS * NSS);
        genJttyWave(tones, FRAME_SYMBOLS, NSS, 2.0f, 6000.0f, 0.0f, frame.data(), nullptr, 1,
                    static_cast<int>(frame.size()));
        PayloadCorrelator pc(NSS);
        CorrelationFrame zsym{}, zhalf{};
        pc.correlate(frame.data(), static_cast<int>(frame.size()), SYNC_SYMBOLS * NSS, zsym, zhalf);
        Decoder decoder;
        std::int8_t payload[PAYLOAD_BITS];
        DecodeResult r;
        bool ok = decoder.decode(zsym, zhalf, payload, &r);
        check(ok, "end-to-end decode succeeds");
        if (ok) {
            std::int8_t chan[INFO_BITS];
            tbccEncode(payload, chan);
            bool same = true;
            for (int i = 0; i < INFO_BITS; ++i) same = same && chan[i] == tones[SYNC_SYMBOLS + i];
            check(same, "end-to-end re-encoded tones equal the transmitted tones");
            std::printf("end-to-end: decoded with coherent length %d, rank %d\n", r.coherentBlockLength,
                        r.acceptedHypothesisRank);
        }
    }

    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
