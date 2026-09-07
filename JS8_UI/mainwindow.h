// -*- Mode: C++ -*-
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <atomic>
#include <cstdint>

#include "JS8_Audio/AudioDevice.h"
#include "JS8_Audio/NotificationAudio.h"
#include "JS8_Audio/SoundInput.h"
#include "JS8_Audio/SoundOutput.h"
#include "JS8_Include/EventFilter.h"
#include "JS8_Include/commons.h"
#include "JS8_Include/qpriorityqueue.h"
#include "JS8_JSC/JSC_checker.h"
#include "JS8_Logbook/LogBook.h"
#include "JS8_Main/APRSISClient.h"
#include "JS8_Main/FileTransfer.h"
#include "JS8_Main/AprsInboundRelay.h"
#include "JS8_Main/Bands.h"
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Main/FrequencyList.h"
#include "JS8_Main/Geodesic.h"
#include "JS8_Main/ChunkedArq.h"
#include "JS8_Main/HelpTextWindow.h"
#include "JS8_Main/Inbox.h"
#include "JS8_Main/JS8MessageBox.h"
#include "JS8_Main/MessageClient.h"
#include "JS8_Main/MessageServer.h"
#include "JS8_Main/Modes.h"
#include "JS8_Main/MultiSettings.h"
#include "JS8_Main/ProcessThread.h"
#include "JS8_Main/Radio.h"
#include "JS8_Main/SelfDestructMessageBox.h"
#include "JS8_Main/SignalMeter.h"
#include "JS8_Main/StationList.h"
#include "JS8_Main/TxLoop.h"
#include "JS8_Main/Varicode.h"
#include "JS8_Main/qt_helpers.h"
#include "JS8_Main/revision_utils.h"
#include "JS8_Mode/DecodedText.h"
#include "JS8_Mode/Decoder.h"
#include "JS8_Mode/Detector.h"
#include "JS8_Mode/JS8.h"
#include "JS8_Mode/JS8Submode.h"
#include "JS8_Mode/Modulator.h"
#include "JS8_Network/NetworkAccessManager.h"
#include "JS8_Network/PSKReporter.h"
#include "JS8_Network/SpotClient.h"
#include "JS8_Network/TCPClient.h"
#include "JS8_Transceiver/Transceiver.h"
#include "JS8_Transceiver/TransceiverFactory.h"
#include "JS8_UDP/WSJTXMessageClient.h"
#include "JS8_UDP/WSJTXMessageMapper.h"
#include "JS8_UI/About.h"
#include "JS8_UI/Configuration.h"
#include "JS8_UI/WideGraph.h"
#include "JS8_UI/MessagePanel.h"
#include "LogQSO.h"
#include "SpotMapWindow.h"
#include "MessageReplyDialog.h"
#include "ui_mainwindow.h"

#include <QAbstractSocket>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QByteArrayView>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMdiSubWindow>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScopedPointer>
#include <QScrollBar>
#include <QSet>
#include <QStandardPaths>
#include <QStringBuilder>
#include <QTableWidget>
#include <QTextEdit>
#include <QThread>
#include <QTime>
#include <QTimeZone>
#include <QTimer>
#include <QToolTip>
#include <QUdpSocket>
#include <QUrl>
#include <QVariant>
#include <QVector>
#include <QVersionNumber>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QtGui>
#include <boost/crc.hpp>
#include <fftw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

Q_DECLARE_LOGGING_CATEGORY(mainwindow_js8)

#ifdef JS8_ENABLE_FT2
extern int volatile itone[FT2_NUM_SYMBOLS]; // Max of JS8 (79) and FT2 (103)
extern float ft2_txwave[FT2_NWAVE];         // Pre-computed FT2 GFSK waveform
extern int ft2_txwave_len;                   // Actual waveform length
#else
extern int volatile itone[JS8_NUM_SYMBOLS]; // Audio tones for all Tx symbols
#endif

//--------------------------------------------------------------- mainwindow
// How often to poll the UI, in MS.
// Some things may depend on this being a divisor of 1000.
constexpr quint32 UI_POLL_INTERVAL_MS = 100;

namespace {
namespace Default {
constexpr Radio::Frequency DIAL_FREQUENCY = 14078000;
constexpr auto FREQUENCY = 1500;
constexpr auto SUBMODE = Varicode::JS8CallFT2;
} // namespace Default

namespace State {
constexpr auto RX = 1;
constexpr auto TX = 2;
} // namespace State
} // namespace

namespace Ui {
class UI_Constructor;
}

class QSettings;
class QLineEdit;
class QFont;
class QHostInfo;
class WideGraph;
class LogQSO;
class Transceiver;
class MessageClient;
class QTime;
class HelpTextWindow;
class SoundOutput;
class Modulator;
class SoundInput;
class Detector;
class AprsInboundRelay;
class MultiSettings;
class DecodedText;
class JSCChecker;
class MessagePanel;

using namespace std;
typedef std::function<void()> Callback;

class WSJTXMessageMapper; // Forward declaration

class UI_Constructor : public QMainWindow {
    Q_OBJECT;
    friend class WSJTXMessageMapper; // Allow WSJTXMessageMapper to access
                                     // private members

    struct CallDetail;
    struct CommandDetail;

  public:
    using Frequency = Radio::Frequency;
    using FrequencyDelta = Radio::FrequencyDelta;
    using Mode = Modes::Mode;

    explicit UI_Constructor(QString const &program_info,
                            QDir const &temp_directory, bool multiple,
                            MultiSettings *settings, QWidget *parent = nullptr);
    ~UI_Constructor();

  private:
    struct SortByReverse {
        QString by;
        bool reverse;
    };

    /**
     * Sometimes, buttons are triggered by code and this refers
     * to a momentary situation only.  Sometimes, the trigger
     * pertains to the long term processing, that is, it should
     * influence (shut off, mostly) the automatic transmit loops
     * for HB and CQ.
     *
     * In an ugly klugde, we try to distinguish these cases
     * via these three *IsLongterm variables.
     */
    // [BUILD 353 haltwrap 2026-08-01] Mechanical TX stop — everything
    // on_stopTxButton_clicked does EXCEPT the operator-terminal
    // actions (haltAll, loop cancels, hail abort, negotiation abort).
    // Every PROGRAMMATIC stop (end-of-frame cleanup, auto_tx_mode,
    // empty-box stop, API RIG.TX_HALT, TX-enable untoggle, pre-flight
    // failures) calls THIS; only genuine operator gestures (Halt
    // button via the auto-connected slot, Escape) reach the terminal
    // actions. Replaces the m_stopTxButtonIsLongterm flag + its
    // save/set/call/restore ritual, which four callers had already
    // forgotten to perform — a convention-enforced invariant turned
    // into a structural one (a caller now CANNOT destroy ARQ session
    // state by merely stopping TX).
    void stopTxMechanical();
    bool m_hbButtonIsLongterm;
    bool m_cqButtonIsLongterm;

  public slots:
    void showSoundInError(const QString &errorMsg);
    void showSoundOutError(const QString &errorMsg);
    // [TODO #113] Configured device missing → system default opened.
    // Warning dialog only; does NOT invalidate the device selection.
    void showSoundInDeviceFallback(const QString &msg);
    void showSoundOutDeviceFallback(const QString &msg);
    void showStatusMessage(const QString &statusMsg);
    void dataSink(qint64 frames);
    /**
     * The name `guiUpdate` suggests updating of the views from the models
     * (in MVC terms, but we don't do MVC in this project), animations and
     * stuff. While it indeed does that, this also contains controller code.
     */
    void guiUpdate();
    void setXIT(int n);
    void qsy(int hzDelta);
    void onDriftChanged(qint64 new_drift_ms);
    void setFreqOffsetForRestore(int freq, bool shouldRestore);
    bool tryRestoreFreqOffset();
    void changeFreq(int);

    bool hasExistingMessageBufferToMe(int *pOffset);
    bool hasExistingMessageBuffer(int submode, int offset, bool drift,
                                  int *pPrevOffset);
    bool hasClosedExistingMessageBuffer(int offset);
    void logCallActivity(CallDetail d, bool spot = true);
    // [#167] `thirdPartyIsEvidence`: does this frame prove `from`
    // received `to`? Pass Varicode::isCommandReceptionEvidence(cmd) for
    // a directed frame; true only when the edge is genuinely observed
    // (e.g. a station named in a HEARING list).
    void logHeardGraph(QString from, QString to,
                       bool thirdPartyIsEvidence);
    QString lookupCallInCompoundCache(QString const &call);
    void cacheActivity(QString key);
    void restoreActivity(QString key);
    void clearActivity();
    void clearBandActivity();
    void clearRXActivity();
    void clearCallActivity();
    void createGroupCallsignTableRows(QTableWidget *table,
                                      const QString &selectedCall,
                                      bool &showIconColumn);
    // [BUILD 358 cppos] Index of the compound entry that ON-AIR
    // immediately precedes consumerAbsPos (largest absPos strictly
    // below it, within 3 frame-lengths), or -1. Position matching is
    // only meaningful when both sides carry a ring position
    // (absPos > 0, async decoder); callers keep arrival-order
    // behavior for standard-decoder events, which ARE in order.
    int compoundIndexBefore(QQueue<CallDetail> const &comp,
                            qint64 consumerAbsPos) const;
    void displayTextForFreq(QString text, int freq, QDateTime date, bool isTx,
                            bool isNewLine, bool isLast, int submode = -1);
    void writeNoticeTextToUI(QDateTime date, QString text);
    int writeMessageTextToUI(QDateTime date, QString text, int freq, bool isTx,
                             int submode = -1, int block = -1);
    bool isMessageQueuedForTransmit();
    bool isInDecodeDelayThreshold(int seconds);
    void prependMessageText(QString text);
    void addMessageText(QString text, bool clear = false,
                        bool selectFirstPlaceholder = false);
    void confirmThenEnqueueMessage(int timeout, int priority, QString message,
                                   int offset, Callback c,
                                   bool autoReply = false);
    // autoReply: the app composed this message on another station's
    // behalf (autoreply dispatch, relay forward, ARQ protocol reply,
    // APRS relay). Such traffic never registers as a reaching attempt
    // on the map -- attempts are only what WE initiate (operator, red-
    // line bug 2026-08-27: our relayed-back HEARING answer "K8IMT>
    // WD4KAV ..." drew as our attempt).
    void enqueueMessage(int priority, QString message, int offset, Callback c,
                        bool autoReply = false);
    // [attemptviz] ONE parser for "is this outgoing text a call we are
    // waiting on an answer to, and along what chain". Called twice on
    // purpose: at ENQUEUE so the path appears the moment the call is
    // issued, and again when frames are built so the countdown runs on
    // the real transmit time. txFrames <= 0 means "not known yet".
    void noteAttemptFromText(QString const &text, int txFrames);
    void resetMessage();
    void resetMessageUI();
    void restoreMessage();
    void initializeDummyData();
    void initializeGroupMessage();
    bool ensureCallsignSet(bool alert = true);
    bool ensureKeyNotStuck(QString const &text);
    bool ensureNotIdle();
    bool ensureCanTransmit();
    bool ensureCreateMessageReady(const QString &text);
    QString createMessage(QString const &text, bool *pDisableTypeahead);
    QString appendMessage(QString const &text, bool isData,
                          bool *pDisableTypeahead);
    QString createMessageTransmitQueue(QString const &text, bool reset,
                                       bool isData, bool *pDisableTypeahead);
    void resetMessageTransmitQueue();
    // [TODO #107] Append one V3 chunk's raw binary frames to
    // m_txFrameQueue (native-layer file transfer; see NativeBinary.h).
    void injectNativeBinaryFrames(int chunkId, QByteArray const &chunkBytes);
    void injectNativeMarkerFrame(QByteArray const &frame9);
    QPair<QString, int> popMessageFrame();
    void tryNotify(const QString &key, int submode = -1);
    void processDecodeEvent(JS8::Event::Variant const &);

