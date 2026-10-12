/**
 * @file jtty_modem.cpp
 * @brief JTTY modem and synchronisation layer.
 *
 * Direct port of WSJT-X lib/jtty (tag v3.3.0-beta1):
 *   gen_jttywave.f90            -> genJttyWave
 *   gen_syncwave.f90            -> genSyncWave
 *   lib/twkfreq.f90             -> twkfreq
 *   jtty_payload_correlators.f90-> PayloadCorrelator
 *   jtty_block_pow.f90          -> jttyBlockPow
 *   jtty_peakup.f90             -> jttyPeakup
 *   ana64a.f90                  -> ana64a
 *   subtract_jtty.f90           -> subtractJtty
 *
 * Arithmetic is written out in the reference's evaluation order and
 * precision (real32 throughout, complex products as the naive four-multiply
 * formula, sequential sums) so tests_jtty/jtty_modem_test.cpp can hold every
 * non-FFT routine bit-identical. The two FFT-based routines use FFTW3 with
 * FFTW_ESTIMATE as the reference's four2a does, cached per (size, direction,
 * buffer) exactly as four2a caches, and are gated to a tolerance.
 */

#ifdef JS8_ENABLE_JTTY

#include "jtty_modem.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

// Defined by the application (declared in JS8_Include/commons.h); the
// standalone tests define their own.
extern std::mutex fftw_mutex;

namespace JTTY {

const int SYNC_TONES[SYNC_SYMBOLS] = {0, 2, 2, 3, 0, 0, 3, 2, 1, 3, 1, 2, 0};

namespace {

using cfloat = std::complex<float>;

// Complex products in the naive form gfortran emits (-fcx-fortran-rules),
// never through operator* whose library implementation may differ.
inline cfloat cmul(cfloat a, cfloat b) {
    return cfloat(a.real() * b.real() - a.imag() * b.imag(),
                  a.real() * b.imag() + a.imag() * b.real());
}
// conjg(a)*b
inline cfloat conjMul(cfloat a, cfloat b) {
    return cfloat(a.real() * b.real() + a.imag() * b.imag(),
                  a.real() * b.imag() - a.imag() * b.real());
}
// a*conjg(b)
inline cfloat mulConj(cfloat a, cfloat b) {
    return cfloat(a.real() * b.real() + a.imag() * b.imag(),
                  a.imag() * b.real() - a.real() * b.imag());
}
// real(z*conjg(z))
inline float power(cfloat z) { return z.real() * z.real() + z.imag() * z.imag(); }

// gfsk_pulse.f90, every step in real32 as the reference has it.
float gfskPulse(float b, float t) {
    static const float pi = 4.0f * atanf(1.0f);
    static const float c = pi * sqrtf(2.0f / logf(2.0f));
    return 0.5f * (erff(c * b * (t + 0.5f)) - erff(c * b * (t - 0.5f)));
}

// four2a's in-place complex FFT: plans cached by (size, direction, buffer),
// FFTW_ESTIMATE, planning serialised on the application's FFTW mutex.
struct PlanEntry {
    int n;
    int sign;
    const void *ptr;
    fftwf_plan plan;
};

void four2aC2C(cfloat *a, int nfft, int isign) {
    static std::vector<PlanEntry> cache;
    static std::mutex cacheMutex;
    fftwf_plan plan = nullptr;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (const PlanEntry &e : cache)
            if (e.n == nfft && e.sign == isign && e.ptr == a) {
                plan = e.plan;
                break;
            }
        if (!plan) {
            std::lock_guard<std::mutex> flock(fftw_mutex);
            plan = fftwf_plan_dft_1d(nfft, reinterpret_cast<fftwf_complex *>(a),
                                     reinterpret_cast<fftwf_complex *>(a), isign, FFTW_ESTIMATE);
            cache.push_back({nfft, isign, a, plan});
        }
    }
    fftwf_execute(plan);
}

} // namespace

/******************************************************************************/
// Waveforms
/******************************************************************************/

