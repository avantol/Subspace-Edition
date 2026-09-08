/**
 * \file UI_Constructor.cpp
 * @brief explicit member function of the UI_Constructor class
 *   constructs and connects UI elements to the JS8 "engine"
 */

// [FT2-L2 ASYNC TOGGLE — build 291 RE-ENABLED]
// The "no gap on receiver waterfall" symptom that motivated the
// build 285 / 290 disable was determined to be a receiver-side
// display artifact (other audio processing smearing the waterfall),
// NOT a wire-level issue. Wired-loopback audio capture confirmed
// sender output is clean in both ARQ-relaxed and non-relaxed modes
// — no audible sputter. So the disable was solving a problem that
// didn't exist on the TX side. Async receive is back on; the
// diagnostic toggle is preserved.
// #define JS8_DISABLE_L2_ASYNC 1

#include "JS8_UI/mainwindow.h"
#include "JS8_UI/SpeechBalloon.h"
#include "JS8_Main/ArqMonitor.h"
#include "JS8_UI/ArqMonitorWindow.h"
#include "JS8_UI/StationMonitorWindow.h" // [stamon]
#include "JS8_Widgets/BandActivityMessageDelegate.h"

#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QMenu>
#include <QToolButton>
#include <QLabel>
#include <QUrl>

#include "JS8_Main/FileTransfer.h"

#ifdef JS8_ENABLE_FT2
#include "JS8_Mode/ft2_bridge.h"
#include "JS8_Mode/DecodeFT2.h"
#include "JS8_Mode/SubspacePreamble.h"
#endif

int ms_minute_error() {
    auto const now = DriftingDateTime::currentDateTimeLocal();
    auto const time = now.time();
    auto const second = time.second();

    return now.msecsTo(now.addSecs(second > 30 ? 60 - second : -second)) -
           time.msec();
}

