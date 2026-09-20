// -*- Mode: C++ -*-
///////////////////////////////////////////////////////////////////////////
// Some code in this file and accompanying files is based on work by
// Moe Wheatley, AE4Y, released under the "Simplified BSD License".
// For more details see the accompanying file LICENSE_WHEATLEY.TXT
///////////////////////////////////////////////////////////////////////////

#ifndef PLOTTER_H
#define PLOTTER_H

#include "JS8_Main/Flatten.h"
#include "RDP.h"
#include "WF.h"

#include <QColor>
#include <QPixmap>
#include <QPolygonF>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <array>
#include <boost/circular_buffer.hpp>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <variant>

class CPlotter final : public QWidget {
    Q_OBJECT

    // Scaler for the waterfall portion of the display; given a
    // y value, returns an index [0, 255) into the colors array.

    class Scaler1D {
        int const &m_avg;
        int const &m_bpp;
        int m_gain = 0;
        int m_zero = 0;
        float m_scale;

      public:
        Scaler1D(int const &avg, int const &bpp) : m_avg(avg), m_bpp(bpp) {
            rescale();
        }

        void rescale() {
            m_scale = 10.0f * std::sqrt(m_bpp * m_avg / 15.0f) *
                      std::pow(10.0f, 0.015f * m_gain);
        }

        int gain() const { return m_gain; }
        int zero() const { return m_zero; }

        void setGain(int const gain) {
            m_gain = gain;
            rescale();
        }
        void setZero(int const zero) { m_zero = zero; }

        inline auto operator()(float const value) const {
            return std::isnan(value)
                       ? 0
                       : std::clamp(m_zero + static_cast<int>(m_scale * value),
                                    0, 254);
        }
    };

    // Scaler for the spectrum portion of the display; given a
    // y value, returns a pixel offset into the spectrum view.

    class Scaler2D {
        int const &m_h2;
        int m_gain = 0;
        int m_zero = 0;
        float m_scaledGain;
        float m_scaledZero;

      public:
        Scaler2D(int const &h2) : m_h2(h2) { rescale(); }

        void rescale() {
            m_scaledGain = m_h2 / 70.0f * std::pow(10.0f, 0.02f * m_gain);
            m_scaledZero = m_h2 * 0.9f - m_h2 / 70.0f * m_zero;
        }

        int gain() const { return m_gain; }
        int zero() const { return m_zero; }

        void setGain(int const gain) {
            m_gain = gain;
            rescale();
        }
        void setZero(int const zero) {
            m_zero = zero;
            rescale();
        }

        inline auto operator()(float const value) const {
            return m_scaledZero - m_scaledGain * value;
        }
    };

  public:
    using Colors = QVector<QColor>;
    using Spectrum = WF::Spectrum;

    explicit CPlotter(QWidget *parent = nullptr);

    ~CPlotter();

    // Sizing

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    // Inline accessors

    int binsPerPixel() const { return m_binsPerPixel; }
    int flatten() const { return m_flatten.live(); }
    int freq() const { return m_freq; }
    int percent2D() const { return m_percent2D; }
    int plot2dGain() const { return m_scaler2D.gain(); }
    int plot2dZero() const { return m_scaler2D.zero(); }
    int plotGain() const { return m_scaler1D.gain(); }
    int plotZero() const { return m_scaler1D.zero(); }
    Spectrum spectrum() const { return m_spectrum; }
    int startFreq() const { return m_startFreq; }

    int frequencyAt(int const x) const {
        return static_cast<int>(freqFromX(x));
    }

    // [BUILD 336] Hit-test a widget-coordinate point against the
    // painted callsign labels; empty string when nothing is close.
    // Public since [stamon]: the waterfall's context menu asks it.
    QString callAt(QPoint const &widgetPos) const;

    // Inline manipulators

    void setFlatten(bool const flatten) { m_flatten(flatten); }
    void setPlot2dGain(int const plot2dGain) { m_scaler2D.setGain(plot2dGain); }
    void setPlot2dZero(int const plot2dZero) { m_scaler2D.setZero(plot2dZero); }
    void setSpectrum(Spectrum const spectrum) { m_spectrum = spectrum; }

    // Manipulators