void genJttyWave(const int *itone, int nsym, int nsps, float bt, float fsample, float f0,
                 cfloat *cwave, float *wave, int icmplx, int nwave) {
    static constexpr int NTAB = 65536;
    static std::vector<float> pulse;
    static int pulseNsps = -1;
    static float pulseBt = -1.0f;
    static std::vector<cfloat> ctab;
    static std::mutex mutex;

    if (nsym <= 0 || nwave <= 0) return;
    std::lock_guard<std::mutex> lock(mutex);

    const float twopi = 8.0f * atanf(1.0f);
    const float dt = 1.0f / fsample;
    const float hmod = 1.0f;

    if (nsps != pulseNsps || bt != pulseBt) {
        pulse.assign(static_cast<std::size_t>(3) * nsps, 0.0f);
        for (int i = 1; i <= 3 * nsps; ++i) {
            float tt = (static_cast<float>(i) - 1.5f * static_cast<float>(nsps)) /
                       static_cast<float>(nsps);
            pulse[i - 1] = gfskPulse(bt, tt);
        }
        pulseNsps = nsps;
        pulseBt = bt;
    }
    if (ctab.empty()) {
        ctab.resize(NTAB);
        for (int i = 0; i < NTAB; ++i) {
            float phi = static_cast<float>(i) * twopi / static_cast<float>(NTAB);
            ctab[i] = cfloat(cosf(phi), sinf(phi));
        }
    }

    // Smoothed frequency waveform, (nsym+2)*nsps long: first and last
    // symbols extended by a dummy symbol on each side.
    const int ntot = (nsym + 2) * nsps;
    std::vector<float> dphi(ntot, 0.0f);
    const float dphiPeak = twopi * hmod / static_cast<float>(nsps);
    for (int j = 1; j <= nsym; ++j) {
        const int ib = (j - 1) * nsps;
        const float tone = static_cast<float>(itone[j - 1]);
        for (int k = 0; k < 3 * nsps; ++k) dphi[ib + k] += dphiPeak * pulse[k] * tone;
    }
    {
        const float s = dphiPeak * static_cast<float>(itone[0]);
        for (int k = 0; k < 2 * nsps; ++k) dphi[k] += s * pulse[nsps + k];
        const float s2 = dphiPeak * static_cast<float>(itone[nsym - 1]);
        for (int k = 0; k < 2 * nsps; ++k) dphi[nsym * nsps + k] += s2 * pulse[k];
    }
    const float shift = twopi * f0 * dt;
    for (float &d : dphi) d += shift;

    if (icmplx == 0)
        std::fill(wave, wave + nwave, 0.0f);
    else
        std::fill(cwave, cwave + nwave, cfloat(0.0f, 0.0f));

    // Output from the first real symbol; the leading dummy symbol is skipped.
    float phi = 0.0f;
    int k = 0;
    for (int j = nsps; j < nsps + nwave && j < ntot; ++j) {
        if (icmplx == 0) {
            wave[k] = sinf(phi);
        } else {
            int i = std::min(NTAB - 1,
                             static_cast<int>(phi * static_cast<float>(NTAB) / twopi));
            cwave[k] = ctab[i];
        }
        ++k;
        phi = fmodf(phi + dphi[j], twopi);
    }

    // Envelope shaping over the first and last nint(nsps/8) samples.
    const int nramp = static_cast<int>(std::lround(static_cast<float>(nsps) / 8.0f));
    for (int i = 0; i < nramp && i < nwave; ++i) {
        float env = (1.0f - cosf(twopi * static_cast<float>(i) /
                                 (2.0f * static_cast<float>(nramp)))) / 2.0f;
        if (icmplx == 0)
            wave[i] = wave[i] * env;
        else
            cwave[i] = cfloat(cwave[i].real() * env, cwave[i].imag() * env);
    }
    const int k1 = nsym * nsps - nramp;
    for (int i = 0; i < nramp; ++i) {
        const int idx = k1 + i;
        if (idx < 0 || idx >= nwave) continue;
        float env = (1.0f + cosf(twopi * static_cast<float>(i) /
                                 (2.0f * static_cast<float>(nramp)))) / 2.0f;
        if (icmplx == 0)
            wave[idx] = wave[idx] * env;
        else
            cwave[idx] = cfloat(cwave[idx].real() * env, cwave[idx].imag() * env);
    }
}

