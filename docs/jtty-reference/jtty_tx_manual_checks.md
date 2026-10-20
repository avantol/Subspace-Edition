# JTTY transmit manual checks

These checks cover MainWindow, PTT, and backend behavior that is outside the
FIFO unit-test boundary.

- Free entry: Enter queues the whole message. Text longer than the encoder's
  80-character capacity is split at spaces where possible; longer words are
  split across messages. Each segment has its own end marker, and the receiver
  displays separate messages. The exchange profile and transmit frequency are
  captured when the message is submitted.
- Pending text: the existing Send button shows `Send (3 left)` while segments
  remain, including the segment currently playing. Its tooltip shows the
  pending text. Yellow transmit text appears when the audio source starts
  serving each segment; it does not confirm reception. Completion waits for
  the backend's final audio drain.
- Status bar: long `Tx:` and `Last Tx:` messages use an ellipsis to leave room
  for the other indicators. Hover to read the complete message.
- Drafts: typing while transmitting starts a new draft; Enter appends it after
  existing text. Halt Tx or Esc cancels all unfinished submissions and leaves the
  new draft alone. Cancellation is reported in the Send tooltip;
  partial transmission may already have occurred. No cancelled text is restored
  to the input, and nothing is retained across application exit.
- Long entry: send enough text to exceed 60 seconds of encoded audio. Verify
  continuous playback, ordered segments, and reception of the final suffix.
  Repeat with an exactly 80-character segment appended during playback.
- Cancellation: stop before PTT rises and during playback, then send a fresh
  long message. Verify that cancelled text never resumes, deferred text never
  appears yellow, and the new message starts normally. Repeat with Esc, a mode
  change, and an audio/backend failure. Check that newer draft edits survive.
- Normalization: enter unsupported characters after character 80. Verify the
  substitution notice in the Send tooltip and canonical pending text, with no
  omitted suffix.
- Soundcard: send one JTTY message and verify the full tail plays before PTT drops.
- Soundcard: queue two or three messages rapidly and verify playback is contiguous with PTT held.
- Soundcard: press Halt Tx during playback and verify queued audio is abandoned before PTT drops.
- Soundcard: press Esc before PTT comes up and during playback and verify clean aborts.
- Soundcard: send again after a natural drain and verify a fresh transmit session starts.
- TCI, where available: repeat the same checks and watch for backend enqueue failure warnings.
- Native macros: activate each default JTTY F1-F8 template by keyboard and by
  clicking its button; verify both paths show the same canonical message and
  enqueue the same number of frames.
- Native macros: enter a valid call and serial, send a call-plus-exchange
  template, and verify EOM appears only on the final atom.
- Native macros: make a recognized template's runtime call or serial invalid
  and verify transmission is rejected rather than sent as literal text.
- Literal fallback: customize a function-key template so it no longer matches
  a native form and verify automatic packing preserves its expanded text after
  the selected profile's normalization.
- Free entry: with MyCall K1ABC, compare F1 with typing `CQ K1ABC CQ` and
  selecting Send message; verify the same text and one frame. Repeat with
  `CQ K1ABC CQ CQ K1ABC CQ`, verifying two frames and unchanged spacing.
- External text: send `CQ K1ABC CQ`, `599 123`, `599 MA`, `599 FN42`, and
  `1D EMA` from N1MM/MMTTY; verify each uses one frame and preserves the text.
  With no special activity or with Field Day, send `599 001`, `599 05`, and
  `599 BRUCE`; verify each uses two frames and preserves the text.
- Automatic profile: select RTTY Roundup and send `599 001` and
  `K1ABC 599 001`; verify one and two frames respectively. Verify `599 123`
  and `599 MA` use SERIAL and STATE_PROVINCE rather than generic atoms.
  Verify `599 05` and `599 0123` become `599 005` and `599 123`, each in one
  frame, with the canonical text shown and logged. Verify bare `001` retains
  its spelling and is not inferred as SERIAL. Verify unsupported serial
  tokens (over six digits or above 131071) retain their spelling. Verify a
  message whose serial normalization expands beyond 80 characters is split
  without omitting the remainder. Submit a message, change activity
  before it plays, and verify the submitted message retains its captured
  profile. Repeat through untagged N1MM/MMTTY and customized macro fallback.
- sjtty: compare default and leading `--exchange-profile=rtty-roundup` with
  `599 001`; verify two and one frames. Exercise `unknown` and `field-day`,
  the eight-argument waveform invocation, and rejection of an invalid value.
- Mixed text: send `HI WB9XYZ`, `TEST WB9XYZ`, and `K1ABC HELLO`; verify no
  spaces are inserted or lost where TEXT5 and compact atoms meet. Send a
  registered control phrase alone and embedded in a longer message.
- Queueing: queue two native messages and verify no visible or transmitted
  TEXT5 spacing frame is inserted between them.
- Profile exchange: exercise `%E` with default serial, FIELD_DAY class/section,
  and both RTTY serial and state/province payloads; verify canonical rendering
  and rejection of a payload from the wrong profile.
- Grid macros: verify `%G` renders field-only GRID4 and exact `599 %G` renders
  full-role GRID4; reject malformed locators.
- Settings migration: load unchanged old F2, F6, and F8 defaults using `%N`
  and verify migration to `%E`; verify any edited variant remains literal.
- Windows/N1MM: load `JTTY Messages.mc`, exercise every tagged Run and S&P
  action, and verify the `[[JTTY:ACTION]]` marker is consumed rather than
  transmitted. Reject malformed tags, invalid profile payloads, and unknown
  actions; verify untagged TXTEXT remains literal. Verify an all-rejected
  transaction completes at OFF, and a rejected action followed by a valid
  action still starts and completes only after the accepted audio drains.
- Windows/N1MM: in a two-radio configuration, verify entry-window focus, `!`
  and `{CALL}` expansion, queued-call selection, `{LOGTHENPOP}`, and Run/S&P
  state changes. Before adding a Run F11 call-stacking macro, capture its
  TXTEXT ordering and verify that the leading tag, callsign, and exchange all
  refer to the intended queued station.