    void drawLine(QString const &);
    void drawData(WF::SWide, WF::State);
    void drawDecodeLine(const QColor &, int, int);
    void drawHorizontalLine(const QColor &, int, int);
    void annotateCall(QString const &call, int offsetHz, int submode,
                      bool inMyGroup = false,
                      bool destIsSubspaceGroup = false,
                      bool isUpgrade = false);
    void setCallsignOverlayEnabled(bool enabled);
    bool callsignOverlayEnabled() const { return m_callsignOverlayEnabled; }
    // [BUILD 353 yesflag TODO #131] Predicate: has this callsign
    // proven ARQ/Subspace-app capability (answered "YES <level>" to a
    // QUERY ARQ?, ours or overheard)? Evaluated at PAINT time so the
    // flag reads the live session cache (m_peerArqLevel in the UI) —
    // no copied state here to drift. Upgrades ONLY the plain
    // white-on-black label case to yellow-on-black; the canonical
    // @SUBSPACE yellow-background and the in-my-group purple keep
    // precedence (operator decision 2026-07-30).
    void setArqCapableCheck(std::function<bool(QString const &)> fn) {
        m_arqCapableCheck = std::move(fn);
    }
    void setBinsPerPixel(int);
    void setColors(Colors const &);
    void setDialFreq(float);
    void setFilter(int, int);
    void setFilterEnabled(bool);
    void setFilterOpacity(int);
    void setFreq(int);
    void setPercent2D(int);
    void setPlotGain(int);
    void setPlotZero(int);
    void setStartFreq(int);
    // [TODO #132] Wipe the waterfall: pixmap, replot history and label
    // history together. Any one left behind is painted back by
    // replot() or by a same-call label upgrade.
    void clearWaterfall();
    void setSubMode(int);
    void setWaterfallAvg(int);

  signals:

    void changeFreq(int);
    // [BUILD 336] Double-click landed on (or very near) a painted
    // callsign label. Payload: the callsign.
    void callDoubleClicked(QString const &call);

  protected:
    // Event Handlers

    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void leaveEvent(QEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

  private:
    // Replot data storage; alternatives of nothing at all, a
    // string denoting the label of a transmit period interval
    // start, and waterfall display data, flattened. Important
    // that the monostate alternative is first in the list.

    using Replot = boost::circular_buffer<
        std::variant<std::monostate, QString, WF::SWide>>;

    // Accessors

    bool shouldDrawSpectrum(WF::State) const;
    bool in30MBand() const;
    int xFromFreq(float f) const;
    float freqFromX(int x) const;

    // Manipulators

    void drawMetrics();
    void drawFilter();
    void drawDials();
    void replot();
    void resize();

    // Data members ** ORDER DEPENDENCY **

    float m_dialFreq = 0.0f;
    int m_nSubMode = 0;
    int m_filterCenter = 0;
    int m_filterWidth = 0;
    int m_filterOpacity = 127;
    int m_percent2D = 0;
    int m_binsPerPixel = 2;
    int m_waterfallAvg = 1;
    int m_lastMouseX = -1;
    int m_line = std::numeric_limits<int>::max();
    int m_startFreq = 0;
    int m_freq = 0;
    int m_w = 0;
    int m_h1 = 0;
    int m_h2 = 0;
    bool m_filterEnabled = false;
    bool m_callsignOverlayEnabled = true;
    // [BUILD 341 wfDblClick] True after a double-click landed on a
    // callsign label (click-to-call fired). The double-click's own
    // trailing mouseReleaseEvent must NOT emit changeFreq — it would
    // re-tune to the clicked pixel AFTER the click-to-call handler's
    // looked-up-frequency QSY, and the looked-up freq lost the race
    // (operator-observed 2026-07-17).
    bool m_suppressReleaseFreq = false;
    float m_freqPerPixel;

    // History of recently-painted call-sign overlays, used to
    // suppress new labels that would visually overlap an existing
    // one. The scroll-rate estimate is updated from drawData
    // timestamps.
    struct LabelEntry {
        int x;            // baseline column (pixel x)
        qint64 row;       // m_waterfallRow at paint time
        int lenPx;        // vertical extent (text horizontalAdvance)
        QString call;     // for same-call upgrade matching
        int submode;      // for color matrix on replot
        bool inMyGroup;
        bool destIsSubspaceGroup;
    };
    void paintLabelAt(QPainter &p, LabelEntry const &e,
                      qint64 yOffset);
    std::deque<LabelEntry> m_recentLabels;
    qint64 m_waterfallRow = 0;  // monotonic 1-per-scroll counter
    // [BUILD 353 yesflag] see setArqCapableCheck.
    std::function<bool(QString const &)> m_arqCapableCheck;

    RDP m_rdp;
    Scaler1D m_scaler1D;
    Scaler2D m_scaler2D;
    Colors m_colors;
    Replot m_replot;
    QPolygonF m_points;
    Flatten m_flatten;
    Spectrum m_spectrum = Spectrum::Current;
    QTimer *m_replotTimer;
    QTimer *m_resizeTimer;

    QPixmap m_ScalePixmap;
    QPixmap m_WaterfallPixmap;
    QPixmap m_OverlayPixmap;
    QPixmap m_SpectrumPixmap;

    std::array<QPixmap, 2> m_FilterPixmap = {};
    std::array<QPixmap, 2> m_DialPixmap = {};

    QString m_text;
};

#endif // PLOTTER_H