void genSyncWave(cfloat *csync, int nss) {
    const float twopi = 8.0f * atanf(1.0f);
    const float fsample = 6000.0f;
    const float dt = 1.0f / fsample;
    const float baud = fsample / static_cast<float>(nss);
    std::fill(csync, csync + SYNC_SYMBOLS * nss, cfloat(0.0f, 0.0f));
    int k = 0;
    float phi = 0.0f;
    for (int i = 0; i < SYNC_SYMBOLS; ++i) {
        const float dphi = twopi * baud * static_cast<float>(SYNC_TONES[i]) * dt;
        for (int j = 0; j < nss; ++j) {
            csync[k++] = cfloat(cosf(phi), sinf(phi));
            phi = phi + dphi;
        }
    }
}

/******************************************************************************/
// Frequency shift
/******************************************************************************/

void twkfreq(const cfloat *c3, cfloat *c4, int npts, float fsample, const float a[3]) {
    const float twopi = 6.283185307f;
    cfloat w(1.0f, 0.0f);
    if (a[1] == 0.0f && a[2] == 0.0f) {
        const float dphi = a[0] * (twopi / fsample);
        const cfloat wstep(cosf(dphi), sinf(dphi));
        for (int i = 0; i < npts; ++i) {
            w = cmul(w, wstep);
            c4[i] = cmul(w, c3[i]);
        }
        return;
    }
    const float x0 = 0.5f * static_cast<float>(npts + 1);
    const float s = 2.0f / static_cast<float>(npts);
    for (int i = 1; i <= npts; ++i) {
        const float x = s * (static_cast<float>(i) - x0);
        const float p2 = 1.5f * x * x - 0.5f;
        const float dphi = (a[0] + x * a[1] + p2 * a[2]) * (twopi / fsample);
        const cfloat wstep(cosf(dphi), sinf(dphi));
        w = cmul(w, wstep);
        c4[i - 1] = cmul(w, c3[i - 1]);
    }
}

/******************************************************************************/
// Payload correlators
/******************************************************************************/

PayloadCorrelator::PayloadCorrelator(int samplesPerSymbol) {
    if (samplesPerSymbol > 0) prepare(samplesPerSymbol);
}

void PayloadCorrelator::prepare(int samplesPerSymbol) {
    if (samplesPerSymbol <= 0 || samplesPerSymbol % 2 != 0)
        throw std::invalid_argument("invalid JTTY payload symbol length");
    if (samplesPerSymbol_ == samplesPerSymbol && !referenceReal_.empty()) return;
    samplesPerSymbol_ = samplesPerSymbol;
    referenceReal_.assign(static_cast<std::size_t>(samplesPerSymbol) * 4, 0.0f);
    referenceImaginary_.assign(static_cast<std::size_t>(samplesPerSymbol) * 4, 0.0f);
    const float TWO_PI = 6.2831853071795864769f;
    for (int tone = 0; tone < 4; ++tone) {
        float phase = 0.0f;
        const float phaseStep =
            TWO_PI * static_cast<float>(tone) / static_cast<float>(samplesPerSymbol);
        for (int sample = 0; sample < samplesPerSymbol; ++sample) {
            referenceReal_[static_cast<std::size_t>(sample) * 4 + tone] = cosf(phase);
            referenceImaginary_[static_cast<std::size_t>(sample) * 4 + tone] = sinf(phase);
            phase = phase + phaseStep;
        }
    }
}

