/**
 * @file jtty_receiver.cpp
 * @brief JTTY multi-signal decode orchestrator and stepping driver.
 *
 * Direct port of WSJT-X lib/jtty (tag v3.3.0-beta1):
 *   jtty_mdecode.f90          -> Receiver::Impl::mdecode and its helpers
 *   jtty_receive_context.f90  -> Receiver::begin / process / end / takeUpdates
 *
 * Deviations, both deliberate:
 *   - The sync-search FFTs are planned with FFTW_ESTIMATE, not FFTW_MEASURE.
 *     MEASURE picks a plan by timing, so the reference's own s0 surface is
 *     not bit-reproducible run to run; ESTIMATE makes this port
 *     deterministic. The Stage 4 gate therefore compares decode outcomes
 *     (message text, ids, terminals exact; frequency, time, SNR within
 *     tolerance), not internal floats.
 *   - The reference falls back to an in-place four2a FFT only when its
 *     plan cache fails to allocate; that path cannot occur here and is not
 *     carried.
 */

#ifdef JS8_ENABLE_JTTY

#include "jtty_receiver.h"

#include "jtty_modem.h"
#include "jtty_source.h"
#include "jtty_tbcc.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

extern std::mutex fftw_mutex;

namespace JTTY {

namespace {

using cfloat = std::complex<float>;

constexpr int MAX_ACTIVE_MESSAGES = 30;
constexpr int MAX_RECENT_FRAMES = MAX_ACTIVE_MESSAGES * MAX_FRAMES;
constexpr int MAX_CONTINUATION_GAP = 3;
constexpr int MAX_RETRO_STEPS = 3;
constexpr float FRAME_HISTORY_TIME_TOLERANCE = 0.05f;
constexpr float FRAME_HISTORY_FREQ_TOLERANCE = 3.0f;
constexpr float NEAR_SIMULTANEOUS_FREQ_TOLERANCE = 12.0f;
constexpr float CONTINUATION_TIME_TOLERANCE = 0.1f;
constexpr int MAX_SUBTRACTED = 16;
constexpr int MAXCAND = 100;
constexpr int NSYNC_SYM = SYNC_SYMBOLS;      // 13
constexpr int NCHAN_SYM = INFO_BITS;         // 46
constexpr int NFRAME_SYM = FRAME_SYMBOLS;    // 59
constexpr float FSAMPLE = 6000.0f;
constexpr float TWOPI = 6.283185307179586f;
constexpr int MIN_SYNC_FFT_ORDER = 11, MAX_SYNC_FFT_ORDER = 13;
constexpr int SYNC_NFFT = 8192;              // sync-search FFT size, df2 = 0.732 Hz

// lib/db.f90
inline float db(float x) {
    float d = -99.0f;
    if (x > 1.259e-10f) d = 10.0f * log10f(x);
    return d;
}

inline cfloat cmul(cfloat a, cfloat b) {
    return cfloat(a.real() * b.real() - a.imag() * b.imag(),
                  a.real() * b.imag() + a.imag() * b.real());
}
inline cfloat conjMul(cfloat a, cfloat b) {   // conjg(a)*b
    return cfloat(a.real() * b.real() + a.imag() * b.imag(),
                  a.real() * b.imag() - a.imag() * b.real());
}
inline float power(cfloat z) { return z.real() * z.real() + z.imag() * z.imag(); }

std::string pad80(const std::string &s) {
    std::string r = s.substr(0, MESSAGE_CHARS);
    r.resize(MESSAGE_CHARS, ' ');
    return r;
}
std::string rtrim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
int lenTrim(const std::string &s) { return static_cast<int>(rtrim(s).size()); }

struct Decode {
    float f1 = 0.0f;
    float xdt = 0.0f;
    double tsync = 0.0;
    float snrdb = 0.0f;
    std::string decoded = std::string(MESSAGE_CHARS, ' ');
    bool trailingSep = false;
    bool isLastFrame = false;
    std::string call;   // standard call of a Call8 frame (addition, for spots)
};

struct MessageAssembly {
    std::int64_t messageId = 0;
    float f1 = 0.0f;
    double tsync = 0.0;
    double startTsync = 0.0;
    float startSnrdb = 0.0f;   // SNR of the message's first frame; never reassigned after
    int k = 0;
    std::string decoded = std::string(MESSAGE_CHARS, ' ');
    bool trailingSep = false;
    std::string call;   // first Call8 call seen in this message (addition)
};

struct FrameFingerprint {
    float f1 = 0.0f;
    double tsync = 0.0;
};

struct PendingUpdate {
    std::int64_t messageId = 0;
    float f1 = 0.0f;
    double startTsync = 0.0;
    double latestTsync = 0.0;
    float snr = 0.0f;
    std::string decoded = std::string(MESSAGE_CHARS, ' ');
    bool complete = false;
    int terminal = 0;
    std::string call;   // addition, for spots
};

// One cached out-of-place FFT pair per power-of-two order 11..13.
struct SyncFftCache {
    int nfft = 0;
    fftwf_plan forward = nullptr;
    fftwf_plan backward = nullptr;
    cfloat *input = nullptr;
    cfloat *spectrum = nullptr;
    cfloat *inverse = nullptr;
};

bool sameFrame(float f1a, double ta, float f1b, double tb) {
    return std::fabs(f1a - f1b) < FRAME_HISTORY_FREQ_TOLERANCE &&
           std::fabs(ta - tb) < static_cast<double>(FRAME_HISTORY_TIME_TOLERANCE);
}
bool sameRecentFrame(float f1a, double ta, float f1b, double tb) {
    return std::fabs(f1a - f1b) < NEAR_SIMULTANEOUS_FREQ_TOLERANCE &&
           std::fabs(ta - tb) < static_cast<double>(FRAME_HISTORY_TIME_TOLERANCE);
}

std::string displayMessageText(const std::string &decodedIn) {
    std::string msg = pad80(decodedIn);
    const int bound = lenTrim(msg) - 4;   // Fortran evaluates the DO bound once
    for (int i = 1; i <= bound; ++i)
        if (msg.compare(i - 1, 5, "~~~~~") == 0) msg.replace(i - 1, 5, " ... ");
    const int lt = lenTrim(msg);
    for (int i = 0; i < lt; ++i)
        if (msg[i] == '~') msg[i] = ' ';
    if (msg[0] == ' ') msg = pad80(rtrim(msg.substr(1)));
    return msg;
}

// jtty_search_window
void searchWindow(float &fc, float fwid, int nfa, int nfb, bool constrainToGraph, float df,
                  int firstBin, int lastBin, int &ja, int &jb, bool &usable) {
    ja = firstBin;
    jb = lastBin;
    usable = false;
    if (df <= 0.0f || fwid < 0.0f || firstBin > lastBin) return;
    if (constrainToGraph) {
        if (nfa > nfb) return;
        fc = std::max(static_cast<float>(nfa), std::min(fc, static_cast<float>(nfb)));
    }
    ja = std::max(firstBin, static_cast<int>((fc - fwid) / df));
    jb = std::min(lastBin, static_cast<int>((fc + fwid) / df));
    if (constrainToGraph) {
        ja = std::max(ja, static_cast<int>(std::ceil(static_cast<float>(nfa) / df)));
        jb = std::min(jb, static_cast<int>(std::floor(static_cast<float>(nfb) / df)));
    }
    usable = ja <= jb;
}

// classify_active_candidate
void classifyActiveCandidate(const MessageAssembly &existing, const Decode &candidate,
                             float framePeriod, bool &match, bool &isWindowDupe,
                             int &nframesGap) {
    const float df1 = candidate.f1 - existing.f1;
    const double dtsync = candidate.tsync - existing.tsync;
    match = false;
    nframesGap = 1;
    const int nfp = static_cast<int>(std::lround(dtsync / static_cast<double>(framePeriod)));
    const double fpResid = std::fabs(dtsync - static_cast<double>(framePeriod) * nfp);
    if (nfp >= 1 && nfp <= MAX_CONTINUATION_GAP &&
        fpResid < static_cast<double>(CONTINUATION_TIME_TOLERANCE)) {
        const float dfTol = 10.0f + 3.0f * static_cast<float>(nfp - 1);
        match = std::fabs(df1) < dfTol;
        if (match) nframesGap = nfp;
    }
    // Retro sweeps revisit only the preceding three quarter-frame windows.
    isWindowDupe = false;
    if (!match) {
        const double qstep = static_cast<double>(framePeriod / 4.0f);
        const int nstep = static_cast<int>(std::lround(dtsync / qstep));
        const double resid = std::fabs(dtsync - qstep * nstep);
        if (std::abs(nstep) <= MAX_RETRO_STEPS && std::fabs(df1) < 10.0f && resid < 0.003 &&
            !(std::abs(nstep) % 4 == 0 && nstep != 0)) {
            match = true;
            isWindowDupe = true;
        }
    }
}

} // namespace

/******************************************************************************/
// Receiver::Impl
/******************************************************************************/

struct Receiver::Impl {
    // ---- reception state (jtty_mdec module + receive_state) ----------------
    int ndecodes = 0;
    int nactive = 0;
    MessageAssembly active[MAX_ACTIVE_MESSAGES];
    int nrecent = 0;
    FrameFingerprint recent[MAX_RECENT_FRAMES];
    std::vector<PendingUpdate> pending;   // pending[pendingFirst .. pendingFirst+npending)
    int npending = 0;
    int pendingFirst = 0;
    std::int64_t nextMessageId = 1;
    std::int64_t sampleOrigin = 0;
    std::int64_t firstSearchSample = 0;
    bool explicitReceiveContext = false;

