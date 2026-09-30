// [STATIONSPLIT 2026-09-30] Offline simulator for the standard decoder's
// same-codeword handling. Modelled on tracking_diag.cpp: includes JS8.cpp
// directly so the decoder under test is EXACTLY the one in the working
// tree, and uses the real JS8::encode so the tones are what a station
// transmits.
//
// Three experiments, one mode at a time (A/B/C/E/I):
//   collapse : N stations, IDENTICAL text, field-like spread, one slot.
//              Before stationsplit the decoder emitted one frame per
//              codeword; after, one per place. Counts both.
//   shadow   : station B placed exactly on, just outside, and well clear
//              of station A's alias multiple, to check the rule's edges.
//   ghost    : ONE very strong station, f0 swept across the passband.
//              Any emission not at f0 is an alias ghost; its distance
//              from the nearest k*aliasSpacing is the error the whole
//              tolerance rests on. The field has ONE measurement (1 Hz).
//
// Build, from the repository root (Qt6, fftw3f):
//   g++ -std=c++17 -O2 -fPIC -I. $(pkg-config --cflags Qt6Core) \
//       tests_nativearq/tools/stationsplit_sim.cpp JS8_Mode/FrequencyTracker.cpp \
//       $(pkg-config --libs Qt6Core) -lfftw3f -lpthread -o stationsplit_sim
// Run: ./stationsplit_sim <A|B|C|E|I> <collapse|shadow|ghost|all>
//
// "SNR" here is per-sample tone-to-noise in dB, the same convention as
// tracking_diag.cpp, NOT the 2500 Hz-bandwidth figure JS8 reports.
// Relative comparisons are what matter.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QEventLoop>
#include <QLoggingCategory>
#include <QThread>

#include "JS8_Include/commons.h"
#include "JS8_Mode/JS8.h"

struct dec_data dec_data;
struct specData specData;
std::mutex fftw_mutex;

Q_LOGGING_CATEGORY(decoder_js8, "decoder.js8", QtWarningMsg)

#include "JS8_Mode/JS8.cpp"

namespace {

constexpr double FS = 12000.0;
bool g_clip = false;   // third argv "clip": overdrive + hard-limit, see synth()

struct Station {
    double freqHz;
    double snrDb;
    double dtSec;      // keying offset, seconds
};

struct Emission {
    float freq;
    int   snr;
    std::string data;
};

// ---- synthesis -------------------------------------------------------

template <class Mode>
std::size_t synth(std::vector<Station> const &stations, char const *message,
                  unsigned seed) {
    int tones[NN] = {};
    // Each mode has its own Costas array (Normal = original, the rest =
    // modified); using the wrong one would simply fail to sync.
    JS8::encode(0, JS8::Costas::array(Mode::NCOSTAS), message, tones);

    constexpr double baud = FS / Mode::NSPS;
    std::vector<double> samples(Mode::NMAX, 0.0);

    // One noise process at unit variance; each station's amplitude is set
    // from its own SNR against that same noise.
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, 1.0);
    for (auto &s : samples) s = noise(rng);

    for (auto const &st : stations) {
        double const amp = std::sqrt(std::pow(10.0, st.snrDb / 10.0));
        double phi = 0.0;
        int const startSample = int(std::round(st.dtSec * FS));
        for (int sym = 0; sym < NN; ++sym) {
            double const freq = st.freqHz + tones[sym] * baud;
            double const dphi = 2.0 * M_PI * freq / FS;
            for (int n = 0; n < Mode::NSPS; ++n) {
                long const idx = long(startSample) + long(sym) * Mode::NSPS + n;
                if (idx >= 0 && idx < long(samples.size()))
                    samples[idx] += amp * std::cos(phi);
                phi += dphi;
                if (phi > 2.0 * M_PI) phi -= 2.0 * M_PI;
            }
        }
    }

    // Scaling. Default: fit the peak into int16, no clipping. With "clip":
    // a FIXED noise floor, so a strong tone overdrives and hard-limits the
    // way a real sound card does -- the only plausible synthetic route to
    // the downsample-skirt fold, since 480 clean runs produced no ghost.
    double scale;
    if (g_clip) {
        scale = 300.0;
    } else {
        double peak = 1e-9;
        for (double v : samples) peak = std::max(peak, std::fabs(v));
        scale = 20000.0 / peak;
    }

    std::size_t const count =
        std::min(samples.size(), std::size_t(JS8_RX_SAMPLE_SIZE));
    for (std::size_t i = 0; i < count; ++i)
        dec_data.d2[i] = std::int16_t(
            std::clamp(std::round(samples[i] * scale), -32767.0, 32767.0));

    dec_data.params.kin       = int(count);
    dec_data.params.nfa       = 0;
    dec_data.params.nfb       = 4000;
    dec_data.params.nfqso     = 1500;
    dec_data.params.syncStats = false;
    dec_data.params.newdat    = true;
    dec_data.params.nutc      = code_time(0, 0, 0);
    dec_data.params.kposA = dec_data.params.kszA = 0;
    dec_data.params.kposB = dec_data.params.kszB = 0;
    dec_data.params.kposC = dec_data.params.kszC = 0;
    dec_data.params.kposE = dec_data.params.kszE = 0;
    dec_data.params.kposI = dec_data.params.kszI = 0;
    return count;
}