//--------------------------------------------------- UI_Constructor
UI_Constructor::UI_Constructor(QString const &program_info,
                               QDir const &temp_directory, bool const multiple,
                               MultiSettings *multi_settings, QWidget *parent)
    : QMainWindow(parent),
      m_hbButtonIsLongterm{true}, m_cqButtonIsLongterm{true},
      m_network_manager{this}, m_valid{true}, m_multiple{multiple},
      m_multi_settings{multi_settings}, m_configurations_button{0},
      m_settings{multi_settings->settings()}, m_settings_read{false},
      ui(new Ui::UI_Constructor), m_config{temp_directory, m_settings, this},
      m_rigErrorMessageBox{JS8MessageBox::Critical, tr("Rig Control Error"),
                           JS8MessageBox::Cancel | JS8MessageBox::Ok |
                               JS8MessageBox::Retry},
      m_wideGraph(new WideGraph(m_settings)),
      // no parent so that it has a taskbar icon
      m_logDlg(new LogQSO(program_title(), m_settings, &m_config, nullptr)),
      // no parent so that it has a taskbar icon
      m_spotMapWindow(new SpotMapWindow(m_settings, &m_config, nullptr)),
      // [#153] Passive overheard-transfer assembler + its window (the
      // window IS the monitoring switch). No parent = taskbar icon,
      // same as the map.
      m_arqMonitor(new ArqMonitor(nullptr)),
      m_arqMonitorWindow(
          new ArqMonitorWindow(m_settings, m_arqMonitor.data(),
                               nullptr)),
      m_lastDialFreq{0},
      m_detector{new Detector{JS8_RX_SAMPLE_RATE, JS8_NTMAX}},
      m_FFTSize{6912 / 2}, // conservative value to avoid buffer overruns
      m_soundInput{new SoundInput}, m_modulator{new Modulator},
      m_soundOutput{new SoundOutput("AUDIO-TX")}, m_notification{new NotificationAudio},
      m_cq_loop{new TxLoop{"CQ calls"}}, m_hb_loop{new TxLoop{"HB calls"}},
      m_decoder{this}, m_secBandChanged{0}, m_freqNominal{0},
      m_freqTxNominal{0}, m_XIT{0}, m_sec0{-1},
      m_RxLog{1}, // Write Date and Time to RxLog
      m_TRperiod{60}, m_inGain{0}, m_idleMinutes{0},
      m_nSubMode{Default::SUBMODE}, m_prevStandardSubmode{Default::SUBMODE},
      m_frequency_list_fcal_iter{m_config.frequencies()->begin()}, m_i3bit{0},
      m_btxok{false}, m_auto{false}, m_restart{false}, m_currentMessageType{-1},
      m_lastMessageType{-1}, m_tuneup{false}, m_isTimeToSend{false}, m_ihsym{0},
      m_px{0.0}, m_iptt0{0}, m_btxok0{false}, m_onAirFreq0{0.0},
      m_first_error{true}, tx_status_label{"Receiving"},
      m_appDir{QApplication::applicationDirPath()}, m_palette{"Linrad"},
      m_txFrameCountEstimate{0}, m_txFrameCount{0}, m_txFrameCountSent{0},
      m_txTextDirty{false},
      m_previousFreq{0}, m_hbInterval{0}, m_cqInterval{0}, m_hbPaused{false},
      m_msAudioOutputBuffered(0u),
      m_framesAudioInputBuffered(JS8_RX_SAMPLE_RATE / 10),
      m_audioThreadPriority(QThread::HighPriority),
      m_notificationAudioThreadPriority(QThread::LowPriority),
      m_decoderThreadPriority(QThread::HighPriority), m_splitMode{false},
      m_monitoring{false}, m_generateAudioWhenPttConfirmedByTX{false},
      m_transmitting{false}, m_tune{false}, m_tx_watchdog{false},
      m_block_pwr_tooltip{false}, m_PwrBandSetOK{true},
      m_lastMonitoredFrequency{Default::DIAL_FREQUENCY},
      m_messageClient{new MessageClient{m_config.udp_server_name(),
                                        m_config.udp_server_port(), this}},
      m_messageServer{new MessageServer()}, m_wsjtxMessageClient{nullptr},
      m_wsjtxMessageMapper{nullptr}, m_n3fjpClient{new TCPClient{this}},
      m_pskReporter{new PSKReporter{&m_config, program_info}}, // UR
      m_spotClient{new SpotClient{"spot.js8call.com", 50000, program_info}},
      m_aprsClient{new APRSISClient{"rotate.aprs2.net", 14580}},
      m_aprsInboundRelay{nullptr} {
    ui->setupUi(this);

    createStatusBar();
    add_child_to_event_filter(this);

    m_baseCall = Radio::base_callsign(m_config.my_callsign());
    m_opCall = m_config.opCall();

    // Closedown.
    connect(ui->actionExit, &QAction::triggered, this, &QMainWindow::close);

    // parts of the rig error message box that are fixed
    m_rigErrorMessageBox.setInformativeText(
        tr("Do you want to reconfigure the radio interface?"));
    m_rigErrorMessageBox.setDefaultButton(JS8MessageBox::Ok);

    // start audio thread and hook up slots & signals for shutdown management
    // these objects need to be in the audio thread so that invoking
    // their slots is done in a thread safe way
    m_soundOutput->moveToThread(&m_audioThread);
    m_modulator->moveToThread(&m_audioThread);
    m_soundInput->moveToThread(&m_audioThread);
#ifdef JS8_ENABLE_FT2
    m_detector->setL2RingBuffer(m_l2RingBuf, FT2_L2_RINGSIZE, &m_l2RingPos);
#endif
    m_detector->moveToThread(&m_audioThread);

    // NotificationAudio stays on the main thread so QSoundEffect is
    // constructed and loaded there. Build 108 switched notification
    // playback from QAudioSink (which needed a worker thread to keep the
    // UI responsive) to QSoundEffect (which is non-blocking and manages
    // its own audio thread internally). On macOS, QSoundEffect's first
    // file access triggers a TCC permission prompt for protected folders
    // like ~/Documents; that prompt is a main-thread UI event, and if the
    // QSoundEffect instance was owned by a worker thread the post-grant
    // loading path could deadlock against the main thread (user reported
    // frozen app with spinning cursor after granting Documents access).
    // With NotificationAudio on the main thread, setSource runs inline
    // with the event loop, the prompt is handled cleanly, and loading
    // continues without cross-thread handoff.

    // Move the aprs client message server, psk reporter, and spot client
    // to the network thread at a lower priority.

    m_aprsClient->moveToThread(&m_networkThread);
    m_messageServer->moveToThread(&m_networkThread);
    m_pskReporter->moveToThread(&m_networkThread);
    m_spotClient->moveToThread(&m_networkThread);

    // hook up the message server slots and signals and disposal
    connect(m_messageServer, &MessageServer::message, this,
            &UI_Constructor::tcpNetworkMessage);
    connect(this, &UI_Constructor::apiSetMaxConnections, m_messageServer,
            &MessageServer::setMaxConnections);
    connect(this, &UI_Constructor::apiSetServer, m_messageServer,
            &MessageServer::setServer);
    connect(this, &UI_Constructor::apiStartServer, m_messageServer,
            &MessageServer::start);
    connect(this, &UI_Constructor::apiStopServer, m_messageServer,
            &MessageServer::stop);
    connect(&m_config, &Configuration::tcp_server_changed, m_messageServer,
            &MessageServer::setServerHost);
    connect(&m_config, &Configuration::tcp_server_port_changed, m_messageServer,
            &MessageServer::setServerPort);
    connect(&m_config, &Configuration::tcp_max_connections_changed,
            m_messageServer, &MessageServer::setMaxConnections);
    connect(&m_networkThread, &QThread::finished, m_messageServer,
            &QObject::deleteLater);

    // ChunkedArq manager — owns all per-peer chunked-ARQ state
    // (outbound sends, inbound reassembly, ACK/NACK timers). Lives on
    // the main thread; receives RX events from processCommandActivity
    // and emits wantToTransmit when it has outgoing chunks/ACKs/NACKs.
    // [TODO #134] Pass the app's real ini settings object so the
    // persistent msg-id counter survives restarts on EVERY platform
    // (the old bare-QSettings default store never persisted under
    // the Windows MSIX container).
    m_chunkedArq = new ChunkedArq::Manager(m_settings, this);
    // Use the FULL callsign (e.g. "WM8Q/P"), not m_baseCall — the
    // ARQ wire format puts the from-call into the chunk marker line
    // "<myCall>: <peer> <body> #NN.CC/TT.HHHH" verbatim, and stripping
    // the /P (or /MM, etc.) suffix sends the wrong identifier on-air
    // (operator observed 2026-06-08: WM8Q/P showed as WM8Q in
    // received chunks). General rule per operator: when extracting
    // callsigns for use on-air, remember prefixes and suffixes.
    m_chunkedArq->setMyCall(m_config.my_callsign().trimmed());
    // ARQ-relax state is driven by the ARQ menu action (its
    // internal Qt name is still actionModeReplicatorProtocol and
    // its QSettings key is still "ReplicatorProtocol" — historical
    // names preserved so existing settings load). The settings
    // load later in UI bring-up (readSettings in mainwindow.cpp)
    // fires on_actionModeReplicatorProtocol_toggled, which pushes
    // the persisted state to Manager + Modulator. Init defensively
    // to false so the audio thread sees a defined value if any TX
    // fires before settings load.
    m_chunkedArq->setArqEnabled(false);
    m_modulator->setArqRelax(false);
    // Idle predicate: mirrors Python prototype's TX.GET_QUEUE_DEPTH +
    // TX.GET_TEXT check. Manager polls this between wantToTransmit and
    // ACK-timer-arm so the timer doesn't burn down during our own
    // cycle-aligned TX (~4-7.5 s on Subspace, ~16 s on Normal).
    // [TODO #107] m_txFrameQueue is the V3 counterpart of the box:
    // V2 text frames are rebuilt per-cycle FROM the box (so box-empty
    // meant TX-done), but injected native-binary frames live only in
    // m_txFrameQueue — without this check the poll saw "idle" between
    // per-frame keyups mid-chunk and armed the 12 s ACK timer with 11
    // frames still queued (bench 2026-07-19: retransmit storm, TX
    // deaf to the peer's ACK, retries exhausted on a healthy link).
    // [BUILD 342.6] m_txFrameCount == 0 closes the arming RACE the
    // queue check alone still had: the queue empties when the LAST
    // frame is STAGED (dequeued), ~3.8 s before it finishes airing,
    // and m_transmitting is briefly false in each ~0.2 s inter-frame
    // gap — the 1 s poll tick landing in that window armed the timer
    // early (bench round 2, 2026-07-19 19:12:01 vs PTT drop
    // 19:12:04.7) and the retry keyup stomped the arriving ACK.
    // m_txFrameCount only resets in resetMessageTransmitQueue at the
    // real end-of-TX wind-down (PTT drop), making this the same full
    // idle predicate the rest of mainwindow uses. A stranded non-zero
    // count is backstopped by the Manager's TX-idle safety cap.
    m_chunkedArq->setTxIdleCheck([this]() {
        return !m_transmitting
            && m_txFrameCount == 0
            && m_txMessageQueue.isEmpty()
            && m_txFrameQueue.isEmpty()
            && ui->extFreeTextMsgEdit->toPlainText().trimmed().isEmpty();
    });
    // ACK timeout scales with the currently active JS8 submode so the
    // budget tracks cycle length (Subspace 3.75 s → 12 s, Normal 15 s
    // → 36 s, etc.). Evaluated at arm time, so mid-QSO mode switches
    // take effect on the next chunk's timer.
    // [BUILD 331-arqTimeoutLock] Maps a submode to its ACK-wait budget.
    // The Manager passes SendState.txSubmode — the mode the chunk
    // actually went out in — not a live read, so an operator mode-
    // switch between capture and arm can't give the timer the wrong
    // mode.
    // [BUILD 353 bounddec] Deadline counted in period boundaries
    // (decide at B0+3P for the 1-frame ACK), computed from the live
    // period grid at arm time. Subspace + FSM harness keep the ms
    // budget inside replyDeadlineMsForSubmode.
    m_chunkedArq->setAckTimeoutFn([this](int submode) {
        return ChunkedArq::replyDeadlineMsForSubmode(
            submode, 1,
            DriftingDateTime::currentMSecsSinceEpoch(),
            static_cast<int>(m_TxDelay * 1000));
    });
    // [#121 2026-07-24 acktrack] Live-submode provider. armAckTimer
    // (post-TX-done) calls this to RE-CAPTURE txSubmode per chunk, so
    // each sub-msg's ACK wait tracks the speed that sub-msg went out in
    // — correcting the case where the operator changes speed mid-V1/V2
    // transfer and later chunks (+ their ACKs) run faster/slower than
    // the first. Covers F/V1, F/V2, L/V1 uniformly (shared Manager
    // path). Corrects the old assumption that "the next chunk captures
    // its own txSubmode at its own sendChunked" — sendChunked runs once
    // per super-message, not per chunk.
    m_chunkedArq->setCurrentSubmodeFn([this]() { return m_nSubMode; });
    // [DYNAMIC CAP 2026-06-12 build 262] TX-idle safety cap scales with
    // submode cycle length (90 s floor; legacy Normal/Slow chunks need
    // ~105 / ~210 s to drain). Operator-observed in 2026-06-12 Normal-
    // mode ARQ run: 90 s cap fired before chunk TX completed, burning
    // ~15 s of the 36 s ACK budget unnecessarily on every chunk.
    m_chunkedArq->setTxIdleCapFn([this]() {
        return ChunkedArq::txIdleMaxWaitMsForSubmode(m_nSubMode);
    });

    // [2026-07-24 updlink] The "Check for updates" label (linkCheckUpdates)
    // used to be a QLabel with a HARDCODED github releases URL and
    // openExternalLinks=true — so clicking it opened GitHub directly,
    // bypassing the channel-aware checkVersion() entirely. That is why
    // the MSIX/Store build still sent users to GitHub. The .ui now has
    // openExternalLinks=false and a neutral href; route the click
    // through checkVersion(true) so it queries the RIGHT source (Store
    // for appx, GitHub for the plain build) and shows the up-to-date /
    // new-version dialog with the correct link. alertOnUpToDate=true so
    // a manual check always reports (unlike the silent startup check).
    if (ui->linkCheckUpdates) {
        ui->linkCheckUpdates->setOpenExternalLinks(false);  // belt + braces
        connect(ui->linkCheckUpdates, &QLabel::linkActivated, this,
                [this](QString const &) { checkVersion(true); });
    }
    connect(m_chunkedArq, &ChunkedArq::Manager::wantToTransmit,
            this, &UI_Constructor::onChunkedWantToTransmit);
    // [TODO.md #57 build 268] RX-side ACK / NACK transmissions route
    // through onChunkedWantsResponseTx so the slot can wrap save /
    // restore of the operator's outgoing-text widget contents around
    // the response TX. Plain chunk TX (wantToTransmit) does NOT need
    // the save/restore — the chunk text *is* what the operator chose
    // to send.
    connect(m_chunkedArq, &ChunkedArq::Manager::wantsResponseTx,
            this, &UI_Constructor::onChunkedWantsResponseTx);
    connect(m_chunkedArq, &ChunkedArq::Manager::chunkAdded,
            this, &UI_Constructor::onChunkedChunkAdded);
    connect(m_chunkedArq, &ChunkedArq::Manager::messageDelivered,
            this, &UI_Constructor::onChunkedMessageDelivered);
    connect(m_chunkedArq, &ChunkedArq::Manager::sendProgress,
            this, &UI_Constructor::onChunkedSendProgress);
    connect(m_chunkedArq, &ChunkedArq::Manager::sendComplete,
            this, &UI_Constructor::onChunkedSendComplete);
    connect(m_chunkedArq, &ChunkedArq::Manager::sendFailed,
            this, &UI_Constructor::onChunkedSendFailed);
    // [TODO #51 2026-06-10 build 235]
    connect(m_chunkedArq, &ChunkedArq::Manager::sendRestoreRequested,
            this, &UI_Constructor::onChunkedSendRestoreRequested);
    connect(m_chunkedArq, &ChunkedArq::Manager::msgDelivered,
            this, &UI_Constructor::onChunkedMsgDelivered);
    connect(m_chunkedArq, &ChunkedArq::Manager::inboxMessageReceived,
            this, &UI_Constructor::onChunkedInboxMessageReceived);
    connect(m_chunkedArq, &ChunkedArq::Manager::relayMessageReceived,
            this, &UI_Constructor::onChunkedRelayMessageReceived);
    // [FILE-XFER 2026-06-16 build 276] Receiver-side hook for ARQ
    // file-transfer super-messages. Slot pops the accept dialog,
    // decodes the base32 payload, verifies SHA-256, writes the file.
    connect(m_chunkedArq, &ChunkedArq::Manager::fileMessageReceived,
            this, &UI_Constructor::onChunkedFileMessageReceived);
    // [BUILD 354 rxsession] The receive-session machine's one signal;
    // the UI renders the in-progress banner from it and nothing else.
    connect(m_chunkedArq, &ChunkedArq::Manager::rxSessionChanged,
            this, &UI_Constructor::onRxSessionChanged);
    // [TODO #107] V3 native-binary transfer hooks: per-chunk TX
    // (marker + injected raw frames) and completed-transfer RX.
    connect(m_chunkedArq,
            &ChunkedArq::Manager::wantToTransmitNativeChunk, this,
            &UI_Constructor::onNativeChunkWantToTransmit);
    connect(m_chunkedArq, &ChunkedArq::Manager::binaryMessageReceived,
            this, &UI_Constructor::onNativeBinaryMessageReceived);
    connect(m_chunkedArq, &ChunkedArq::Manager::nativeChunkCollected,
            this, &UI_Constructor::onNativeChunkCollected);
    connect(m_chunkedArq, &ChunkedArq::Manager::nativeMarkerSeen,
            this, &UI_Constructor::onNativeMarkerSeen);
    connect(m_chunkedArq, &ChunkedArq::Manager::progressUpdate,
            this, &UI_Constructor::onChunkedProgressUpdate);
    connect(m_chunkedArq, &ChunkedArq::Manager::progressEnd,
            this, &UI_Constructor::onChunkedProgressEnd);

    m_aprsInboundRelay = new AprsInboundRelay(
        &m_config,
        [this](QString const &call) {
            AprsInboundRelay::CallActivityInfo info;
            auto const it = m_callActivity.constFind(call);
            if (it == m_callActivity.constEnd()) {
                return info;
            }
            info.heard = true;
            info.lastHeardUtc = it->utcTimestamp;
            return info;
        },
        [this](QDateTime const &utc, QString const &text) {
            writeNoticeTextToUI(utc, text);
        },
        [this](QString const &relayMsg) {
            enqueueMessage(PriorityHigh, relayMsg, -1, nullptr,
                           /*autoReply=*/true);
        },
        [this](QString const &fromCall, QString const &toCall,
               QString const &messageId) {
            emit aprsClientEnqueueAck(fromCall, toCall, messageId);
        },
        this);

    // hook up the aprs client slots and signals and disposal
    connect(this, &UI_Constructor::aprsClientEnqueueSpot, m_aprsClient,
            &APRSISClient::enqueueSpot);
    connect(this, &UI_Constructor::aprsClientEnqueueThirdParty, m_aprsClient,
            &APRSISClient::enqueueThirdParty);
    connect(this, &UI_Constructor::aprsClientEnqueueAck, m_aprsClient,
            &APRSISClient::enqueueMessageAck);
    connect(this, &UI_Constructor::aprsClientSendReports, m_aprsClient,
            &APRSISClient::sendReports);
    connect(this, &UI_Constructor::aprsClientSetLocalStation, m_aprsClient,
            &APRSISClient::setLocalStation);
    connect(this, &UI_Constructor::aprsClientSetPaused, m_aprsClient,
            &APRSISClient::setPaused);
    connect(this, &UI_Constructor::aprsClientSetServer, m_aprsClient,
            &APRSISClient::setServer);
    connect(this, &UI_Constructor::aprsClientSetSkipPercent, m_aprsClient,
            &APRSISClient::setSkipPercent);
    connect(this, &UI_Constructor::aprsClientSetIncomingRelayEnabled,
            m_aprsClient, &APRSISClient::setIncomingRelayEnabled);
    connect(&m_config, &Configuration::spot_to_aprs_relay_changed, m_aprsClient,
            &APRSISClient::setIncomingRelayEnabled);
    connect(&m_config, &Configuration::show_calls_on_waterfall_changed,
            this, [this](bool enabled) {
                m_wideGraph->setCallsignOverlayEnabled(enabled);
            });
    connect(m_aprsClient, &APRSISClient::messageReceived, m_aprsInboundRelay,
            &AprsInboundRelay::onMessageReceived);
    connect(&m_networkThread, &QThread::finished, m_aprsClient,
            &QObject::deleteLater);

    // hook up the psk reporter slots and signals and disposal
    connect(m_pskReporter, &PSKReporter::errorOccurred, this,
            &UI_Constructor::pskReporterError);
    connect(this, &UI_Constructor::pskReporterSendReport, m_pskReporter,
            &PSKReporter::sendReport);
    connect(this, &UI_Constructor::pskReporterAddRemoteStation, m_pskReporter,
            &PSKReporter::addRemoteStation);
    connect(this, &UI_Constructor::pskReporterSetLocalStation, m_pskReporter,
            &PSKReporter::setLocalStation);
    connect(&m_networkThread, &QThread::started, m_pskReporter,
            &PSKReporter::start);
    connect(&m_networkThread, &QThread::finished, m_pskReporter,
            &QObject::deleteLater);

    // hook up the spot client signals and disposal
    connect(this, &UI_Constructor::spotClientEnqueueCmd, m_spotClient,
            &SpotClient::enqueueCmd);
    connect(this, &UI_Constructor::spotClientEnqueueSpot, m_spotClient,
            &SpotClient::enqueueSpot);
    connect(this, &UI_Constructor::spotClientSetLocalStation, m_spotClient,
            &SpotClient::setLocalStation);
    connect(&m_networkThread, &QThread::started, m_spotClient,
            &SpotClient::start);
    connect(&m_networkThread, &QThread::finished, m_spotClient,
            &QObject::deleteLater);

    // hook up sound output stream slots & signals and disposal
    connect(this, &UI_Constructor::initializeAudioOutputStream, m_soundOutput,
            &SoundOutput::setFormat);
    connect(m_soundOutput, &SoundOutput::error, this,
            &UI_Constructor::showSoundOutError);
    connect(m_soundOutput, &SoundOutput::error, &m_config,
            &Configuration::invalidate_audio_output_device);
    // [TODO #113] Device-fallback warning: dialog only, no invalidate.
    connect(m_soundOutput, &SoundOutput::deviceFallback, this,
            &UI_Constructor::showSoundOutDeviceFallback);
    connect(this, &UI_Constructor::outAttenuationChanged, m_soundOutput,
            &SoundOutput::setAttenuation);
    connect(&m_audioThread, &QThread::finished, m_soundOutput,
            &QObject::deleteLater);

    connect(this, &UI_Constructor::initializeNotificationAudioOutputStream,
            m_notification, &NotificationAudio::setDevice);
    connect(&m_config, &Configuration::test_notify, this,
            [this](QString const &key) { tryNotify(key); });
    // Dialog Test button: play the .wav directly, bypassing
    // notification_path()'s enable-notifications/per-row/stored-path gates.
    // Fixes the "Test silent until Apply" bug where unapplied dialog edits
    // leave enable_notifications_ stored as false.
    connect(&m_config, &Configuration::test_play, m_notification,
            &NotificationAudio::play);
    connect(this, &UI_Constructor::playNotification, m_notification,
            &NotificationAudio::play);
    connect(&m_notificationAudioThread, &QThread::finished, m_notification,
            &QObject::deleteLater);

    // hook up Modulator slots and disposal
    connect(this, &UI_Constructor::transmitFrequency, m_modulator,
            &Modulator::setAudioFrequency);
    connect(this, &UI_Constructor::endTransmitMessage, m_modulator,
            &Modulator::stop);
    connect(this, &UI_Constructor::tune, m_modulator, &Modulator::tune);
    connect(this, &UI_Constructor::sendMessage, m_modulator, &Modulator::start);
    connect(this, &UI_Constructor::warmStartAudioOutput, m_modulator,
            &Modulator::warmStart);
    connect(m_modulator, &Modulator::ft2WaveformDone, this, [this]() {
        qWarning() << "[FT2-TX] waveform complete: triggering stopTx()";
        stopTx();
    }, Qt::QueuedConnection);
    // [BUILD 331-visHailEpi3] Visible-Hail chain advancement is in
    // stopTx() (mainwindow.cpp), NOT here. ft2WaveformDone is dead
    // code for FT2 mode — only emitted in the legacy JS8 else-branch
    // in Modulator.cpp, never fires for FT2 TX. FT2 completion is
    // detected by guiUpdate's isFT2WaveformDone() poll → stopTx().
    connect(&m_audioThread, &QThread::finished, m_modulator,
            &QObject::deleteLater);

    // hook up the audio input stream signals, slots and disposal
    connect(this, &UI_Constructor::startAudioInputStream, m_soundInput,
            &SoundInput::start);
    connect(this, &UI_Constructor::suspendAudioInputStream, m_soundInput,
            &SoundInput::suspend);
    connect(this, &UI_Constructor::resumeAudioInputStream, m_soundInput,
            &SoundInput::resume);
    connect(this, &UI_Constructor::finished, m_soundInput, &SoundInput::stop);
    connect(m_soundInput, &SoundInput::error, this,
            &UI_Constructor::showSoundInError);
    connect(m_soundInput, &SoundInput::error, &m_config,
            &Configuration::invalidate_audio_input_device);
    // [TODO #113] Device-fallback warning: dialog only, no invalidate.
    connect(m_soundInput, &SoundInput::deviceFallback, this,
            &UI_Constructor::showSoundInDeviceFallback);
    // connect(m_soundInput, &SoundInput::status, this,
    // &UI_Constructor::showStatusMessage);
    connect(&m_audioThread, &QThread::finished, m_soundInput,
            &QObject::deleteLater);

    connect(this, &UI_Constructor::finished, this, &UI_Constructor::close);

    // hook up the detector signals, slots and disposal
    connect(this, &UI_Constructor::FFTSize, m_detector,
            &Detector::setBlockSize);
    connect(m_detector, &Detector::framesWritten, this,
            &UI_Constructor::dataSink);
    connect(&m_audioThread, &QThread::finished, m_detector,
            &QObject::deleteLater);

    // setup the waterfall
    connect(m_wideGraph.data(), &WideGraph::f11f12, this,
            &UI_Constructor::f11f12);
    connect(m_wideGraph.data(), &WideGraph::setXIT, this,
            &UI_Constructor::setXIT);

    connect(this, &UI_Constructor::finished, m_wideGraph.data(),
            &WideGraph::close);

    // setup the log QSO dialog
    connect(m_logDlg.data(), &LogQSO::acceptQSO, this,
            &UI_Constructor::acceptQSO);
    connect(this, &UI_Constructor::finished, m_logDlg.data(), &LogQSO::close);

    // "Spots Map": close with the app, sync the menu checkbox when the
    // operator closes the window directly, and seed the station so the
    // MQTT client begins accumulating spot history at launch.
    connect(this, &UI_Constructor::finished, m_spotMapWindow.data(),
            &SpotMapWindow::close);
    connect(m_spotMapWindow.data(), &SpotMapWindow::closed, this, [this]() {
        ui->actionShow_Spots_Map->setChecked(false);
    });
    // [#153] ARQ Monitor: same lifecycle trio as the map.
    connect(this, &UI_Constructor::finished, m_arqMonitorWindow.data(),
            &ArqMonitorWindow::close);
    connect(m_arqMonitorWindow.data(), &ArqMonitorWindow::closed, this,
            [this]() {
                ui->actionShow_ARQ_Monitor->setChecked(false);
            });
    // [stamon] Station monitors close with the app (top-level
    // windows would otherwise outlive it). [operator 2026-09-03]
    // First save each OPEN monitor's station + position; startup
    // restores the windows and the normal backfill re-derives
    // their content. A window the operator closed mid-session is
    // not in the map/visible and does not come back.
    connect(this, &UI_Constructor::finished, this, [this]() {
        QStringList restore;
        for (auto const &w : m_stationMonitors)
            if (!w.isNull() && w->isVisible())
                restore << QStringLiteral("%1;%2;%3")
                               .arg(w->station())
                               .arg(w->x())
                               .arg(w->y());
        m_settings->beginGroup("StationMonitor");
        m_settings->setValue("Restore", restore);
        m_settings->endGroup();
        for (auto const &w : m_stationMonitors)
            if (!w.isNull())
                w->close();
        m_stationMonitors.clear();
    });
    // [stamon] Restore the monitors that were open at last exit,
    // at their saved positions (deferred so the main window and
    // settings are fully up first).
    QTimer::singleShot(1200, this, [this]() {
        m_settings->beginGroup("StationMonitor");
        QStringList const restore =
            m_settings->value("Restore").toStringList();
        m_settings->endGroup();
        for (QString const &r : restore) {
            QStringList const parts = r.split(QLatin1Char(';'));
            if (parts.size() != 3 || parts[0].isEmpty())
                continue;
            openStationMonitor(parts[0]);
            if (auto const w = m_stationMonitors.value(parts[0]))
                w->move(parts[1].toInt(), parts[2].toInt());
        }
    });
    m_spotMapWindow->setStation(m_config.my_callsign(), m_config.my_grid());
    // [BUILD 336 TODO #96 first slice] Clicking a spot dot seeds the
    // outgoing text box with that callsign. Rules (Andy 2026-07-16):
    // any selected callsign is CLEARED first (selection is not a
    // guard — the first cut guarded on it and silently ate every
    // click while something was selected); the box is overwritten
    // when empty OR holding nothing but a bare callsign (a previous
    // click's seed — repeated clicks switch stations); real draft
    // text is never clobbered. Every suppression logs — no silent
    // misses. Confirmed with a toast on the map window.
    // [BUILD 340.1] Load the logbook (and with it cty.dat) at
    // STARTUP — previously init() only ran from QSO-logging paths,
    // so the country table stayed EMPTY until the first logged
    // contact and every lookup returned the "where?" placeholder
    // (operator-observed on hover). One-time cost: cty.dat resource
    // + ADIF log read.
    m_logBook.init();
    // [BUILD 340] Country names for spot hover (LogBook/cty.dat by
    // callsign; the map compares topic DXCC codes to skip our own
    // country before calling this).
    m_spotMapWindow->setTxBusyProbe(
        [this]() { return txBusyToastText(); });
    m_spotMapWindow->setCongestionProbe(
        [this]() { return bandCongestionIndex(); });
    // [#207 waitopts] Options dialog: probe pulls the live values
    // when it opens (dodges init order); sink pushes changes back
    // to the one owner, which persists them.
    m_spotMapWindow->setWaitConfigProbe([this]() {
        return SpotMapWindow::WaitConfig{m_reachWaitMode,
                                         m_reachBusyThreshold,
                                         m_reachBusyThresholdPskr};
    });
    m_spotMapWindow->setWaitConfigSink(
        [this](int mode, int onAir, int pskr) {
            setReachWaitConfig(mode, onAir, pskr);
        });
    m_spotMapWindow->setCountryLookup([this](QString const &call) {
        QString country;
        bool workedCall = false, workedCountry = false;
        m_logBook.match(call, country, workedCall, workedCountry);
        // match() substitutes "where?" when the prefix isn't found —
        // a placeholder, not a country. No line beats a riddle.
        if (country == QStringLiteral("where?")) {
            country.clear();
        }
        // CountryDat names are compound: "Spain; EA; EU" (name;
        // principal prefix; continent). Drop the prefix component
        // for the hover (Andy 2026-07-17) — other consumers
        // (CountriesWorked keys) need the compound intact, so strip
        // here, not in CountryDat.
        if (QStringList parts = country.split(QStringLiteral("; "));
            parts.size() >= 3) {
            parts.removeAt(1);
            country = parts.join(QStringLiteral("; "));
        }
        return country;
    });
    // [BUILD 340] Double-click a spot → QSY to the DX station's
    // audio offset (map gates to >1000 Hz; changeFreq has the
    // TX-queue guard).
    connect(m_spotMapWindow.data(), &SpotMapWindow::qsyToOffset,
            this, &UI_Constructor::changeFreq);
    // [stamon] Station-monitor right-clicks: map spots directly;
    // waterfall resolves by signal-bandwidth tolerance (take 2).
    connect(m_spotMapWindow.data(),
            &SpotMapWindow::stationRightClicked, this,
            &UI_Constructor::stationMonitorMenu);
    connect(m_wideGraph.data(), &WideGraph::openStationMonitor,
            this, &UI_Constructor::openStationMonitor);
    // [stamon] Window-menu entry, right after Show Spots Map:
    // asks for the callsign in a dialog. Standard feature since
    // Build 465 (env gate removed, operator 2026-09-01).
    {
        if (ui->menuWindow) {
            auto *act = new QAction(tr("Show Station Monitor"), this);
            connect(act, &QAction::triggered, this, [this]() {
                bool ok = false;
                QString const call =
                    QInputDialog::getText(
                        this, tr("Station monitor"),
                        tr("Callsign to monitor:"),
                        QLineEdit::Normal, QString(), &ok)
                        .trimmed()
                        .toUpper();
                if (ok && Radio::is_callsign(call))
                    openStationMonitor(call);
            });
            QList<QAction *> const acts = ui->menuWindow->actions();
            if (int const i =
                    acts.indexOf(ui->actionShow_Spots_Map);
                i >= 0 && i + 1 < acts.size())
                ui->menuWindow->insertAction(acts[i + 1], act);
            else
                ui->menuWindow->addAction(act);
        }
    }
    // [#187 intelminer] Build/refresh the intel corpus from the
    // user's own logs, in the background, once the window is up.
    // Full re-mine (mine.py semantics); skipped when logs unchanged.
    QTimer::singleShot(0, this, [this]() { startIntelMine(false); });
    if (ui->menuControl) {
        auto *rebuild = ui->menuControl->addAction(
            tr("Rebuild routing knowledge"));
        connect(rebuild, &QAction::triggered, this,
                [this]() { startIntelMine(true); });
    }
    // [autoroute 2026-08-28] The map picked a validated target; run
    // the reaching executor with the main screen locked. The map's
    // "Halt auto-route" click cancels the mode. NOTE: connected ONCE,
    // here -- unlike the block below, which is duplicated wholesale
    // further down (TODO #176's double-connect).
    connect(m_spotMapWindow.data(), &SpotMapWindow::autoRouteStart,
            this, &UI_Constructor::autoRouteBegin);
    connect(m_spotMapWindow.data(), &SpotMapWindow::autoRouteHalt,
            this, &UI_Constructor::autoRouteCancel);
    connect(m_spotMapWindow.data(), &SpotMapWindow::spotClicked, this,
            [this](QString const &call) {
                switch (trySeedOutgoingGreeting(call)) {
                case GreetingSeedResult::Seeded:
                    m_spotMapWindow->showToast(
                        tr("Standard greeting to %1 placed in the "
                           "outgoing message box")
                            .arg(call.trimmed()));
                    break;
                case GreetingSeedResult::DraftBlocked:
                    m_spotMapWindow->showToast(tr(
                        "Clear or send current outgoing message first"));
                    break;
                case GreetingSeedResult::InvalidCall:
                    break; // logged by the helper
                }
            });
    // [relaysel] Relay-path builder "Done": put the template in the
    // outgoing box with the [MESSAGE] placeholder SELECTED so typing
    // replaces it immediately. Plain directed text — no ARQ wrap on
    // any hop (operator directive 2026-08-14).
    connect(m_spotMapWindow.data(), &SpotMapWindow::relayTemplateReady,
            this, [this](QString const &tpl) {
                // [relaysel] The template IS the full addressing —
                // a selected call-list row would re-direct the send
                // to the wrong station (operator 2026-08-15).
                clearCallsignSelected();
                ui->extFreeTextMsgEdit->setPlainText(tpl);
                QTextCursor c = ui->extFreeTextMsgEdit->textCursor();
                if (int const at =
                        tpl.indexOf(QStringLiteral("[MESSAGE]"));
                    at >= 0) {
                    c.setPosition(at);
                    c.setPosition(at + 9, QTextCursor::KeepAnchor);
                } else {
                    c.movePosition(QTextCursor::End);
                }
                ui->extFreeTextMsgEdit->setTextCursor(c);
                ui->extFreeTextMsgEdit->setFocus();
                activateWindow();
                raise();
            });
    // [BUILD 336] Waterfall: double-click on (or very near) a painted
    // callsign label — seed logic; feedback via the status bar (the
    // waterfall has no toast overlay).
    // [BUILD 341 wfDblClick] Andy 2026-07-17: the waterfall form is
    // deliberate enough to OVERWRITE any draft (force seed; the ARQ
    // box lock still wins), and it QSYs to the station's last-heard
    // audio offset from Call Activity when that offset is > 1000 Hz
    // (same floor as the spots-map QSY; changeFreq has the TX-queue
    // guard).
    connect(m_wideGraph.data(), &WideGraph::callDoubleClicked, this,
            [this](QString const &call) {
                switch (trySeedOutgoingGreeting(call, /*force=*/true)) {
                case GreetingSeedResult::Seeded: {
                    QString const key = call.trimmed().toUpper();
                    int offset = -1;
                    if (auto const it = m_callActivity.constFind(key);
                        it != m_callActivity.constEnd()) {
                        offset = it->offset;
                    }
                    if (offset > 1000) {
                        changeFreq(offset);
                        statusBar()->showMessage(
                            tr("Copied %1 to outgoing message — QSY %2 Hz")
                                .arg(key).arg(offset), 5000);
                    } else {
                        statusBar()->showMessage(
                            tr("Standard greeting to %1 placed in the "
                           "outgoing message box")
                                .arg(key), 5000);
                    }
                    break;
                }
                case GreetingSeedResult::DraftBlocked:
                    // Only the ARQ lock blocks now (force overwrites
                    // drafts).
                    statusBar()->showMessage(tr(
                        "Multi-part message in progress — try later"),
                        5000);
                    break;
                case GreetingSeedResult::InvalidCall:
                    break; // logged by the helper
                }
            });
    // [relaysel] Relay-path builder "Done": put the template in the
    // outgoing box with the [MESSAGE] placeholder SELECTED so typing
    // replaces it immediately. Plain directed text — no ARQ wrap on
    // any hop (operator directive 2026-08-14).
    connect(m_spotMapWindow.data(), &SpotMapWindow::relayTemplateReady,
            this, [this](QString const &tpl) {
                // [relaysel] The template IS the full addressing —
                // a selected call-list row would re-direct the send
                // to the wrong station (operator 2026-08-15).
                clearCallsignSelected();
                ui->extFreeTextMsgEdit->setPlainText(tpl);
                QTextCursor c = ui->extFreeTextMsgEdit->textCursor();
                if (int const at =
                        tpl.indexOf(QStringLiteral("[MESSAGE]"));
                    at >= 0) {
                    c.setPosition(at);
                    c.setPosition(at + 9, QTextCursor::KeepAnchor);
                } else {
                    c.movePosition(QTextCursor::End);
                }
                ui->extFreeTextMsgEdit->setTextCursor(c);
                ui->extFreeTextMsgEdit->setFocus();
                activateWindow();
                raise();
            });
    // [TODO #106 / BUILD 342.20] File menu: open the received-files
    // folder (<Downloads>/Subspace-FileTransfer) in the platform file
    // manager. mkpath first so the action works before the first
    // transfer ever lands; path comes from
    // FileTransfer::receiveDirectory() — the same single definition
    // the RX save hook uses. openUrl goes through the shell, so it
    // behaves under MSIX too.
    {
        auto *openXferDir = new QAction(
            QStringLiteral("Open file transfer directory"), this);
        connect(openXferDir, &QAction::triggered, this, []() {
            QString const dir = FileTransfer::receiveDirectory();
            QDir().mkpath(dir);
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });
        ui->menuFile->insertAction(ui->actionSettings, openXferDir);
        ui->menuFile->insertSeparator(ui->actionSettings);
    }

    // [2026-07-24] Removed the two V3 debug Help-menu entries ("TX
    // native test chunk" / "TX native burst chunk (experiment)") per
    // operator request. The underlying debugSendNativeTestChunk /
    // debugSendNativeBurstChunk member functions are retained (no
    // longer exposed in the UI); re-add the menu wiring here if the
    // rigs are needed again for a support session.

    // Reopen the Spots Map at startup if it was open at last exit
    // (one-time — NOT in a menu aboutToShow handler).
    if (m_spotMapWindow->wasVisibleAtShutdown()) {
        ui->actionShow_Spots_Map->setChecked(true);
        m_spotMapWindow->setBand(m_lastBand);
        m_spotMapWindow->show();
    }
    // [#153] Same one-time restore for the ARQ Monitor.
    if (m_arqMonitorWindow->wasVisibleAtShutdown()) {
        ui->actionShow_ARQ_Monitor->setChecked(true);
        m_arqMonitorWindow->show();
    }

    // Network message handling
    connect(m_messageClient, &MessageClient::message, this,
            &UI_Constructor::udpNetworkMessage);

    /**
     * @brief Initialize WSJT-X protocol if enabled
     *
     * Creates and configures the WSJT-X message client and mapper when the
     * WSJT-X protocol is enabled. Sets up signal connections for configuration
     * changes and disables the native JSON client if it conflicts with WSJT-X
     * on the same port/address.
     */
    if (m_config.wsjtx_protocol_enabled()) {
        QString id = QApplication::applicationName();
        QString version = QApplication::applicationVersion();
        QString revision = ""; // Get from your version system if available

        m_wsjtxMessageClient = new WSJTXMessageClient{
            id,
            version,
            revision,
            m_config.wsjtx_server_name(),
            m_config.wsjtx_server_port(),
            m_config.wsjtx_interface_names(), // Use selected interfaces
            m_config.wsjtx_TTL(),
            this};

        m_wsjtxMessageClient->enable(m_config.wsjtx_accept_requests());

        m_wsjtxMessageMapper =
            new WSJTXMessageMapper(m_wsjtxMessageClient, this, this);

        // Disable native JSON client if it's using the same port/address as
        // WSJT-X
        if (m_config.wsjtx_server_port() == m_config.udp_server_port() &&
            m_config.wsjtx_server_name() == m_config.udp_server_name()) {
            m_messageClient->set_server_port(0); // Disable native JSON client
        }

        // Connect configuration changes
        connect(&m_config, &Configuration::wsjtx_server_changed,
                [this](QString const &server_name) {
                    m_wsjtxMessageClient->set_server(
                        server_name, m_config.wsjtx_interface_names());
                    // Check if we need to disable native JSON client
                    if (m_config.wsjtx_protocol_enabled() &&
                        m_config.wsjtx_server_port() ==
                            m_config.udp_server_port() &&
                        server_name == m_config.udp_server_name()) {
                        m_messageClient->set_server_port(0);
                    } else if (m_config.wsjtx_protocol_enabled() &&
                               m_config.wsjtx_server_port() !=
                                   m_config.udp_server_port()) {
                        m_messageClient->set_server_port(
                            m_config.udp_server_port());
                    }
                });
        connect(&m_config, &Configuration::wsjtx_server_port_changed,
                [this](quint16 port) {
                    m_wsjtxMessageClient->set_server_port(port);
                    // Check if we need to disable native JSON client
                    if (m_config.wsjtx_protocol_enabled() &&
                        port == m_config.udp_server_port() &&
                        m_config.wsjtx_server_name() ==
                            m_config.udp_server_name()) {
                        m_messageClient->set_server_port(0);
                    } else if (m_config.wsjtx_protocol_enabled() &&
                               port != m_config.udp_server_port()) {
                        m_messageClient->set_server_port(
                            m_config.udp_server_port());
                    }
                });
        connect(&m_config, &Configuration::wsjtx_TTL_changed, this,
                [this](int ttl) {
                    if (m_wsjtxMessageClient) {
                        m_wsjtxMessageClient->set_TTL(ttl);
                    }
                });
        connect(&m_config, &Configuration::wsjtx_interfaces_changed,
                [this](QStringList const &interfaces) {
                    if (m_wsjtxMessageClient) {
                        m_wsjtxMessageClient->set_server(
                            m_config.wsjtx_server_name(), interfaces);
                    }
                });
    }

    // decoder queue handler
    // connect (&m_decodeThread, &QThread::finished, m_notification,
    // &QObject::deleteLater); connect(this, &UI_Constructor::decodedLineReady,
    // this, &UI_Constructor::processDecodedLine);
    connect(&m_decoder, &JS8::Decoder::decodeEvent, this,
            &UI_Constructor::processDecodeEvent);

    m_dateTimeQSOOn = QDateTime{};

    // initialize decoded text font and hook up font change signals
    // defer initialization until after construction otherwise menu
    // fonts do not get set
    QTimer::singleShot(0, this, &UI_Constructor::initialize_fonts);
    connect(&m_config, &Configuration::gui_text_font_changed,
            [this](QFont const &font) { set_application_font(font); });
    connect(&m_config, &Configuration::table_font_changed,
            [this](QFont const &) {
                ui->tableWidgetRXAll->setFont(m_config.table_font());
                ui->tableWidgetCalls->setFont(m_config.table_font());
            });
    connect(&m_config, &Configuration::rx_text_font_changed,
            [this](QFont const &) {
                setTextEditFont(ui->textEditRX, m_config.rx_text_font());
            });
    connect(&m_config, &Configuration::compose_text_font_changed,
            [this](QFont const &) {
                setTextEditFont(ui->extFreeTextMsgEdit,
                                m_config.compose_text_font());
            });
    connect(&m_config, &Configuration::colors_changed, [this]() {
        setTextEditStyle(ui->textEditRX, m_config.color_rx_foreground(),
                         m_config.color_rx_background(),
                         m_config.rx_text_font());
        setTextEditStyle(
            ui->extFreeTextMsgEdit, m_config.color_compose_foreground(),
            m_config.color_compose_background(), m_config.compose_text_font());
        ui->extFreeTextMsgEdit->setFont(m_config.compose_text_font(),
                                        m_config.color_compose_foreground(),
                                        m_config.color_compose_background());

        // rehighlight
        auto d = ui->textEditRX->document();
        if (d) {
            for (int i = 0; i < d->lineCount(); i++) {
                auto b = d->findBlockByLineNumber(i);

                switch (b.userState()) {
                case State::RX:
                    highlightBlock(b, m_config.rx_text_font(),
                                   m_config.color_rx_foreground(),
                                   QColor(Qt::transparent));
                    break;
                case State::TX:
                    highlightBlock(b, m_config.tx_text_font(),
                                   m_config.color_tx_foreground(),
                                   QColor(Qt::transparent));
                    break;
                }
            }
        }
    });

    // [magtitle] One title authority -- appends " - MAGNET" inside
    // the version parens when the SuperSpotter latch is set. The
    // persisted latch is read later in this constructor (the L2
    // setup block), which calls updateWindowTitle again -- so a
    // latched station briefly paints the plain title, then the
    // MAGNET one, before the window is even shown.
    updateWindowTitle();
    ui->labVersion->setText("Subspace Edition v" + version());
    // Link label spacing — leave at .ui defaults
    buildColumnLabelMap();

    // Hook up working frequencies.

    ui->currentFreq->setCursor(QCursor(Qt::PointingHandCursor));
    ui->currentFreq->display("14.078 000");
    ui->currentFreq->installEventFilter(new EventFilter::MouseButtonPress(
        [this](QMouseEvent *event) {
            QMenu *menu = new QMenu(ui->currentFreq);
            buildFrequencyMenu(menu);
            menu->popup(event->globalPosition().toPoint());
            return true;
        },
        this));

    ui->labDialFreqOffset->setCursor(QCursor(Qt::PointingHandCursor));
    ui->labDialFreqOffset->installEventFilter(new EventFilter::MouseButtonPress(
        [this](QMouseEvent *) {
            on_actionSetOffset_triggered();
            return true;
        },
        this));

    // Hook up callsign label click to open preferences

    ui->labCallsign->setCursor(QCursor(Qt::PointingHandCursor));
    ui->labCallsign->installEventFilter(new EventFilter::MouseButtonPress(
        [this](QMouseEvent *) {
            openSettings(0);
            return true;
        },
        this));

    // hook up configuration signals
    connect(&m_config, &Configuration::transceiver_update, this,
            &UI_Constructor::handle_transceiver_update);
    connect(&m_config, &Configuration::transceiver_failure, this,
            &UI_Constructor::handle_transceiver_failure);
    connect(&m_config, &Configuration::udp_server_name_changed, m_messageClient,
            &MessageClient::set_server_name);
    connect(&m_config, &Configuration::udp_server_port_changed, m_messageClient,
            &MessageClient::set_server_port);

    // Disable native JSON client if WSJT-X protocol is enabled on the same
    // port/address This prevents JSON PING messages from interfering with
    // WSJT-X binary protocol
    connect(
        &m_config, &Configuration::wsjtx_protocol_enabled_changed, this,
        [this](bool enabled) {
            if (enabled &&
                m_config.wsjtx_server_port() == m_config.udp_server_port() &&
                m_config.wsjtx_server_name() == m_config.udp_server_name()) {
                // Disable native JSON client to avoid conflicts with WSJT-X
                // protocol
                m_messageClient->set_server_port(0);
            } else if (!enabled) {
                // Re-enable native JSON client if WSJT-X is disabled
                m_messageClient->set_server_port(m_config.udp_server_port());
            }
        });
    connect(&m_config, &Configuration::band_schedule_changed, this,
            [this]() { this->m_bandHopped = true; });
    connect(&m_config, &Configuration::auto_switch_bands_changed, this,
            [this](bool auto_switch_bands) {
                this->m_bandHopped = this->m_bandHopped || auto_switch_bands;
            });
    connect(&m_config, &Configuration::manual_band_hop_requested, this,
            &UI_Constructor::manualBandHop);
    connect(&m_config, &Configuration::enumerating_audio_devices,
            [this]() { showStatusMessage(tr("Enumerating audio devices")); });

    // set up configurations menu
    connect(m_multi_settings, &MultiSettings::configurationNameChanged,
            [this](QString const &name) {
                if ("Default" != name) {
                    config_label.setText(name);
                    config_label.show();
                } else {
                    config_label.hide();
                }
            });
    m_multi_settings->create_menu_actions(this, ui->menuConfig);
    m_configurations_button = m_rigErrorMessageBox.addButton(
        tr("Configurations..."), QMessageBox::ActionRole);
    connect(ui->extFreeTextMsgEdit, &QTextEdit::textChanged,
            [this]() { currentTextChanged(); matchCallsignFromInput(); });

    m_guiTimer.setTimerType(Qt::PreciseTimer);
    m_guiTimer.setSingleShot(true);
    connect(&m_guiTimer, &QTimer::timeout, this, &UI_Constructor::guiUpdate);
    m_guiTimer.start(UI_POLL_INTERVAL_MS);

    pttReleaseTimer.setTimerType(Qt::PreciseTimer);
    pttReleaseTimer.setSingleShot(true);
    connect(&pttReleaseTimer, &QTimer::timeout, this, &UI_Constructor::stopTx2);

    logQSOTimer.setSingleShot(true);
    connect(&logQSOTimer, &QTimer::timeout, this,
            &UI_Constructor::on_logQSOButton_clicked);

    tuneButtonTimer.setSingleShot(true);
    connect(&tuneButtonTimer, &QTimer::timeout, this,
            &UI_Constructor::end_tuning);

    tuneATU_Timer.setSingleShot(true);
    connect(&tuneATU_Timer, &QTimer::timeout, this,
            &UI_Constructor::stopTuneATU);

    TxAgainTimer.setSingleShot(true);
    connect(&TxAgainTimer, &QTimer::timeout, this, &UI_Constructor::TxAgain);

    connect(m_wideGraph.data(), &WideGraph::changeFreq, this,
            &UI_Constructor::changeFreq);
    connect(m_wideGraph.data(), &WideGraph::qsy, this, &UI_Constructor::qsy);

    // DriftingDateTime management:
    connect(m_wideGraph.data(), &WideGraph::want_new_drift,
            &DriftingDateTimeSingleton::getSingleton(),
            &DriftingDateTimeSingleton::setDrift);

    // Distribute Drift change:
    connect(&DriftingDateTimeSingleton::getSingleton(),
            &DriftingDateTimeSingleton::driftChanged, this,
            &UI_Constructor::onDriftChanged);
    connect(&DriftingDateTimeSingleton::getSingleton(),
            &DriftingDateTimeSingleton::driftChanged, m_wideGraph.data(),
            &WideGraph::onDriftChanged);
    connect(&DriftingDateTimeSingleton::getSingleton(),
            &DriftingDateTimeSingleton::driftChanged, m_cq_loop,
            &TxLoop::onDriftChange);
    connect(&DriftingDateTimeSingleton::getSingleton(),
            &DriftingDateTimeSingleton::driftChanged, m_hb_loop,
            &TxLoop::onDriftChange);

    // HB and CQ loop:
    // For now, disable HB loop while CQ loop runs and vice versa:
    connect(m_cq_loop, &TxLoop::nextActivityChanged, this,
            [this](const QDateTime &) { this->m_hb_loop->onLoopCancel(); });
    connect(m_hb_loop, &TxLoop::nextActivityChanged, this,
            [this](const QDateTime &) { this->m_cq_loop->onLoopCancel(); });
    // It is not advisable to send a HB in one period and a CQ in the very next,
    // or a CQ first and then a HB too soon, so that transmissions of people
    // interested in the QSO might be drowned.
    //
    // We could conceivably devise a clean conflict resolution mechanism
    // disallowing automatic transmissions of one type if an automatic
    // transmission of the other type has happened recently. In the absence of
    // such a mechanism, just do either one or the other.
    //
    // People who call CQ are expected to monitor somewhat closely, so they will
    // have no problems triggering HBs at will.

    // Propagate tx submode changes to the CQ and HB loop:
    connect(this, &UI_Constructor::submodeChanged, this->m_hb_loop,
            &TxLoop::onModeChange);
    connect(this, &UI_Constructor::submodeChanged, this->m_cq_loop,
            &TxLoop::onModeChange);

    // When the loops are switched off, tell the UI:
    connect(m_hb_loop, &TxLoop::canceled, ui->hbMacroButton,
            [this]() { this->ui->hbMacroButton->setChecked(false); });
    connect(m_cq_loop, &TxLoop::canceled, ui->cqMacroButton,
            [this]() { this->ui->cqMacroButton->setChecked(false); });

    // The loops can trigger transmissions. That is what they are for.
    connect(m_hb_loop, &TxLoop::triggerTxNow, this,
            [this]() { this->sendHB(); });
    connect(m_cq_loop, &TxLoop::triggerTxNow, this,
            [this]() { this->sendCQ(true); });

    // Something like this would be nice to have:
    // connect(m_config, &Configuration::txDelayChanged, m_cq_loop,
    // &TxLoop::onTxDelayChange); connect(m_config,
    // &Configuration::txDelayChanged, m_hb_loop, &TxLoop::onTxDelayChange); But
    // the pertaining signals are not offered by Configuration, and the code was
    // somewhat of a beast to get into to change that, so some equivalent is
    // done in a pedestrian way via our polling routine.  That pedestrian code
    // also handles conversion of incoming tx delay in (double) seconds to
    // outgoing (qint64) milliseconds.

    decodeBusy(false);

    m_msg[0][0] = 0;

    displayDialFrequency();
    readSettings(); // Restore user's setup params

    {
        std::lock_guard<std::mutex> lock(fftw_mutex);
        fftwf_import_wisdom_from_filename(wisdomFileName());
    }

    m_networkThread.start(m_networkThreadPriority);
    m_audioThread.start(m_audioThreadPriority);
    m_notificationAudioThread.start(m_notificationAudioThreadPriority);
    m_decoder.start(m_decoderThreadPriority);

    Q_EMIT startAudioInputStream(m_config.audio_input_device(),
                                 m_framesAudioInputBuffered, m_detector,
                                 m_config.audio_input_channel());
    Q_EMIT initializeAudioOutputStream(
        m_config.audio_output_device(),
        AudioDevice::Mono == m_config.audio_output_channel() ? 1 : 2,
        m_msAudioOutputBuffered);
    // [TODO #108 keep-warm] First TX must be a warm restart — open the
    // output stream into KeepAlive silence now, at startup.
    Q_EMIT warmStartAudioOutput(m_soundOutput,
                                m_config.audio_output_channel());
    Q_EMIT initializeNotificationAudioOutputStream(
        m_config.notification_audio_output_device(), m_msAudioOutputBuffered);
    Q_EMIT transmitFrequency(freq() + m_XIT);

    enable_DXCC_entity(
        m_config
            .DXCC()); // sets text window proportions and (re)inits the logbook

    // this must be done before initializing the mode as some modes need
    // to turn off split on the rig e.g. WSPR
    m_config.transceiver_online();

#ifdef JS8_ENABLE_FT2
    ft2_init_c();
    // Self-test disabled — blocks on Windows (static gfortran runtime issue).
    // The encode/decode pipeline is verified by the Linux build's test suite.
    // QTimer::singleShot(2000, []() { JS8::DecodeFT2::selfTest(); });

    // L2 async decode: event-driven — each decode triggers the next.
    // Timer is watchdog only (2s), kicks off decode if nothing is running.
    connect(&m_l2DecodeWatcher, &QFutureWatcher<void>::finished,
            this, &UI_Constructor::l2DecodeDone);
    connect(&m_l2DecodeTimer, &QTimer::timeout, this, [this]() {
        // [TODO #113/#120 2026-07-24 l2watch] This timer was called a
        // watchdog but only re-invoked the SAME gated function, so a
        // stuck latch made it a no-op forever. Check the latches first
        // — that is what makes it an actual watchdog.
        l2DecodeWatchdogCheck();
        l2TryDecode("watchdog");
    });
    // [FT2-L2 ASYNC TOGGLE 2026-06-16] Define JS8_DISABLE_L2_ASYNC
    // to force the FT2/Subspace decoder to run ONLY on period
    // boundaries (the standard ft2_decode_c path), disabling the
    // 7.5 s rolling-window L2 async decoder. Operator-driven A/B
    // test: WM8Q's hypothesis is the async path is responsible for
    // missed-frame regressions in chunked ARQ under wired loopback
    // — the first and last Costas arrays in successive frames appear
    // jammed up against each other in the waterfall, suggesting the
    // async window is straddling frame boundaries and bricking decode.
    // The standard FT2 decode in decoder.cpp is untouched.
    //
    // To toggle: add `#define JS8_DISABLE_L2_ASYNC 1` at the very top
    // of this file (above the #include lines is fine — it just needs
    // to be visible at this point), or pass -DJS8_DISABLE_L2_ASYNC=1
    // via CMake. Touch this file and rebuild to re-evaluate.
#ifndef JS8_DISABLE_L2_ASYNC
    // [ssdetect] Startup honors the persisted switch. The switch's
    // menu item exists only on stations that have ever seen the
    // SuperSpotter client signature; everyone else always starts
    // (and stays) enabled with no visible surface.
    m_superSpotterSeen =
        m_settings->value("SuperSpotterSeen", false).toBool();
    ui->actionModeSubspaceDecode->setVisible(m_superSpotterSeen);
    // [magtitle] Persisted latch changes the version segment to
    // "(vX - MAGNET)"; re-paint now that the flag is known.
    updateWindowTitle();
    bool const ssDecode =
        m_settings->value("SubspaceDecodeEnabled", true).toBool();
    m_l2Enabled = ssDecode;
    ui->actionModeSubspaceDecode->blockSignals(true);
    ui->actionModeSubspaceDecode->setChecked(ssDecode);
    ui->actionModeSubspaceDecode->blockSignals(false);
    if (!ssDecode)
        qWarning() << "[SSDETECT] Subspace decode starts DISABLED "
                      "(persisted operator choice)";
    m_l2DecodeTimer.start(2000);  // watchdog only — normal path is l2DecodeDone → l2TryDecode
#else
    qWarning() << "[FT2-L2] async decode DISABLED at compile time "
                  "(JS8_DISABLE_L2_ASYNC) — period-aligned FT2 "
                  "decoder is the only RX path";
#endif
#endif

    setupJS8();

    Q_EMIT transmitFrequency(freq() + m_XIT);

    statusChanged();

    connect(&minuteTimer, &QTimer::timeout, this,
            &UI_Constructor::on_the_minute);
    minuteTimer.setSingleShot(true);
    minuteTimer.start(ms_minute_error() + 60 * 1000);

    QTimer::singleShot(0, this, &UI_Constructor::checkStartupWarnings);

    // UI Customizations & Tweaks
    ui->horizontalLayoutBand->insertSpacing(1, 6);
    ui->horizontalLayoutBand->insertWidget(2, m_wideGraph.data(), 1);
    ui->horizontalLayoutBand->insertSpacing(3, 8);

    // Push the call-sign waterfall overlay setting so the time/band
    // label scoots to the right immediately if the user has it on.
    m_wideGraph->setCallsignOverlayEnabled(
        m_config.show_calls_on_waterfall());

    // [BUILD 353 yesflag TODO #131] Waterfall label capability flag:
    // paint-time predicate over the ONE capability cache (the passive
    // YES-capture's m_peerArqLevel — ours or overheard). Any cached
    // bare-digits level counts, so a future protocol level 4 flags
    // without a code change. Evaluated per paint; no copied state.
    m_wideGraph->setArqCapableCheck([this](QString const &call) {
        return m_peerArqLevel.contains(call.toUpper());
    });

    // remove disabled menus from the menu bar
    foreach (auto action, ui->menuBar->actions()) {
        if (action->isEnabled()) {
            continue;
        }
        ui->menuBar->removeAction(action);
    }

    // auto f = findFreeFreqOffset(1000, 2000, 50);
    // setFreqOffsetForRestore(f, false);

    ui->actionModeAutoreply->setChecked(m_config.autoreply_on_at_startup());
    ui->spotButton->setChecked(m_config.spot_to_reporting_networks());

    QActionGroup *modeActionGroup = new QActionGroup(this);
    ui->actionModeJS8Normal->setActionGroup(modeActionGroup);
    ui->actionModeJS8Fast->setActionGroup(modeActionGroup);
    ui->actionModeJS8Turbo->setActionGroup(modeActionGroup);
    ui->actionModeJS8Slow->setActionGroup(modeActionGroup);
    ui->actionModeJS8Ultra->setActionGroup(modeActionGroup);
#ifdef JS8_ENABLE_FT2
    ui->actionModeFT2->setActionGroup(modeActionGroup);
#endif

    ui->modeButton->installEventFilter(new EventFilter::MouseButtonPress(
        [this](QMouseEvent *event) {
            ui->menuModeJS8->popup(event->globalPosition().toPoint());
            return true;
        },
        this));

    if (!JS8_ENABLE_JS8A)
        ui->actionModeJS8Normal->setVisible(false);
    if (!JS8_ENABLE_JS8B)
        ui->actionModeJS8Fast->setVisible(false);
    if (!JS8_ENABLE_JS8C)
        ui->actionModeJS8Turbo->setVisible(false);
    if (!JS8_ENABLE_JS8E)
        ui->actionModeJS8Slow->setVisible(false);
    if (!JS8_ENABLE_JS8I)
        ui->actionModeJS8Ultra->setVisible(false);

    // prep
    prepareMonitorControls();
    prepareHeartbeatMode(canCurrentModeSendHeartbeat() &&
                         ui->actionModeJS8HB->isChecked());

    ui->extFreeTextMsgEdit->installEventFilter(new EventFilter::EnterKeyPress(
        [this](QKeyEvent *const event) {
            if (event->modifiers() & Qt::ShiftModifier)
                return false;
            if (ui->extFreeTextMsgEdit->isReadOnly())
                return false;

            if (ui->extFreeTextMsgEdit->toPlainText().trimmed().isEmpty())
                return true;
            if (!ensureCanTransmit())
                return true;
            if (!ensureCallsignSet(true))
                return true;

            toggleTx(true);
            return true;
        },
        this));

    ui->textEditRX->viewport()->installEventFilter(
        new EventFilter::MouseButtonDblClick(
            [this](QMouseEvent *) {
                // Double-click in conversation history: parse callsign for reply
                auto cursor = ui->textEditRX->textCursor();
                auto block = cursor.block();
                auto lineText = block.text();

                // Find the last valid CALLSIGN: pattern in the line
                // Handles both "N - HH:MM:SS - (freq) - CALL: msg" and
                // "HH:MM:SS - (freq) - CALL: msg" formats
                QString callsign;
                int searchFrom = 0;

                // Skip past the ") - " that precedes the message content
                int contentStart = lineText.lastIndexOf(QStringLiteral(") - "));
                if (contentStart >= 0)
                    searchFrom = contentStart + 4;

                // Scan for last CALLSIGN: pattern (handles multi-message lines)
                int lastColonPos = -1;
                for (int i = lineText.length() - 1; i >= searchFrom; --i) {
                    if (lineText[i] == ':') {
                        lastColonPos = i;
                        break;
                    }
                }

                if (lastColonPos > searchFrom) {
                    // Extract word before the last colon
                    int wordStart = lastColonPos - 1;
                    while (wordStart >= searchFrom && (lineText[wordStart].isLetterOrNumber() || lineText[wordStart] == '/'))
                        --wordStart;
                    ++wordStart;
                    QString candidate = lineText.mid(wordStart, lastColonPos - wordStart).trimmed();

                    // Validate: 3-15 chars, has letters and digits (callsign pattern, allows /)
                    if (candidate.length() >= 3 && candidate.length() <= 15) {
                        bool hasLetter = false, hasDigit = false;
                        for (auto ch : candidate) {
                            if (ch.isLetter()) hasLetter = true;
                            if (ch.isDigit()) hasDigit = true;
                        }
                        if (hasLetter && hasDigit) {
                            // If it's exactly our own callsign, look for the target after the colon
                            // (WM8Q/P is NOT WM8Q — exact match required)
                            if (candidate == m_config.my_callsign()) {
                                QString afterColon = lineText.mid(lastColonPos + 1).trimmed();
                                QString target = afterColon.split(QRegularExpression("\\s+")).first();
                                if (target.length() >= 3 && target.length() <= 10) {
                                    bool tl = false, td = false;
                                    for (auto ch : target) {
                                        if (ch.isLetter()) tl = true;
                                        if (ch.isDigit()) td = true;
                                    }
                                    if (tl && td)
                                        callsign = target;
                                }
                            } else {
                                callsign = candidate;
                            }
                        }
                    }
                }

                qWarning() << "[UI] dblclick center: lineText=" << lineText.left(60)
                           << "callsign=" << callsign
                           << "searchFrom=" << searchFrom
                           << "lastColonPos=" << lastColonPos;

                if (!callsign.isEmpty()) {
                    // Determine mode from line prefix
                    auto trimmed = lineText.trimmed();
                    int lineSubmode = -1;
                    if (trimmed.startsWith(QString::fromUtf8("\xe2\x9a\xa1")))
                        lineSubmode = Varicode::JS8CallFT2;
                    else if (trimmed.startsWith("N "))
                        lineSubmode = Varicode::JS8CallNormal;
                    else if (trimmed.startsWith("F "))
                        lineSubmode = Varicode::JS8CallFast;
                    else if (trimmed.startsWith("T "))
                        lineSubmode = Varicode::JS8CallTurbo;
                    else if (trimmed.startsWith("S "))
                        lineSubmode = Varicode::JS8CallSlow;

                    qWarning() << "[UI] dblclick center: selecting" << callsign
                               << "lineSubmode=" << lineSubmode;
                    selectCallsign(callsign, lineSubmode);

                    // Build 122: prefer the callsign list's live offset
                    // (per-frame fresh + sub-band-aware preserved as of
                    // Build 120) over the line's frozen offset. Fall back
                    // to parsing the line's "(NNNN)" only if the callsign
                    // isn't in m_callActivity (aged-out, or the user is
                    // scrolled back into history of a station no longer
                    // tracked). Inhibit either source if <= 1000 Hz
                    // (noise band).
                    ui->tableWidgetRXAll->blockSignals(true);
                    ui->tableWidgetRXAll->clearSelection();
                    int targetFreq = -1;
                    if (m_callActivity.contains(callsign))
                        targetFreq = m_callActivity[callsign].offset;
                    if (targetFreq <= 1000) {
                        int parenOpen = lineText.lastIndexOf('(');
                        int parenClose = lineText.lastIndexOf(')');
                        if (parenOpen >= 0 && parenClose > parenOpen)
                            targetFreq = lineText.mid(parenOpen + 1,
                                                      parenClose - parenOpen - 1)
                                             .toInt();
                    }
                    if (targetFreq > 1000) {
                        setFreqOffsetForRestore(targetFreq, false);
                        // Highlight the band-activity row matching the
                        // LIVE tuned offset, not the line's frozen offset.
                        // [bandcall] In callsign mode prefer the row
                        // whose station IS the clicked call; offset
                        // match is the fallback (two stations can
                        // share a newest offset).
                        int offsetMatch = -1;
                        int callMatch = -1;
                        for (int r = 0; r < ui->tableWidgetRXAll->rowCount(); ++r) {
                            auto item = ui->tableWidgetRXAll->item(r, BAOffset);
                            if (item && item->data(Qt::UserRole).toInt() == targetFreq &&
                                offsetMatch < 0) {
                                offsetMatch = r;
                            }
                            auto ci = ui->tableWidgetRXAll->item(r, BACallsign);
                            if (ci && ci->data(Qt::UserRole).toString() == callsign) {
                                callMatch = r;
                                break;
                            }
                        }
                        int const target = callMatch >= 0 ? callMatch : offsetMatch;
                        if (target >= 0) {
                            ui->tableWidgetRXAll->selectRow(target);
                        }
                    }
                    ui->tableWidgetRXAll->blockSignals(false);

                } else {
                    clearSelection();
                }
                return true;  // consume event
            },
            this));

    auto clearActionSep = new QAction(nullptr);
    clearActionSep->setSeparator(true);

    auto clearActionAll = new QAction(QString("Clear All Lists"), nullptr);
    connect(clearActionAll, &QAction::triggered, this, [this]() {
        if (QMessageBox::Yes !=
            QMessageBox::question(
                this, "Clear All Activity",
                "Are you sure you would like to clear all activity?",
                QMessageBox::Yes | QMessageBox::No)) {
            return;
        }

        clearActivity();
    });

    // setup tablewidget context menus
    auto clearAction1 = new QAction(QString("Clear"), ui->textEditRX);
    connect(clearAction1, &QAction::triggered, this,
            [this]() { clearRXActivity(); });

    auto saveAction = new QAction(QString("Save As..."), ui->textEditRX);
    connect(saveAction, &QAction::triggered, this, [this]() {
        auto writePath =
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        auto writeDir = QDir(writePath);
        auto defaultFilename = writeDir.absoluteFilePath(
            QString("js8call-%1.txt")
                .arg(DriftingDateTime::currentDateTimeUtc().toString(
                    "yyyyMMdd")));

        QString selectedFilter = "*.txt";

        auto filename = QFileDialog::getSaveFileName(
            this, "Save As...", defaultFilename,
            "Text files (*.txt);; All files (*)", &selectedFilter);
        if (filename.isEmpty()) {
            return;
        }

        auto text = ui->textEditRX->toPlainText();
        QFile f(filename);
        if (f.open(QIODevice::Truncate | QIODevice::WriteOnly |
                   QIODevice::Text)) {
            QTextStream stream(&f);
            stream << text;
        }
    });

    ui->textEditRX->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        ui->textEditRX, &QTableWidget::customContextMenuRequested, this,
        [this, clearAction1, clearActionAll, saveAction](QPoint const &point) {
            QMenu *menu = new QMenu(ui->textEditRX);

            buildEditMenu(menu, ui->textEditRX);

            menu->addSeparator();

            menu->addAction(clearAction1);
            menu->addAction(clearActionAll);

            menu->addSeparator();
            menu->addAction(saveAction);

            menu->popup(ui->textEditRX->mapToGlobal(point));
        });

    auto clearAction2 = new QAction(QString("Clear"), ui->extFreeTextMsgEdit);
    connect(clearAction2, &QAction::triggered, this, [this]() {
        resetMessage();
        m_lastTxMessage.clear();
    });

    auto restoreAction = new QAction(QString("Restore Previous Message"),
                                     ui->extFreeTextMsgEdit);
    connect(restoreAction, &QAction::triggered, this,
            [this]() { this->restoreMessage(); });

    ui->extFreeTextMsgEdit->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        ui->extFreeTextMsgEdit, &QTableWidget::customContextMenuRequested, this,
        [this, clearAction2, clearActionAll,
         restoreAction](QPoint const &point) {
            QMenu *menu = new QMenu(ui->extFreeTextMsgEdit);

            auto selectedCall = callsignSelected();
            bool missingCallsign = selectedCall.isEmpty();

            buildSuggestionsMenu(menu, ui->extFreeTextMsgEdit, point);

            restoreAction->setDisabled(m_lastTxMessage.isEmpty());
            menu->addAction(restoreAction);

            auto savedMenu = menu->addMenu("Saved Messages...");
            buildSavedMessagesMenu(savedMenu);

            auto directedMenu =
                menu->addMenu(QString("Directed to %1...").arg(selectedCall));
            directedMenu->setDisabled(missingCallsign);
            buildQueryMenu(directedMenu, selectedCall);

            auto relayMenu = menu->addMenu("Relay via...");
            relayMenu->setDisabled(
                ui->extFreeTextMsgEdit->toPlainText().isEmpty() ||
                m_callActivity.isEmpty());
            buildRelayMenu(relayMenu);

            menu->addSeparator();

            buildEditMenu(menu, ui->extFreeTextMsgEdit);

            menu->addSeparator();

            menu->addAction(clearAction2);
            menu->addAction(clearActionAll);

            // [monfrombox, operator 2026-09-04] LAST entry: when the
            // current (or, if empty, the last transmitted) message
            // has an addressee, offer its monitor. Addressee via the
            // ONE extraction authority (FROM-prefix strip, first hop
            // of a relay chain, @groups refused).
            {
                QString to = ChunkedArq::effectivePeer(
                    QString(),
                    ui->extFreeTextMsgEdit->toPlainText());
                if (to.isEmpty())
                    to = ChunkedArq::effectivePeer(QString(),
                                                   m_lastTxMessage);
                if (!to.isEmpty() &&
                    to.compare(m_config.my_callsign().trimmed(),
                               Qt::CaseInsensitive) != 0) {
                    menu->addSeparator();
                    connect(
                        menu->addAction(
                            tr("Open station monitor for %1").arg(to)),
                        &QAction::triggered, this,
                        [this, to]() { openStationMonitor(to); });
                }
            }

            menu->popup(ui->extFreeTextMsgEdit->mapToGlobal(point));

            displayActivity(true);
        });

    // Install custom delegate for sub-divided Message(s) column
    auto *msgDelegate = new BandActivityMessageDelegate(ui->tableWidgetRXAll);
    msgDelegate->setMyCallsign(m_config.my_callsign());
    ui->tableWidgetRXAll->setItemDelegateForColumn(BAMessage, msgDelegate);

    auto clearAction3 = new QAction(QString("Clear"), ui->tableWidgetRXAll);
    connect(clearAction3, &QAction::triggered, this,
            [this]() { clearBandActivity(); });

    auto removeActivity =
        new QAction(QString("Remove Activity"), ui->tableWidgetRXAll);
    connect(removeActivity, &QAction::triggered, this, [this]() {
        auto selectedItems = ui->tableWidgetRXAll->selectedItems();
        if (selectedItems.isEmpty()) {
            return;
        }

        // [bandcall] Offset from the FIXED Offset column -- the old
        // first-selected-item read returned the wrong column's data
        // whenever the Offset column was hidden.
        int const selRow = selectedItems.first()->row();
        auto *offsetItem = ui->tableWidgetRXAll->item(selRow, BAOffset);
        if (!offsetItem) {
            return;
        }
        int selectedOffset = offsetItem->data(Qt::UserRole).toInt();

        m_bandActivity.remove(selectedOffset);
        displayActivity(true);
    });

    auto logAction = new QAction(QString("Log..."), ui->tableWidgetCalls);
    connect(logAction, &QAction::triggered, this,
            &UI_Constructor::on_logQSOButton_clicked);

    // Disable default header mouseover and click behaviors, they are confusing
    // to users because they give the appearance of allowing sorting by header
    // clicks, which is not actually implemented
    ui->tableWidgetRXAll->horizontalHeader()->setHighlightSections(false);
    ui->tableWidgetRXAll->horizontalHeader()->setSectionsClickable(false);

    ui->tableWidgetRXAll->horizontalHeader()->setContextMenuPolicy(
        Qt::CustomContextMenu);
    connect(
        ui->tableWidgetRXAll->horizontalHeader(),
        &QHeaderView::customContextMenuRequested, this,
        [this](QPoint const &point) {
            QMenu *menu = new QMenu(ui->tableWidgetRXAll);

            // [bandcall] Listing mode first: one row per Offset
            // (classic) or one row per Callsign.
            QMenu *listByMenu = menu->addMenu("List by...");
            buildBandActivityListByMenu(listByMenu);

            QMenu *sortByMenu = menu->addMenu("Sort By...");
            buildBandActivitySortByMenu(sortByMenu);

            QMenu *showColumnsMenu = menu->addMenu("Show Columns...");
            buildShowColumnsMenu(showColumnsMenu, "band");

            menu->popup(
                ui->tableWidgetRXAll->horizontalHeader()->mapToGlobal(point));
        });

    ui->tableWidgetRXAll->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        ui->tableWidgetRXAll, &QTableWidget::customContextMenuRequested, this,
        [this, clearAction3, clearActionAll, removeActivity,
         logAction](QPoint const &point) {
            QMenu *menu = new QMenu(ui->tableWidgetRXAll);

            // clear the selection of the call widget on right click
            // but only if the table has rows.
            if (ui->tableWidgetRXAll->rowAt(point.y()) != -1) {
                ui->tableWidgetCalls->selectionModel()->clearSelection();
            }

            QString selectedCall = callsignSelected();
            bool missingCallsign = selectedCall.isEmpty();
            bool isAllCall = isAllCallIncluded(selectedCall);

            // [bandcall] Row identity from FIXED columns (the old
            // first-selected-item read broke with the Offset column
            // hidden). In callsign mode the row's speed/drift are the
            // station's newest frame, carried in the row's own items --
            // the model bucket's .last() could be a DIFFERENT station.
            int selectedOffset = -1;
            int rowSubmode = -1;
            float rowTdrift = 0;
            bool haveRowData = false;
            if (!ui->tableWidgetRXAll->selectedItems().isEmpty()) {
                int const selRow =
                    ui->tableWidgetRXAll->selectedItems().first()->row();
                if (auto *it = ui->tableWidgetRXAll->item(selRow, BAOffset))
                    selectedOffset = it->data(Qt::UserRole).toInt();
                if (auto *it = ui->tableWidgetRXAll->item(selRow, BASpeed)) {
                    rowSubmode = it->data(Qt::UserRole).toInt();
                    haveRowData = true;
                }
                if (auto *it = ui->tableWidgetRXAll->item(selRow, BATDrift))
                    rowTdrift = it->data(Qt::UserRole).toFloat();
            }
            bool const listByCall = bandListByCall();

            // [bandcall5] In callsign mode every row IS a station:
            // offer its monitor first, from the RIGHT-CLICKED row's
            // Callsign column (any column of the row), falling back
            // to the selection -- same shape as the calls table's
            // entry ([stamon, TODO #210]).
            if (listByCall) {
                QString mcall;
                if (int const row =
                        ui->tableWidgetRXAll->rowAt(point.y());
                    row != -1)
                    if (auto *it =
                            ui->tableWidgetRXAll->item(row, BACallsign))
                        mcall = it->data(Qt::UserRole).toString();
                if (mcall.isEmpty())
                    mcall = selectedCall;
                if (!mcall.isEmpty() &&
                    !mcall.startsWith(QLatin1Char('@'))) {
                    auto *monAction = menu->addAction(
                        tr("Open station monitor for %1").arg(mcall));
                    connect(monAction, &QAction::triggered, this,
                            [this, mcall]() {
                                openStationMonitor(mcall);
                            });
                    menu->addSeparator();
                }
            }

            if (selectedOffset != -1) {
                auto qsyAction = menu->addAction(
                    QString("Jump to %1Hz").arg(selectedOffset));
                connect(qsyAction, &QAction::triggered, this,
                        [this, selectedOffset]() {
                            setFreqOffsetForRestore(selectedOffset, false);
                        });

                if (m_wideGraph->filterEnabled()) {
                    auto filterQsyAction = menu->addAction(
                        QString("Center filter at %1Hz").arg(selectedOffset));
                    connect(filterQsyAction, &QAction::triggered, this,
                            [this, selectedOffset]() {
                                m_wideGraph->setFilterCenter(selectedOffset);
                            });
                }

                // Speed and drift source: in callsign mode the row's
                // own stored values (the station's newest frame); in
                // offset mode the bucket's newest frame, as before.
                int submode = -1;
                float tdriftVal = 0;
                bool haveFrame = false;
                if (listByCall) {
                    submode = rowSubmode;
                    tdriftVal = rowTdrift;
                    haveFrame = haveRowData && rowSubmode >= 0;
                } else {
                    auto items = m_bandActivity.value(selectedOffset);
                    if (!items.isEmpty()) {
                        submode = items.last().submode;
                        tdriftVal = items.last().tdrift;
                        haveFrame = true;
                    }
                }
                if (haveFrame) {
                    auto speed = JS8::Submode::name(submode);
                    if (submode != m_nSubMode) {
                        auto qrqAction =
                            menu->addAction(QString("Jump to %1%2 speed")
                                                .arg(speed.left(1))
                                                .arg(speed.mid(1).toLower()));
                        connect(qrqAction, &QAction::triggered, this,
                                [this, submode]() { setSubmode(submode); });
                    }

                    int tdrift = -int(tdriftVal * 1000);
                    auto qtrAction = menu->addAction(
                        QString("Jump to %1 ms time drift").arg(tdrift));
                    connect(qtrAction, &QAction::triggered, this,
                            [this, tdrift]() { setDrift(tdrift); });
                }

                menu->addSeparator();
            }

            menu->addAction(logAction);
            logAction->setDisabled(missingCallsign || isAllCall);

            menu->addSeparator();

            auto savedMenu = menu->addMenu("Saved Messages...");
            buildSavedMessagesMenu(savedMenu);

            auto directedMenu =
                menu->addMenu(QString("Directed to %1...").arg(selectedCall));
            directedMenu->setDisabled(missingCallsign);
            buildQueryMenu(directedMenu, selectedCall);

            auto relayAction = buildRelayAction(selectedCall);
            relayAction->setText(QString("Relay via %1...").arg(selectedCall));
            relayAction->setDisabled(missingCallsign);
            menu->addActions({relayAction});

            auto deselectAction =
                menu->addAction(QString("Deselect %1").arg(selectedCall));
            deselectAction->setDisabled(missingCallsign);
            connect(deselectAction, &QAction::triggered, this, [this]() {
                ui->tableWidgetRXAll->clearSelection();
                ui->tableWidgetCalls->clearSelection();
                messagePanel_->setCall("%");
            });

            // [bandcall] "Remove Activity" is offset-bucket removal;
            // in callsign mode a row is a station spanning several
            // buckets, so the entry is absent (operator ruling).
            if (!listByCall) {
                menu->addSeparator();

                removeActivity->setDisabled(selectedOffset == -1);
                menu->addAction(removeActivity);
            }

            menu->addSeparator();
            menu->addAction(clearAction3);
            menu->addAction(clearActionAll);

            menu->popup(ui->tableWidgetRXAll->mapToGlobal(point));

            displayActivity(true);
        });

    auto clearAction4 =
        new QAction(QString("Clear Entire List"), ui->tableWidgetCalls);
    connect(clearAction4, &QAction::triggered, this,
            [this]() { clearCallActivity(); });

    auto addStation = new QAction(QString("Add New Station or Group..."),
                                  ui->tableWidgetCalls);
    connect(addStation, &QAction::triggered, this, [this]() {
        bool ok = false;
        QString callsign =
            QInputDialog::getText(this, tr("Add New Station or Group"),
                                  tr("Station or Group Callsign:"),
                                  QLineEdit::Normal, "", &ok)
                .toUpper()
                .trimmed();
        if (!ok || callsign.trimmed().isEmpty()) {
            return;
        }

        // if we're adding allcall, turn off allcall avoidance
        if (callsign == "@ALLCALL") {
            m_config.set_avoid_allcall(false);
        } else if (callsign.startsWith("@")) {
            if (Varicode::isCompoundCallsign(callsign)) {
                m_config.addGroup(callsign);
            } else {
                JS8MessageBox::critical_message(
                    this, QString("%1 is not a valid group").arg(callsign));
            }

        } else {
            if (Varicode::isValidCallsign(callsign, nullptr)) {
                CallDetail cd = {};
                cd.call = callsign;
                m_callActivity[callsign] = cd;
            } else {
                JS8MessageBox::critical_message(
                    this, QString("%1 is not a valid callsign or group")
                              .arg(callsign));
            }
        }

        displayActivity(true);
    });

    auto removeStation =
        new QAction(QString("Remove Station"), ui->tableWidgetCalls);
    connect(removeStation, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        if (selectedCall == "@ALLCALL") {
            m_config.set_avoid_allcall(true);
        } else if (selectedCall.startsWith("@")) {
            m_config.removeGroup(selectedCall);
        } else if (m_callActivity.contains(selectedCall)) {
            m_callActivity.remove(selectedCall);
        }

        displayActivity(true);
    });

    connect(ui->actionShow_Message_Inbox, &QAction::toggled, this,
            [this](bool checked) {

        if (checked) {
            ensureMessageDock();
            messageDock_->show();
            messageDock_->raise();

            /*
             * Disable the automatic selected call filter now that the inbox lives
             * in a dockable panel
            QString selectedCall = callsignSelected();
            if (selectedCall.isEmpty()) selectedCall = "%";
            messagePanel_->setCall(selectedCall);
            */
          messagePanel_->setCall("%");
        } else {
            if (messageDock_) {
                // pick ONE: hide or close; for docks, hide is simplest
                messageDock_->hide();
            }
        }
    });

    // When a message is added to the inbox, refresh the message panel
    connect(this, &UI_Constructor::messageAdded, messagePanel_, &MessagePanel::refresh);

    auto historyAction =
        new QAction(QString("Show Message Inbox..."), ui->tableWidgetCalls);
    connect(historyAction, &QAction::triggered, this, [this]() {
      ensureMessageDock();
      messageDock_->show();
      messageDock_->raise();

      QString selectedCall = callsignSelected();
      messagePanel_->setCall(selectedCall);
    });

    auto localMessageAction =
        new QAction(QString("Store Message..."), ui->tableWidgetCalls);
    connect(localMessageAction, &QAction::triggered, this, [this]() {
        QString selectedCall = callsignSelected();
        if (selectedCall.isEmpty()) {
            return;
        }

        auto m = new MessageReplyDialog(this);
        m->setWindowTitle("Message");
        m->setLabel(
            QString("Store this message locally for %1:").arg(selectedCall));
        if (m->exec() != QMessageBox::Accepted) {
            return;
        }

        CommandDetail d = {};
        d.cmd = " MSG ";
        d.to = selectedCall;
        d.from = m_config.my_callsign();
        d.relayPath = d.from;
        d.text = m->textValue();
        d.utcTimestamp = DriftingDateTime::currentDateTimeUtc();
        d.submode = m_nSubMode;

        addCommandToStorage("STORE", d);
    });

    // Disable default header mouseover and click behaviors, they are confusing
    // to users because they give the appearance of allowing sorting by header
    // clicks, which is not actually implemented
    ui->tableWidgetCalls->horizontalHeader()->setHighlightSections(false);
    ui->tableWidgetCalls->horizontalHeader()->setSectionsClickable(false);

    ui->tableWidgetCalls->horizontalHeader()->setContextMenuPolicy(
        Qt::CustomContextMenu);
    connect(
        ui->tableWidgetCalls->horizontalHeader(),
        &QHeaderView::customContextMenuRequested, this,
        [this](QPoint const &point) {
            QMenu *menu = new QMenu(ui->tableWidgetCalls);

            QMenu *sortByMenu = menu->addMenu("Sort By...");
            buildCallActivitySortByMenu(sortByMenu);

            QMenu *showColumnsMenu = menu->addMenu("Show Columns...");
            buildShowColumnsMenu(showColumnsMenu, "call");

            menu->popup(
                ui->tableWidgetCalls->horizontalHeader()->mapToGlobal(point));
        });

    ui->tableWidgetCalls->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        ui->tableWidgetCalls, &QTableWidget::customContextMenuRequested, this,
        [this, logAction, historyAction, localMessageAction, clearAction4,
         clearActionAll, addStation, removeStation](QPoint const &point) {
            QMenu *menu = new QMenu(ui->tableWidgetCalls);

            // clear the selection of the call widget on right click
            // but only if the table has rows.
            if (ui->tableWidgetCalls->rowAt(point.y()) != -1) {
                ui->tableWidgetRXAll->selectionModel()->clearSelection();
            }

            QString selectedCall = callsignSelected();
            bool isAllCall = isAllCallIncluded(selectedCall);
            // bool isGroupCall = isGroupCallIncluded(selectedCall);
            bool missingCallsign = selectedCall.isEmpty();

            // [stamon, TODO #210] Follow the RIGHT-CLICKED row's
            // station (UserRole carries the bare call), falling
            // back to the selection. Standard feature since 465.
            {
                QString mcall;
                if (int const row =
                        ui->tableWidgetCalls->rowAt(point.y());
                    row != -1)
                    if (auto *it = ui->tableWidgetCalls->item(row, 0))
                        mcall = it->data(Qt::UserRole).toString();
                if (mcall.isEmpty())
                    mcall = selectedCall;
                if (!mcall.isEmpty() &&
                    !mcall.startsWith(QLatin1Char('@'))) {
                    auto *monAction = menu->addAction(
                        tr("Open station monitor for %1").arg(mcall));
                    connect(monAction, &QAction::triggered, this,
                            [this, mcall]() {
                                openStationMonitor(mcall);
                            });
                    menu->addSeparator();
                }
            }

            if (!missingCallsign && !isAllCall) {
                int selectedOffset = m_callActivity[selectedCall].offset;
                if (selectedOffset != -1) {
                    auto qsyAction = menu->addAction(
                        QString("Jump to %1Hz").arg(selectedOffset));
                    connect(qsyAction, &QAction::triggered, this,
                            [this, selectedOffset]() {
                                setFreqOffsetForRestore(selectedOffset, false);
                            });

                    if (m_wideGraph->filterEnabled()) {
                        auto filterQsyAction =
                            menu->addAction(QString("Center filter at %1Hz")
                                                .arg(selectedOffset));
                        connect(filterQsyAction, &QAction::triggered, this,
                                [this, selectedOffset]() {
                                    m_wideGraph->setFilterCenter(
                                        selectedOffset);
                                });
                    }

                    int submode = m_callActivity[selectedCall].submode;
                    auto speed = JS8::Submode::name(submode);
                    if (submode != m_nSubMode) {
                        auto qrqAction =
                            menu->addAction(QString("Jump to %1%2 speed")
                                                .arg(speed.left(1))
                                                .arg(speed.mid(1).toLower()));
                        connect(qrqAction, &QAction::triggered, this,
                                [this, submode]() { setSubmode(submode); });
                    }

                    int tdrift =
                        -int(m_callActivity[selectedCall].tdrift * 1000);
                    auto qtrAction = menu->addAction(
                        QString("Jump to %1 ms time drift").arg(tdrift));
                    connect(qtrAction, &QAction::triggered, this,
                            [this, tdrift]() { setDrift(tdrift); });

                    menu->addSeparator();
                }
            }

            menu->addAction(logAction);
            logAction->setDisabled(missingCallsign || isAllCall);

            menu->addAction(historyAction);
            historyAction->setDisabled(missingCallsign || isAllCall ||
                                       !hasMessageHistory(selectedCall));

            menu->addAction(localMessageAction);
            localMessageAction->setDisabled(missingCallsign || isAllCall);

            menu->addSeparator();

            auto savedMenu = menu->addMenu("Saved Messages...");
            buildSavedMessagesMenu(savedMenu);

            auto directedMenu =
                menu->addMenu(QString("Directed to %1...").arg(selectedCall));
            directedMenu->setDisabled(missingCallsign);
            buildQueryMenu(directedMenu, selectedCall);

            auto relayAction = buildRelayAction(selectedCall);
            relayAction->setText(QString("Relay via %1...").arg(selectedCall));
            relayAction->setDisabled(missingCallsign || isAllCall);
            menu->addActions({relayAction});

            auto deselect =
                menu->addAction(QString("Deselect %1").arg(selectedCall));
            deselect->setDisabled(missingCallsign);
            connect(deselect, &QAction::triggered, this, [this]() {
                ui->tableWidgetRXAll->clearSelection();
                ui->tableWidgetCalls->clearSelection();
                messagePanel_->setCall("%");
            });

            menu->addSeparator();

            menu->addAction(addStation);
            removeStation->setDisabled(missingCallsign);
            removeStation->setText(selectedCall.startsWith("@")
                                       ? "Remove This Group"
                                       : "Remove This Station");
            menu->addAction(removeStation);

            menu->addSeparator();
            menu->addAction(clearAction4);
            menu->addAction(clearActionAll);

            menu->popup(ui->tableWidgetCalls->mapToGlobal(point));
        });

    connect(ui->tableWidgetRXAll->selectionModel(),
            &QItemSelectionModel::selectionChanged, this,
            &UI_Constructor::tableSelectionChanged);
    connect(ui->tableWidgetCalls->selectionModel(),
            &QItemSelectionModel::selectionChanged, this,
            &UI_Constructor::tableSelectionChanged);

    auto p = ui->tableWidgetRXAll->palette();
    p.setColor(QPalette::Inactive, QPalette::Highlight,
               p.color(QPalette::Active, QPalette::Highlight));
    ui->tableWidgetRXAll->setPalette(p);

    p = ui->tableWidgetCalls->palette();
    p.setColor(QPalette::Inactive, QPalette::Highlight,
               p.color(QPalette::Active, QPalette::Highlight));
    ui->tableWidgetCalls->setPalette(p);

    ui->hbMacroButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->hbMacroButton, &QPushButton::customContextMenuRequested, this,
            [this](QPoint const &point) {
                QMenu *menu = new QMenu(ui->hbMacroButton);

                buildHeartbeatMenu(menu);

                menu->popup(ui->hbMacroButton->mapToGlobal(point));
            });

    ui->cqMacroButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->cqMacroButton, &QPushButton::customContextMenuRequested, this,
            [this](QPoint const &point) {
                QMenu *menu = new QMenu(ui->cqMacroButton);

                buildCQMenu(menu);

                menu->popup(ui->cqMacroButton->mapToGlobal(point));
            });

    // Don't block heartbeat's first run...
    m_lastTxStartTime = DriftingDateTime::currentDateTimeUtc().addSecs(-300);

    // But do block the decoder's first run until 50% through next transmit
    // period
    m_lastTxStopTime = nextTransmitCycle().addSecs(-m_TRperiod / 2);

    int width = 75;
    /*
    QList<QPushButton*> btns;
    foreach(auto child, ui->buttonGrid->children()){
        if(!child->isWidgetType()){
            continue;
        }

        if(!child->objectName().contains("Button")){
            continue;
        }

        auto b = qobject_cast<QPushButton*>(child);
        width = qMax(width, b->geometry().width());
        btns.append(b);
    }
    */
    foreach (auto child, ui->buttonGrid->children()) {
        if (!child->isWidgetType()) {
            continue;
        }

        if (!child->objectName().contains("Button")) {
            continue;
        }

        auto b = qobject_cast<QPushButton *>(child);
        b->setCursor(QCursor(Qt::PointingHandCursor));
    }
    auto buttonLayout = ui->buttonGrid->layout();
    auto gridButtonLayout = qobject_cast<QGridLayout *>(buttonLayout);
    gridButtonLayout->setColumnMinimumWidth(0, width);
    gridButtonLayout->setColumnMinimumWidth(1, width);
    gridButtonLayout->setColumnMinimumWidth(2, width);
    gridButtonLayout->setColumnStretch(0, 1);
    gridButtonLayout->setColumnStretch(1, 1);
    gridButtonLayout->setColumnStretch(2, 1);

    // dial up and down buttons sizes
    ui->dialFreqUpButton->setFixedSize(30, 24);
    ui->dialFreqDownButton->setFixedSize(30, 24);

    // Prepare spotting configuration...
    prepareApi();
    prepareSpotting();

    displayActivity(true);

    // [#148] Every action-row widget — including the five
    // mode-speed buttons and the Send chevron — is DECLARED in the
    // .ui (actionRowLayout). Code wires behavior only.
    {
        {
            struct ModeWire { QPushButton *btn; int submode; };
            for (auto const &mw : {
                     ModeWire{ui->modeBtnSlow, Varicode::JS8CallSlow},
                     ModeWire{ui->modeBtnNormal,
                              Varicode::JS8CallNormal},
                     ModeWire{ui->modeBtnFast, Varicode::JS8CallFast},
                     ModeWire{ui->modeBtnTurbo,
                              Varicode::JS8CallTurbo},
                     ModeWire{ui->modeBtnFT2, Varicode::JS8CallFT2}})
                connect(mw.btn, &QPushButton::clicked, this,
                        [this, submode = mw.submode]() {
                            setSubmode(submode);
                        });

            // [#148] Live width distribution: recompute on every bar
            // resize (buttons expand AS the window widens) and once
            // now for the initial state.
            ui->macroHorizonalWidget->installEventFilter(this);
            distributeActionRowWidths();

            // [#148 split Send] Widget live from the start; the .ui
            // enabled=false intent is carried by the sendOff state.
            ui->startTxButton->setEnabled(true);
            ui->startTxButton->setProperty("sendOff", true);
            m_sendSideOn = false;


            // ARQ enable toggle. Proxies the existing
            // actionModeReplicatorProtocol menu action so the on/off
            // state stays single-sourced (settings persistence,
            // ChunkedArq plumbing, mode_label/+ARQ suffix, and the
            // [BUILD 298] m_arqButton REMOVED. The persistent "ARQ on/off"
            // toggle is gone; ARQ is now opt-in per-message via the
            // Send options menu's "Send using ARQ" action. The internal
            // ui->actionModeReplicatorProtocol QAction still exists (the
            // ChunkedArq::Manager checks it; toggling it pushes state to
            // the Modulator) — it's just no longer bound to a visible
            // button. The "Send using ARQ" action sets it true at
            // dispatch time and the sendComplete/sendFailed handlers
            // unset it post-send.
            // canChangeMode gate at mainwindow.cpp:3415 area no longer
            // applies to a button (m_arqButton is null) — the gate now
            // governs the action's enable state instead.

            // [FILE-XFER build 280 2026-06-16] Send-action chevron —
            // small QToolButton glued to the right edge of the Send
            // button, opens a menu with "Send file…" (and future
            // send-actions). The standalone "File" button from build
            // 276 is gone — operator now reaches file send via
            // Send-button-chevron → "Send file…". Visually the
            // chevron sits flush against startTxButton with no gap
            // (zero-spacing sendPair layout in the flat row, #148).
            // [#148] Chevron declared in the .ui; menu wired here.
            {
                auto *menu = new QMenu(ui->startTxButton); // [#148] split Send
                // [BUILD 298] "Send using ARQ" — first menu item,
                // replaces the standalone ARQ toggle button. Enabling
                // ARQ is now opt-in per-message: this action turns
                // ARQ on, fires the regular Send path, and
                // sendComplete/sendFailed disables it again. No
                // persistent "armed" state.
                m_sendArqAction = menu->addAction(
                    QStringLiteral("Send using ARQ"));
                m_sendArqAction->setToolTip(
                    "Send the current outgoing message using ARQ "
                    "(Auto Repeat Request). Reliable delivery with "
                    "per-sub-message ACK/NACK and CRC verification. Requires "
                    "a selected call sign.");
                connect(m_sendArqAction, &QAction::triggered,
                        this, &UI_Constructor::on_sendUsingArqAction_triggered);

                m_sendFileAction = menu->addAction(QStringLiteral("Send file…"));
                // [FILE-XFER build 284] Wire the action's triggered
                // signal to the file-send handler. (Builds 280-283
                // shipped without this connect — the menu opened but
                // clicking "Send file…" was a no-op. Qt's named-slot
                // auto-connect only fires for widgets registered via
                // QMetaObject::connectSlotsByName, not for QActions
                // we add to a programmatically-built QMenu.)
                connect(m_sendFileAction, &QAction::triggered,
                        this, &UI_Constructor::on_sendFileButton_clicked);
                // Action tooltip surfaces the size envelope + auto-
                // ARQ behavior on hover so the operator knows what
                // they're committing to before the file picker opens.
                // [BUILD 344 tooltip] Rewritten for the V3 era: size
                // numbers were V2-only, and "Base32" is untrue for
                // native transfers (raw binary frames).
                m_sendFileAction->setToolTip(
                    "Send file via ARQ. Pick a local file (any "
                    "format). Capacity depends on compressibility and "
                    "speed mode — in Subspace up to ~6 KB on the air "
                    "after built-in compression (often 10-30 KB of "
                    "text); less in legacy speeds or to older builds. "
                    "Preserves the file exactly (byte-for-byte, "
                    "SHA-256 verified), so it's ideal for forms where "
                    "spacing matters. Compressed with GZIP but NOT "
                    "encrypted — encryption is not permitted on "
                    "amateur bands. Tip: a text file can carry a "
                    "number of web links — the receiver gets them as "
                    "clickable URLs.");

                // [BUILD 338] "Send web link (URL)…" — directly below
                // Send file. Prompts for a URL, wraps it in link.txt,
                // sends via the same ARQ file-transfer pipeline; the
                // receiver renders it as a clickable link (TODO #95).
                m_sendWebLinkAction =
                    menu->addAction(QStringLiteral("Send web link (URL)…"));
                connect(m_sendWebLinkAction, &QAction::triggered,
                        this,
                        &UI_Constructor::on_sendWebLinkAction_triggered);
                m_sendWebLinkAction->setToolTip(
                    "Send a web link (URL) by copying / pasting and "
                    "transmit");

                // [BUILD 331-visHailEpi8] "Send audio-visual HAIL" —
                // third item. Three-cycle sequence: standard Subspace
                // HAIL message (`<mycall>: @ALLCALL ACK`) followed by
                // two back-to-back lightning-bolt waterfall silhouettes
                // (Hellschreiber-style raster, decoder-agnostic, in-
                // band). Reaches both software-decoder peers AND
                // waterfall-watching humans. Disabled during ARQ
                // transfers and while another audio-visual HAIL is
                // already in flight.
                // [ICS213 2026-08-18] Form-entry replaces the file
                // picker; everything downstream is the ordinary ARQ
                // file transfer (operator constraint: no new wire
                // format, V1-compatible).
                m_sendIcs213Action = menu->addAction(
                    QStringLiteral("Send ICS-213 form…"));
                m_sendIcs213Action->setToolTip(
                    "Compose a standard ICS-213 General Message form "
                    "and send it as an ARQ file transfer.");
                connect(m_sendIcs213Action, &QAction::triggered, this,
                        &UI_Constructor::on_sendIcs213FormAction_triggered);

                m_sendVisibleHailAction = menu->addAction(
                    QStringLiteral("Send audio-visual HAIL"));
                m_sendVisibleHailAction->setToolTip(
                    "Send a standard Subspace HAIL "
                    "message followed by a distinctive image visible on the waterfall, and a readily identifiable sound from your radio speaker.");
                connect(m_sendVisibleHailAction, &QAction::triggered,
                        this,
                        &UI_Constructor::on_sendVisibleHailAction_triggered);

                // QMenu doesn't show action tooltips by default;
                // enable the standard tooltips role so the hover
                // delay surfaces the text.
                menu->setToolTipsVisible(true);
                ui->startTxButton->setMenu(menu); // MenuButtonPopup arrow opens it
            }

            // Set initial checked state
            ui->modeBtnNormal->setChecked(m_nSubMode == Varicode::JS8CallNormal);
            ui->modeBtnFast->setChecked(m_nSubMode == Varicode::JS8CallFast);
            ui->modeBtnTurbo->setChecked(m_nSubMode == Varicode::JS8CallTurbo);
            ui->modeBtnSlow->setChecked(m_nSubMode == Varicode::JS8CallSlow);
            ui->modeBtnFT2->setChecked(m_nSubMode == Varicode::JS8CallFT2);
        }
    }

    m_txTextDirtyDebounce.setSingleShot(true);
    connect(&m_txTextDirtyDebounce, &QTimer::timeout, this,
            &UI_Constructor::refreshTextDisplay);
    qCDebug(mainwindow_js8)
        << "Main window constructor has done all connect (aka plumbing) work.";

    m_TxDelay = m_config.txDelay();
    m_hb_loop->onTxDelayChange(llround(m_TxDelay * 1000.0));
    m_cq_loop->onTxDelayChange(llround(m_TxDelay * 1000.0));
    m_hb_loop->onPlumbingCompleted();
    m_cq_loop->onPlumbingCompleted();
    DriftingDateTimeSingleton::getSingleton().onPlumbingCompleted();
    qCDebug(mainwindow_js8)
        << "Initialization with onPlumbingCompleted() has completed.";

    QTimer::singleShot(500, this, &UI_Constructor::initializeDummyData);
    QTimer::singleShot(500, this, &UI_Constructor::initializeGroupMessage);

    // [BUILD 314, extended] First-run discovery balloons. Exactly ONE
    // balloon may show per startup — hints are checked in priority
    // order and a suppressed hint keeps its flag unset, so it shows
    // on a later startup instead. Deferred 1500 ms so the main window
    // is fully shown and anchors have their global positions.
    {
        QPointer<UI_Constructor> const self(this);
        QTimer::singleShot(1500, this, [self]() {
            if (!self) return;

            // Priority 1: ARQ / Send-chevron discovery (Build 314).
            if (!self->m_settings->value("FirstRunArqHintShown", false)
                     .toBool() &&
                self->ui->startTxButton) {
                auto *balloon = new SpeechBalloon(
                    tr("\xf0\x9f\x91\x8b  Click here to to use the ARQ protocol "
                       "(reliable delivery) to send or relay a message or to send a file. "
                       "Use '@ALLCALL QUERY ARQ?' to find ARQ-ready stations. "
                       "Click anywhere to dismiss."),
                    self->ui->startTxButton);
                balloon->setTailSide(SpeechBalloon::TailSide::Bottom);
                balloon->setAutoDismissMs(45000);
                balloon->showAtTarget();
                self->m_settings->setValue("FirstRunArqHintShown", true);
                return; // one balloon per startup
            }

            // Priority 2: Spots Map discovery (Build 334 era).
            if (!self->m_settings
                     ->value("FirstRunSpotsMapHintShown", false)
                     .toBool()) {
                auto *bar = self->menuBar();
                auto *balloon = new SpeechBalloon(
                    tr("Select the 'Spots Map' here to see who has "
                       "heard you recently. To help others get "
                       "spotted, select 'Enable spotting to reporting "
                       "networks' in Settings | Reporting. "
                       "Click to dismiss."),
                    bar);
                balloon->setTargetRectOverride(bar->actionGeometry(
                    self->ui->menuWindow->menuAction()));
                balloon->setTailSide(SpeechBalloon::TailSide::Top);
                balloon->setAutoDismissMs(45000);
                balloon->showAtTarget();
                self->m_settings->setValue("FirstRunSpotsMapHintShown",
                                           true);
                return; // one balloon per startup
            }

            // Priority 3: Auto-route discovery (TODO #203).
            if (!self->m_settings
                     ->value("FirstRunAutoRouteHintShown", false)
                     .toBool()) {
                auto *bar = self->menuBar();
                auto *balloon = new SpeechBalloon(
                    tr("Auto-route can find a relay path to a "
                       "station or grid square for you: open the "
                       "Spots Map from this menu and click "
                       "'Auto-route'. Click to dismiss."),
                    bar);
                balloon->setTargetRectOverride(bar->actionGeometry(
                    self->ui->menuWindow->menuAction()));
                balloon->setTailSide(SpeechBalloon::TailSide::Top);
                balloon->setAutoDismissMs(45000);
                balloon->showAtTarget();
                self->m_settings->setValue(
                    "FirstRunAutoRouteHintShown", true);
                return; // one balloon per startup
            }

            // Priority 4: waterfall double-click-to-call discovery
            // (Build 336). Only when the waterfall window is visible
            // — otherwise the flag stays unset and the hint shows on
            // a later startup.
            if (!self->m_settings
                     ->value("FirstRunWaterfallDblClickHintShown",
                             false)
                     .toBool() &&
                self->m_wideGraph && self->m_wideGraph->isVisible()) {
                auto *balloon = new SpeechBalloon(
                    tr("Double-click on any call sign on the waterfall "
                       "to create an outgoing message. "
                       "Click here to dismiss."),
                    self->m_wideGraph.data());
                balloon->setTargetRectOverride(
                    QRect(self->m_wideGraph->width() / 2 - 60, 40,
                          120, 24));
                balloon->setTailSide(SpeechBalloon::TailSide::Top);
                balloon->setAutoDismissMs(45000);
                balloon->showAtTarget();
                self->m_settings->setValue(
                    "FirstRunWaterfallDblClickHintShown", true);
                return; // one balloon per startup
            }

            // [hints 2026-08-19] Feature reminders (operator-specified
            // wording and priority order a > b > c > d).
            // (a) Spots Map QSO + Relay Builder — View menu.
            if (!self->m_settings
                     ->value("HintSpotsMapRelayShown", false)
                     .toBool()) {
                auto *bar = self->menuBar();
                auto *balloon = new SpeechBalloon(
                    tr("Click on 'Show Spots Map' to start QSOs and "
                       "set up message relays between stations.\n"
                       "Click here to dismiss."),
                    bar);
                balloon->setTargetRectOverride(bar->actionGeometry(
                    self->ui->menuWindow->menuAction()));
                balloon->setTailSide(SpeechBalloon::TailSide::Top);
                balloon->setAutoDismissMs(45000);
                balloon->showAtTarget();
                self->m_settings->setValue("HintSpotsMapRelayShown",
                                           true);
                return; // one balloon per startup
            }

            // (b) ICS-213 forms — the Send button's down arrow.
            if (!self->m_settings->value("HintIcs213Shown", false)
                     .toBool() &&
                self->ui->startTxButton) {
                auto *balloon = new SpeechBalloon(
                    tr("Click on the down arrow to compose and send "
                       "ICS-213 Forms.\n"
                       "Click here to dismiss."),
                    self->ui->startTxButton);
                balloon->setTailSide(SpeechBalloon::TailSide::Bottom);
                balloon->setAutoDismissMs(45000);
                balloon->showAtTarget();
                self->m_settings->setValue("HintIcs213Shown", true);
                return; // one balloon per startup
            }

            // (c) ARQ Monitor — View menu.
            if (!self->m_settings->value("HintArqMonitorShown", false)
                     .toBool()) {
                auto *bar = self->menuBar();
                auto *balloon = new SpeechBalloon(
                    tr("Click on 'Show ARQ Monitor' to view ARQ "
                       "conversations between other stations.\n"
                       "Click here to dismiss."),
                    bar);
                balloon->setTargetRectOverride(bar->actionGeometry(
                    self->ui->menuWindow->menuAction()));
                balloon->setTailSide(SpeechBalloon::TailSide::Top);
                balloon->setAutoDismissMs(45000);
                balloon->showAtTarget();
                self->m_settings->setValue("HintArqMonitorShown", true);
                return; // one balloon per startup
            }

            // (d) How-to guides — Help menu.
            if (!self->m_settings->value("HintGuideShown", false)
                     .toBool()) {
                auto *bar = self->menuBar();
                auto *balloon = new SpeechBalloon(
                    tr("Click on 'Subspace Edition Guide' to view "
                       "step-by-step 'How-to' guides.\n"
                       "Click here to dismiss."),
                    bar);
                balloon->setTargetRectOverride(bar->actionGeometry(
                    self->ui->menuHelp->menuAction()));
                balloon->setTailSide(SpeechBalloon::TailSide::Top);
                balloon->setAutoDismissMs(45000);
                balloon->showAtTarget();
                self->m_settings->setValue("HintGuideShown", true);
                return; // one balloon per startup
            }

            // (e) Station monitor -- View menu (Build 465; joined
            // the one-per-startup chain in 465 after the first cut
            // jumped the line with an ad-hoc QLabel).
            if (!self->m_settings
                     ->value("HintStationMonitorShown", false)
                     .toBool()) {
                auto *bar = self->menuBar();
                auto *balloon = new SpeechBalloon(
                    tr("View conversation(s) by call sign in a "
                       "separate window. Select 'Show Station "
                       "Monitor', or right-click on the Spots Map, "
                       "waterfall, or call sign list.\n"
                       "Click to dismiss."),
                    bar);
                balloon->setTargetRectOverride(bar->actionGeometry(
                    self->ui->menuWindow->menuAction()));
                balloon->setTailSide(SpeechBalloon::TailSide::Top);
                balloon->setAutoDismissMs(45000);
                balloon->showAtTarget();
                self->m_settings->setValue("HintStationMonitorShown",
                                           true);
                return; // one balloon per startup
            }

            // (f) [bandhint] Band activity per-station listing
            // (Build 472) -- first Yes/No balloon: Yes switches the
            // band activity window to "List by Callsign"; No, a
            // body click, or the 45 s timeout just dismisses. Shown
            // once either way.
            if (!self->m_settings->value("HintBandListByShown", false)
                     .toBool()) {
                auto *balloon = new SpeechBalloon(
                    tr("The Band Activity window is more useful "
                       "when there's only one call sign per line. "
                       "Do you want to try it?"),
                    self->ui->tableWidgetRXAll);
                balloon->setTailSide(SpeechBalloon::TailSide::Left);
                balloon->setYesNoChoice([self]() {
                    self->setBandListBy(QStringLiteral("call"));
                });
                balloon->setAutoDismissMs(45000);
                balloon->showAtTarget();
                self->m_settings->setValue("HintBandListByShown",
                                           true);
                return; // one balloon per startup
            }
        });
    }

    // this must be the last statement of constructor
    if (!m_valid)
        throw std::runtime_error{"Fatal initialization exception"};
}

