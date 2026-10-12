/**
 * @file JS8Submode.cpp
 * @brief Implementation of JS8 submode parameter inquiry functions
 */
#include "JS8Submode.h"
#include "JS8_Include/commons.h"
#include "JS8_Main/Varicode.h"

#include <QLoggingCategory>

#include <concepts>

/******************************************************************************/
// Private Implementation
/******************************************************************************/

Q_DECLARE_LOGGING_CATEGORY(js8submode_js8)

namespace JS8::Submode {
namespace {
// std::floor doesn't become constexpr until C++23; until then, our own
// implementation. We only use this for computation of the number of
// frames needed for a submode, so it doesn't need to be complicated.

template <std::floating_point T> constexpr int floor(T const v) {
    auto const i = static_cast<int>(v);
    return v < i ? i - 1 : i;
}

// Ensure that our implementation works as expected.

static_assert(floor(0.0) == 0);
static_assert(floor(0.499999) == 0);
static_assert(floor(0.5) == 0);
static_assert(floor(0.999999) == 0);
static_assert(floor(1.0) == 1);
static_assert(floor(123.0) == 123);
static_assert(floor(123.4) == 123);
static_assert(floor(-0.499999) == -1);
static_assert(floor(-0.5) == -1);
static_assert(floor(-0.999999) == -1);
static_assert(floor(-1.0) == -1);
static_assert(floor(-123.0) == -123);
static_assert(floor(-123.4) == -124);

// Data that describes a JS8 submode. Anything here should be able to be
// completely determined at compile time, i.e., any instance of this is
// just constant data.

class Data {
  public:
    // Constructor; in addition to the basics provided by the constructor
    // parameters, we'll determine various convenience constants in order
    // to simplify calling code. These derived values depend only on the
    // JS8_NUM_SYMBOLS and RX_SAMPLE_RATE definitions, and therefore can
    // be entirely computed at compile time.

    constexpr Data(const char *const name,
                   unsigned int const samplesForOneSymbol,
                   unsigned int const startDelayMS,
                   unsigned int const periodMS,
                   Costas::Type const costas, int const rxSNRThreshold,
                   int const rxThreshold = 10,
                   unsigned int const numSymbols = JS8_NUM_SYMBOLS,
                   unsigned int const numTones = 8)
        : m_name(name), m_samplesForOneSymbol(samplesForOneSymbol),
          m_startDelayMS(startDelayMS), m_periodMS(periodMS),
          m_costas(costas),
          m_rxSNRThreshold(rxSNRThreshold), m_rxThreshold(rxThreshold),
          m_samplesForSymbols(numSymbols * samplesForOneSymbol),
          m_bandwidth(numTones * JS8_RX_SAMPLE_RATE / samplesForOneSymbol),
          m_samplesPerPeriod(JS8_RX_SAMPLE_RATE * periodMS / 1000),
          m_toneSpacing(JS8_RX_SAMPLE_RATE / (double)samplesForOneSymbol),
          m_samplesNeeded(
              floor(m_samplesForSymbols +
                    (0.5 + startDelayMS / 1000.0) * JS8_RX_SAMPLE_RATE)),
          m_dataDuration(m_samplesForSymbols / (double)JS8_RX_SAMPLE_RATE),
          m_txDuration(m_dataDuration + startDelayMS / 1000.0) {}

    // Inline accessors

    constexpr const char *name() const { return m_name; }
    constexpr unsigned int samplesForOneSymbol() const {
        return m_samplesForOneSymbol;
    }
    constexpr unsigned int startDelayMS() const { return m_startDelayMS; }
    constexpr unsigned int period() const { return (m_periodMS + 999) / 1000; }
    constexpr unsigned int periodMS() const { return m_periodMS; }
    constexpr Costas::Type costas() const { return m_costas; }
    constexpr int rxSNRThreshold() const { return m_rxSNRThreshold; }
    constexpr int rxThreshold() const { return m_rxThreshold; }
    constexpr int samplesForSymbols() const { return m_samplesForSymbols; }
    constexpr int bandwidth() const { return m_bandwidth; }
    constexpr int samplesPerPeriod() const { return m_samplesPerPeriod; }
    constexpr int samplesNeeded() const { return m_samplesNeeded; }
    constexpr double dataDuration() const { return m_dataDuration; }
    constexpr double toneSpacing() const { return m_toneSpacing; }
    constexpr double txDuration() const { return m_txDuration; }