void selectMode(char mode, int count) {
    switch (mode) {
    case 'A': dec_data.params.nsubmodes = 1 << 0; dec_data.params.kszA = count; break;
    case 'B': dec_data.params.nsubmodes = 1 << 1; dec_data.params.kszB = count; break;
    case 'C': dec_data.params.nsubmodes = 1 << 2; dec_data.params.kszC = count; break;
    case 'E': dec_data.params.nsubmodes = 1 << 3; dec_data.params.kszE = count; break;
    case 'I': dec_data.params.nsubmodes = 1 << 4; dec_data.params.kszI = count; break;
    }
}

// ---- decode ----------------------------------------------------------

std::vector<Emission> g_emissions;

std::vector<Emission> runDecode() {
    g_emissions.clear();
    JS8::Decoder decoder;
    QEventLoop loop;
    QObject::connect(&decoder, &JS8::Decoder::decodeEvent,
                     [&loop](JS8::Event::Variant const &ev) {
        if (auto d = std::get_if<JS8::Event::Decoded>(&ev))
            g_emissions.push_back({d->frequency, d->snr, d->data});
        else if (std::get_if<JS8::Event::DecodeFinished>(&ev))
            loop.quit();
    });
    decoder.start(QThread::LowestPriority);
    decoder.decode();
    loop.exec();
    decoder.quit();
    return g_emissions;
}

// ---- geometry ----------------------------------------------------------

template <class Mode>
struct Geom {
    static constexpr double alias = 12000.0 / double(Mode::NDOWN);
    static constexpr double tone  = 12000.0 / double(Mode::NSPS);
};

// ---- experiments -------------------------------------------------------

template <class Mode>
void report(std::vector<Station> const &stations,
            std::vector<Emission> const &em, char const *title) {
    double const tol = Geom<Mode>::tone;
    std::cout << "\n== " << title << " ==\n";
    std::cout << "stations placed : " << stations.size() << "\n";
    std::cout << "frames emitted  : " << em.size() << "\n";
    int matched = 0;
    for (auto const &st : stations) {
        bool hit = false;
        for (auto const &e : em)
            if (std::fabs(e.freq - st.freqHz) <= tol) { hit = true; break; }
        std::cout << "  station " << std::setw(7) << st.freqHz << " Hz  snr "
                  << std::setw(5) << st.snrDb << " dB  : "
                  << (hit ? "EMITTED" : "LOST") << "\n";
        if (hit) ++matched;
    }
    for (auto const &e : em) {
        bool owned = false;
        for (auto const &st : stations)
            if (std::fabs(e.freq - st.freqHz) <= tol) { owned = true; break; }
        if (!owned)
            std::cout << "  GHOST at " << e.freq << " Hz snr " << e.snr
                      << "  text \"" << e.data << "\"\n";
    }
    std::cout << "matched " << matched << " of " << stations.size() << "\n";
}

template <class Mode>
void expCollapse(char mode) {
    // Field-like: twelve stations, 1850..2650, identical text, moderate and
    // varied SNR, small keying spread. Same shape as the 00:37:30 event.
    std::vector<Station> st;
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dt(0.0, 0.25);
    double const freqs[12] = {1849, 1911, 1932, 2003, 2103, 2154,
                              2204, 2253, 2345, 2404, 2635, 2680};
    double const snrs[12]  = {-4, -9, -8, 10, 0, -6, 2, 9, 5, 1, 2, -12};
    // STATIONSPLIT_SNR_BOOST=<dB>: lift every station by that much, to
    // separate "collapsed by the rule" from "too weak to decode in this
    // mode" -- Turbo is several dB less sensitive than Normal and the SNRs
    // above were chosen for Normal.
    double const boost = std::getenv("STATIONSPLIT_SNR_BOOST")
                             ? std::atof(std::getenv("STATIONSPLIT_SNR_BOOST"))
                             : 0.0;
    if (boost != 0.0) std::cout << "(snr boost " << boost << " dB)\n";
    for (int i = 0; i < 12; ++i)
        st.push_back({freqs[i], snrs[i] + boost, dt(rng)});
    auto n = synth<Mode>(st, "TESTTEST1234", 7);
    selectMode(mode, int(n));
    report<Mode>(st, runDecode(), "collapse: 12 stations, identical text");
}

// The MAGNET layout, exactly: every station one alias spacing from the
// next, so EVERY pair is on a k>=1 multiple. Geometry alone collapses all
// of them to one; the SNR gate must keep every one whose neighbours are
// within the margin. Also the only physically valid multi-station test at
// Turbo, where a signal is 160 Hz wide and the Normal-style 50-100 Hz
// spacing of expCollapse makes stations overlap and jam each other.
template <class Mode>
void expLadder(char mode) {
    double const A = Geom<Mode>::alias;
    std::vector<Station> st;
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> dt(0.0, 0.25);
    double const snrs[12] = {6, -2, 4, 0, 8, -4, 2, 5, -1, 7, 1, 3};  // spread 12 dB
    for (int i = 0; i < 12; ++i) st.push_back({700.0 + i * A, snrs[i], dt(rng)});
    auto n = synth<Mode>(st, "TESTTEST1234", 13);
    selectMode(mode, int(n));
    report<Mode>(st, runDecode(), "ladder: 12 stations exactly one alias apart");
}