// [#187 intelminer] Kick a full corpus mine on a worker thread; the
// result comes back queued to the GUI thread for the grid-bank
// seeding (GridDb lives there) and the status line. force=true is
// the "Rebuild routing knowledge" menu action (ignores the
// unchanged-logs skip).
#include "JS8_Main/IntelMiner.h"
#include <QPointer>
#include <QThread>
void UI_Constructor::startIntelMine(bool force) {
    if (m_intelMineRunning)
        return;
    QString const call = m_config.my_callsign();
    if (call.trimmed().isEmpty())
        return; // no identity configured yet; next launch mines
    m_intelMineRunning = true;
    QString const grid = m_config.my_grid();
    QPointer<UI_Constructor> self{this};
    QThread *th = QThread::create([self, call, grid, force]() {
        IntelMiner miner;
        IntelMiner::Result const res = miner.mine(call, grid, force);
        if (self)
            QMetaObject::invokeMethod(
                self,
                [self, res]() {
                    if (!self)
                        return;
                    self->m_intelMineRunning = false;
                    if (res.skipped)
                        return;
                    if (res.ok && self->m_spotMapWindow)
                        self->m_spotMapWindow->seedLogGrids(res.logGrids);
                    if (res.ok)
                        self->statusBar()->showMessage(
                            tr("Routing knowledge rebuilt from your "
                               "logs: %1 stations, %2 sightings, "
                               "%3 grids (%4 s)")
                                .arg(res.stations)
                                .arg(res.sightings)
                                .arg(res.logGrids.size())
                                .arg((res.elapsedMs + 999) / 1000),
                            10000);
                },
                Qt::QueuedConnection);
    });
    m_intelMineThread = th; // joined at shutdown (mainwindow.cpp)
    QObject::connect(th, &QThread::finished, this, [this, th]() {
        if (m_intelMineThread == th)
            m_intelMineThread = nullptr;
    });
    QObject::connect(th, &QThread::finished, th, &QObject::deleteLater);
    th->start(QThread::LowPriority);
}