    // retro re-sweep plumbing
    bool interfererPending = false;
    float interfererF1 = 0.0f;
    double interfererTsync = 0.0;
    std::int8_t interfererPayload[PAYLOAD_BITS] = {};
    int nsubtracted = 0;
    float subtractedF1[MAX_SUBTRACTED] = {};
    double subtractedTsync[MAX_SUBTRACTED] = {};
    std::int8_t subtractedPayload[MAX_SUBTRACTED][PAYLOAD_BITS] = {};

    // ---- context (receive_context) -----------------------------------------
    bool running = false;
    int nsps = 384;
    std::int64_t sessionId = 0, gridOrigin = 0, firstSearch = 0, nextSearch = 0;
    struct Retro {
        bool active = false;
        int count = 0, signal = 1, step = 1;
        float frequency[MAX_SUBTRACTED] = {};
        double syncTime[MAX_SUBTRACTED] = {};
        std::int8_t payload[MAX_SUBTRACTED][PAYLOAD_BITS] = {};
    } retro;

    // ---- DSP scratch, rebuilt when nsps changes (the saved locals) ---------
    int nsps0 = -999;
    int nss = 0, nfft = SYNC_NFFT, nh2 = 0, nframe6 = 0, ntstep = 0, ntgrid = 0, nana = 0;
    float baud = 0.0f, dt = 0.0f, df2 = 0.0f;
    std::vector<cfloat> csync, c0, c, c1;
    std::vector<cfloat> ctones;   // [tone*nss + sample]
    std::vector<float> s0;        // [j + (nh2+1)*istep]
    std::vector<char> mask0;
    PayloadCorrelator correlator;
    Decoder fec;

    // sync chirp (Bluestein) state and FFT caches
    std::vector<cfloat> syncChirpWeights, syncChirpKernel;
    int syncChirpSamples = 0, syncChirpFirstBin = -1, syncChirpOutputCount = 0;
    SyncFftCache caches[MAX_SYNC_FFT_ORDER - MIN_SYNC_FFT_ORDER + 1];

    // ---- per-call host variables of jtty_mdecode ---------------------------
    int istart = 0;
    int nchunk = 0, nchunk6 = 0;
    int nfa = 0, nfb = 0;
    float f0 = 0.0f, ftol = 0.0f, smin = 0.0f;
    int ichan = 0, ipass = 0, ja = 0, jb = 0;
    bool coverBand = false; // setWideCoverage: tile nfa..nfb instead of 1350/1650
    int nchan = 2;          // wide channels in play this call
    bool usable = false;
    float fc = 0.0f, fwid = 0.0f;
    int nfz = 0, ntz = 0, nc = 2, nc0 = 2, ncand = 0, nstepSearch = 0;
    int firstSyncBin = 0, lastSyncBin = 0;
    int nCh0Ok = 0;
    int jaCh0Ok[16] = {}, jbCh0Ok[16] = {};
    float f1Ch0Ok[16] = {};
    double tsyncCh0Ok[16] = {};
    int nsync = 0;
    float pt = 0.0f, pa = 0.0f;
    float pow[NCHAN_SYM][4] = {};
    int irxsync[NSYNC_SYM] = {}, irxchan[NCHAN_SYM] = {};
    Decode cand[MAXCAND];
    bool channelDecoded = false, anySubtracted = false, s0Valid = false;
    bool useInterferer = false;
    std::int8_t tonesChk[NCHAN_SYM] = {};
    std::int8_t finalPayload[PAYLOAD_BITS] = {};

    ~Impl() { releaseFftResources(); }

    // ---- small helpers ------------------------------------------------------
    double sampleTime(int index) const {
        return static_cast<double>(sampleOrigin + static_cast<std::int64_t>(index - 1)) / 12000.0;
    }
    float &S0(int j, int istep) { return s0[static_cast<std::size_t>(j) + static_cast<std::size_t>(nh2 + 1) * istep]; }
    char &M0(int j, int istep) { return mask0[static_cast<std::size_t>(j) + static_cast<std::size_t>(nh2 + 1) * istep]; }