template <class Mode>
void expShadow(char mode) {
    double const A = Geom<Mode>::alias, T = Geom<Mode>::tone;
    // snrB: equal to A (6 dB) = a genuine neighbour, must be EMITTED even on
    // a multiple once the SNR gate is in; far below A = the ghost signature,
    // must still COLLAPSE.
    struct Case { char const *name; double gap; double snrB; };
    Case const cases[] = {
        {"B on k=1, equal SNR (genuine neighbour)",        A,           6.0},
        {"B on k=2, equal SNR (genuine neighbour)",        2 * A,       6.0},
        {"B on k=1, 25 dB weaker (ghost signature)",       A,         -19.0},
        {"B on k=2, 25 dB weaker (ghost signature)",       2 * A,     -19.0},
        {"B just inside tolerance (k=1, +0.8 tone), equal", A + 0.8 * T, 6.0},
        {"B just outside tolerance (k=1, +1.5 tone)",      A + 1.5 * T, 6.0},
        {"B well clear (k=1 + half alias)",                A + 0.5 * A, 6.0},
    };
    for (auto const &c : cases) {
        std::vector<Station> st = {{1500.0, 6.0, 0.0}, {1500.0 + c.gap, c.snrB, 0.05}};
        auto n = synth<Mode>(st, "TESTTEST1234", 11);
        selectMode(mode, int(n));
        report<Mode>(st, runDecode(), c.name);
    }
}

template <class Mode>
void expGhost(char mode) {
    // One strong station, f0 swept. Report every emission not at f0 and
    // its error from the nearest alias multiple.
    double const A = Geom<Mode>::alias, T = Geom<Mode>::tone;
    std::cout << "\n== ghost: one strong station, f0 swept ==\n";
    std::cout << "aliasSpacing " << A << " Hz  tolerance " << T << " Hz\n";
    int ghosts = 0, runs = 0;
    double worstErr = 0.0;
    for (double snr : {15.0, 25.0, 35.0, 45.0, 55.0}) {
        for (double f0 = 700.0; f0 <= 2900.0; f0 += 37.0) {
            std::vector<Station> st = {{f0, snr, 0.0}};
            auto n = synth<Mode>(st, "TESTTEST1234", unsigned(f0));
            selectMode(mode, int(n));
            auto em = runDecode();
            ++runs;
            for (auto const &e : em) {
                if (std::fabs(e.freq - f0) <= T) continue;
                double const gap = std::fabs(e.freq - f0);
                double const k   = std::round(gap / A);
                double const err = std::fabs(gap - k * A);
                ++ghosts;
                worstErr = std::max(worstErr, err);
                std::cout << "  f0 " << f0 << " snr " << snr << " -> ghost at "
                          << e.freq << " (gap " << gap << ", k=" << k
                          << ", err " << err << " Hz, snr " << e.snr << ")"
                          << (err > T ? "  <<< OUTSIDE TOLERANCE, WOULD BE EMITTED" : "")
                          << "\n";
            }
        }
    }
    std::cout << "runs " << runs << "  ghost decodes seen " << ghosts
              << "  worst alias error " << worstErr << " Hz (tolerance " << T
              << ")\n";
}

template <class Mode>
void runAll(char mode, std::string const &which) {
    std::cout << "MODE " << mode << ": NSPS " << Mode::NSPS << " NDOWN "
              << Mode::NDOWN << " alias " << Geom<Mode>::alias << " Hz tone "
              << Geom<Mode>::tone << " Hz\n";
    if (which == "collapse" || which == "all") expCollapse<Mode>(mode);
    if (which == "ladder"   || which == "all") expLadder<Mode>(mode);
    if (which == "shadow"   || which == "all") expShadow<Mode>(mode);
    if (which == "ghost"    || which == "all") expGhost<Mode>(mode);
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    char const mode = argc > 1 ? argv[1][0] : 'A';
    std::string const which = argc > 2 ? argv[2] : "all";
    g_clip = argc > 3 && std::string(argv[3]) == "clip";
    if (g_clip) std::cout << "(clip: fixed noise floor, strong tones hard-limit)\n";

    // Echo every warning so [STATIONSPLIT] lines from JS8.cpp are visible.
    qInstallMessageHandler(+[](QtMsgType, QMessageLogContext const &,
                               QString const &msg) {
        std::cerr << msg.toStdString() << "\n";
    });

    switch (mode) {
    case 'A': runAll<ModeA>(mode, which); break;
    case 'B': runAll<ModeB>(mode, which); break;
    case 'C': runAll<ModeC>(mode, which); break;
    case 'E': runAll<ModeE>(mode, which); break;
    case 'I': runAll<ModeI>(mode, which); break;
    default:
        std::cerr << "mode must be one of A B C E I\n";
        return 2;
    }
    return 0;
}
