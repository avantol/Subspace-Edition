#ifndef __JS8
#define __JS8

#include <QObject>
#include <QSemaphore>
#include <QThread>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>

namespace JS8 {
Q_NAMESPACE

namespace Costas {
// JS8 originally used the same Costas arrays as FT8 did, and so
// that's still the array in use by 'normal' mode. All the other
// modes use the modified arrays.

enum class Type { ORIGINAL, MODIFIED };

using Array = std::array<std::array<int, 7>, 3>;

constexpr auto array = [] {
    constexpr auto COSTAS =
        std::array{std::array{std::array{4, 2, 5, 6, 1, 3, 0},
                              std::array{4, 2, 5, 6, 1, 3, 0},
                              std::array{4, 2, 5, 6, 1, 3, 0}},
                   std::array{std::array{0, 6, 2, 3, 5, 4, 1},
                              std::array{1, 5, 0, 2, 3, 6, 4},
                              std::array{2, 5, 0, 6, 4, 1, 3}}};

    return [COSTAS](Type type) -> Array const & {
        return COSTAS[static_cast<std::underlying_type_t<Type>>(type)];
    };
}();
} // namespace Costas

void encode(int type, Costas::Array const &costas, const char *message,
            int *tones);

namespace Event {
struct DecodeStarted {
    int submodes;
};

struct SyncStart {
    int position;
    int size;
};

struct SyncState {
    enum class Type { CANDIDATE, DECODED } type;
    int mode;
    float frequency;
    float dt;
    union {
        int candidate;
        float decoded;
    } sync;
};

struct Decoded {
    int utc; // you can use the output of code_time() from commons.h here.
    int snr;
    float xdt;
    float frequency;
    std::string data;
    int type;
    float quality;
    int mode;
    bool l2 = false;
    // [BUILD 294] Absolute global sample-buffer position where this
    // frame's Costas was found. Computed at the L2 callback site as
    // (snapshot_base_pos + ibest). Invariant across decode passes —
    // same physical frame produces the same absPos regardless of
    // which sliding-window pass found it. Sort key for ordering
    // and dedup key for sliding-window double-decodes in
    // processBufferedActivity. Default 0 means "not computed"
    // (standard period-aligned decoder leaves it unset; absPos=0
    // entries sort to the front in their relative arrival order
    // via stable_sort, which matches the standard-decoder's once-
    // per-period firing pattern).
    std::int64_t absPos = 0;
    // [dialstamp 2026-09-11] OUR dial frequency, in Hz, at the moment
    // the audio this frame came from was SNAPSHOTTED for decoding.
    // Set by the Subspace async pass beside absPos (same precedent:
    // per-snapshot truth stamped at the emitter). 0 = not stamped,
    // and the consumer falls back to the normal decoder's cycle-start
    // capture (m_decoderBusyFreq).
    //
    // Why it exists: the normal decoder captures its dial once per
    // cycle start; Subspace decodes arrive continuously through the
    // same handler and were stamped with THAT dial -- up to a full
    // period stale (30 s in Slow). Retune mid-cycle and every
    // Subspace decode in flight got dial + offset wrong by the amount
    // moved, into ALL.TXT, the API FREQ field, our PSKR spot report
    // and (since #218) the spots-map frequency store, where it became
    // visible as stations judged out-of-passband at the very dial we
    // were decoding them on. A remote station's own QSY is irrelevant:
    // a frame is received at whatever offset it lands on, and offset
    // plus OUR dial at capture is its true frequency.
    std::int64_t dial = 0;
};

struct DecodeFinished {
    std::size_t decoded;
};

using Variant =
    std::variant<DecodeStarted, SyncStart, SyncState, Decoded, DecodeFinished>;

using Emitter = std::function<void(Variant const &)>;
} // namespace Event

class Worker;

class Decoder : public QObject {
    Q_OBJECT

    QSemaphore m_semaphore;
    QThread m_thread;
    Worker *m_worker;

  public:
    Decoder(QObject *parent = nullptr);

  signals:

    void decodeEvent(Event::Variant const &);

  public slots:

    void start(QThread::Priority priority);
    void quit();
    void decode();
};
} // namespace JS8

#endif