    void resetDecodeSearchState() {
        nactive = 0;
        nrecent = 0;
    }

    bool isRecentFrame(const Decode &candidate) const {
        for (int i = 0; i < nrecent; ++i)
            if (sameRecentFrame(candidate.f1, candidate.tsync, recent[i].f1, recent[i].tsync)) return true;
        return false;
    }

    void rememberRecentFrame(const Decode &candidate) {
        if (nrecent >= MAX_RECENT_FRAMES) {
            // Duplicate history must never prevent an otherwise valid decode.
            for (int i = 0; i < MAX_RECENT_FRAMES - 1; ++i) recent[i] = recent[i + 1];
        } else {
            ++nrecent;
        }
        recent[nrecent - 1].f1 = candidate.f1;
        recent[nrecent - 1].tsync = candidate.tsync;
    }

    void queueMessageUpdate(const MessageAssembly &message, bool complete, int terminal = -1) {
        int status = UPDATE_GROWING;
        if (complete) status = UPDATE_COMPLETE;
        if (terminal >= 0) status = terminal;
        for (int i = 0; i < npending; ++i) {
            PendingUpdate &u = pending[static_cast<std::size_t>(pendingFirst + i)];
            if (u.messageId == message.messageId) {
                u.f1 = message.f1;
                u.decoded = message.decoded;
                u.complete = complete;
                u.latestTsync = message.tsync;
                u.terminal = status;
                if (u.call.empty()) u.call = message.call;
                return;
            }
        }
        if (pendingFirst + npending >= static_cast<int>(pending.size())) {
            if (pendingFirst > 0) {
                for (int i = 0; i < npending; ++i)
                    pending[static_cast<std::size_t>(i)] = pending[static_cast<std::size_t>(pendingFirst + i)];
                pendingFirst = 0;
            }
            if (npending >= static_cast<int>(pending.size()))
                pending.resize(std::max(MAX_ACTIVE_MESSAGES, 2 * npending));
        }
        PendingUpdate &u = pending[static_cast<std::size_t>(pendingFirst + npending)];
        ++npending;
        u.messageId = message.messageId;
        u.f1 = message.f1;
        u.startTsync = message.startTsync;
        u.latestTsync = message.tsync;
        u.snr = message.startSnrdb;
        u.decoded = message.decoded;
        u.complete = complete;
        u.terminal = status;
        u.call = message.call;
    }

    void removeActiveMessage(int index) {   // 1-based, swap-with-last as the reference
        if (index < 1 || index > nactive) return;
        if (index < nactive) active[index - 1] = active[nactive - 1];
        --nactive;
    }

    void startMessage(const Decode &candidate, MessageAssembly &message, bool &accepted) {
        message = MessageAssembly{};
        accepted = false;
        if (!candidate.isLastFrame && nactive >= MAX_ACTIVE_MESSAGES) return;
        rememberRecentFrame(candidate);
        message.messageId = nextMessageId;
        message.f1 = candidate.f1;
        message.tsync = candidate.tsync;
        message.startTsync = candidate.tsync;
        message.startSnrdb = candidate.snrdb;
        message.decoded = candidate.decoded;
        if (message.decoded.compare(0, 4, "599 ") == 0) message.decoded = pad80("~" + rtrim(message.decoded));
        message.k = lenTrim(message.decoded);
        message.trailingSep = candidate.trailingSep;
        message.call = candidate.call;
        ++nextMessageId;
        queueMessageUpdate(message, candidate.isLastFrame);
        if (!candidate.isLastFrame) {
            ++nactive;
            active[nactive - 1] = message;
        }
        accepted = true;
    }

    void appendActiveMessage(int index, const Decode &candidate, int nframesGap,
                             MessageAssembly &message, bool &accepted) {
        message = MessageAssembly{};
        accepted = false;
        if (index < 1 || index > nactive) return;
        rememberRecentFrame(candidate);
        MessageAssembly &a = active[index - 1];
        const int k = a.k;
        const int n = lenTrim(candidate.decoded);
        if (nframesGap > 1) {
            int nstart = 1;
            if (n >= 1 && candidate.decoded[0] == '~') nstart = 2;
            const int kz = std::min(k + 5 + (n - nstart + 1), MESSAGE_CHARS);
            const int nchar = std::max(kz - k - 5, 0);
            a.decoded = pad80(rtrim(a.decoded) + "~~~~~" + candidate.decoded.substr(nstart - 1, nchar));
            a.k = k + 5 + nchar;
        } else {
            const int kz = std::min(k + n, MESSAGE_CHARS);
            const int take = std::max(kz - k, 0);
            if (a.trailingSep)
                a.decoded = pad80(rtrim(a.decoded) + " " + candidate.decoded.substr(0, take));
            else
                a.decoded = pad80(rtrim(a.decoded) + candidate.decoded.substr(0, take));
            a.k = kz;
        }
        a.trailingSep = candidate.trailingSep;
        a.f1 = candidate.f1;
        a.tsync = candidate.tsync;
        if (a.call.empty()) a.call = candidate.call;
        queueMessageUpdate(a, candidate.isLastFrame);
        message = a;
        if (candidate.isLastFrame) removeActiveMessage(index);
        accepted = true;
    }

    void pruneReceiveState(double forwardTsync, float framePeriod) {
        const double oldestRevisit =
            forwardTsync - static_cast<double>(static_cast<float>(MAX_RETRO_STEPS) * framePeriod / 4.0f);
        // Retro candidates move backward in time, so only the forward watermark may expire history.
        int keep = 0;
        for (int i = 0; i < nrecent; ++i) {
            if (recent[i].tsync < oldestRevisit - static_cast<double>(FRAME_HISTORY_TIME_TOLERANCE)) continue;
            if (keep != i) recent[keep] = recent[i];
            ++keep;
        }
        nrecent = keep;
        int i = 1;
        while (i <= nactive) {
            if (oldestRevisit - active[i - 1].tsync >
                static_cast<double>(static_cast<float>(MAX_CONTINUATION_GAP) * framePeriod +
                                    CONTINUATION_TIME_TOLERANCE)) {
                queueMessageUpdate(active[i - 1], false, UPDATE_EXPIRED);
                removeActiveMessage(i);
            } else {
                ++i;
            }
        }
    }