  private:
    // Data members ** ORDER DEPENDENCY **

    const char *m_name;
    unsigned int m_samplesForOneSymbol;
    unsigned int m_startDelayMS;
    unsigned int m_periodMS;
    Costas::Type m_costas;
    int m_rxSNRThreshold;
    int m_rxThreshold;
    int m_samplesForSymbols;
    int m_bandwidth;
    int m_samplesPerPeriod;
    double m_toneSpacing;
    int m_samplesNeeded;
    double m_dataDuration;
    double m_txDuration;
};

// Data for known submodes. Normal mode uses the old Costas Array
// definition; all other modes use the new one. Note that as of this
// writing, Ultra is a known, but unused, submode; we handle it here
// nevertheless, but it's in general disabled in the calling code.

constexpr Data Normal = {
    "NORMAL",            JS8A_SYMBOL_SAMPLES,        JS8A_START_DELAY_MS,
    JS8A_TX_SECONDS * 1000, Costas::Type::ORIGINAL, -24};
constexpr Data Fast = {"FAST",
                       JS8B_SYMBOL_SAMPLES,
                       JS8B_START_DELAY_MS,
                       JS8B_TX_SECONDS * 1000,
                       Costas::Type::MODIFIED,
                       -22,
                       16};
constexpr Data Turbo = {"TURBO",
                        JS8C_SYMBOL_SAMPLES,
                        JS8C_START_DELAY_MS,
                        JS8C_TX_SECONDS * 1000,
                        Costas::Type::MODIFIED,
                        -20,
                        32};
constexpr Data Slow = {
    "SLOW",              JS8E_SYMBOL_SAMPLES,        JS8E_START_DELAY_MS,
    JS8E_TX_SECONDS * 1000, Costas::Type::MODIFIED, -28};
constexpr Data Ultra = {"ULTRA",
                        JS8I_SYMBOL_SAMPLES,
                        JS8I_START_DELAY_MS,
                        JS8I_TX_SECONDS * 1000,
                        Costas::Type::MODIFIED,
                        -18,
                        50};

#ifdef JS8_ENABLE_FT2
constexpr Data Subspace = {"SUBSPACE",
                           FT2_NSPS,
                           FT2_START_DELAY_MS,
                           FT2_TX_PERIOD_MS,
                           Costas::Type::MODIFIED,
                           -20,
                           32,
                           FT2_NUM_SYMBOLS,
                           4};  // 4-GFSK (4 tones, not JS8's 8)
#endif

#ifdef JS8_ENABLE_JTTY
// [JTTY] Transmit-mode row. Un-slotted like Subspace: one frame is 59
// symbols of 384 samples at 12 kHz = 1.888 s; the period is that frame
// time, so the slot arithmetic that still runs (guiUpdate, ALL.TXT time
// rounding, dupe aging) has a true number. Costas and the thresholds are
// never consulted for JTTY audio -- its receiver is JTTY::Receiver, fed
// from the Subspace ring -- but rxThreshold (32 Hz) is what the
// band-activity attribution reads. 4 tones * 31.25 Hz = 125 Hz.
constexpr Data Jtty = {"JTTY",
                       384,
                       0,
                       1888,
                       Costas::Type::MODIFIED,
                       -20,
                       32,
                       59,
                       4};
#endif

// Given a submode, return data for it, or, if we don't have any idea
// what the caller is talking about, throw.
//
// Note that the original code in all cases did its best to just carry
// on in the event of an invalid submode, e.g., by returning 0, etc.,
// but that approach will in general lead to things like division by
// zero in computeCycleForDecode(), below, so either way we're going
// to end up with a runtime error, and it seems preferable that it's
// an informative one.
//
// Note that the Varicode::SubModeType enum is not dense, so we can't
// just do direct indexed access here.

constexpr Data const &data(int const submode) {
    switch (submode) {
    case Varicode::JS8CallNormal:
        return Normal;
    case Varicode::JS8CallFast:
        return Fast;
    case Varicode::JS8CallTurbo:
        return Turbo;
    case Varicode::JS8CallSlow:
        return Slow;
    case Varicode::JS8CallUltra:
        return Ultra;
#ifdef JS8_ENABLE_FT2
    case Varicode::JS8CallFT2:
        return Subspace;
#endif
#ifdef JS8_ENABLE_JTTY
    case Varicode::JS8CallJTTY:
        return Jtty;
#endif
    default: {
        throw error{QObject::tr("Invalid JS8 submode %1").arg(submode)};
    }
    }
};

class ListDataAsDebugOutput {
  private:
    inline void list_one_mode(Data data) {
        qCDebug(js8submode_js8)
            << "\nname               " << data.name() << "\nsamplesForOneSymbol"
            << data.samplesForOneSymbol() << "\nstartDelayMS       "
            << data.startDelayMS() << "\nperiod             " << data.period()
            << "\ncostas             "
            << (data.costas() == Costas::Type::MODIFIED
                    ? "MODIFIED"
                    : (data.costas() == Costas::Type::ORIGINAL ? "ORIGINAL"
                                                               : "????"))
            << "\nrxSNRThreshold     " << data.rxSNRThreshold()
            << "\nrxThreshold        " << data.rxThreshold()
            << "\nsamplesForSymbols  " << data.samplesForSymbols()
            << "\nbandwidth          " << data.bandwidth()
            << "\nsamplesPerPeriod   " << data.samplesPerPeriod()
            << "\nsamplesNeeded      " << data.samplesNeeded()
            << "\ndataDuration       " << data.dataDuration()
            << "\ntoneSpacing        " << data.toneSpacing()
            << "\ntxDuration         " << data.txDuration() << "\n";
    }

