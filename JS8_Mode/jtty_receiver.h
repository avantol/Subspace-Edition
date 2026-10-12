#pragma once
#ifdef JS8_ENABLE_JTTY
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// JTTY receiver: the multi-signal decode orchestrator and its stepping
// driver, ported from WSJT-X lib/jtty (tag v3.3.0-beta1) jtty_mdecode.f90
// and jtty_receive_context.f90. One Receiver owns everything the reference
// keeps in module state plus the per-context state it swaps in and out, so
// several receivers can coexist without the reference's exchange_state.
//
// Call sequence, as the reference's C API: begin() -> process() as audio
// arrives -> takeUpdates() -> end(). Sample positions are absolute 12 kHz
// sample indices; the receiver searches in quarter-frame steps from the
// grid origin and may revisit up to three earlier steps after it subtracts
// a decoded signal ("retro re-sweep").

namespace JTTY {

enum UpdateTerminal : int {
    UPDATE_GROWING = 0,
    UPDATE_COMPLETE = 1,
    UPDATE_EXPIRED = 2,
    UPDATE_RECEPTION_ENDED = 3
};

struct MessageUpdate {
    std::int64_t messageId = 0;
    float frequency = 0.0f;       // Hz, sync frequency of the latest frame
    double startTsync = 0.0;      // seconds, first frame's sync time
    double latestTsync = 0.0;     // seconds, latest frame's sync time
    int terminal = UPDATE_GROWING;
    int snr = 0;                  // dB, nint of the first frame's SNR
    std::string text;             // display text, 80 characters, blank padded
    // Addition over the reference: the standard callsign carried by the
    // message's first Call8 atom, if any -- what a spot report needs.
    std::string spotCall;
};

class Receiver {
  public:
    Receiver();
    ~Receiver();
    Receiver(const Receiver &) = delete;
    Receiver &operator=(const Receiver &) = delete;

    // jtty_rx_begin. nsps is samples per symbol at 12 kHz: 240, 320, 384
    // or 480. Ends any running reception first. Returns false and stays
    // stopped for any other nsps.
    bool begin(std::int64_t sessionId, std::int64_t gridOrigin, std::int64_t firstSearch,
               int nsps);

    std::int64_t nextSearchSample() const;     // jtty_rx_next_search_sample
    std::int64_t nextRequiredSample() const;   // jtty_rx_next_required_sample

    // jtty_rx_process: pcm[0..count) are samples firstSample..; stopSample
    // bounds the usable range. Runs up to maxSteps decode steps. Returns the
    // number performed, -2 if the buffer does not reach back far enough, or
    // -1 if not running.
    int process(const std::int16_t *pcm, int count, std::int64_t firstSample,
                std::int64_t stopSample, int maxSteps, int nfa, int nfb, float f0, float ftol);

    // jtty_rx_end: queues a terminal update for every active message.
    void end(int reason);

    // jtty_rx_take_updates: moves up to capacity pending updates out.
    int takeUpdates(std::vector<MessageUpdate> &out, int capacity);

    bool running() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace JTTY
#endif // JS8_ENABLE_JTTY