    // ---- FFT caches -----------------------------------------------------------
    SyncFftCache *ensureSyncFftCache(int n, bool needsBackward) {
        if (n <= 0) return nullptr;
        int order = 0;
        while ((1 << order) < n) ++order;
        if ((1 << order) != n || order < MIN_SYNC_FFT_ORDER || order > MAX_SYNC_FFT_ORDER) return nullptr;
        SyncFftCache &cache = caches[order - MIN_SYNC_FFT_ORDER];
        if (!cache.forward) {
            cache.nfft = n;
            cache.input = static_cast<cfloat *>(fftwf_malloc(sizeof(cfloat) * n));
            cache.spectrum = static_cast<cfloat *>(fftwf_malloc(sizeof(cfloat) * n));
            std::fill(cache.input, cache.input + n, cfloat(0.0f, 0.0f));
            std::lock_guard<std::mutex> lock(fftw_mutex);
            cache.forward = fftwf_plan_dft_1d(n, reinterpret_cast<fftwf_complex *>(cache.input),
                                              reinterpret_cast<fftwf_complex *>(cache.spectrum),
                                              FFTW_FORWARD, FFTW_ESTIMATE | FFTW_PRESERVE_INPUT);
        }
        if (needsBackward && !cache.backward) {
            cache.inverse = static_cast<cfloat *>(fftwf_malloc(sizeof(cfloat) * n));
            std::lock_guard<std::mutex> lock(fftw_mutex);
            cache.backward = fftwf_plan_dft_1d(n, reinterpret_cast<fftwf_complex *>(cache.spectrum),
                                               reinterpret_cast<fftwf_complex *>(cache.inverse),
                                               FFTW_BACKWARD, FFTW_ESTIMATE);
        }
        return &cache;
    }

    void releaseFftResources() {
        for (SyncFftCache &cache : caches) {
            {
                std::lock_guard<std::mutex> lock(fftw_mutex);
                if (cache.forward) fftwf_destroy_plan(cache.forward);
                if (cache.backward) fftwf_destroy_plan(cache.backward);
            }
            if (cache.input) fftwf_free(cache.input);
            if (cache.spectrum) fftwf_free(cache.spectrum);
            if (cache.inverse) fftwf_free(cache.inverse);
            cache = SyncFftCache{};
        }
        syncChirpWeights.clear();
        syncChirpKernel.clear();
        syncChirpSamples = 0;
        syncChirpFirstBin = -1;
        syncChirpOutputCount = 0;
    }

    // ---- jtty_mdecode -----------------------------------------------------------
    void channelWindow() {
        if (ichan == 0) {
            fc = f0;
            fwid = ftol;
        } else if (coverBand) {
            // Wide coverage: 300 Hz windows tiling nfa..nfb, the last one
            // pulled back to end at nfb (overlap rather than a gap).
            fwid = 150.0f;
            fc = static_cast<float>(nfa) + 150.0f + 300.0f * static_cast<float>(ichan - 1);
            if (fc + fwid > static_cast<float>(nfb))
                fc = std::max(static_cast<float>(nfa) + fwid, static_cast<float>(nfb) - fwid);
        } else {
            fc = 1350.0f;
            if (ichan == 2) fc = 1650.0f;
            fwid = 150.0f;
        }
        searchWindow(fc, fwid, nfa, nfb, ichan != 0, df2, 3, nh2 - 2, ja, jb, usable);
    }