void PayloadCorrelator::correlate(const cfloat *samples, int nsamples, int payloadStart,
                                  CorrelationFrame &correlations,
                                  CorrelationFrame &halfCorrelations) const {
    if (referenceReal_.empty() || samplesPerSymbol_ <= 0)
        throw std::logic_error("unprepared JTTY payload correlator");
    if (payloadStart < 0) throw std::invalid_argument("invalid JTTY payload offset");
    for (int s = 0; s < INFO_BITS; ++s)
        for (int t = 0; t < 4; ++t) {
            correlations.c[s][t] = cfloat(0.0f, 0.0f);
            halfCorrelations.c[s][t] = cfloat(0.0f, 0.0f);
        }
    if (payloadStart >= nsamples) return;
    const int nss = samplesPerSymbol_;
    const int availableSymbols = std::min(INFO_BITS, (nsamples - payloadStart) / nss);
    const int halfLength = nss / 2;

    for (int symbol = 1; symbol <= availableSymbols; ++symbol) {
        const int first = payloadStart + (symbol - 1) * nss;
        float fullReal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float fullImaginary[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        double energy[4] = {0.0, 0.0, 0.0, 0.0};
        for (int segment = 0; segment < 2; ++segment) {
            float halfReal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float halfImaginary[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int offset = segment * halfLength; offset < (segment + 1) * halfLength; ++offset) {
                const float sampleReal = samples[first + offset].real();
                const float sampleImaginary = samples[first + offset].imag();
                for (int tone = 0; tone < 4; ++tone) {
                    const std::size_t idx = static_cast<std::size_t>(offset) * 4 + tone;
                    const float productRr = referenceReal_[idx] * sampleReal;
                    const float productIi = referenceImaginary_[idx] * sampleImaginary;
                    const float productRi = referenceReal_[idx] * sampleImaginary;
                    const float productIr = referenceImaginary_[idx] * sampleReal;
                    // Preserve each observation's accumulation order while sharing products.
                    fullReal[tone] = fullReal[tone] + productRr + productIi;
                    fullImaginary[tone] = fullImaginary[tone] + productRi - productIr;
                    halfReal[tone] = halfReal[tone] + productRr + productIi;
                    halfImaginary[tone] = halfImaginary[tone] + productRi - productIr;
                }
            }
            for (int tone = 0; tone < 4; ++tone)
                energy[tone] = energy[tone] +
                               static_cast<double>(halfReal[tone] * halfReal[tone] +
                                                   halfImaginary[tone] * halfImaginary[tone]);
        }
        for (int tone = 0; tone < 4; ++tone) {
            correlations.c[symbol - 1][tone] = cfloat(fullReal[tone], fullImaginary[tone]);
            halfCorrelations.c[symbol - 1][tone] =
                cfloat(static_cast<float>(std::sqrt(std::max(0.0, energy[tone]))), 0.0f);
        }
    }
}

/******************************************************************************/
// Coherent block power (energy-only decoder input)
/******************************************************************************/

void jttyBlockPow(const CorrelationFrame &z, int iblock, EnergyFrame &powBlock) {
    const int nsym = INFO_BITS;
    const int npair = nsym / 2;
    // c2(ta, tb, ip): pair sums, 1-based ip
    std::vector<cfloat> c2(static_cast<std::size_t>(16) * npair);
    auto C2 = [&](int ta, int tb, int ip) -> cfloat & {
        return c2[(static_cast<std::size_t>(ip - 1) * 4 + tb) * 4 + ta];
    };

    for (int ip = 1; ip <= npair; ++ip) {
        const int posA = 2 * ip - 1, posB = 2 * ip;
        for (int ta = 0; ta < 4; ++ta)
            for (int tb = 0; tb < 4; ++tb) C2(ta, tb, ip) = z.c[posA - 1][ta] + z.c[posB - 1][tb];
    }

    // L=2 reduction, always computed: used directly for iblock==2, and as the
    // trailing-remainder fallback for iblock==4.
    for (int ip = 1; ip <= npair; ++ip) {
        const int posA = 2 * ip - 1, posB = 2 * ip;
        for (int ta = 0; ta < 4; ++ta) {
            float p2 = power(C2(ta, 0, ip));
            p2 = std::max(p2, power(C2(ta, 1, ip)));
            p2 = std::max(p2, power(C2(ta, 2, ip)));
            p2 = std::max(p2, power(C2(ta, 3, ip)));
            powBlock.e[posA - 1][ta] = p2 / 2.0f;
        }
        for (int tb = 0; tb < 4; ++tb) {
            float p2 = power(C2(0, tb, ip));
            p2 = std::max(p2, power(C2(1, tb, ip)));
            p2 = std::max(p2, power(C2(2, tb, ip)));
            p2 = std::max(p2, power(C2(3, tb, ip)));
            powBlock.e[posB - 1][tb] = p2 / 2.0f;
        }
    }

    if (iblock == 4) {
        const int nquad = npair / 2;
        for (int iq = 1; iq <= nquad; ++iq) {
            const int ipL = 2 * iq - 1, ipR = 2 * iq;
            const int pos1 = 2 * ipL - 1, pos2 = 2 * ipL, pos3 = 2 * ipR - 1, pos4 = 2 * ipR;
            for (int t = 0; t < 4; ++t) {
                powBlock.e[pos1 - 1][t] = 0.0f;
                powBlock.e[pos2 - 1][t] = 0.0f;
                powBlock.e[pos3 - 1][t] = 0.0f;
                powBlock.e[pos4 - 1][t] = 0.0f;
            }
            for (int ta = 0; ta < 4; ++ta)
                for (int tb = 0; tb < 4; ++tb)
                    for (int tc = 0; tc < 4; ++tc)
                        for (int td = 0; td < 4; ++td) {
                            const cfloat s4 = C2(ta, tb, ipL) + C2(tc, td, ipR);
                            const float p2 = power(s4) / 4.0f;
                            powBlock.e[pos1 - 1][ta] = std::max(powBlock.e[pos1 - 1][ta], p2);
                            powBlock.e[pos2 - 1][tb] = std::max(powBlock.e[pos2 - 1][tb], p2);
                            powBlock.e[pos3 - 1][tc] = std::max(powBlock.e[pos3 - 1][tc], p2);
                            powBlock.e[pos4 - 1][td] = std::max(powBlock.e[pos4 - 1][td], p2);
                        }
        }
    }
}

/******************************************************************************/
// Fine sync search
/******************************************************************************/

void jttyPeakup(const cfloat *c0, cfloat *c1, const cfloat *csync, int nchunk, int nss,
                float xdt0, float f0, float &xdt, float &f1, float &snr) {
    constexpr int hop = 4;
    const float TWOPI = 6.283185307179586f;
    const float PI = 3.141592653589793f;
    const int npsync = SYNC_SYMBOLS * nss;
    std::vector<cfloat> c(npsync);
    cfloat zcur[SYNC_SYMBOLS], zbest[SYNC_SYMBOLS], qstep[SYNC_SYMBOLS];
    float a[3];

    for (int i = 0; i < SYNC_SYMBOLS; ++i) {
        const int istart = i * nss;
        qstep[i] = mulConj(csync[istart + hop], csync[istart]);
    }
    const float fsample = 6000.0f;
    const float dt = 1.0f / fsample;
    const int ia = std::max(0, static_cast<int>(std::lround((xdt0 - 0.004f) / dt)));
    const int ib = std::min(nchunk - npsync, static_cast<int>(std::lround((xdt0 + 0.004f) / dt)));

    float pmax = 0.0f, fpk = 0.0f, xdtpk = 0.0f;
    std::fill(c1, c1 + nchunk, cfloat(0.0f, 0.0f));
    for (int i = 0; i < SYNC_SYMBOLS; ++i) zcur[i] = zbest[i] = cfloat(0.0f, 0.0f);

    for (int idf = -5; idf <= 5; ++idf) {
        a[0] = -f0 + 0.5f * static_cast<float>(idf);   // shift assumed peak to zero frequency
        a[1] = 0.0f;
        a[2] = 0.0f;
        if (ib + npsync > 0) twkfreq(c0, c1, ib + npsync, fsample, a);
        if (ia <= ib) {
            for (int n = 0; n < npsync; ++n) c[n] = conjMul(csync[n], c1[ia + n]);
            for (int i = 0; i < SYNC_SYMBOLS; ++i) {
                const int istart = i * nss;
                cfloat sum(0.0f, 0.0f);
                for (int n = istart; n < istart + nss; ++n) sum = sum + c[n];
                zcur[i] = sum;
            }
        }
        for (int i0 = ia; i0 <= ib; i0 += hop) {   // search over xdt for sync pattern
            xdt = static_cast<float>(i0) * dt;
            if (ia <= ib) {
                // Coherent only within a symbol here; this locates the sync
                // instant. Stage 2 below refines f1 across all 13 symbols.
                float p = 0.0f;
                for (int i = 0; i < SYNC_SYMBOLS; ++i)
                    p = p + zcur[i].real() * zcur[i].real() + zcur[i].imag() * zcur[i].imag();
                if (p > pmax) {
                    pmax = p;
                    fpk = -a[0];
                    xdtpk = static_cast<float>(i0) * dt;
                    for (int i = 0; i < SYNC_SYMBOLS; ++i) zbest[i] = zcur[i];
                }
                if (i0 + hop <= ib) {
                    for (int i = 0; i < SYNC_SYMBOLS; ++i) {
                        const int istart = i * nss;
                        cfloat z(0.0f, 0.0f);
                        for (int r = 0; r < hop; ++r)
                            z = z + conjMul(csync[istart + r], c1[i0 + istart + r]);
                        zcur[i] = cmul(qstep[i], zcur[i] - z);
                        z = cfloat(0.0f, 0.0f);
                        for (int r = 0; r < hop; ++r)
                            z = z + conjMul(csync[istart + nss - hop + r], c1[i0 + istart + nss + r]);
                        zcur[i] = zcur[i] + z;
                    }
                }
            }
        }
    }

    // Stage 2: fit and remove the linear phase ramp across the 13 sync
    // symbols, then combine them coherently for a sharper f1.
    if (pmax > 0.0f) {
        const float tsym = static_cast<float>(nss) / fsample;
        float phase[SYNC_SYMBOLS], uw[SYNC_SYMBOLS];
        for (int i = 0; i < SYNC_SYMBOLS; ++i) phase[i] = atan2f(zbest[i].imag(), zbest[i].real());
        uw[0] = phase[0];
        for (int i = 1; i < SYNC_SYMBOLS; ++i) {
            float dphi = phase[i] - phase[i - 1];
            while (dphi > PI) dphi = dphi - TWOPI;
            while (dphi < -PI) dphi = dphi + TWOPI;
            uw[i] = uw[i - 1] + dphi;
        }
        const float xm = 6.0f;
        float ym = 0.0f;
        for (int i = 0; i < SYNC_SYMBOLS; ++i) ym = ym + uw[i];
        ym = ym / 13.0f;
        float sxy = 0.0f, sxx = 0.0f;
        for (int i = 0; i < SYNC_SYMBOLS; ++i) {
            const float di = static_cast<float>(i) - xm;
            sxy = sxy + di * (uw[i] - ym);
            sxx = sxx + di * di;
        }
        const float slope = sxy / sxx;
        const float intercept = ym - slope * xm;
        float residRms = 0.0f;
        for (int i = 0; i < SYNC_SYMBOLS; ++i) {
            const float resid = uw[i] - (slope * static_cast<float>(i) + intercept);
            residRms = residRms + resid * resid;
        }
        residRms = sqrtf(residRms / 13.0f);
        const float dfhz = slope / (TWOPI * tsym);

        // Only trust the refinement when the fit is clean and the correction
        // stays within the coarse grid's own 0.5 Hz step.
        if (residRms < 1.0f && fabsf(dfhz) <= 0.5f) {
            cfloat ztot(0.0f, 0.0f);
            for (int i = 0; i < SYNC_SYMBOLS; ++i) {
                const float ang = -(slope * static_cast<float>(i));
                ztot = ztot + cmul(zbest[i], cfloat(cosf(ang), sinf(ang)));
            }
            fpk = fpk + dfhz;
            pmax = ztot.real() * ztot.real() + ztot.imag() * ztot.imag();
        }
    }

    f1 = fpk;
    xdt = xdtpk;
    snr = pmax;
}

/******************************************************************************/
// Analytic signal and subtraction (FFT based)
/******************************************************************************/

void ana64a(const std::int16_t *iwave, int npts, cfloat *c0, int nfft1) {
    const int nfft2 = nfft1 / 2;
    const float fac = 2.0f / (32767.0f * static_cast<float>(nfft1));
    for (int i = 0; i < npts; ++i) c0[i] = cfloat(fac * static_cast<float>(iwave[i]), 0.0f);
    for (int i = npts; i < nfft1; ++i) c0[i] = cfloat(0.0f, 0.0f);
    four2aC2C(c0, nfft1, -1);                                   // forward c2c
    for (int i = nfft2 / 2 + 1; i < nfft2; ++i) c0[i] = cfloat(0.0f, 0.0f);   // negative freqs
    c0[0] = cfloat(0.5f * c0[0].real(), 0.5f * c0[0].imag());
    four2aC2C(c0, nfft2, 1);                                    // inverse c2c, half length
    for (int i = npts; i < nfft1; ++i) c0[i] = cfloat(0.0f, 0.0f);
}

void subtractJtty(cfloat *c0, int n, int nvalid, const int *toneSymbols, int nsymTotal,
                  int nss, float f1, float xdt) {
    static std::mutex mutex;
    static int nframeSave = -1, nfftSave = -1;
    static std::vector<cfloat> cref, camp, cfilt, cw;
    std::lock_guard<std::mutex> lock(mutex);

    const float FSAMPLE = 6000.0f;
    const int nframe = nsymTotal * nss;
    const int nfft = n;
    const int nfilt = 2 * nss;   // ~2 symbol periods, as subtractft8's NFILT

    if (nframe != nframeSave || nfft != nfftSave) {
        cref.assign(nframe, cfloat(0.0f, 0.0f));
        camp.assign(nfft, cfloat(0.0f, 0.0f));
        cfilt.assign(nfft, cfloat(0.0f, 0.0f));
        cw.assign(nfft, cfloat(0.0f, 0.0f));
        const float pi = 4.0f * atanf(1.0f);
        const float fac = 1.0f / static_cast<float>(nfft);
        std::vector<float> window(nfilt + 1);
        float sumw = 0.0f;
        for (int j = -nfilt / 2; j <= nfilt / 2; ++j) {
            float w = cosf(pi * static_cast<float>(j) / static_cast<float>(nfilt));
            w = w * w;
            window[j + nfilt / 2] = w;
            sumw = sumw + w;
        }
        // cw=0; cw(1:nfilt+1)=window/sumw; cw=cshift(cw,nfilt/2+1)
        std::vector<cfloat> tmp(nfft, cfloat(0.0f, 0.0f));
        for (int k = 0; k <= nfilt; ++k) tmp[k] = cfloat(window[k] / sumw, 0.0f);
        const int shift = nfilt / 2 + 1;
        for (int k = 0; k < nfft; ++k) cw[k] = tmp[(k + shift) % nfft];
        four2aC2C(cw.data(), nfft, -1);
        for (cfloat &v : cw) v = cfloat(v.real() * fac, v.imag() * fac);
        nframeSave = nframe;
        nfftSave = nfft;
    }

    // Unit-amplitude reference for the full frame at this signal's frequency.
    genJttyWave(toneSymbols, nsymTotal, nss, 2.0f, FSAMPLE, f1, cref.data(), nullptr, 1, nframe);

    const int nstart = static_cast<int>(std::lround(xdt * FSAMPLE));   // first sample of the frame
    std::fill(camp.begin(), camp.end(), cfloat(0.0f, 0.0f));
    for (int i = 1; i <= nframe; ++i) {
        const int j = nstart + i - 1;
        if (j >= 0 && j < nvalid) camp[i - 1] = mulConj(c0[j], cref[i - 1]);
    }

    cfilt = camp;
    four2aC2C(cfilt.data(), nfft, -1);                  // forward FFT
    for (int k = 0; k < nfft; ++k) cfilt[k] = cmul(cfilt[k], cw[k]);   // low-pass
    four2aC2C(cfilt.data(), nfft, 1);                   // inverse FFT -> smoothed complex gain

    for (int i = 1; i <= nframe; ++i) {
        const int j = nstart + i - 1;
        if (j >= 0 && j < nvalid) {
            const cfloat z = cmul(cfilt[i - 1], cref[i - 1]);
            c0[j] = c0[j] - z;
        }
    }
}

} // namespace JTTY

#endif // JS8_ENABLE_JTTY
