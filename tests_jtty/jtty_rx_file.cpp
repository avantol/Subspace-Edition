// Run the gated JTTY receiver over a raw capture and print every update.
//
//   ./jtty_rx_file capture.pcm [nfa nfb]
//
// capture.pcm: 12 kHz, mono, int16 little-endian, headerless (what
//   parecord --rate=12000 --channels=1 --format=s16le --raw writes).
// The receiver is driven exactly as the application drives it: a 90000-
// sample window (the L2 ring) advanced 6000 samples (0.5 s) per call,
// eight decode steps per call, channel 0 spanning nfa..nfb (default
// 200..2800 Hz, upstream rjtty's limits). Level statistics come first, so a capture that clips or
// is near-silent is visible before any decode verdict.
#include "JS8_Mode/jtty_receiver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <vector>

// The application defines this beside its own FFTW users; the standalone
// tools each define their own, as the gated tests do.
std::mutex fftw_mutex;

namespace {

const char *word(int terminal) {
    switch (terminal) {
    case JTTY::UPDATE_GROWING: return "growing";
    case JTTY::UPDATE_COMPLETE: return "complete";
    case JTTY::UPDATE_EXPIRED: return "expired";
    case JTTY::UPDATE_RECEPTION_ENDED: return "ended";
    }
    return "?";
}

void drain(JTTY::Receiver &rx, std::int64_t pos) {
    std::vector<JTTY::MessageUpdate> batch;
    while (rx.takeUpdates(batch, 16) > 0) {
        for (auto const &u : batch) {
            std::string text = u.text;
            while (!text.empty() && text.back() == ' ') text.pop_back();
            std::printf("%8.2fs %-8s id=%lld f=%7.1f snr=%3d t0=%7.2f call='%s' '%s'\n",
                        pos / 12000.0, word(u.terminal),
                        static_cast<long long>(u.messageId), u.frequency, u.snr,
                        u.startTsync, u.spotCall.c_str(), text.c_str());
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s capture.pcm [nfa nfb]\n", argv[0]);
        return 2;
    }
    // Defaults are upstream rjtty's search limits (nfa=200, nfb=2800).
    // Optional: f0 ftol (channel 0 centre and half-width; default = the
    // whole span as one loose channel, the application's pre-499.28
    // driver) and cover (1 = tile the span with strict 300 Hz channels
    // instead of the reference's fixed 1350/1650 pair).
    int const nfa = argc > 2 ? std::atoi(argv[2]) : 200;
    int const nfb = argc > 3 ? std::atoi(argv[3]) : 2800;
    float const f0Arg = argc > 4 ? static_cast<float>(std::atof(argv[4])) : -1.0f;
    float const ftolArg = argc > 5 ? static_cast<float>(std::atof(argv[5])) : -1.0f;
    bool const cover = argc > 6 && std::atoi(argv[6]) != 0;

    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    auto const bytes = static_cast<std::int64_t>(in.tellg());
    in.seekg(0);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(bytes / 2));
    in.read(reinterpret_cast<char *>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
    std::int64_t const n = static_cast<std::int64_t>(pcm.size());

    int peak = 0;
    double sumsq = 0.0;
    std::int64_t clipped = 0;
    for (auto s : pcm) {
        int const a = std::abs(static_cast<int>(s));
        peak = std::max(peak, a);
        sumsq += static_cast<double>(s) * s;
        if (a >= 32000) ++clipped;
    }
    double const rms = n > 0 ? std::sqrt(sumsq / n) : 0.0;
    std::printf("samples %lld (%.1f s)  peak %d (%.1f dBFS)  rms %.0f (%.1f dBFS)  clipped %lld\n",
                static_cast<long long>(n), n / 12000.0, peak,
                20.0 * std::log10(std::max(peak, 1) / 32768.0), rms,
                20.0 * std::log10(std::max(rms, 1.0) / 32768.0),
                static_cast<long long>(clipped));
    std::printf("search %d..%d Hz\n", nfa, nfb);

    float const f0 = f0Arg > 0.0f ? f0Arg : 0.5f * static_cast<float>(nfa + nfb);
    float const ftol = ftolArg > 0.0f ? ftolArg : 0.5f * static_cast<float>(nfb - nfa);
    std::printf("channel 0: %.0f +- %.0f Hz, wide coverage %s\n", f0, ftol, cover ? "on" : "off (1350/1650)");
    std::int64_t const RING = 90000;
    std::int64_t const STEP = 6000;

    JTTY::Receiver rx;
    rx.setWideCoverage(cover);
    std::int64_t session = 1;
    if (!rx.begin(session, 0, 0, 384)) {
        std::fprintf(stderr, "begin refused\n");
        return 1;
    }
    for (std::int64_t pos = STEP; pos <= n; pos += STEP) {
        std::int64_t const first = std::max<std::int64_t>(0, pos - RING);
        int const count = static_cast<int>(pos - first);
        int const r = rx.process(pcm.data() + first, count, first, pos, 8, nfa, nfb, f0, ftol);
        if (r == -2) {
            std::printf("%8.2fs receiver fell behind; restarted\n", pos / 12000.0);
            rx.end(JTTY::UPDATE_EXPIRED);
            rx.begin(++session, 0, pos, 384);
        } else if (r < 0) {
            std::printf("%8.2fs process returned %d\n", pos / 12000.0, r);
        }
        drain(rx, pos);
    }
    rx.end(JTTY::UPDATE_RECEPTION_ENDED);
    drain(rx, n);
    return 0;
}