    void buildS0() {
        const double CHIRP_TWOPI = 6.2831853071795864769;
        const int firstOutputBin = firstSyncBin - 2;
        const int outputCount = lastSyncBin - firstSyncBin + 5;
        const int convolutionLength = NSYNC_SYM * nss + outputCount - 1;
        int chirpNfft = 1;
        while (chirpNfft < convolutionLength) chirpNfft *= 2;
        bool useChirp = outputCount > 0 && chirpNfft < nfft && chirpNfft <= nfft / 2;
        SyncFftCache *cache = nullptr;
        if (useChirp) {
            cache = ensureSyncFftCache(chirpNfft, true);
            useChirp = cache != nullptr;
        }
        if (useChirp && (syncChirpSamples != NSYNC_SYM * nss || syncChirpFirstBin != firstOutputBin ||
                         syncChirpOutputCount != outputCount)) {
            syncChirpWeights.assign(static_cast<std::size_t>(NSYNC_SYM) * nss, cfloat(0.0f, 0.0f));
            syncChirpKernel.assign(chirpNfft, cfloat(0.0f, 0.0f));
            for (int index = 0; index < NSYNC_SYM * nss; ++index) {
                const double phase = -CHIRP_TWOPI * (static_cast<double>(firstOutputBin) * index +
                                                     0.5 * static_cast<double>(index) * index) /
                                     static_cast<double>(nfft);
                const cfloat rot(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
                const cfloat cs = csync[index];
                syncChirpWeights[index] = cmul(cfloat(cs.real(), -cs.imag()), rot);
            }
            std::fill(cache->input, cache->input + chirpNfft, cfloat(0.0f, 0.0f));
            for (int distance = 0; distance < std::max(NSYNC_SYM * nss, outputCount); ++distance) {
                const double phase = 0.5 * CHIRP_TWOPI * static_cast<double>(distance) * distance /
                                     static_cast<double>(nfft);
                const cfloat rot(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
                if (distance < outputCount) cache->input[distance] = rot;
                if (distance > 0 && distance < NSYNC_SYM * nss) cache->input[chirpNfft - distance] = rot;
            }
            fftwf_execute_dft(cache->forward, reinterpret_cast<fftwf_complex *>(cache->input),
                              reinterpret_cast<fftwf_complex *>(cache->spectrum));
            std::copy(cache->spectrum, cache->spectrum + chirpNfft, syncChirpKernel.begin());
            syncChirpSamples = NSYNC_SYM * nss;
            syncChirpFirstBin = firstOutputBin;
            syncChirpOutputCount = outputCount;
        }

        // Wider searches retain the full FFT path.
        SyncFftCache *full = nullptr;
        if (!useChirp) full = ensureSyncFftCache(nfft, false);
        float chirpScale = 0.0f;
        if (useChirp) {
            std::fill(cache->input + NSYNC_SYM * nss, cache->input + chirpNfft, cfloat(0.0f, 0.0f));
            chirpScale = 1.0f / static_cast<float>(chirpNfft);
        } else if (full) {
            std::fill(full->input + NSYNC_SYM * nss, full->input + nfft, cfloat(0.0f, 0.0f));
        }

        int istep = 0;
        for (int i0 = 0; i0 <= ntstep; i0 += 12) {   // search over quarter-frame segment
            if (useChirp) {
                for (int n = 0; n < NSYNC_SYM * nss; ++n) cache->input[n] = cmul(syncChirpWeights[n], c0[i0 + n]);
                fftwf_execute_dft(cache->forward, reinterpret_cast<fftwf_complex *>(cache->input),
                                  reinterpret_cast<fftwf_complex *>(cache->spectrum));
                for (int n = 0; n < chirpNfft; ++n) cache->spectrum[n] = cmul(cache->spectrum[n], syncChirpKernel[n]);
                fftwf_execute_dft(cache->backward, reinterpret_cast<fftwf_complex *>(cache->spectrum),
                                  reinterpret_cast<fftwf_complex *>(cache->inverse));
                // Bluestein's omitted output chirp has unit magnitude.
                auto scaledPower = [&](int n) {
                    const cfloat cz(cache->inverse[n].real() * chirpScale, cache->inverse[n].imag() * chirpScale);
                    return cz.real() * cz.real() + cz.imag() * cz.imag();
                };
                float p0 = scaledPower(0), p1 = scaledPower(1), p2 = scaledPower(2), p3 = scaledPower(3);
                for (int j = firstSyncBin; j <= lastSyncBin; ++j) {
                    const float p4 = scaledPower(j - firstSyncBin + 4);
                    S0(j, istep) = p0 + 2 * p1 + 3 * p2 + 2 * p3 + p4;
                    p0 = p1;
                    p1 = p2;
                    p2 = p3;
                    p3 = p4;
                }
            } else if (full) {
                for (int n = 0; n < NSYNC_SYM * nss; ++n) full->input[n] = conjMul(csync[n], c0[i0 + n]);
                fftwf_execute_dft(full->forward, reinterpret_cast<fftwf_complex *>(full->input),
                                  reinterpret_cast<fftwf_complex *>(full->spectrum));
                const cfloat *out = full->spectrum;
                // Keep the two-bin halo while advancing the five-bin smoothing kernel.
                float p0 = power(out[firstSyncBin - 2]), p1 = power(out[firstSyncBin - 1]),
                      p2 = power(out[firstSyncBin]), p3 = power(out[firstSyncBin + 1]);
                for (int j = firstSyncBin; j <= lastSyncBin; ++j) {
                    const float p4 = power(out[j + 2]);
                    S0(j, istep) = p0 + 2 * p1 + 3 * p2 + 2 * p3 + p4;
                    p0 = p1;
                    p1 = p2;
                    p2 = p3;
                    p3 = p4;
                }
            }
            ++istep;
        }
        nstepSearch = istep - 1;
        s0Valid = true;
    }

    // maxloc over s0(ja:jb, 0:nstepSearch), optionally masked: first maximum
    // in column-major order, (0,0) when nothing qualifies.
    void surfaceMaxloc(bool masked, int &loc1, int &loc2) {
        loc1 = 0;
        loc2 = 0;
        bool any = false;
        float best = 0.0f;
        for (int istep = 0; istep <= nstepSearch; ++istep)
            for (int j = ja; j <= jb; ++j) {
                if (masked && !M0(j, istep)) continue;
                const float v = S0(j, istep);
                if (!any || v > best) {
                    any = true;
                    best = v;
                    loc1 = j - ja + 1;
                    loc2 = istep + 1;
                }
            }
    }

    void recordCh0Success() {
        if (ichan != 0) return;
        if (nCh0Ok >= 16) return;
        const Decode &cd = cand[ncand - 1];
        jaCh0Ok[nCh0Ok] = static_cast<int>(std::lround(cd.f1 / df2)) - nfz;
        jbCh0Ok[nCh0Ok] = static_cast<int>(std::lround(cd.f1 / df2)) + nfz;
        f1Ch0Ok[nCh0Ok] = cd.f1;
        tsyncCh0Ok[nCh0Ok] = cd.tsync;
        ++nCh0Ok;
    }

    void decodeAndMerge(bool &decodedOk) {
        decodedOk = false;
        Decode &cd = cand[ncand - 1];
        const int payloadStart = static_cast<int>(std::lround(cd.xdt / dt)) + NSYNC_SYM * nss;
        CorrelationFrame zsym, zhalf;
        correlator.correlate(c1.data(), nchunk6, payloadStart, zsym, zhalf);
        DecodeResult result;
        const bool successDec = fec.decode(zsym, zhalf, finalPayload, &result);
        // Half-symbol off-tone leakage is not a noise estimate; diagnostics use M1.
        for (int j = 0; j < NCHAN_SYM; ++j) {
            int best = 0;
            for (int i = 0; i < 4; ++i) {
                const float a = hypotf(zsym.c[j][i].real(), zsym.c[j][i].imag());
                pow[j][i] = a * a;
                if (i > 0 && pow[j][i] > pow[j][best]) best = i;
            }
            irxchan[j] = best;
        }
        cd.decoded = std::string(MESSAGE_CHARS, ' ');
        if (!successDec) return;

        SourceFrame frame;
        for (int b = 0; b < PAYLOAD_BITS; ++b) frame.bits[b] = finalPayload[b];
        const AssembledMessage assembled = unpackJtty(&frame, 1);
        cd.decoded = assembled.text;
        cd.trailingSep = assembled.trailingSeparator;
        cd.isLastFrame = assembled.isLastFrame;
        if (!assembled.sourceValid) return;
        {
            // Addition over the reference: remember a Call8 frame's call so
            // the message can be spotted. Grammar-validated above.
            SourceAtom atom;
            bool eomFlag;
            cd.call.clear();
            if (unpackJttyAtom(frame, atom, eomFlag) && atom.kind == ATOM_CALL) cd.call = atom.text;
        }

        ++ndecodes;
        // Re-encode the decoded payload to recover the expected tone per
        // symbol for the SNR diagnostic.
        tbccEncode(finalPayload, tonesChk);
        for (int j = 0; j < NCHAN_SYM; ++j) {
            const int is = tonesChk[j];
            pt = pt + pow[j][is];
            pa = pa + (((pow[j][0] + pow[j][1]) + pow[j][2]) + pow[j][3]);
        }
        const float pn = (pa - pt) / 3.0f;
        if (pn > 0.0f) {
            // Solve pt=S+N, pn=N+SNR_LEAKAGE_K*S for S/N
            const float snrdb = std::max(db((pt - pn) / pn) - db(2500.0f / baud), SNR_FLOOR_DB);
            cd.snrdb = snrdb;
        }
        cd.tsync = sampleTime(istart) + static_cast<double>(cd.xdt);
        decodedOk = true;

        // dupe detection
        bool dupe = false;
        for (int i = 0; i < ncand - 1; ++i)
            if (cand[i].decoded == cd.decoded && std::fabs(cand[i].tsync - cd.tsync) < 0.032) dupe = true;
        // A channel-1/2 candidate matching a frame channel 0 already decoded
        // this call is sync-estimation noise, not a distinct signal.
        if (ichan != 0)
            for (int i = 0; i < nCh0Ok; ++i)
                if (sameFrame(cd.f1, cd.tsync, f1Ch0Ok[i], tsyncCh0Ok[i])) dupe = true;
        if (dupe) return;

        // Subtract this signal from c0 so a weaker one underneath can be
        // found by a follow-up sweep over the residual.
        int tonesFull[NFRAME_SYM];
        for (int i = 0; i < NSYNC_SYM; ++i) tonesFull[i] = SYNC_TONES[i];
        for (int i = 0; i < NCHAN_SYM; ++i) tonesFull[NSYNC_SYM + i] = tonesChk[i];
        subtractJtty(c0.data(), nana, nchunk6, tonesFull, NFRAME_SYM, nss, cd.f1, cd.xdt);
        anySubtracted = true;
        s0Valid = false;
        if (nsubtracted < MAX_SUBTRACTED) {
            subtractedF1[nsubtracted] = cd.f1;
            subtractedTsync[nsubtracted] = cd.tsync;
            std::memcpy(subtractedPayload[nsubtracted], finalPayload, sizeof finalPayload);
            ++nsubtracted;
        }

        const Decode dec = cd;
        bool match = false;
        bool isPureDupe = isRecentFrame(dec);
        int iactive = 0, bestGap = 1;
        if (!isPureDupe && nactive > 0) {
            bool haveWin = false;
            int bestCont = 0;
            float bestDf = 1.0e30f;
            for (int i = 1; i <= nactive; ++i) {
                bool isWindowDupe;
                int gap;
                classifyActiveCandidate(active[i - 1], dec, static_cast<float>(nframe6) / 6000.0f, match,
                                        isWindowDupe, gap);
                if (!match) continue;
                if (isWindowDupe) {
                    if (!haveWin) iactive = i;
                    haveWin = true;
                    continue;
                }
                const float dfabs = std::fabs(dec.f1 - active[i - 1].f1);
                if (dfabs < bestDf) {
                    bestDf = dfabs;
                    bestCont = i;
                    bestGap = gap;
                }
            }
            if (haveWin) {
                match = true;
                isPureDupe = true;
            } else if (bestCont > 0) {
                match = true;
                iactive = bestCont;
            } else {
                match = false;
            }
        }

        MessageAssembly acceptedMessage;
        bool accepted;
        if (!isPureDupe && match) {
            appendActiveMessage(iactive, dec, bestGap, acceptedMessage, accepted);
        } else if (!isPureDupe) {
            startMessage(dec, acceptedMessage, accepted);
        }
    }

    void processChannel() {
        // One channel's candidate search plus sticky-sync retry.
        channelWindow();
        if (!usable) return;
        nc0 = nc;
        // A wide QSO band needs more candidates to avoid crowding out its signal.
        if (ichan == 0)
            nc0 = std::max(2, std::min(8, static_cast<int>(std::lround(fwid / (static_cast<float>(nfz) * df2)))));
        channelDecoded = false;

        if (ichan != 0 && nCh0Ok > 0) {
            // Erase only channel 0's actual successful-decode neighbourhoods.
            for (int i = 0; i < nCh0Ok; ++i) {
                const int lo = std::max(ja, jaCh0Ok[i]), hi = std::min(jb, jbCh0Ok[i]);
                if (lo <= hi)
                    for (int istep = 0; istep <= nstepSearch; ++istep)
                        for (int j = lo; j <= hi; ++j) S0(j, istep) = 0.0f;
            }
        }
        if (ichan == 0)
            for (int istep = 0; istep <= nstepSearch; ++istep)
                for (int j = ja; j <= jb; ++j) M0(j, istep) = 1;

        for (int ic = 1; ic <= nc0; ++ic) {
            int loc1, loc2;
            surfaceMaxloc(ichan == 0, loc1, loc2);
            const int jlo = std::max(ja, loc1 - nfz + ja), jhi = std::min(jb, loc1 + nfz + ja);
            const int slo = std::max(0, loc2 - ntz), shi = std::min(nstepSearch, loc2 + ntz);
            for (int istep = slo; istep <= shi; ++istep)
                for (int j = jlo; j <= jhi; ++j) {
                    if (ichan == 0)
                        M0(j, istep) = 0;   // private mask: keeps the shared surface for channels 1/2
                    else
                        S0(j, istep) = 0.0f;
                }
            float fbest = static_cast<float>(loc1 - 1 + ja) * df2;
            float xdtbest = static_cast<float>(loc2 - 1) * dt * 12.0f;

            if (ichan == 0) {
                float xdt1, f11, snr0;
                jttyPeakup(c0.data(), c1.data(), csync.data(), nchunk6, nss, xdtbest, fbest, xdt1, f11, snr0);
                xdtbest = xdt1;
                fbest = f11;
            }

            if (ncand >= MAXCAND) break;
            ++ncand;
            Decode &cd = cand[ncand - 1];
            cd = Decode{};
            cd.xdt = xdtbest;
            cd.f1 = fbest;

            float a[3] = {-cd.f1, 0.0f, 0.0f};   // shift peak to zero frequency
            twkfreq(c0.data(), c1.data(), nchunk6, 6000.0f, a);

            pt = 0.0f;
            pa = 0.0f;
            for (auto &row : pow)
                for (float &v : row) v = 0.0f;
            for (int j = 0; j < NSYNC_SYM; ++j) irxsync[j] = -1;
            for (int j = 1; j <= NSYNC_SYM; ++j) {   // tone powers for sync symbols
                const int i0 = static_cast<int>(std::lround(cd.xdt / dt)) + (j - 1) * nss;
                if (i0 + nss > nchunk6) break;
                for (int i = 0; i < 4; ++i) {
                    cfloat z(0.0f, 0.0f);
                    for (int k = 0; k < nss; ++k) z = z + conjMul(ctones[static_cast<std::size_t>(i) * nss + k], c1[i0 + k]);
                    pow[j - 1][i] = power(z);
                }
                int best = 0;
                for (int i = 1; i < 4; ++i)
                    if (pow[j - 1][i] > pow[j - 1][best]) best = i;
                irxsync[j - 1] = best;
                pt = pt + pow[j - 1][SYNC_TONES[j - 1]];                          // signal plus noise
                pa = pa + (((pow[j - 1][0] + pow[j - 1][1]) + pow[j - 1][2]) + pow[j - 1][3]);   // signal plus 4*noise
            }
            float snrdb = -99.9f;
            const float pn = (pa - pt) / 3.0f;
            if (pn > 0.0f) snrdb = db(pt / pn);
            nsync = 0;
            for (int j = 0; j < NSYNC_SYM; ++j)
                if (SYNC_TONES[j] == irxsync[j]) ++nsync;
            cd.snrdb = snrdb;

            if (ichan == 0 && (nsync <= 6 || snrdb < smin)) continue;
            if (ichan != 0 && (nsync <= 8 || snrdb < 5.0f)) continue;

            // looks like a real candidate - try to decode
            bool decodedOk;
            decodeAndMerge(decodedOk);
            if (decodedOk) recordCh0Success();
            if (decodedOk) channelDecoded = true;
        }

        if (!channelDecoded) {
            // Sticky-sync retry: if an active message's continuation frame is
            // due almost exactly one frame period ago, retry the FEC decode at
            // that remembered sync point instead of giving up on it.
            for (int ir = 1; ir <= nactive; ++ir) {
                const MessageAssembly &am = active[ir - 1];
                if (am.f1 < fc - fwid || am.f1 > fc + fwid) continue;
                const float framePeriod = static_cast<float>(nframe6) / 6000.0f;
                if (std::fabs((sampleTime(istart) - am.tsync) - static_cast<double>(framePeriod)) > 0.1) continue;
                const float xdtRetry =
                    static_cast<float>(am.tsync + static_cast<double>(framePeriod) - sampleTime(istart));
                if (xdtRetry < 0.0f) continue;
                if (ncand >= MAXCAND) break;
                ++ncand;
                Decode &cd = cand[ncand - 1];
                cd = Decode{};
                cd.xdt = xdtRetry;
                cd.f1 = am.f1;
                float a[3] = {-cd.f1, 0.0f, 0.0f};
                twkfreq(c0.data(), c1.data(), nchunk6, 6000.0f, a);
                nsync = -1;
                bool decodedOk;
                decodeAndMerge(decodedOk);
                if (decodedOk) recordCh0Success();
                if (decodedOk) channelDecoded = true;
                break;   // at most one retry attempt per channel per call
            }
        }
    }

    void mdecode(int istartIn, int istart0, const std::int16_t *iwave, int nchunkIn, int nspsIn,
                 int nfaIn, int nfbIn, float f0In, float ftolIn, float sminIn) {
        istart = istartIn;
        nchunk = nchunkIn;
        nfa = nfaIn;
        nfb = nfbIn;
        f0 = f0In;
        ftol = ftolIn;
        smin = sminIn;

        // Capture and clear the retro-resweep interferer request first.
        useInterferer = interfererPending;
        const float useInterfererF1 = interfererF1;
        const double useInterfererTsync = interfererTsync;
        std::int8_t useInterfererPayload[PAYLOAD_BITS];
        std::memcpy(useInterfererPayload, interfererPayload, sizeof useInterfererPayload);
        interfererPending = false;
        nsubtracted = 0;
        nsync = 0;

        if (istart == istart0 && !useInterferer && !explicitReceiveContext) {
            ndecodes = 0;
            resetDecodeSearchState();
        }
        long long sumAbs = 0;
        for (int i = 0; i < nchunk; ++i) sumAbs += std::abs(static_cast<int>(iwave[i]));
        if (sumAbs == 0) return;

        nchunk6 = nchunk / 2;
        nana = 1 << static_cast<int>(std::lround(logf(static_cast<float>(nchunk)) / logf(2.0f) + 0.5f));

        if (nspsIn != nsps0) {
            nsps0 = nspsIn;
            nss = nspsIn / 2;
            nfft = SYNC_NFFT;
            df2 = FSAMPLE / static_cast<float>(nfft);
            nh2 = nfft / 2;
            nframe6 = NFRAME_SYM * nss;
            ntstep = nframe6 / 4;
            ntgrid = ntstep / 12;
            csync.assign(static_cast<std::size_t>(NSYNC_SYM) * nss, cfloat(0.0f, 0.0f));
            ctones.assign(static_cast<std::size_t>(4) * nss, cfloat(0.0f, 0.0f));
            c0.assign(nana, cfloat(0.0f, 0.0f));
            c.assign(nfft, cfloat(0.0f, 0.0f));
            c1.assign(nchunk6, cfloat(0.0f, 0.0f));
            s0.assign(static_cast<std::size_t>(nh2 + 1) * (ntgrid + 1), 0.0f);
            mask0.assign(static_cast<std::size_t>(nh2 + 1) * (ntgrid + 1), 0);
            baud = FSAMPLE / static_cast<float>(nss);
            dt = 1.0f / FSAMPLE;
            genSyncWave(csync.data(), nss);
            for (int i = 0; i < 4; ++i) {
                float phi = 0.0f;
                const float dphi = static_cast<float>(i) * TWOPI / static_cast<float>(nss);
                for (int j = 0; j < nss; ++j) {
                    ctones[static_cast<std::size_t>(i) * nss + j] = cfloat(cosf(phi), sinf(phi));
                    phi = phi + dphi;
                }
            }
        }
        correlator.prepare(nss);

        // 12 kHz integer samples -> complex analytic signal at 6 kHz
        ana64a(iwave, nchunk, c0.data(), nana);
        for (int i = nchunk6; i < nana; ++i) c0[i] = cfloat(0.0f, 0.0f);

        if (useInterferer) {
            // Retro re-sweep call: subtract the already-known signal before searching.
            int tonesFull[NFRAME_SYM];
            for (int i = 0; i < NSYNC_SYM; ++i) tonesFull[i] = SYNC_TONES[i];
            tbccEncode(useInterfererPayload, tonesChk);
            for (int i = 0; i < NCHAN_SYM; ++i) tonesFull[NSYNC_SYM + i] = tonesChk[i];
            subtractJtty(c0.data(), nana, nchunk6, tonesFull, NFRAME_SYM, nss, useInterfererF1,
                         static_cast<float>(useInterfererTsync - sampleTime(istart)));
        }

        // Up to 2 sync candidates per quarter-frame by 2*ftol rectangle.
        nfz = static_cast<int>(std::lround(10.0f / df2));              // 14
        ntz = static_cast<int>(std::lround(0.016f * 6000.0f / 12.0f)); // 8

        nchan = coverBand ? std::max(1, (nfb - nfa + 299) / 300) : 2;
        firstSyncBin = nh2 - 2;
        lastSyncBin = 3;
        for (ichan = 0; ichan <= nchan; ++ichan) {
            channelWindow();
            if (!usable) continue;
            firstSyncBin = std::min(firstSyncBin, ja);
            lastSyncBin = std::max(lastSyncBin, jb);
        }

        nc = 2;
        ncand = 0;
        anySubtracted = false;
        s0Valid = false;

        // Phase A: channel 0 gets first claim on every signal.
        nCh0Ok = 0;
        for (ipass = 1; ipass <= 2; ++ipass) {
            if (ipass == 2 && !anySubtracted) break;
            if (!s0Valid) buildS0();
            ichan = 0;
            processChannel();
        }
        // Phase B: channels 1/2 on the shared surface.
        anySubtracted = false;
        for (ipass = 1; ipass <= 2; ++ipass) {
            if (ipass == 2 && !anySubtracted) break;
            if (!s0Valid) buildS0();
            for (ichan = 1; ichan <= nchan; ++ichan) processChannel();
            s0Valid = false;
        }
    }
};

/******************************************************************************/
// Receiver: the receive-context API
/******************************************************************************/

Receiver::Receiver() : impl_(new Impl) {}
Receiver::~Receiver() = default;

bool Receiver::running() const { return impl_->running; }

bool Receiver::begin(std::int64_t sessionId, std::int64_t gridOrigin, std::int64_t firstSearch, int nsps) {
    Impl &s = *impl_;
    end(UPDATE_RECEPTION_ENDED);
    if (nsps != 240 && nsps != 320 && nsps != 384 && nsps != 480) return false;
    const std::int64_t step = static_cast<std::int64_t>(59 * nsps / 4);
    std::int64_t start = std::max(firstSearch, gridOrigin);
    std::int64_t m = (gridOrigin - start) % step;   // Fortran modulo: non-negative for step > 0
    if (m < 0) m += step;
    start += m;
    s.sessionId = sessionId;
    s.gridOrigin = gridOrigin;
    s.firstSearch = start;
    s.nextSearch = start;
    s.nsps = nsps;
    s.nactive = 0;
    s.nrecent = 0;
    s.ndecodes = 0;
    s.retro = Impl::Retro{};
    s.running = true;
    return true;
}

std::int64_t Receiver::nextSearchSample() const { return impl_->nextSearch; }

std::int64_t Receiver::nextRequiredSample() const {
    const Impl &s = *impl_;
    const std::int64_t step = static_cast<std::int64_t>(59 * s.nsps / 4);
    return std::max(s.firstSearch, s.nextSearch - static_cast<std::int64_t>(MAX_RETRO_STEPS) * step);
}

void Receiver::setWideCoverage(bool on) { impl_->coverBand = on; }

int Receiver::process(const std::int16_t *pcm, int count, std::int64_t firstSample, std::int64_t stopSample,
                      int maxSteps, int nfa, int nfb, float f0, float ftol) {
    Impl &s = *impl_;
    if (!s.running || count < 0) return -1;
    if (firstSample > nextRequiredSample()) return -2;
    int performedSteps = 0;
    if (maxSteps <= 0 || count == 0) return 0;
    const int nframe = 59 * s.nsps;
    const int nchunk = nframe + nframe / 4;
    const int step = nframe / 4;
    const std::int64_t limit = std::min(stopSample, firstSample + static_cast<std::int64_t>(count));
    s.sampleOrigin = firstSample;
    s.firstSearchSample = s.firstSearch;
    s.explicitReceiveContext = true;

    while (performedSteps < maxSteps) {
        bool performed = false;
        while (s.retro.active && !performed) {
            const int signal = s.retro.signal;
            const int retroStep = s.retro.step;
            if (signal > s.retro.count) {
                s.retro = Impl::Retro{};
                s.nextSearch += step;
                break;
            }
            s.retro.step = retroStep + 1;
            if (retroStep > MAX_RETRO_STEPS) {
                s.retro.signal = signal + 1;
                s.retro.step = 1;
                continue;
            }
            const std::int64_t search = s.nextSearch - static_cast<std::int64_t>(retroStep) * step;
            if (search < s.firstSearch) continue;
            const int index = static_cast<int>(search - firstSample) + 1;
            if (index < 1 || index + nchunk - 1 > count) {
                performedSteps = -2;
                break;
            }
            s.interfererPending = true;
            s.interfererF1 = s.retro.frequency[signal - 1];
            s.interfererTsync = s.retro.syncTime[signal - 1];
            std::memcpy(s.interfererPayload, s.retro.payload[signal - 1], sizeof s.interfererPayload);
            s.mdecode(index, 0, pcm + (index - 1), nchunk, s.nsps, nfa, nfb, f0, ftol, 4.6f);
            ++performedSteps;
            performed = true;
        }
        if (performedSteps < 0) break;
        if (performed) continue;
        if (s.nextSearch + static_cast<std::int64_t>(nchunk) > limit) break;
        const int index = static_cast<int>(s.nextSearch - firstSample) + 1;
        s.pruneReceiveState(s.sampleTime(index), static_cast<float>(nframe) / 12000.0f);
        s.interfererPending = false;
        s.mdecode(index, 0, pcm + (index - 1), nchunk, s.nsps, nfa, nfb, f0, ftol, 4.6f);
        ++performedSteps;
        if (s.nsubtracted > 0) {
            s.retro.active = true;
            s.retro.count = s.nsubtracted;
            s.retro.signal = 1;
            s.retro.step = 1;
            for (int i = 0; i < s.nsubtracted; ++i) {
                s.retro.frequency[i] = s.subtractedF1[i];
                s.retro.syncTime[i] = s.subtractedTsync[i];
                std::memcpy(s.retro.payload[i], s.subtractedPayload[i], sizeof s.retro.payload[i]);
            }
        } else {
            s.nextSearch += step;
        }
    }
    return performedSteps;
}

void Receiver::end(int reason) {
    Impl &s = *impl_;
    if (!s.running) return;
    int terminal = UPDATE_RECEPTION_ENDED;
    if (reason == UPDATE_EXPIRED) terminal = UPDATE_EXPIRED;
    for (int i = 0; i < s.nactive; ++i) s.queueMessageUpdate(s.active[i], false, terminal);
    s.resetDecodeSearchState();
    s.retro = Impl::Retro{};
    s.running = false;
}

int Receiver::takeUpdates(std::vector<MessageUpdate> &out, int capacity) {
    Impl &s = *impl_;
    out.clear();
    if (capacity <= 0) return 0;
    const int taken = std::min(capacity, s.npending);
    for (int i = 0; i < taken; ++i) {
        const PendingUpdate &u = s.pending[static_cast<std::size_t>(s.pendingFirst + i)];
        MessageUpdate m;
        m.text = displayMessageText(u.decoded);
        m.messageId = u.messageId;
        m.frequency = u.f1;
        m.startTsync = u.startTsync;
        m.latestTsync = u.latestTsync;
        m.terminal = u.terminal;
        m.snr = static_cast<int>(std::lround(u.snr));
        m.spotCall = u.call;
        out.push_back(std::move(m));
    }
    s.pendingFirst += taken;
    s.npending -= taken;
    if (s.npending == 0) {
        s.pendingFirst = 0;
        if (static_cast<int>(s.pending.size()) > MAX_ACTIVE_MESSAGES) s.pending.resize(MAX_ACTIVE_MESSAGES);
    }
    return taken;
}

} // namespace JTTY

#endif // JS8_ENABLE_JTTY