  public:
    inline ListDataAsDebugOutput() {
        list_one_mode(Slow);
        list_one_mode(Normal);
        list_one_mode(Fast);
        list_one_mode(Turbo);
        // list_one_mode(Ultra);
    }
};

static ListDataAsDebugOutput list_data_as_debug_output =
    ListDataAsDebugOutput{};
} // namespace
} // namespace JS8::Submode

/******************************************************************************/
// Public Implementation
/******************************************************************************/
/**
 * @brief JS8 Submode namespace implementation
 */
namespace JS8::Submode {
// Submode name inquiry function; return a translated value, if there is
// a translated value, otherwise, the untranslated mode name.

/**
 * @brief Get the name of the submode
 *
 * @param submode
 * @return QString
 */
QString name(int const submode) { return QObject::tr(data(submode).name()); }

QString indicator(int const submode) {
    switch (submode) {
    case Varicode::JS8CallFT2:    return QString::fromUtf8("\xe2\x9a\xa1");
    case Varicode::JS8CallJTTY:   return QStringLiteral("J");
    case Varicode::JS8CallNormal: return QStringLiteral("N");
    case Varicode::JS8CallFast:   return QStringLiteral("F");
    case Varicode::JS8CallTurbo:  return QStringLiteral("T");
    case Varicode::JS8CallSlow:   return QStringLiteral("S");
    default:                      return QStringLiteral("?");
    }
}

// Basic submode numeric inquiry functions, i.e., parameterized only by
// the submode, returning constant data.

/**
 * @brief Get the bandwidth of the submode
 *
 * @param submode
 * @return unsigned int
 */
unsigned int bandwidth(int const submode) {
    auto bw = data(submode).bandwidth();
    // FT2/Subspace: reduce by 25 Hz to align waterfall dial with
    // visible GFSK energy distribution
    if (submode == Varicode::JS8CallFT2 && bw > 7)
        bw -= 7;
    return bw;
}
/**
 * @brief Get the Costas array type of the submode
 *
 * @param submode
 * @return Costas::Type
 */
Costas::Type costas(int const submode) { return data(submode).costas(); }
/**
 * @brief Get the number of samples per period of the submode
 *
 * @param submode
 * @return unsigned int
 */
unsigned int samplesPerPeriod(int const submode) {
    return data(submode).samplesPerPeriod();
}
/**
 * @brief Get the number of samples for symbols of the submode
 *
 * @param submode
 * @return unsigned int
 */
unsigned int samplesForSymbols(int const submode) {
    return data(submode).samplesForSymbols();
}
/**
 * @brief Get the number of samples needed for the submode
 *
 * @param submode
 * @return unsigned int
 */
unsigned int samplesNeeded(int const submode) {
    return data(submode).samplesNeeded();
}
/**
 * @brief Get the period of the submode
 *
 * @param submode
 * @return unsigned int
 */
unsigned int period(int const submode) { return data(submode).period(); }
/**
 * @brief Get the period of the submode in milliseconds
 *
 * @param submode
 * @return unsigned int
 */
unsigned int periodMS(int const submode) { return data(submode).periodMS(); }
/**
 * @brief Get the receive SNR threshold of the submode
 *
 * @param submode
 * @return int
 */
int rxSNRThreshold(int const submode) { return data(submode).rxSNRThreshold(); }
/**
 * @brief Get the receive threshold of the submode
 *
 * @param submode
 * @return int
 */
int rxThreshold(int const submode) { return data(submode).rxThreshold(); }
/**
 * @brief Get the start delay in milliseconds of the submode
 *
 * @param submode
 * @return unsigned int
 */
unsigned int startDelayMS(int const submode) {
    return data(submode).startDelayMS();
}
/**
 * @brief Get the number of samples for one symbol of the submode
 *
 * @param submode
 * @return unsigned int
 */
unsigned int samplesForOneSymbol(int const submode) {
    return data(submode).samplesForOneSymbol();
}
/**
 * @brief Get the tone spacing of the submode
 *
 * @param submode
 * @return double
 */
double toneSpacing(int const submode) { return data(submode).toneSpacing(); }
/**
 * @brief Get the data duration of the submode
 *
 * @param submode
 * @return double
 */
double dataDuration(int const submode) { return data(submode).dataDuration(); }
/**
 * @brief Get the transmit duration of the submode
 *
 * @param submode
 * @return double
 */
double txDuration(int const submode) { return data(submode).txDuration(); }

// Compute which cycle we are currently in based on submode frames per cycle
// and current k position.

/**
 * @brief Compute the cycle for decode
 *
 * @param submode
 * @param k
 * @return int
 */
int computeCycleForDecode(int const submode, int const k) {
    int const maxFrames = JS8_RX_SAMPLE_SIZE;
    int const cycleFrames = samplesPerPeriod(submode);

    return (k / cycleFrames) %        // we mod here so we loop
           (maxFrames / cycleFrames); // back to zero correctly
}

// Compute an alternate cycle offset by a specific number of frames e.g.,
// if we want the 0 cycle to start at second 5, we'd provide an offset of
// (5 * RX_SAMPLE_RATE).

/**
 * @brief Compute the alternate cycle for decode
 *
 * @param submode
 * @param k
 * @param offsetFrames
 * @return int
 */
int computeAltCycleForDecode(int const submode, int const k,
                             int const offsetFrames) {
    int const altK = k - offsetFrames;

    return computeCycleForDecode(submode,
                                 altK < 0 ? altK + JS8_RX_SAMPLE_SIZE : altK);
}

/**
 * @brief Compute the ratio of data duration to period
 *
 * @param submode
 * @param period
 * @return double
 */
double computeRatio(int const submode, double const period) {
    return (period - data(submode).dataDuration()) / period;
}
} // namespace JS8::Submode

Q_LOGGING_CATEGORY(js8submode_js8, "js8submode.js8", QtWarningMsg)

/******************************************************************************/
