/**
 * @file jtty_receiver_test.cpp
 * @brief Outcome gate for the JTTY receiver port against the reference.
 *
 * vectors/jtty_receiver_vectors.txt and vectors/jtty_rx_audio.pcm are written
 * by the reference Fortran compiled standalone (see vectors/README.md): 28 s
 * of synthetic 12 kHz audio carrying eight JTTY messages in noise, and the
 * reference receive-context API driven through four call patterns (one shot,
 * growing buffer, sliding window, a window too short to reach back). This
 * program replays the identical call sequence through JTTY::Receiver and
 * compares every return value, next-search/required sample, and every
 * message update.
 *
 * The reference plans its sync-search FFTs with FFTW_MEASURE, whose plan
 * choice is timing dependent, so its own sync surface is not reproducible
 * to the bit; this port uses FFTW_ESTIMATE. The gate is therefore on
 * outcomes: ids, terminals and text exact; frequency within 1 Hz, sync
 * times within 5 ms, SNR within 1 dB.
 *
 * Build: tests_jtty/build.sh
 */

#include "JS8_Mode/jtty_receiver.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

std::mutex fftw_mutex;

using namespace JTTY;

namespace {

int g_failures = 0;
int g_checks = 0;
double g_worstDf = 0.0, g_worstDt = 0.0;
int g_worstDsnr = 0;

void fail(const std::string &what) {
    ++g_failures;
    if (g_failures <= 60) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
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

double hexToDouble(const std::string &tok) {
    std::uint64_t u = std::strtoull(tok.c_str(), nullptr, 16);
    double d;
    std::memcpy(&d, &u, sizeof d);
    return d;
}

std::string rtrim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

struct Expected {
    std::int64_t id;
    float f1;
    double start, latest;
    int terminal, snr;
    std::string text;
};

} // namespace

int main(int argc, char **argv) {
    const std::string vectorsPath = argc > 1 ? argv[1] : "vectors/jtty_receiver_vectors.txt";
    std::ifstream in(vectorsPath);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", vectorsPath.c_str());
        return 2;
    }
    std::string line;
    std::getline(in, line);
    check(line == "JTTY_RX_VECTORS 1", "header: " + line);

