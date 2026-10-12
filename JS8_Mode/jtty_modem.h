#pragma once
#ifdef JS8_ENABLE_JTTY
#include "jtty_tbcc.h"

#include <complex>
#include <cstdint>
#include <vector>

// JTTY modem and synchronisation layer, ported from WSJT-X lib/jtty (tag
// v3.3.0-beta1): gen_jttywave.f90, gen_syncwave.f90,
// jtty_payload_correlators.f90, jtty_block_pow.f90, jtty_peakup.f90,
// subtract_jtty.f90 and ana64a.f90, plus lib/twkfreq.f90 which they use.
//
// The receiver works on a complex analytic signal at 6000 samples/s:
// 12 kHz int16 audio -> ana64a -> coarse sync scan (the decode driver) ->
// jttyPeakup -> twkfreq to baseband -> PayloadCorrelator -> the FEC in
// jtty_tbcc.h -> re-encode -> subtractJtty for the next pass.
//
// Everything here except the two FFT-based routines (ana64a, subtractJtty)
// is held bit-identical to the reference by tests_jtty/jtty_modem_test.cpp.

namespace JTTY {

constexpr int SYNC_SYMBOLS = 13;
constexpr int FRAME_SYMBOLS = SYNC_SYMBOLS + INFO_BITS;   // 59
constexpr float FSAMPLE6 = 6000.0f;

// The 13-symbol 4-FSK sync sequence (is13 in jtty_fec_mod.f90).
extern const int SYNC_TONES[SYNC_SYMBOLS];

// Gaussian-filtered 4-FSK waveform. Output starts at the first real symbol
// (the reference's leading dummy symbol is skipped), length nwave samples,
// with a short raised-cosine ramp of nint(nsps/8) samples at each end.
// icmplx == 0 writes `wave` (real), otherwise `cwave` (complex, phase
// quantised through a 65536-entry table exactly as the reference).
void genJttyWave(const int *itone, int nsym, int nsps, float bt, float fsample,
                 float f0, std::complex<float> *cwave, float *wave, int icmplx,
                 int nwave);

// Complex sync reference at 6000 S/s: 13 symbols of nss samples each.
void genSyncWave(std::complex<float> *csync, int nss);

// Frequency shift: a[0] Hz, a[1] Hz/(0.5*T), a[2] quadratic term.
void twkfreq(const std::complex<float> *c3, std::complex<float> *c4, int npts,
             float fsample, const float a[3]);

// Per-symbol complex correlations against the four baseband tones.
class PayloadCorrelator {
  public:
    explicit PayloadCorrelator(int samplesPerSymbol = 0);
    void prepare(int samplesPerSymbol);
    int samplesPerSymbol() const { return samplesPerSymbol_; }

    // samples: candidate-centred analytic baseband; payloadStart is 0-based.
    // Fills INFO_BITS symbols (fewer if the buffer runs out; rest zero).
    void correlate(const std::complex<float> *samples, int nsamples,
                   int payloadStart, CorrelationFrame &correlations,
                   CorrelationFrame &halfCorrelations) const;

  private:
    int samplesPerSymbol_ = 0;
    std::vector<float> referenceReal_;        // [sample*4 + tone]
    std::vector<float> referenceImaginary_;
};

// Coherent block detection for the energy-only decoder: cascades the
// single-symbol correlations into block-2 (iblock == 2) or block-4
// (iblock == 4) max-over-consistent-sequences powers.
void jttyBlockPow(const CorrelationFrame &z, int iblock, EnergyFrame &powBlock);

// Fine time/frequency search around a coarse candidate (xdt0, f0): a
// +/-5 x 0.5 Hz grid with a sliding sync correlation, then a linear phase-
// ramp fit across the 13 sync symbols. c1 is scratch of nchunk samples.
void jttyPeakup(const std::complex<float> *c0, std::complex<float> *c1,
                const std::complex<float> *csync, int nchunk, int nss,
                float xdt0, float f0, float &xdt, float &f1, float &snr);

// 12 kHz int16 audio -> complex analytic signal at 6 kHz in c0[0..nfft1/2).
// nfft1 is a power of two >= npts; c0 must hold nfft1 samples.
void ana64a(const std::int16_t *iwave, int npts, std::complex<float> *c0,
            int nfft1);

// Subtract an already-decoded frame (sync + channel tones, nsymTotal = 59)
// from the analytic buffer so a weaker overlapping signal can be found.
// n is the buffer size (power of two), nvalid the valid sample count.
void subtractJtty(std::complex<float> *c0, int n, int nvalid,
                  const int *toneSymbols, int nsymTotal, int nss, float f1,
                  float xdt);

} // namespace JTTY
#endif // JS8_ENABLE_JTTY
