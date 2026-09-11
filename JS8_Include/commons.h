#ifndef COMMONS_H
#define COMMONS_H

#include <cstdint>
#include <mutex>

// NSPS, the number of samples per second (at a sample rate of 12000
// samples per second) is a constant, chosen so as to be a number
// with no prime factor greater than 7.

#define JS8_NSPS           6192
#define JS8_NSMAX          6827
#define JS8_NTMAX          60
#define JS8_RX_SAMPLE_RATE 12000
#define JS8_RX_SAMPLE_SIZE (JS8_NTMAX * JS8_RX_SAMPLE_RATE)

#define JS8_RING_BUFFER    1       // use a ring buffer instead of clearing the decode frames
#define JS8_DECODE_THREAD  1       // use a separate thread for decode process handling
#define JS8_ALLOW_EXTENDED 1       // allow extended latin-1 capital charset
#define JS8_AUTO_SYNC      1       // enable the experimental auto sync feature

#define JS8_NUM_SYMBOLS    79
#define JS8_ENABLE_JS8A    1
#define JS8_ENABLE_JS8B    1
#define JS8_ENABLE_JS8C    1
#define JS8_ENABLE_JS8E    1
#define JS8_ENABLE_JS8I    0

// FT2 mode constants (different protocol from JS8: 4-GFSK, LDPC(174,91))
#ifdef JS8_ENABLE_FT2
#define FT2_NUM_SYMBOLS    103    // 16 sync + 87 data
#define FT2_NSPS           288    // samples/symbol at 12kHz
#define FT2_NMAX           90000  // 7.5s * 12000 (2 periods, matches Fortran NMAX)
#define FT2_L2_RINGSIZE    90000  // 7.5s ring buffer (2 periods) for L2 async decode
#define FT2_TX_PERIOD_MS   3750   // T/R period in milliseconds (3.75s)
#define FT2_START_DELAY_MS 100
#define FT2_TX_NSPS        1152   // samples/symbol at 48kHz
#define FT2_NWAVE          120960 // (103+2)*1152
#endif

#define JS8A_SYMBOL_SAMPLES 1920
#define JS8A_TX_SECONDS     15
#define JS8A_START_DELAY_MS 500

#define JS8B_SYMBOL_SAMPLES 1200
#define JS8B_TX_SECONDS     10
#define JS8B_START_DELAY_MS 200

#define JS8C_SYMBOL_SAMPLES 600
#define JS8C_TX_SECONDS     6
#define JS8C_START_DELAY_MS 100

#define JS8E_SYMBOL_SAMPLES 3840
#define JS8E_TX_SECONDS     30
#define JS8E_START_DELAY_MS 500

#define JS8I_SYMBOL_SAMPLES 384
#define JS8I_TX_SECONDS     4
#define JS8I_START_DELAY_MS 100

extern struct dec_data
{
  std::int16_t d2[JS8_RX_SAMPLE_SIZE]; // sample frame buffer for sample collection
  struct
  {
    int nutc;                   // UTC as integer. See code_time() below for details.
    int nfqso;                  // User-selected QSO freq (kHz)
    bool newdat;                // true ==> new data, must do long FFT
    int nfa;                    // Low decode limit (Hz) (filter min)
    int nfb;                    // High decode limit (Hz) (filter max)
    bool syncStats;             // only compute sync candidates
    int kin;                    // number of frames written to d2
    int kposA;                  // starting position of decode for submode A
    int kposB;                  // starting position of decode for submode B
    int kposC;                  // starting position of decode for submode C
    int kposE;                  // starting position of decode for submode E
    int kposI;                  // starting position of decode for submode I
    int kszA;                   // number of frames for decode for submode A
    int kszB;                   // number of frames for decode for submode B
    int kszC;                   // number of frames for decode for submode C
    int kszE;                   // number of frames for decode for submode E
    int kszI;                   // number of frames for decode for submode I
    int nsubmodes;              // which submodes to decode
#ifdef JS8_ENABLE_FT2
    int kposFT2;                // starting position of decode for FT2
    int kszFT2;                 // number of frames for decode for FT2
    int kposFT2b;               // overlap decode start (half-cycle offset)
    int kszFT2b;                // overlap decode size (0 = no overlap)
#endif
  } params;
#ifdef JS8_ENABLE_FT2
  std::int16_t ft2_d2[FT2_NMAX]; // FT2 sample buffer (3.75s at 12kHz)
#endif
} dec_data;

extern struct
specData
{
  float savg[JS8_NSMAX];
  float slin[JS8_NSMAX];
}
specData;

extern std::mutex fftw_mutex;

// The way we squeeze a timestamp into an int.
// See also decode_time() below.
inline int code_time(int hour, int minute, int second){
  return hour * 10000 + minute * 100 + second;
}

struct hour_minute_second {
  int hour;
  int minute;
  int second;
};

// Undo code_time().
inline hour_minute_second decode_time(int nutc){
  struct hour_minute_second result;
  result.hour = nutc / 10000;
  result.minute = nutc % 10000 / 100;
  result.second = nutc % 100;
  return result;
}

// [passband #218] THE ONE AUTHORITY for the receive passband width,
// in Hz of audio offset above our dial.
//
// Anything asking "is that station's transmit frequency inside the
// passband we are actually listening to?" measures against this, and
// nothing carries a width of its own. Before this existed the same
// source file held two different hardcoded widths that had drifted
// apart -- 2400 Hz in the spots-map hover line and 2500 Hz in the QSY
// double-click -- which is exactly the situation that cannot be
// allowed once the number starts REJECTING stations.
//
// 2800 Hz by operator ruling 2026-09-10: the @MAGNET group redefined
// it and we accept 2800 for all usages.
//
// BOUNDARIES ARE INCLUSIVE: an offset of 0 and an offset of
// JS8_PASSBAND_WIDTH_HZ both count as inside.
inline constexpr int JS8_PASSBAND_WIDTH_HZ = 2800;

// [passband #218] How long a station's observed transmit frequency
// stays believable, in seconds. 60 minutes by operator ruling
// 2026-09-10, chosen to match the map's own maximum age.
//
// Past this the frequency becomes UNKNOWN -- deliberately NOT "last
// known". Stations QSY, and a stale number would let us reject a
// station we can hear perfectly well. Unknown is exempt everywhere:
// unknown stations stay visible on the map and stay eligible as relay
// candidates. Only a frequency we currently believe AND that falls
// outside the passband causes a rejection.
inline constexpr int JS8_FREQ_STALE_SECS = 60 * 60;

#endif // COMMONS_H