    void updateCQButtonDisplay();
    void updateHBButtonDisplay();
    // [#148] Expansion distributor: at minimum window width every
    // button sits at its NATURAL text width with minimum gaps (the
    // approved btnpad2 state — invariant). Extra window width is
    // split among the like-button groups, water-filled inside each
    // group so members CONVERGE to a common width, then grow
    // together. Enforced via per-button maximum caps; minimums stay
    // at the naturals, so shrinking always returns to the approved
    // state.
    void distributeActionRowWidths();


  protected:
    void keyPressEvent(QKeyEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void childEvent(QChildEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

  private slots:
    // ChunkedArq integration: TX-side relay + RX-side UI rendering.
    void onChunkedWantToTransmit(QString const &text);
    // [TODO.md #57 / build 268] Per-response wrapper for ACK / NACK.
    // Saves the operator's outgoing-box draft text into
    // m_arqResponseSavedText, then delegates to onChunkedWantToTransmit
    // for the actual TX. stopTx() restores the saved text after the
    // ACK/NACK transmission completes.
    void onChunkedWantsResponseTx(QString const &text);
    void onChunkedChunkAdded(QString const &fromCall,
                             QString const &chunkBody,
                             int            chunkId,
                             int            total);
    void onChunkedMessageDelivered(QString const &fromCall,
                                   QString const &toCall,
                                   QString const &assembledBody,
                                   int            msgId);
    void onChunkedSendProgress(QString const &peer, int msgId,
                               int delivered, int total);
    void onChunkedSendComplete(QString const &peer, int msgId,
                               int total, int totalRetries);
    void onChunkedSendFailed(QString const &peer, int msgId,
                             int delivered, int total,
                             int totalRetries,
                             QString const &reason);
    // [TODO #51 2026-06-10 build 235] Restore original outbound body
    // to the outgoing-text widget on terminal-failure paths so the
    // operator can retry without re-typing. Connected to
    // ChunkedArq::Manager::sendRestoreRequested.
    void onChunkedSendRestoreRequested(QString const &body,
                                       QString const &reason);
    void onChunkedMsgDelivered(QString const &peer,
                               QString const &addressee,
                               QString const &body,
                               int            msgId);
    void onChunkedInboxMessageReceived(QString const &fromCall,
                                       QString const &addressee,
                                       QString const &body,
                                       int            msgId);
    void onChunkedRelayMessageReceived(QString const &fromCall,
                                       QString const &body,
                                       int            msgId);
    // [FILE-XFER 2026-06-16 build 276] RX slot for ARQ file-transfer
    // super-messages. body is the assembled "F/V1 <header-b32> <pay-b32>".
    // Slot pops the accept dialog, verifies SHA-256, writes the file
    // to the configured save dir.
    void onChunkedFileMessageReceived(QString const &fromCall,
                                      QString const &body,
                                      int            msgId);
    // TX-side handler for the "Send File…" button.
    void on_sendFileButton_clicked();
    void on_sendWebLinkAction_triggered();
    // [BUILD 298] TX-side handler for the "Send using ARQ" menu action.
    // Enables ARQ for this one send, fires the normal Send path, and
    // arranges for ARQ to auto-disable on sendComplete / sendFailed.
    void on_sendUsingArqAction_triggered();
    // [BUILD 331-visibleHail] TX-side handler for the "Send Visible
    // Hail" menu action. Two-cycle sequence: bolt frame (paints
    // ⚡ silhouette on waterfall as Hellschreiber-style raster), one
    // silent cycle gap, then standard Subspace HAIL message
    // (encoded `<mycall>: @ALLCALL ACK`). Chain step 2 (the HAIL TX)
    // is scheduled in the ft2WaveformDone handler when
    // m_visibleHailPendingHail is set.
    void on_sendVisibleHailAction_triggered();
    void restoreVisibleHailSubmodeIfPending();
    void onChunkedProgressUpdate(int chunkId, int total, int retries);
    void onChunkedProgressEnd();

    void initialize_fonts();
    void on_menuModeJS8_aboutToShow();
    void on_menuControl_aboutToShow();
    void on_actionEnable_Monitor_RX_toggled(bool checked);
    void on_actionEnable_Transmitter_TX_toggled(bool checked);
    void on_actionEnable_Reporting_SPOT_toggled(bool checked);
    void on_actionEnable_Tuning_Tone_TUNE_toggled(bool checked);
    void on_menuWindow_aboutToShow();
    void on_actionFocus_Message_Receive_Area_triggered();
    void on_actionFocus_Message_Reply_Area_triggered();
    void on_actionFocus_Band_Activity_Table_triggered();
    void on_actionFocus_Call_Activity_Table_triggered();
    void on_actionClear_All_Activity_triggered();
    void on_actionClear_Band_Activity_triggered();
    void on_actionClear_RX_Activity_triggered();
    void on_actionClear_Call_Activity_triggered();
    void on_actionSetOffset_triggered();
    void on_actionShow_Fullscreen_triggered(bool checked);
    void on_actionShow_Statusbar_triggered(bool checked);
    void on_actionShow_Frequency_Clock_triggered(bool checked);
    void on_actionShow_Band_Activity_triggered(bool checked);
    void on_actionShow_Band_Heartbeats_and_ACKs_triggered(bool checked);
    void on_actionShow_Call_Activity_triggered(bool checked);
    void on_actionShow_Waterfall_triggered(bool checked);
    void on_actionShow_Spots_Map_triggered(bool checked);
    void on_actionShow_ARQ_Monitor_triggered(bool checked); // [#153]
    void on_actionShow_Waterfall_Controls_triggered(bool checked);
    void on_actionShow_Waterfall_Time_Drift_Controls_triggered(bool checked);
    void on_actionReset_Window_Sizes_triggered();
    void on_actionSettings_triggered();
    void openSettings(int tab = 0);
    void prepareApi();
    void prepareSpotting();
    void on_spotButton_clicked(bool checked);
    void on_monitorButton_clicked(bool);
    void on_actionAbout_triggered();
    void resetPushButtonToggleText(QPushButton *btn);
    void on_stopTxButton_clicked();
    void on_dialFreqUpButton_clicked();
    void on_dialFreqDownButton_clicked();
    void on_actionAdd_Log_Entry_triggered();
    void on_actionOpen_log_directory_triggered();
    void on_actionCopyright_Notice_triggered();
    void on_actionUser_Guide_triggered();
    void on_actionSubspace_Guide_triggered();
    bool decode(qint32 k);
    bool isDecodeReady(int submode, qint32 k, qint32 k0,
                       qint32 *pCurrentDecodeStart, qint32 *pNextDecodeStart,
                       qint32 *pStart, qint32 *pSz, qint32 *pCycle);
    bool decodeEnqueueReady(qint32 k, qint32 k0);
    bool decodeEnqueueReadyExperiment(qint32 k, qint32 k0);
    bool decodeProcessQueue(qint32 *pSubmode);
    void decodeStart();
    void decodeBusy(bool b);
    void decodeDone();
    void on_startTxButton_toggled(bool checked);
    // [ICS213] Compose-and-send dialog (chevron menu) — the file
    // picker of an ordinary ARQ file transfer replaced by a form.
    void on_sendIcs213FormAction_triggered();
    // [#148 split Send] The WIDGET stays enabled full-time so the
    // arrow half (send-options menu) is always reachable — build-367
    // chevron convention: "chevron enabled full-time; the menu
    // actions mirror Send's disabled state". The SEND side's
    // enabled-ness is this flag: visual gray via the [sendOff]
    // property, clicks guarded in the toggled handler.
    void setSendSideEnabled(bool on);
    void toggleTx(bool start);
    void on_logQSOButton_clicked();
    void on_actionModeJS8HB_toggled(bool checked);
    void on_actionModeJS8Normal_triggered();
    void on_actionModeJS8Fast_triggered();
    void on_actionModeJS8Turbo_triggered();
    void on_actionModeJS8Slow_triggered();
    void on_actionModeJS8Ultra_triggered();
    void on_actionModeFT2_triggered();
    void on_actionHeartbeatAcknowledgements_toggled(bool checked);
    void on_actionModeMultiDecoder_toggled(bool checked);
    void on_actionModeReplicatorProtocol_toggled(bool checked);
    void on_actionModeAutoreply_toggled(bool checked);
    bool canCurrentModeSendHeartbeat() const;
    bool canCurrentModeAckHeartbeat() const;
    void prepareMonitorControls();
    void prepareHeartbeatMode(bool enabled);
    void f11f12(int n);
    void on_actionErase_ALL_TXT_triggered();
    void on_actionErase_js8call_log_adi_triggered();
    void startTx();
    void startTxNonArq();
    void stopTx();
    void stopTx2();
    void buildFrequencyMenu(QMenu *menu);
    void buildHeartbeatMenu(QMenu *menu);
    void buildCQMenu(QMenu *menu);
    void buildRepeatMenu(QMenu *menu, QPushButton *button, bool isLowInterval,
                         int *interval);
    void sendHB();
    void sendHeartbeatAck(QString to, int snr, QString extra);
    void on_hbMacroButton_toggled(bool checked);
    void on_hbMacroButton_clicked();
    void sendCQ(bool repeat = false);
    void on_cqMacroButton_toggled(bool checked);
    void on_cqMacroButton_clicked();
    void on_replyMacroButton_clicked();
    void on_snrMacroButton_clicked();
    void on_infoMacroButton_clicked();
    void on_statusMacroButton_clicked();
    void on_typingMacroButton_clicked();
    void setShowColumn(QString tableKey, QString columnKey, bool value);
    bool showColumn(QString tableKey, QString columnKey, bool default_ = true);
    void buildShowColumnsMenu(QMenu *menu, QString tableKey);
    void setSortBy(QString key, QString value);
    QString getSortBy(QString const &key, QString const &defaultValue) const;
    SortByReverse getSortByReverse(QString const &key,
                                   QString const &defaultValue) const;
    void buildSortByMenu(QMenu *menu, QString key, QString defaultValue,
                         QList<QPair<QString, QString>> values);
    void buildBandActivitySortByMenu(QMenu *menu);
    void buildCallActivitySortByMenu(QMenu *menu);
    // [bandcall] "List by..." mode for the band activity table
    bool bandListByCall() const;
    void setBandListBy(QString const &value);
    void buildBandActivityListByMenu(QMenu *menu);
    // [bandcall] shared "CALL: " prefix parse (was inline in the
    // render loop) -- returns empty when the text has no valid prefix
    static QString frameFromCall(QString const &text);
    // [bandcall2] orphan fallback: the station last seen NEAREST this
    // offset within the submode's rx tolerance, from m_callActivity;
    // empty when none is close enough
    QString mostLikelyCallAtOffset(int offset, int submode) const;
    void buildQueryMenu(QMenu *, QString callsign);
    QMap<QString, QString> buildMacroValues();
    void buildColumnLabelMap();
    void buildSuggestionsMenu(QMenu *menu, QTextEdit *edit,
                              const QPoint &point);
    void buildSavedMessagesMenu(QMenu *menu);
    void buildRelayMenu(QMenu *menu);
    QAction *buildRelayAction(QString call);
    void buildEditMenu(QMenu *, QTextEdit *);
    void on_queryButton_pressed();
    void on_macrosMacroButton_pressed();
    void on_deselectButton_pressed();
    void on_tableWidgetRXAll_cellClicked(int row, int col);
    void on_tableWidgetRXAll_cellDoubleClicked(int row, int col);
    QString generateCallDetail(QString selectedCall);
    void on_tableWidgetCalls_cellClicked(int row, int col);
    void on_tableWidgetCalls_cellDoubleClicked(int row, int col);
    QList<QPair<QString, int>> buildMessageFrames(QString const &text,
                                                  bool isData,
                                                  bool *pDisableTypeahead);
    bool prepareNextMessageFrame();
    bool isFreqOffsetFree(int f, int bw);
    int findFreeFreqOffset(int fmin, int fmax, int bw);
    void setDrift(int n);
    void matchCallsignFromInput();
    void on_tuneButton_clicked(bool);
    void acceptQSO(QDateTime const &, QString const &call, QString const &grid,
                   Frequency dial_freq, QString const &mode,
                   QString const &submode, QString const &rpt_sent,
                   QString const &rpt_received, QString const &comments,
                   QString const &name, QDateTime const &QSO_date_on,
                   QString const &operator_call, QString const &my_call,
                   QString const &my_grid, QByteArray const &ADIF,
                   QVariantMap const &additionalFields);
    void on_readFreq_clicked();
    void on_outAttenuation_valueChanged(int);
    void rigOpen();
    void handle_transceiver_update(Transceiver::TransceiverState const &);
    void handle_transceiver_failure(QString const &reason);
    void band_changed();
    void monitor(bool);
    void end_tuning();
    void stop_tuning();
    void stopTuneATU();
    void auto_tx_mode(bool);
    void on_monitorButton_toggled(bool checked);
    void on_monitorTxButton_toggled(bool checked);
    void on_tuneButton_toggled(bool checked);
    void on_spotButton_toggled(bool checked);

    void emitPTT(bool on);
    void emitTones();
    void udpNetworkMessage(Message const &message);
    void tcpNetworkMessage(Message const &message);
    void networkMessage(Message const &message);
    // [TODO #112] ARQ transfer in flight (either direction), and a short
    // reason string, for the API busy status.
    bool arqBusyNow() const;
    QString busyReason() const;
    // [TODO #112] THE speed-change gate — shared by the UI polls and the
    // TCP API so they cannot drift apart.
    bool canChangeSpeedNow() const;
    // [txidle] The radio-idle half of that gate, without the
    // auto-route lock term — the executor's own pin/restore use this.
    bool txIdleNow() const;
    // Busy-wait toast wording, empty when idle — the ONE authority.
    QString txBusyToastText() const;
    // [#207 waitopts] busy-band predicate for the two
    // busy-conditional wait points; mode + threshold rule it.
    bool reachBandBusy() const;
    void setReachWaitConfig(int mode, int threshOnAir,
                            int threshPskr); // persists
    // [stamon] create-or-raise a per-station follow window; feed all
    // open windows one assembled directed line.
    void openStationMonitor(QString const &call);
    // Pop the env-gated "Open station monitor" menu at globalPos.
    void stationMonitorMenu(QString const &call,
                            QPoint const &globalPos);
    void feedStationMonitors(QString const &from, QString const &to,
                             QString const &relayPath,
                             QString const &text, int offset,
                             QDateTime const &utc, int submode);
    void reachComputeExpected();   // [structured] packed counts
    // [congestion] 1-10 = ceil(10 x occupied-slot fraction, trailing
    // 20 slots). Radio-only; works with no internet.
    int bandCongestionIndex() const;
    bool canSendNetworkMessage();
    void sendNetworkMessage(QString const &type, QString const &message);
    void sendNetworkMessage(QString const &type, QString const &message,
                            const QVariantMap &params);
    void pskReporterError(QString const &);
    void TxAgain();
    void checkVersion(bool alertOnUpToDate);
    void checkStartupWarnings();
    void selectCallsign(QString call, int submode = -1);
    // [unkrow] keepBandRow: drop the callsign selection but leave the
    // band activity row highlighted (clicking an UNKNOWN callsign-mode
    // row selects the row itself; there is no station to select)
    void clearSelection(bool keepBandRow = false);
    void autoSwitchMode(int submode);
    void clearCallsignSelected();  // legacy — calls clearSelection()
    void refreshTextDisplay();

    void manualBandHop(const StationList::Station station);

  private:
    Q_SIGNAL void apiSetMaxConnections(int n);
    Q_SIGNAL void apiSetServer(QString host, quint16 port);
    Q_SIGNAL void apiStartServer();
    Q_SIGNAL void apiStopServer();

    Q_SIGNAL void aprsClientEnqueueSpot(QString by_call, QString from_call,
                                        QString grid, QString comment);
    Q_SIGNAL void aprsClientEnqueueThirdParty(QString by_call,
                                              QString from_call, QString text);
    /**
     * @brief Send a standard APRS message ACK frame.
     * @param from_call Source callsign for the ACK.
     * @param to_call Destination callsign being acknowledged.
     * @param messageId APRS message identifier to acknowledge.
     */
    Q_SIGNAL void aprsClientEnqueueAck(QString from_call, QString to_call,
                                       QString messageId);
    Q_SIGNAL void aprsClientSetSkipPercent(float skipPercent);
    Q_SIGNAL void aprsClientSetIncomingRelayEnabled(bool enabled);
    Q_SIGNAL void aprsClientSetServer(QString host, quint16 port);
    Q_SIGNAL void aprsClientSetPaused(bool paused);
    Q_SIGNAL void aprsClientSetLocalStation(QString mycall, QString passcode);
    Q_SIGNAL void aprsClientSendReports();

    Q_SIGNAL void pskReporterSendReport(bool);
    Q_SIGNAL void pskReporterSetLocalStation(QString, QString, QString);
    Q_SIGNAL void pskReporterAddRemoteStation(QString, QString,
                                              Radio::Frequency, QString, int,
                                              QDateTime);

    Q_SIGNAL void spotClientSetLocalStation(QString, QString, QString);
    Q_SIGNAL void spotClientEnqueueCmd(QString, QString, QString, QString,
                                       QString, QString, QString, int, int, int,
                                       int);
    Q_SIGNAL void spotClientEnqueueSpot(QString, QString, int, int, int, int);

    Q_SIGNAL void decodedLineReady(QByteArray t);
    Q_SIGNAL void playNotification(const QString &name);
    Q_SIGNAL void initializeNotificationAudioOutputStream(AudioDeviceInfo const &,
                                                          unsigned) const;
    Q_SIGNAL void initializeAudioOutputStream(AudioDeviceInfo, unsigned channels,
                                              unsigned msBuffered) const;
    // [TODO #108 keep-warm] Emitted right after
    // initializeAudioOutputStream (startup + device change): opens the
    // output stream into KeepAlive silence so the first TX is a warm
    // restart. Queued to the same audio thread, so ordering after the
    // format set is guaranteed.
    Q_SIGNAL void warmStartAudioOutput(SoundOutput *,
                                       AudioDevice::Channel) const;
    Q_SIGNAL void stopAudioOutputStream() const;
    Q_SIGNAL void startAudioInputStream(AudioDeviceInfo const &,
                                        int framesPerBuffer, AudioDevice *sink,
                                        AudioDevice::Channel) const;
    Q_SIGNAL void suspendAudioInputStream() const;
    Q_SIGNAL void resumeAudioInputStream() const;
    Q_SIGNAL void startDetector(AudioDevice::Channel) const;
    Q_SIGNAL void FFTSize(unsigned) const;
    Q_SIGNAL void detectorClose() const;
    Q_SIGNAL void finished() const;
    Q_SIGNAL void transmitFrequency(double) const;
    Q_SIGNAL void endTransmitMessage(bool quick = false) const;
    Q_SIGNAL void tune(bool = true) const;
    Q_SIGNAL void sendMessage(double frequency, int submode, double txDelay,
                              SoundOutput *, AudioDevice::Channel) const;
    Q_SIGNAL void outAttenuationChanged(qreal) const;
    Q_SIGNAL void toggleShorthand() const;
    Q_SIGNAL void submodeChanged(Varicode::SubmodeType) const;

    Q_SIGNAL void messageAdded(int) const;

  private:
    QByteArray wisdomFileName() const;

    void writeAllTxt(QStringView message);
    void writeMsgTxt(QStringView message, int snr, int offset);

    void currentTextChanged();
    void tableSelectionChanged(QItemSelection const &, QItemSelection const &);
    void setupJS8();

    int freq() const { return m_freq; }

    /** Sabotage transmission if frequency would result in WSPR band. */
    void refuseToSendIn30mWSPRBand();

    void prepareSending(qint64 nowMS);

    /** Update the clock shown. */
    void updateClockUI(const QDateTime &);

    void setFreq(int);
    void transmit();

    bool presentlyWantHBReplies();

    QString m_nextFreeTextMsg;

    NetworkAccessManager m_network_manager;
    bool m_valid;
    [[maybe_unused]] bool m_multiple; // Used only in Windows builds
    MultiSettings *m_multi_settings;
    QPushButton *m_configurations_button;
    // [BUILD 298] m_arqButton DELETED. The persistent ARQ-on toggle
    // button is gone; ARQ is now opt-in per-message via the Send
    // options menu's "Send using ARQ" action. The internal
    // ui->actionModeReplicatorProtocol QAction still exists and is
    // still the canonical enable flag — the visible button just no
    // longer mirrors it. The "Send using ARQ" handler at
    // on_sendUsingArqAction_triggered toggles the QAction true for
    // the duration of one send, then sendComplete / sendFailed
    // disables it.
    // [FILE-XFER build 280 2026-06-16] Chevron QToolButton glued to the
    // right edge of ui->startTxButton. InstantPopup mode → click opens
    // a menu with "Send file…" (and any future send-action entries).
    // The standalone QPushButton "File" from build 276 is gone — file
    // send now lives inside this menu. Built programmatically in
    // UI_Constructor since the Send button itself comes from the .ui.
    // [FILE-XFER build 282] The "Send file…" action inside
    // the Send chevron's dropdown. Held as a member so the
    // updateTextDisplay / updateTxButtonDisplay paths can toggle its
    // enabled state — the chevron button itself stays enabled full-
    // time for discoverability; gating lives on the action.
    QAction *m_sendFileAction{nullptr};
    // [BUILD 338] "Send web link (URL)…" — below Send file. Prompts
    // for a URL, wraps it in link.txt, sends via ARQ file transfer
    // (receiver renders it clickable per TODO #95).
    QAction *m_sendWebLinkAction{nullptr};
    // [BUILD 298] "Send using ARQ" menu action — first item in the
    // Send options menu, replaces the standalone ARQ-toggle button.
    // Enable state is updated live from updateButtonDisplay using
    // evaluateArqGateForText.
    QAction *m_sendArqAction{nullptr};
    // [BUILD 331-visibleHail] "Send Visible Hail" menu action — third
    // item. One composite transmission: the encoded Subspace HAIL
    // frame (`<mycall>: @ALLCALL ACK`, 2.52 s) followed back-to-back
    // by two painted ⚡ diag frames (Hellschreiber-style audio
    // raster, 3.0 s each) — ~8.8 s total under a single PTT cycle.
    // The encoded ID + the visual together announce the operator's
    // presence to BOTH software-decoder AND eyeballs-on-waterfall
    // peers.
    QAction *m_sendVisibleHailAction{nullptr};
    // [BUILD 336 TODO #94] Visible Hail is ONE transmission: the
    // encoded HAIL frame and both diag bolts are concatenated into a
    // single composite waveform staged via the Modulator's full-frame
    // override, played under a single PTT key/unkey. The former
    // 3-step state machine (per-frame PTT re-key advanced from
    // stopTx side effects) lost frames when CAT-port contention
    // swallowed a re-key on slow machines. This flag is the sequence
    // lifecycle: set when the hail TX is armed, cleared in stopTx()
    // (or operator Halt). Guards the remote AVHAIL? trigger and the
    // menu-enable gate against double-fire while the TX is pending.
    bool m_visibleHailActive{false};
    // [BUILD 336 TODO #87] Submode to restore after a REMOTE-triggered
    // AVHAIL? completes (the trigger switches the receiver to Subspace
    // to transmit the hail; the operator's original mode speed comes
    // back afterward). -1 = no restore pending (manual menu hails
    // never set this — the operator chose Subspace deliberately).
    int m_visibleHailRestoreSubmode{-1};
    // [BUILD 336] Click-to-call: seed the outgoing box with
    // "<CALL> <standard greeting>" (Configuration::standard_greeting,
    // macros substituted). Shared by the Spots Map dot click and the
    // waterfall callsign-label double-click; callers translate the
    // result into their own feedback (toast / status message).
    // NOTE: plain private members, NOT in a slots section — moc
    // cannot parse an enum declaration there.
    enum class GreetingSeedResult { Seeded, DraftBlocked, InvalidCall };
    // force=true overwrites ANY draft in the outgoing box (waterfall
    // label dbl-click, Andy 2026-07-17); the ARQ box lock still wins.
    GreetingSeedResult trySeedOutgoingGreeting(QString const &call,
                                               bool force = false);
    // [BUILD 338] Shared by "Send file…" and "Send web link (URL)…":
    // pre-flight peer resolution (dialogs shown inside; empty return
    // = abort) and the transfer pipeline from a file path onward.
    QString resolveArqFilePeer();
    // requireLevel2: ICS-213 form sends refuse V1 outright — the
    // transfer aborts (with a notice) instead of falling back when
    // the peer is cached as V1, answers YES 1, or never answers.
    // formSparsePath: reply sends carry the sparse packet path; the
    // wire shape is chosen by the peer's answered level (>= 4 sparse/
    // trimmed, == 3 the complete document — graceful fallback).
    void startFileTransferViaArq(QString const &filePath,
                                 QString const &peer,
                                 bool requireLevel2 = false,
                                 QString const &formSparsePath =
                                     QString());
    QString formWirePath(QString const &fullPath,
                         QString const &sparsePath, int level) const;
    void notifyFormTransferAborted(QString const &peer,
                                   QString const &why); // [ICS213]
    // [BUILD 339 TODO #103] Session cache of peers' advertised ARQ
    // protocol levels, populated from "YES <level>" replies to
    // QUERY ARQ?. Key = FULL callsign (uppercased). A level >= 2
    // peer gets file-transfer wire-format V2; everyone else V1. A
    // reply WITHOUT a level never lands here — absence means V1-only
    // for the whole session (down-level peers can't change builds
    // without restarting, so never re-ask). Cleared only by app
    // restart.
    QHash<QString, int> m_peerArqLevel;
    // [BUILD 339 TODO #103] Auto-negotiation state: a file transfer
    // initiated toward a peer with UNKNOWN capability stashes here,
    // auto-sends "<peer> QUERY ARQ?", and resumes when the YES
    // <level> reply lands in the capture above (or falls back to V1
    // after one retry + timeout — silence is NOT cached; it may be
    // QRM, not a down-level build). Cache hits — either level —
    // skip the query entirely. FILE TRANSFERS ONLY (plain ARQ text
    // is format-agnostic).
    QString m_pendingFilePath;
    bool m_pendingRequiresV2{false}; // [ICS213] form send: V1 refused
    QString m_pendingFormSparsePath; // [ICS213] parked reply's packet
    QString m_pendingLinkUrl;   // set INSTEAD of path for link sends
    QString m_pendingFilePeer;
    int m_capQueryRetries{0};
    // Generation counter: every park/abort/resume bumps it, and each
    // armed timeout captures the value — a stale timer from an
    // earlier attempt can never act on a later attempt's pending
    // state (observed 2026-07-17: aborted attempt #1's 20 s timer
    // fired a bogus 1.7 s "retry" against attempt #2).
    int m_capQueryGen{0};
    // [2026-07-23 negophase] SINGLE WRITERS for the fields above. The
    // parked payload (UI's business — what to send) and the session
    // phase (the Manager's business — are we busy) were split across
    // two objects and drifted: the Manager never learned about
    // negotiation at all, so no lock, banner or busy flag fired for it.
    // These three are now the ONLY places the pending fields change,
    // and each one moves BOTH halves together. Do not clear the
    // m_pending* fields directly.
    //   begin — park the payload + open the session's negotiation phase
    //   take  — hand the payload back and close the phase (resume path)
    //   abort — drop the payload and close the phase (Halt / callsign)
    void beginCapabilityNegotiation(QString const &peer,
                                    QString const &filePath,
                                    QString const &linkUrl,
                                    bool requireLevel2 = false,
                                    QString const &formSparsePath =
                                        QString());
    // keepPhaseOpen: the resume path defers 1.5 s before it starts the
    // transfer. Closing the phase at take() time would drop every lock
    // for exactly that gap — the same hole this fix closes, in
    // miniature — so the resume path holds the phase open across the
    // defer and closes it with endCapabilityNegotiationPhase() once
    // the transfer owns the lock (m_sends non-empty) or has failed to
    // start. Callers that start synchronously pass false.
    bool takeCapabilityNegotiation(QString *filePath, QString *linkUrl,
                                   QString *peer,
                                   bool keepPhaseOpen = false);
    void endCapabilityNegotiationPhase();
    void abortCapabilityNegotiation(char const *why);
    bool capabilityNegotiationPending() const {
        return !m_pendingFilePath.isEmpty() || !m_pendingLinkUrl.isEmpty();
    }
    void onCapQueryTimeout(int gen);
    // [BUILD 352 capUnify] Arm the QUERY ARQ? reply window the same
    // way the ARQ ACK timer is armed: at TX-done (via the Manager's
    // TX-idle poller), sized by the ONE unified reply budget
    // (ChunkedArq::replyTimeoutMsForSubmode) at the submode in effect
    // when our query actually finished airing. Replaces three
    // enqueue-anchored QTimer::singleShot sites whose budget formula
    // was a compensator for the wrong anchor (see the deleted
    // capQueryTimeoutMsForSubmode's replacement comment in
    // ChunkedArq.h).
    void armCapQueryTimeout(int gen);
    void startFileTransferWithFormat(QString const &filePath,
                                     QString const &peer,
                                     int peerLevel);
    void sendWebLink(QString const &url, QString const &peer);
    // [BUILD 341] Directed-menu auto-send qualifier: after a menu
    // item populates the outgoing box, transmit IMMEDIATELY iff the
    // text classifies as a directed command — the same class the ARQ
    // gate excludes (bare token ± arg). Rationale (Andy 2026-07-17):
    // the explicit-Send requirement exists so the operator can choose
    // plain vs ARQ; for directed commands ARQ is not an option, so
    // waiting is pure friction. Free text, templates with unedited
    // placeholders, and custom reply text still populate-only.
    void autoSendIfDirectedCmd();
    // [TODO #107] V3 native-binary hooks: per-chunk TX (marker text +
    // injected raw frames), completed-transfer RX (envelope parse →
    // shared accept/save flow), and the extracted shared tail.
    void onNativeChunkWantToTransmit(QString const &peer,
                                     QString const &markerText,
                                     int chunkId, int totalChunks,
                                     QByteArray const &markerFrame9,
                                     QByteArray const &chunkBytes);
    void onNativeChunkCollected(QString const &peer, int chunkId,
                                int totalChunks);
    void onNativeMarkerSeen(QString const &peer, int chunkId,
                            int totalChunks);
    // [BUILD 354 rxsession] Pure renderer of the Manager's receive-
    // session machine (rxSessionChanged): Receiving → banner up with
    // (N/T); any other phase → default placeholder. Replaces the
    // refreshArqPlaceholder flag/timer machinery.
    void onRxSessionChanged(QString const &peer, int phase,
                            int chunkId, int totalChunks);
    void restoreArqPlaceholder();
    void onNativeBinaryMessageReceived(QString const &fromCall,
                                       QByteArray const &envelope,
                                       int msgId);
    void promptAndSaveReceivedFile(QString const &fromCall,
                                   FileTransfer::FileHeader const &header,
                                   QString const &payloadBase32,
                                   QByteArray const &payloadBytes,
                                   int wireVersion, int msgId);
    // [TODO #107 Phase 1 DEBUG — remove before push] Env-gated
    // (JS8_V3_DEBUG=1) test TX: marker + one 64-byte native chunk.
    void debugSendNativeTestChunk();
    // [TODO #107 Phase 2 DEBUG — remove before push] Burst experiment:
    // 8 back-to-back encoded frames as ONE composite waveform under a
    // single PTT (Visible Hail override mechanism). Level-4 gate: if
    // the receiver decodes all 8, single-PTT chunk bursts are viable.
    void debugSendNativeBurstChunk();
    // [BUILD 341] Send-chevron ARQ-validity indicator state (red =
    // current box text refuses ARQ). Cached so the stylesheet is
    // swapped only on TRANSITIONS — Build 309 proved per-frame
    // styling on this button lags.
    bool m_sendChevronRed{false};
    QAction *m_sendIcs213Action{nullptr}; // [ICS213]
    QPointer<class ICS213Dialog> m_ics213Dialog; // [ICS213] open form
    QPointer<class ICS213Dialog> m_ics213ReplyDialog; // [ICS213] reply
    void syncIcs213ArqGate(); // [ICS213] gate = m_sendFileAction state
    void openIcs213Reply(QString const &savedPath,
                         QString const &fromCall); // [ICS213]
    bool m_sendSideOn{true}; // [#148] see setSendSideEnabled
    void dispatchArqBody(QString const &body, QString const &peer,
                         int peerLevel);
    // [TODO #107] Binary sibling: raw V3 envelope via sendChunkedBinary.
    // [K-FALLBACK 2026-07-21] chunkBytes: K=8 default; the fail-dialog
    // "Retry with smaller sub-messages" offer re-enters at K=4.
    void dispatchArqBodyBinary(QByteArray const &envelope,
                               QString const &peer,
                               int chunkBytes =
                                   NativeBinary::DEFAULT_CHUNK_BYTES);
    // [BUILD 336 TODO #97] Wall-clock ms of the last accepted AVHAIL?
    // remote trigger — global once-per-hour rate limit / replay guard
    // (the protocol has no anti-replay; a replayed trigger must not
    // chain-key the station). 0 = never triggered this session.
    qint64 m_lastAvhailResponseMs{0};
    // [BUILD 298] When set non-zero, an in-flight chunked-ARQ send
    // was initiated by "Send using ARQ" (text), as opposed to
    // "Send file…" (file transfer). sendComplete / sendFailed on a
    // matching msgId disables ARQ. Independent of m_fileSendMsgId
    // which serves the same role for file sends.
    int  m_arqTextSendMsgId{0};
    // [FILE-XFER build 282] ARQ auto-enable bookkeeping. When the
    // operator picks "Send file…" and ARQ was off, we flip it on for
    // the duration of the transfer and restore the prior state on
    // sendComplete / sendFailed. Match is by msgId — a non-zero
    // m_fileSendMsgId means we're holding ARQ open for that send.
    int  m_fileSendMsgId{0};
    bool m_arqStateBeforeFileSend{false};
    // [K-FALLBACK 2026-07-21] Last dispatched V3 send, retained for
    // the one-shot "Retry with smaller sub-messages" (K=4) offer on a
    // timeout_exhausted failure — the V3 mirror of the V2 text path
    // restoring the failed body into the outgoing box. Consumed by
    // onChunkedSendFailed, cleared by onChunkedSendComplete.
    QByteArray m_v3SendEnvelope;
    QString    m_v3SendPeer;
    int        m_v3SendMsgId{0};
    int        m_v3SendChunkBytes{0};
    QSettings *m_settings;
    bool m_settings_read;
    QScopedPointer<Ui::UI_Constructor> ui;

    // other windows
    Configuration m_config;
    JS8MessageBox m_rigErrorMessageBox;

    QDockWidget* messageDock_ = nullptr;
    MessagePanel* messagePanel_ = nullptr;
    QDialog* messageFloatDialog_ = nullptr;

    enum class MessageHost { Dock, Dialog };
    MessageHost messageHost_ = MessageHost::Dock;

    QScopedPointer<WideGraph> m_wideGraph;
    QScopedPointer<LogQSO> m_logDlg;
    QScopedPointer<SpotMapWindow> m_spotMapWindow; // "Spots Map" view
    // [stamon, TODO #210] Per-station debug follow windows, keyed by
    // callsign; entries appear only via the env-gated right-click.
    QMap<QString, QPointer<class StationMonitorWindow>>
        m_stationMonitors;
    QScopedPointer<class ArqMonitor> m_arqMonitor; // [#153] passive
    QScopedPointer<class ArqMonitorWindow> m_arqMonitorWindow;
    QScopedPointer<HelpTextWindow> m_shortcuts;
    QScopedPointer<HelpTextWindow> m_prefixes;
    QScopedPointer<HelpTextWindow> m_mouseCmnds;

    Transceiver::TransceiverState m_rigState;
    Frequency m_lastDialFreq = 0;
    QString m_lastBand;

    Detector *m_detector;
    unsigned m_FFTSize = 0;
    SoundInput *m_soundInput;
    Modulator *m_modulator;
    SoundOutput *m_soundOutput;
    NotificationAudio *m_notification;

    // Configuration might one day offer to send a txDelayChanged signal.
    // As long as it doesn't, we poll and compare with the previous value.
    double m_TxDelay = 0.0; // in seconds.

    TxLoop *m_cq_loop;
    TxLoop *m_hb_loop;

    QThread m_networkThread;
    QThread m_audioThread;
    QThread m_notificationAudioThread;
    JS8::Decoder m_decoder;

    qint64 m_secBandChanged = 0;

    Frequency m_freqNominal = 0;
    Frequency m_freqTxNominal = 0;

    int m_freq = 0;

    qint32 m_XIT = 0;
    qint32 m_sec0 = 0;
    qint32 m_RxLog = 0;
    QDate m_lastAllTxtHeaderDate; // [alldate] last ALL.TXT header's UTC date
    // The period of the current submode, in seconds. (15 for normal, 10 for
    // fast, ...)
    qint32 m_TRperiod = 15;
    qint32 m_inGain = 0;
    qint32 m_idleMinutes = 0;
    qint32 m_nSubMode = 0;
    qint32 m_prevStandardSubmode = 0;  // saved standard mode for click-to-switch
    bool m_submodeChanging{false}; // guard against recursive mode switch
    FrequencyList_v3::const_iterator m_frequency_list_fcal_iter;
    qint32 m_i3bit = 0;

    bool m_btxok = false; // True if OK to transmit
    bool m_decoderBusy = false;
    QString m_decoderBusyBand;
    QMap<qint32, qint32>
        m_lastDecodeStartMap; // submode, decode k start position
    Radio::Frequency m_decoderBusyFreq = 0;
    QDateTime m_decoderBusyStartTime;
    bool m_auto = false;
    bool m_restart = false;
    bool m_bDecoded = false;
    int m_currentMessageType = 0;
    QString m_currentMessage;
    int m_currentMessageBits = 0;
    int m_lastMessageType = 0;
    QString m_lastMessageSent;
    bool m_tuneup = false;
    bool m_isTimeToSend = false;

    int m_ihsym = 0;
    float m_px = 0.0f;
    float m_pxmax = 0.0f;
    float m_df3 = 0.0f;
    quint32 m_iptt = 0;
    quint32 m_iptt0 = 0;
    bool m_btxok0 = false;
    double m_onAirFreq0 = 0.0;
    bool m_first_error = false;

    char m_msg[100][80];

    // labels in status bar
    QLabel tx_status_label;
    QLabel config_label;
    QLabel mode_label;
    QLabel last_tx_label;
    QLabel auto_tx_label;
    QProgressBar progressBar;
    QLabel wpm_label;

    // QPointer<QProcess> proc_js8;

    QTimer m_guiTimer;
    // Timer to switch off PTT after end of transmission.
    QTimer pttReleaseTimer;
    QTimer logQSOTimer;
    QTimer tuneButtonTimer;
    QTimer tuneATU_Timer;
    QTimer TxAgainTimer;
    QTimer minuteTimer;
    QString m_baseCall;
    QString m_hisCall;
    QString m_hisGrid;
    QString m_appDir;
    QString m_palette;
    QString m_rptSent;
    QString m_rptRcvd;
    QString m_msgSent0;
    QString m_opCall;

    struct CallDetail {
        QString call;
        QString through;
        QString grid;
        int dial;
        int offset;
        QDateTime cqTimestamp;
        QDateTime ackTimestamp;
        QDateTime utcTimestamp;
        int snr;
        int bits;
        float tdrift;
        int submode;
        // [BUILD 358 cppos] Ring position of the decode (0 = standard
        // decoder, no position). Lets compound-callsign pairing match
        // by ON-AIR order instead of arrival order — the async
        // decoder delivers out of order across passes.
        qint64 absPos{0};
    };

    struct CommandDetail {
        bool isCompound;
        bool isBuffered;
        QString from;
        QString to;
        QString cmd;
        int dial;
        int offset;
        QDateTime utcTimestamp;
        int snr;
        int bits;
        QString grid;
        QString text;
        QString extra;
        float tdrift;
        int submode;
        QString relayPath;
        // [TURNHOLD 2026-07-21] Ring position of this frame's Costas
        // (mirror of ActivityDetail::absPos; 0 = not set). The ARQ
        // ACK/NACK handler derives the peer's frame end-of-air from
        // it to time the inter-chunk turnaround hold.
        std::int64_t absPos{0};
    };

    struct ActivityDetail {
        bool isLowConfidence;
        bool isCompound;
        bool isDirected;
        bool isBuffered;
        int bits;
        int dial;
        int offset;
        QString text;
        QDateTime utcTimestamp;
        int snr;
        bool snrSuspect{false}; // true when single frame in buffer (noisy SNR estimate)
        bool shouldDisplay;
        float tdrift;
        int submode;
        // [BUILD 294] Absolute global sample-buffer position where
        // this frame's Costas was found (Subspace async decoder).
        // Sort key for processBufferedActivity assembly. 0 = not
        // populated (e.g., standard period-aligned decoder doesn't
        // set it). See Event::Decoded::absPos for derivation.
        std::int64_t absPos{0};
    };

    struct MessageBuffer {
        CommandDetail cmd;
        QQueue<CallDetail> compound;
        QList<ActivityDetail> msgs;
    };

    QString m_selectedCallsign;
    int m_bandActivityWidth;
    int m_callActivityWidth;
    int m_textActivityWidth;
    int m_waterfallHeight;
    bool m_bandActivityWasVisible;
    bool m_rxDirty;
    bool m_rxDisplayDirty = false;
    // Counter bumped in logCallActivity on every insert/update. The
    // once-per-second tick compares against m_callActivityRenderedVersion
    // and only does a full displayCallActivity rebuild when they differ —
    // otherwise it just rewrites the Age column in place. Keeps the
    // right pane ticking without burning Windows CPU on a full sort +
    // QTableWidget repopulate every second.
    int m_callActivityVersion = 0;
    int m_callActivityRenderedVersion = 0;
    int m_txFrameCountEstimate = 0;
    int m_txFrameCount = 0;
    int m_txFrameCountSent = 0;
    QTimer m_txTextDirtyDebounce;
    bool m_txTextDirty;
    QString m_txTextDirtyLastText;
    QString m_txTextDirtyLastSelectedCall;
    QString m_lastTxMessage;
    QString m_totalTxMessage;
    // [QUEUE PROVENANCE 2026-06-10 build 247]
    // Snapshot of extFreeTextMsgEdit's plain-text content RIGHT AFTER
    // processTxQueue called addMessageText to inject a system-built
    // reply (autoreply / relay forward / TCP API send). Used by startTx
    // to decide whether the current Send-button click is on operator-
    // typed text or queue-injected text. Matches the Build 205 design:
    // queue-injected text NEVER gets ARQ-wrapped, regardless of body
    // shape. Cleared at TX completion or once the operator edits the
    // text away from the snapshot.
    QString m_lastQueueInjectedText;
    // True when the queue-injected text above was an auto-reply (see
    // enqueueMessage). Lives and dies with the injection cycle: set at
    // injection, cleared in resetMessageUI. createMessageTransmitQueue
    // consults it so the frame-time attempt refresh stays silent for
    // auto-replies, matching the enqueue-time skip.
    bool m_lastQueueInjectedAutoReply = false;
    // [STATUS-BAR ARQ PROGRESS 2026-06-11 build 252]
    // Cache of the real "Last Tx: <message>" text in the leftmost
    // status-bar widget (last_tx_label), captured on the FIRST
    // onChunkedProgressUpdate of an ARQ session so we can restore it
    // when the session ends. Empty when no ARQ session is in flight.
    QString m_lastTxLabelCache;
    bool    m_lastTxLabelCacheValid{false};
    // [TODO.md #57 build 268] Per-response outgoing-text preservation.
    // Saved on the wantsResponseTx slot before the ACK / NACK text
    // gets written into the outgoing-msg widget; restored in stopTx()
    // once the response TX completes. Empty + flag-false when no
    // restore is pending.
    QString m_arqResponseSavedText;
    bool    m_arqResponseRestorePending{false};
    // [TODO #73 build 312] Pre-auto-switch submode, stashed when
    // the arq-modeFollow logic at processCommandActivity.cpp auto-
    // switches the receiver into the sender's mode for an inbound
    // chunk. Restored unconditionally 750 ms after the ACK / NACK
    // transmits (same hook as m_arqResponseSavedText). The speed
    // buttons are disabled during TX so the operator can't race a
    // manual mode change in. -1 = no stash pending.
    int     m_arqPreSwitchSubmode{-1};
    // [BUILD 341 arqPrompt] True while the guiUpdate session lock
    // holds the outgoing box read-only for an ARQ operation (active
    // TX session or parked capability negotiation). Drives
    // refreshOutgoingPlaceholder(): "MULTI-PART MSG IN PROGRESS..."
    // while locked, the standard selection-aware prompt otherwise.
    bool    m_arqBoxLocked{false};
    // [TODO #107] True for the duration of a native-binary (V3) send.
    // Gates prepareNextMessageFrame's typeahead refill, which clears
    // and rebuilds m_txFrameQueue from the text box — that would
    // vaporize queued binary frames. Set by the V3 TX hook / debug
    // sender; cleared on sendComplete/sendFailed/haltAll.
    bool    m_nativeBinaryTxActive{false};
    // [TODO #107] RX-side V3 feedback: outgoing-box placeholder swap
    // while a multi-part native transfer is inbound (restored by
    // timer when the next marker fails to appear, or on delivery).

    // [BUILD 355 oneban] Live RX-session banner text; empty = no
    // receive in progress. ONLY refreshOutgoingPlaceholder() renders
    // it (single-writer rule) — onRxSessionChanged just sets/clears.
    QString m_rxBannerText;

    // [TODO #107 Phase 2 DEBUG — remove before push] Burst-experiment
    // composite, staged into the Modulator's full-frame override by
    // guiUpdate's FT2 block (same flag pattern as Visible Hail).
    QVector<float> m_v3BurstWave;
    bool           m_v3BurstPending{false};
    // [TODO.md #58 build 268] Multi-mode RX runtime override.
    // Set true (set-once / sticky for the program run) when the ARQ
    // button is toggled on OR when the first inbound ARQ chunk is
    // auto-detected. Once true, never cleared until program exit.
    // Read by the legacy-decoder dispatch gate (mainwindow.cpp around
    // line 1891) to force FT2/Subspace decoding even when the
    // operator is in a legacy submode and actionModeMultiDecoder is
    // unchecked. Configuration / QSettings is never written.
    bool    m_arqMultiModeOverride{false};

    // moved from mainwindow.cpp, is used in multiple functions
    QString since(QDateTime const &time) {
        auto const delta = time.secsTo(DriftingDateTime::currentDateTimeUtc());

        if (delta >= 60 * 60 * 24)
            return QString("%1d").arg(delta / (60 * 60 * 24));
        else if (delta >= 60 * 60)
            return QString("%1h").arg(delta / (60 * 60));
        else if (delta >= 60)
            return QString("%1m").arg(delta / (60));
        else if (delta >= 15)
            return QString("%1s").arg(delta - (delta % 15));
        else
            return QString("now");
    }

    // [#161 querycall] Pending QUERY CALL sent by US. Key = askee
    // (UPPER; the relay chain's LAST head for relayed forms) or
    // "@ALLCALL" (wildcard — any responder binds, entry survives the
    // whole window for multiple YES replies). The wire reply never
    // names the target, so only this state can bind "YES +snr (age)"
    // to the station we asked about.
    // Timing (operator spec 2026-08-20, all at NORMAL speed):
    //   3 frames per hop, <= 3 hops out + 3 back
    //   window = 6 hops x 3 frames x 15 s = 270 s (+30 s margin)
    //   backdate = parsed age + inboundHops x 3 x 15 s
    struct PendingCallQuery {
        QString target;
        int hops{1};     // relay heads outbound (reply retraces them)
        qint64 sentMs{};
    };
    QHash<QString, PendingCallQuery> m_pendingCallQueries;
    // [#178] The message as COMPOSED, kept for the query capture.
    // m_totalTxMessage is assembled frame by frame and does not hold
    // the whole thing at end of transmission -- instrumented
    // 2026-08-25, it contained "AI5TS? FC5" for a query whose first
    // frame was "WM8Q: @ALLCALL QUERY CALL". Composition is the one
    // place the text is known complete.
    QString m_lastComposedMessage;
    static constexpr int kQCallFrameSecs = 15;   // NORMAL frame
    static constexpr int kQCallFramesPerHop = 3;
    static constexpr qint64 kQCallReplyWindowMs =
        (3 + 3) * kQCallFramesPerHop * kQCallFrameSecs * 1000 + 30000;
    void captureOutgoingCallQuery(QString const &sentMsg);
    // Returns true when the reply text bound to a pending query
    // (age/snr parsed, hearing edge fed backdated).
    bool bindCallQueryReply(QString const &responder,
                            QString const &replyText, int dial);

    // [reachport 2026-08-27] THE REACHING EXECUTOR, ported from the
    // frozen python (tools/js8reach @ c6e2e7e3) after a night proved
    // the split-brain problem: reply-knowledge lived in five places
    // over the TCP straw. Here each fact has its native owner: the
    // hearing store is the model (bindCallQueryReply already files
    // age-graded YES edges), the assembler is the watcher substrate,
    // stopTx is the TX-end anchor. Engine body: reachExecutor.cpp.
    struct ReachWatcher {
        qint64 lastMs = 0;   // last frame delivery on this offset
        bool   done = false; // assembled
        // DEAD STAYS DEAD (operator, 2026-08-27): a frameless slot
        // means a frame is MISSING and the reply can never assemble
        // cleanly -- a later trailing frame must not resurrect the
        // watcher and stretch the wait (MM7MMU cost +76s for a reply
        // whose claim was already in frame 1).
        bool   dead = false;
    };
    struct ReachState {
        bool        active = false;
        QString     target;         // LITERAL, upper (affixes kept)
        QString     band;
        int         moveNo = 0;
        int         sent = 0;
        int         maxMoves = 6;
        // [#196] g_book.learned size at attempt start -- exact
        // "learned during THIS attempt" counter for the failure
        // dialog's will/can line (append-time truth; edge whenMs can
        // be backdated by QUERY CALL ages, so it cannot serve).
        int         learnedAt0 = 0;
        // [structured 2026-08-31] computed expected frame counts
        // (reachReplyFrames = the packer, one owner; compound calls
        // native) + the flag-check state for the forward watch.
        int         fwdExpFrames = 0;
        int         ansExpFrames = 0;
        int         retExpFrames = 0;
        // The return window actually armed (slots), so the verdict
        // names the REAL number even if congestion moved since.
        int         retWindowSlots = 0;
        int         fwdFrames = 0;
        int         fwdOffset = 0;
        QString     forcedVerdict;
        QString     kind;           // "snr" | "shout" | "relay"
        QString     via;            // relay move only: chain[0]
        QStringList chain;          // full relay chain (excl. target)
        QStringList triedRelays;
        bool        triedDirect = false;
        bool        triedShout = false;
        qint64      startMs = 0;
        qint64      txEndMs = 0;    // signal end (stopTx, per spec)
        qint64      deadlineMs = 0;
        qint64      fwdStartedMs = 0;  // via keyed in the first slot
        qint64      fwdDoneMs = 0;     // forward assembled (checksum)
        qint64      retStartedMs = 0;  // via keyed again: return leg
        int         retOffset = 0;     // return rides this offset
        qint64      ansStartedMs = 0;
        QMap<int, ReachWatcher> watchers;   // offset -> watcher
        int         savedSubmode = -1;      // speed restored on stop
        QString     lastWire;
        // [reachport2] full-port state (audit 2026-08-27)
        QString     forcedVia;              // --via equivalent
        QStringList pastVias;               // late-forward memory
        qint64      holdUntilMs = 0;        // late-forward TX hold
        qint64      moveCapMs = 0;          // 330 s per-move hard cap
        QHash<QString, qint64> triedAt;     // "kind:who" -> ms tried
        QStringList askedHearing, askedGrid;
        bool        relaysBlocked = false;  // freshgate: RESTRICTED
        qint64      gateSilenceMs = 0;       // latest delivered-silent
        // Proven deliveries that went unanswered THIS attempt
        // (attempt-scope, reset by reachStart's fresh struct only).
        // At >= 2 -- the freshgate's own standard -- budget
        // extensions stop buying re-tests of a settled question.
        int         silentDeliveries = 0;
    };
    ReachState m_reach;
    // [#187 intelminer] Background corpus build from the user's own
    // logs; full re-mine per run (mine.py semantics), skipped when
    // the logs are unchanged. See JS8_Main/IntelMiner.h.
    bool m_intelMineRunning = false;
    class QThread *m_intelMineThread = nullptr; // joined at shutdown
    void startIntelMine(bool force);
    // [manualask 2026-08-29, operator: "manual relay is a worthwhile
    // probe"] Deadline-keeper for HAND-SENT relay asks: a forward is
    // a decodable event (the *DE* journal records it whoever asked),
    // but a failure is the ABSENCE of one, which only becomes a fact
    // when someone waits with a deadline. The executor waits for its
    // own asks; this list waits for manual ones. Armed at TX
    // frame-build for chain sends while the executor is idle;
    // cleared by the *DE* success journal; expired entries journal a
    // failed ask after mine.py's own ask-to-forward settle window.
    struct ManualAsk {
        QString via;
        qint64 askedMs = 0;
    };
    QVector<ManualAsk> m_manualAsks;
    // [congestion 2026-08-31] slot ids (epochSecs / current period)
    // that contained >=1 decoded frame; pruned in the recorder.
    mutable QSet<qint64> m_congestionSlots;
    // [#207 waitopts] Response-wait mode for the auto-route
    // executor's two busy-conditional points: 0 Short (never the
    // extra slot), 1 Adaptive on-air, 2 Long (always), 3 Adaptive
    // PSKR (falls back to on-air while MQTT data is absent). Owner
    // of all three facts; the map's Options dialog reads/writes
    // through the probe/sink pair. Persisted.
    int m_reachWaitMode = 1;
    int m_reachBusyThreshold = 7;
    int m_reachBusyThresholdPskr = 7;
    static constexpr qint64 kManualAskSettleMs =
        900 * 1000; // mine.py:267 settle_relays' 15-minute window
    // [autoroute 2026-08-28] Auto-route mode: the map picked a
    // target and the reaching executor runs it with the main screen
    // locked exactly as for ARQ mode (the flag is folded into the
    // same gate predicates). m_autoRouteCancel marks an operator
    // cancel so reachStop routes to the "canceled" toast instead of
    // the failure dialog.
    bool m_autoRouteActive = false;
    bool m_autoRouteCancel = false;
    QString m_autoRouteTarget;
    void autoRouteBegin(QString const &target);
    void autoRouteCancel();
    void autoRouteReachStopped(QString const &reason);
    // [TODO #177] The exact relayed reply that closed an attempt as
    // REACHED; the relay-ACK composer skips this one message and
    // clears the pair. Empty otherwise.
    QString m_reachAnsweredFrom;
    QString m_reachAnsweredText;
    QTimer *m_reachTimer = nullptr;
    void reachStart(QString const &target, int maxMoves = 6,
                    QString const &via = QString{});
    void reachStop(QString const &reason);
    void reachRestoreSpeed();
    void reachNextMove();
    void reachExplain(void const *cand);
    QString reachResolveGrid(QString const &square);
    void reachRefreshBook();
    void reachSend(QString const &wire);
    void reachOnTxComplete();
    void reachOnFrame(ActivityDetail const &d);
    void reachOnDirected(CommandDetail const &d, QString const &line);
    void reachTick();
    void reachArmTimer();
    void reachLog(QString const &line);
    qint64 reachSlotEndMs(qint64 tMs, int n) const;
    int reachReplyFrames(QString const &from, QString const &text) const;
    double reachPLink(qint64 whenMs, int snr) const;
    void reachPlaceTemplate(QStringList const &path);

    QDateTime m_lastTxStartTime;
    QDateTime m_lastTxStopTime;
    QDateTime m_txQueueStartTime;  // wall-clock when first frame TX began (for countdown)
    // moved from mainwindow.cpp, is used in multiple functions
    auto replaceMacros(QString const &text,
                       QMap<QString, QString> const &values, bool const prune) {
        QString output = text;

        for (auto const [key, value] : values.asKeyValueRange()) {
            output = output.replace(key, value.toUpper());
        }

        return prune ? output.replace(QRegularExpression("[<](?:[^>]+)[>]"), "")
                     : output;
    }

    QList<int> generateOffsets(int minOffset, int maxOffset) {
        QList<int> offsets;

        // TODO: these offsets aren't ordered correctly...

        for (int i = minOffset; i <= maxOffset; i++) {
            offsets.append(i);
        }
        return offsets;
    }

    enum Priority {
        PriorityLow = 10,
        PriorityNormal = 100,
        PriorityHigh = 1000
    };

    struct PrioritizedMessage {
        QDateTime date;
        int priority;
        QString message;
        int offset;
        Callback callback;
        bool autoReply = false; // see enqueueMessage

        friend bool operator<(PrioritizedMessage const &a,
                              PrioritizedMessage const &b) {
            if (a.priority < b.priority) {
                return true;
            }
            return a.date < b.date;
        }
    };

    struct CachedDirectedType {
        bool isAllcall;
        QDateTime date;
    };

    struct DecodeParams {
        int submode;
        int start;
        int sz;
    };

    struct FrameCacheKey {
        int submode;
        QString frame;

        FrameCacheKey(int submode, QString frame)
            : submode(submode), frame(std::move(frame)) {}

        bool operator==(FrameCacheKey const &) const noexcept = default;

        struct Hash {
            std::size_t operator()(FrameCacheKey const &key) const noexcept {
                std::size_t const h1 = std::hash<int>{}(key.submode);
                std::size_t const h2 = qHash(key.frame);
                return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
            }
        };
    };

    // [POS-DEDUP 2026-07-14 TODO #72] Dedup identity is the monotonic
    // L2 buffer position (absPos), per the design intent of the
    // monotonic ring counter. Each cache entry keeps the last few
    // occurrences of a frame's bits: a new decode is a duplicate IFF
    // its absPos falls within one frame-length of a stored occurrence
    // (same physical transmission re-decoded from the ring). Same
    // bits at a distant position = a genuinely new transmission (ARQ
    // chunks all share a bit-identical first frame: "K9AVT: WM8Q " is
    // exactly one frame) and must pass regardless of arrival time.
    // Frames without absPos (standard period decoder) fall back to
    // the legacy time-window check via `when`.
    struct FrameOccurrence {
        QDateTime    when;
        std::int64_t absPos;   // 0 = unknown (standard decoder)
    };
    struct FrameCacheEntry {
        static constexpr int MAX_OCC = 3;
        FrameOccurrence occ[MAX_OCC];
        int n = 0;
        void add(FrameOccurrence const &o) {
            // keep the newest MAX_OCC occurrences (shift-down ring)
            for (int i = std::min(n, MAX_OCC - 1); i > 0; --i)
                occ[i] = occ[i - 1];
            occ[0] = o;
            n = std::min(n + 1, MAX_OCC);
        }
    };
    using FrameCache =
        std::unordered_map<FrameCacheKey, FrameCacheEntry,
                           FrameCacheKey::Hash>;
    using BandActivity = QMap<int, QList<ActivityDetail>>;

    // [bandcall] Band activity column indices -- ONE authority for the
    // table layout; never index tableWidgetRXAll with a literal.
    // Column 0 (Callsign) is hidden in offset mode, shown in callsign
    // mode ("List by..." on the header menu).
    enum BACol : int {
        BACallsign = 0,
        BAOffset   = 1,
        BATDrift   = 2,
        BAAge      = 3,
        BASnr      = 4,
        BASpeed    = 5,
        BAMessage  = 6,
    };

    QQueue<DecodeParams> m_decoderQueue;
    FrameCache m_messageDupeCache;  // submode, frame -> date seen
    QVariantMap m_showColumnsCache; // table column:key -> show boolean
    QVariantMap m_sortCache;        // table key -> sort by
    // [bandcall] Band activity listing mode: "offset" (classic, one
    // row per offset) or "call" (one row per station). Persisted as
    // Common/ListBy.
    QString m_bandListBy = QStringLiteral("offset");
    QPriorityQueue<PrioritizedMessage> m_txMessageQueue; // messages to be sent
    QQueue<QPair<QString, int>> m_txFrameQueue;          // frames to be sent
    QQueue<ActivityDetail> m_rxActivityQueue; // all rx activity queue
    QQueue<CommandDetail>
        m_rxCommandQueue; // command queue for processing commands
    QQueue<CallDetail>
        m_rxCallQueue; // call detail queue for spots to pskreporter
    QMap<QString, QString>
        m_compoundCallCache; // base callsign -> compound callsign
    QCache<QString, QDateTime> m_txAllcallCommandCache; // callsign -> last tx
    QCache<int, QDateTime> m_rxRecentCache;             // freq -> last rx
    QCache<int, CachedDirectedType>
        m_rxDirectedCache;                // freq -> last directed rx
    QCache<QString, int> m_rxCallCache;   // call -> last freq seen
    QMap<int, int> m_rxFrameBlockNumbers; // freq -> block
    BandActivity m_bandActivity; // freq -> [(text, last timestamp), ...]
    QMap<int, MessageBuffer> m_messageBuffer; // freq -> (cmd, [frames, ...])
    // [EARLY-FRAMES 2026-07-22] Data frames decoded BEFORE the header
    // frame that opens their message buffer. Until now such a frame was
    // silently DISCARDED (it only reached band activity), because
    // buffering required an already-established buffer — so a message
    // assembled one frame short, failed CRC, and every retransmission
    // failed IDENTICALLY (the same audio yields the same decode order),
    // making the chunk permanently undeliverable. Proven on air
    // 2026-07-22: sub-msg 4/7 lost exactly "AYDAMGKF" four times.
    //
    // This is the text-path mirror of the V3 native ORPHAN STORE, which
    // already solves the same problem for binary frames: hold what you
    // can't place yet, drain it when the owner appears, and let the
    // monotonic ring position decide order. Bounded by age AND count so
    // stray traffic from other stations can't accumulate.
    QList<ActivityDetail> m_earlyTextFrames;
    void holdEarlyTextFrame(ActivityDetail const &d);
    void drainEarlyTextFrames(int submode, int offset,
                              std::int64_t headerAbsPos);
    int m_lastClosedMessageBufferOffset;
    QMap<QString, CallDetail>
        m_callActivity; // call -> (last freq, last timestamp)

    QMap<int, QString> m_origRxHeaderLabelMap;           // colIndex, label
    QMap<int, QString> m_origCallActivityHeaderLabelMap; // colIndex, label
    QMap<QString, QString> m_columnLabelMap;             // full, minimal

    QMap<QString, QSet<QString>>
        m_heardGraphOutgoing; // callsign -> [stations who've this callsign has
                              // heard]
    QMap<QString, QSet<QString>>
        m_heardGraphIncoming; // callsign -> [stations who've heard this
                              // callsign]

    QMap<QString, int> m_rxInboxCountCache; // call -> count


#ifdef JS8_ENABLE_FT2
    // L2 async decode infrastructure
    QTimer m_l2DecodeTimer;                     // fires every 750ms
    QFutureWatcher<void> m_l2DecodeWatcher;     // monitors async decode
    std::int16_t m_l2RingBuf[FT2_L2_RINGSIZE] = {};  // 7.5s ring buffer (90000 samples, 2 periods)
    // Write position as a monotonic 64-bit sample counter. Indexing into
    // the ring uses (pos % FT2_L2_RINGSIZE). Prior impl wrapped at 180000
    // back to 90000, which made known-frame expiration unreliable: if the
    // wrap happened between two expiration checks (every ~380 ms), the
    // next check saw a small positive delta and concluded the entry was
    // fresh -- losing track of how many full cycles had elapsed. A
    // monotonic counter makes subtraction unambiguous and expiration
    // correct at any sampling cadence.
    std::atomic<std::int64_t> m_l2RingPos{0};   // monotonic sample count (audio thread writes, main thread reads)
    // [BUILD 356 ringpurge] Samples older than this monotonic position
    // are own-TX-era — presented as silence to the L2 decoder (see
    // l2TryDecode linearization). Main thread only (set at per-frame
    // TX end, read at decode launch) — no atomics needed.
    std::int64_t m_l2ZeroBeforePos{0};
    bool m_l2Decoding = false;                  // decode in progress
    bool m_l2Enabled = false;                   // L2 decode active
    qint64 m_l2DecodeFinishedMs = 0;            // timestamp when decode thread finished
    // [TODO #113/#120 2026-07-24 l2watch] The decoder is gated behind
    // two latches (m_l2Decoding, DecodeFT2::fortranLock) that are
    // released ONLY on the async task's success path. If that task
    // never completes — or its finished() signal is lost — both stay
    // set and l2TryDecode returns early FOREVER: decoder dead, capture
    // still warm, waterfall still painting, not one log line. Proven
    // in the field 2026-07-24 (sender log 020026Z: zero decodes from
    // 02:12:05 to session end while transmitting normally; only an app
    // restart recovered it). The 2 s timer was called a watchdog but
    // merely re-invoked the same gated function, so it could never
    // detect or clear either latch. These give it something to watch.
    // Normal decode is ~1-2 s; 30 s is far outside any legitimate run
    // (the whole L2 ring is only 7.5 s of audio) yet short enough that
    // the operator is told within one over.
    static constexpr qint64 L2_DECODE_STUCK_MS = 30000;
    qint64 m_l2DecodeStartedMs = 0;             // when m_l2Decoding was set
    qint64 m_l2LastGateLogMs   = 0;             // rate-limit for gate diagnostics
    bool   m_l2StuckWarned     = false;         // one warning per stuck episode
    void   l2DecodeWatchdogCheck();             // detect + recover stuck latches
    void l2DecodeDone();                        // called when async decode finishes
    void l2TryDecode(char const *source);       // attempt to start an L2 decode

    // L2 known-frame suppression: pass last decoded frame's raw bits to
    // the decoder so it skips re-decoding the same content
    std::int8_t m_l2KnownBits[77 * 20] = {};   // known frames' raw bits
    std::int64_t m_l2KnownPos[20] = {};         // monotonic pos when each frame was added
    int m_l2NKnown = 0;                         // number of known frames
    int m_l2SignalFreq = 0;                     // last decoded signal freq (Hz), 0=unknown
    int m_l2EmptyCycles = 0;                    // consecutive empty decode cycles

    // L2 deduplication (5s window, best SNR wins)
    struct L2DedupeEntry { int snr; qint64 msec; };
    QMap<QString, L2DedupeEntry> m_l2Dedup;
    qint64 m_l2DedupLastPurge = 0;
#endif

    QMap<QString, QMap<QString, CallDetail>>
        m_callActivityBandCache; // band -> call activity
    QMap<QString, QMap<int, QList<ActivityDetail>>>
        m_bandActivityBandCache;              // band -> band activity
    QMap<QString, QString> m_rxTextBandCache; // band -> rx text
    QMap<QString, QMap<QString, QSet<QString>>>
        m_heardGraphOutgoingBandCache; // band -> heard in
    QMap<QString, QMap<QString, QSet<QString>>>
        m_heardGraphIncomingBandCache; // band -> heard out

    QMap<QString, QDateTime>
        m_callSelectedTime; // call -> timestamp when callsign was last selected
    /**
     * Cache of recently seen APRS relay keys used to suppress duplicates.
     * Key format: "TO|TEXT|SENDER" (uppercased), value is last seen UTC.
     */
    QHash<QString, QDateTime> m_aprsRelayDedupCache;
    QSet<QString> m_callSeenHeartbeat; // call
    int m_previousFreq;
    bool m_shouldRestoreFreq = false;
    bool m_bandHopped = false;
    Frequency m_bandHoppedFreq = 0;

    /** Repeat period of HBs, in seconds. */
    int m_hbInterval;
    /** Repeat period of CQ calls, in seconds. */
    int m_cqInterval;

    /** Whether to resume HBs at the next opportunity. */
    bool m_hbPaused;
    /** Whether the current mode supports heartbeats. */
    bool m_hbModeAvailable{true};

    QDateTime m_dateTimeQSOOn;
    QDateTime m_dateTimeLastTX;

    LogBook m_logBook;
    unsigned m_msAudioOutputBuffered;
    unsigned m_framesAudioInputBuffered = 0;
    QThread::Priority m_audioThreadPriority = QThread::HighPriority;
    QThread::Priority m_notificationAudioThreadPriority = QThread::LowPriority;
    QThread::Priority m_decoderThreadPriority = QThread::HighPriority;
    QThread::Priority m_networkThreadPriority = QThread::LowPriority;
    bool m_splitMode = false;
    bool m_monitoring = false;
    bool m_generateAudioWhenPttConfirmedByTX = false;
    bool m_transmitting = false;
    bool m_tune = false;
    bool m_tx_watchdog = false; // true when watchdog triggered
    bool m_block_pwr_tooltip = false;
    bool m_PwrBandSetOK = true;
    Frequency m_lastMonitoredFrequency = 0;
    MessageClient *m_messageClient;
    MessageServer *m_messageServer;
    ChunkedArq::Manager *m_chunkedArq{nullptr};
    WSJTXMessageClient *m_wsjtxMessageClient;
    WSJTXMessageMapper *m_wsjtxMessageMapper;
    TCPClient *m_n3fjpClient;
    PSKReporter *m_pskReporter;
    SpotClient *m_spotClient;
    APRSISClient *m_aprsClient;
    AprsInboundRelay *m_aprsInboundRelay;
    QVariantHash m_pwrBandTxMemory; // Remembers power level by band
    QVariantHash
        m_pwrBandTuneMemory; // Remembers power level by band for tuning
    QByteArray m_geometryNoControls;

    //---------------------------------------------------- private functions
    void readSettings();
    void set_application_font(QFont const &);
    void writeSettings();
    void createStatusBar();
    void statusChanged();
    void rigFailure(QString const &reason);
    void spotSetLocal();
    void pskSetLocal();
    void aprsSetLocal();
    void spotReport(int submode, int dial, int offset, int snr,
                    QString const &callsign, QString const &grid);
    void spotCmd(CommandDetail const &cmd);
    void spotAprsCmd(CommandDetail const &cmd);
    void pskLogReport(QString const &mode, int dial, int offset, int snr,
                      QString const &callsign, QString const &grid,
                      QDateTime const &utcTimestamp);
    void spotAprsGrid(int dial, int offset, int snr, QString callsign,
                      QString grid);
    Radio::Frequency dialFrequency();
    void setSubmode(int submode);     // full reconfiguration (radio, FFT, tables)
    // switchSubmode() removed — use setSubmode() for all mode changes
    void updateCurrentBand();
    void displayDialFrequency();
    void transmitDisplay(bool);
    void postDecode(bool is_new, QString const &message);
    void displayTransmit();
    void updateModeButtonText();
    void updateButtonDisplay();
    // [BUILD 341 arqPrompt] ONE writer for the outgoing box's
    // placeholder: ARQ-locked banner > directed-to-<call> prompt >
    // generic prompt. Callers: the guiUpdate ARQ lock transitions,
    // selectCallsign(), clearSelection().
    void refreshOutgoingPlaceholder();
    // [TODO.md #67 build 272] Tri-state for the ARQ button's live
    // armed/not-armed visual, mirroring the full TX-time gate logic
    // at on_startTxButton_clicked (mainwindow.cpp ~3485-3629).
    enum class ArqGateState {
        NotArmed_NoPeer,       // no valid peer from selection or text
        NotArmed_DirectedCmd,  // packs as directed cmd (excl MSG/MSG TO:/relay)
        Armed                  // would go via ARQ on the next Send
    };
    ArqGateState evaluateArqGateForText(QString const &text) const;
    void updateTextDisplay();
    void updateTextWordCheckerDisplay();
    void updateTextStatsDisplay(QString text, int count);
    void updateTxButtonDisplay();
    bool isMyCallIncluded(QString const &text);
    bool isAllCallIncluded(QString const &text);
    bool isGroupCallIncluded(const QString &text);
    QString callsignSelected(bool useInputText = false);
    bool isRecentOffset(int submode, int offset);
    void markOffsetRecent(int offset);
    bool isDirectedOffset(int offset, bool *pIsAllCall);
    void markOffsetDirected(int offset, bool isAllCall);
    void clearOffsetDirected(int offset);
    void processActivity(bool force = false);
    void processRxActivity();
    void processIdleActivity();
    void processCompoundActivity();
    void processBufferedActivity();
    void processCommandActivity();
    QString inboxPath();
    void refreshInboxCounts();
    bool hasMessageHistory(QString call);
    int addCommandToMyInbox(CommandDetail d);
    int addCommandToStorage(QString type, CommandDetail d);
    int getNextMessageIdForCallsign(QString callsign);
    int getLookaheadMessageIdForCallsign(QString callsign, int afterMsgId);
    int getNextGroupMessageIdForCallsign(QString group_name, QString callsign);
    int getLookaheadGroupMessageIdForCallsign(QString group_name,
                                              QString callsign, int afterMsgId);
    int countUnreadForCallsign(const QString &callsign);
    int countGroupUnreadForCallsign(const QString &group_name,
                                    const QString &callsign);
    bool markGroupMsgDeliveredForCallsign(int msgId, QString callsign);
    bool markMsgDelivered(int mid, Message msg);
    QStringList parseRelayPathCallsigns(QString from, QString text);
    void processSpots();
    void processTxQueue();
    void displayActivity(bool force = false);
    void displayBandActivity();
    void displayCallActivity();
    // Cheap per-second Age-only refresh of the callsign list. Walks
    // existing rows and rewrites the Age cell in place — no sort, no
    // allocations, no table rebuild. Used when m_callActivityVersion
    // has NOT advanced since the last full render.
    void refreshCallActivityAgeOnly();
    void enable_DXCC_entity(bool on);
    void setRig(Frequency = 0); // zero frequency means no change
    QDateTime nextTransmitCycle();
    void statusUpdate();
    void on_the_minute();
    void tryBandHop();
    void add_child_to_event_filter(QObject *);
    void remove_child_from_event_filter(QObject *);
    void setup_status_bar();
    QString columnLabel(QString defaultLabel);
    void ensureMessageDock();

    void resetIdleTimer();
    void incrementIdleTimer();
    void tx_watchdog(bool triggered);
    void write_frequency_entry(QString const &file_name);
    void write_transmit_entry(QString const &file_name);
};

#endif // MAINWINDOW_H