    std::string tag, audioName;
    int ntotal = 0;
    in >> tag >> audioName >> ntotal;
    check(tag == "AUDIO", "AUDIO record");
    std::string dir = vectorsPath.substr(0, vectorsPath.find_last_of('/') + 1);
    std::vector<std::int16_t> pcm(ntotal);
    {
        std::ifstream audio(dir + audioName, std::ios::binary);
        if (!audio) {
            std::fprintf(stderr, "cannot open %s\n", (dir + audioName).c_str());
            return 2;
        }
        audio.read(reinterpret_cast<char *>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
        check(audio.gcount() == static_cast<std::streamsize>(pcm.size() * 2), "audio length");
    }
    int nsps = 0, nfa = 0, nfb = 0;
    std::string f0hex, ftolhex;
    in >> tag >> nsps >> f0hex >> ftolhex >> nfa >> nfb;
    check(tag == "PARAMS", "PARAMS record");
    const float f0 = hexToFloat(f0hex), ftol = hexToFloat(ftolhex);
    std::getline(in, line);

    Receiver rx;
    std::string scenario;
    int updatesTotal = 0, processCalls = 0;
    int lineNo = 3;

    // Drain the port the way the generator drained the reference, comparing
    // block by block against the UPDATES records that follow.
    auto drainAndCompare = [&](const std::string &where) {
        for (;;) {
            std::string t;
            int k = 0;
            in >> t >> k;
            ++lineNo;
            check(t == "UPDATES", where + ": expected UPDATES at line " + std::to_string(lineNo) + " got " + t);
            std::getline(in, line);   // rest of the UPDATES line
            std::vector<Expected> expected;
            for (int i = 0; i < k; ++i) {
                std::getline(in, line);   // one U line per update
                ++lineNo;
                std::istringstream ss(line);
                std::string u, fh, sh, lh;
                Expected e;
                ss >> u >> e.id >> fh >> sh >> lh >> e.terminal >> e.snr;
                e.f1 = hexToFloat(fh);
                e.start = hexToDouble(sh);
                e.latest = hexToDouble(lh);
                std::size_t a = line.find('@');
                std::size_t b = line.rfind('@');
                e.text = (a != std::string::npos && b > a) ? line.substr(a + 1, b - a - 1) : "";
                expected.push_back(e);
            }
            std::vector<MessageUpdate> got;
            rx.takeUpdates(got, 8);
            check(static_cast<int>(got.size()) == k,
                  where + ": update block size got " + std::to_string(got.size()) + " want " + std::to_string(k));
            const int m = std::min(static_cast<int>(got.size()), k);
            for (int i = 0; i < m; ++i) {
                const MessageUpdate &g = got[i];
                const Expected &e = expected[i];
                std::string w = where + " update id " + std::to_string(e.id);
                check(g.messageId == e.id, w + " id got " + std::to_string(g.messageId));
                check(g.terminal == e.terminal,
                      w + " terminal got " + std::to_string(g.terminal) + " want " + std::to_string(e.terminal));
                check(rtrim(g.text) == rtrim(e.text), w + " text got '" + rtrim(g.text) + "' want '" + rtrim(e.text) + "'");
                const double df = std::fabs(static_cast<double>(g.frequency) - e.f1);
                const double ds = std::fabs(g.startTsync - e.start);
                const double dl = std::fabs(g.latestTsync - e.latest);
                const int dsnr = std::abs(g.snr - e.snr);
                g_worstDf = std::max(g_worstDf, df);
                g_worstDt = std::max(g_worstDt, std::max(ds, dl));
                g_worstDsnr = std::max(g_worstDsnr, dsnr);
                check(df <= 1.0, w + " frequency got " + std::to_string(g.frequency) + " want " + std::to_string(e.f1));
                check(ds <= 0.005 && dl <= 0.005, w + " sync times");
                check(dsnr <= 1, w + " snr got " + std::to_string(g.snr) + " want " + std::to_string(e.snr));
                ++updatesTotal;
            }
            if (k == 0) break;
        }
    };

    while (in >> tag) {
        ++lineNo;
        if (tag == "SCENARIO") {
            in >> scenario;
            continue;
        }
        if (tag == "BEGIN") {
            long long session, grid, first, nspsRec, next, required;
            in >> session >> grid >> first >> nspsRec >> next >> required;
            bool ok = rx.begin(session, grid, first, static_cast<int>(nspsRec));
            check(ok, scenario + " begin accepted");
            check(rx.nextSearchSample() == next, scenario + " begin next_search got " + std::to_string(rx.nextSearchSample()) + " want " + std::to_string(next));
            check(rx.nextRequiredSample() == required, scenario + " begin next_required");
            continue;
        }
        if (tag == "PROCESS") {
            long long first, count, stop, maxSteps, ret, next, required;
            std::string arrow;
            in >> first >> count >> stop >> maxSteps >> arrow >> ret >> next >> required;
            ++processCalls;
            std::string where = scenario + " process(first=" + std::to_string(first) + ", count=" + std::to_string(count) + ")";
            int r = rx.process(pcm.data() + first, static_cast<int>(count), first, stop, static_cast<int>(maxSteps), nfa, nfb, f0, ftol);
            check(r == ret, where + " returned " + std::to_string(r) + " want " + std::to_string(ret));
            check(rx.nextSearchSample() == next, where + " next_search got " + std::to_string(rx.nextSearchSample()) + " want " + std::to_string(next));
            check(rx.nextRequiredSample() == required, where + " next_required");
            drainAndCompare(where);
            continue;
        }
        if (tag == "END") {
            int reason;
            in >> reason;
            rx.end(reason);
            drainAndCompare(scenario + " end");
            continue;
        }
        fail("unknown record '" + tag + "' at line " + std::to_string(lineNo));
        break;
    }

    std::printf("process calls: %d, updates compared: %d\n", processCalls, updatesTotal);
    std::printf("worst deviations: frequency %.4f Hz, sync time %.6f s, snr %d dB\n", g_worstDf, g_worstDt, g_worstDsnr);
    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
