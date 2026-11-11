# JS8Call Subspace API Extensions

Additions and clarifications to the JS8Call TCP/UDP JSON API, introduced
in Subspace Edition. All existing commands continue to work unchanged.
This document covers Subspace Edition up to Build 521.

## Connection Model

The API server listens on TCP port **2442** by default. Multiple clients
may connect simultaneously; every outbound event (`RX.DIRECTED`,
`RX.ACTIVITY`, `RX.SPOT`, `TX.FRAME`, `TX.COMPLETE`, `STATION.*`, etc.)
is broadcast to every connected client.

**Two usage patterns are supported:**

1. **Poll-based** — connect, send a command, read the response, disconnect.
   Simple and stateless; miss push events between polls. This is what
   short-lived helpers like `nc` scripts use.

2. **Persistent (recommended for automation)** — connect once, read
   forever. You receive every push event as it happens. Send commands on
   the same socket when you need to act. Much gentler on the server's
   TCP stack, especially under activity bursts.

### Message Format

```json
{"type":"CMD","value":"optional-string","params":{"KEY":"VALUE"}}
```

Responses and pushed events share the same shape. The `_ID` field (when
present) correlates a response to its request. Push events that were not
solicited use `_ID: -1`.

---

## New Commands

### `EVENTS.KEEPALIVE`

Lightweight round-trip ping. Use on a persistent connection to confirm
the socket and event stream are alive without touching any state.

**Request:** `{"type":"EVENTS.KEEPALIVE"}`

**Response:** `{"type":"EVENTS.PONG","params":{"_ID":<id>,"UTC":<ms>}}`

### `MODE.GET_SUBMODE_NAME`

Returns the current submode with both its numeric speed and human-readable
name, so clients need not maintain the mapping locally.

**Request:** `{"type":"MODE.GET_SUBMODE_NAME"}`

**Response:**
```json
{"type":"MODE.SUBMODE_NAME",
 "params":{"_ID":<id>,"SPEED":<int>,"NAME":"Normal|Fast|Turbo|Slow|Ultra|Subspace"}}
```

### `RX.GET_BAND_ACTIVITY` (extended)

In addition to the existing per-offset fields (`FREQ`, `DIAL`, `OFFSET`,
`TEXT`, `SNR`, `UTC` — unchanged, describing the *latest* decode at each
offset), each offset now also carries a `HISTORY` array containing up
to ten recent decodes at that offset. Fixes the divergence where the
UI's band panel shows multiple distinct stations that landed on similar
offsets while the API only exposed the most recent one.

```json
"1700": {
  "OFFSET": 1700, "TEXT": "WD4KAV: @HB HEARTBEAT CN87", "SNR": -6,
  "UTC": 1776486000000, "DIAL": 7078000, "FREQ": 7079700,
  "HISTORY": [
    {"TEXT":"N6GRG: @HB HEARTBEAT DM04","SNR":-9,"UTC":1776485994000},
    {"TEXT":"WD4KAV: @HB HEARTBEAT CN87","SNR":-6,"UTC":1776486000000}
  ]
}
```

Existing clients continue to work unchanged; they'll see exactly the
same latest-only fields as before. Clients that want the full picture
iterate `HISTORY`.

### `TX.SEND_DIRECTED`

Server-side composer for properly-formatted directed messages. Avoids
the FROM/TO-reversal mistake that's easy to make with raw `TX.SEND_MESSAGE`.

**Request:**
```json
{"type":"TX.SEND_DIRECTED",
 "params":{"TO":"KD8SKZ","CMD":"SNR","EXTRA":"-13","PRIORITY":"HIGH"}}
```

Server composes `<MYCALL>: KD8SKZ SNR -13` and enqueues at the given
priority. `TO` may be a callsign or an `@GROUP` (e.g. `@ALLCALL`).
Validates `TO` against a callsign regex.

**Response:**
```json
{"type":"TX.SEND_DIRECTED",
 "value":"<composed message>",
 "params":{"_ID":<id>,"OK":true,"ERROR":"","COMPOSED":"WM8Q: KD8SKZ SNR -13"}}
```

On failure, `OK:false` and `ERROR` names the reason; nothing is queued.

### `QSO.GET_CONTEXT`

One-shot bundled state snapshot. Replaces 4-5 separate polls with a
single response — cheap "what's going on right now?" for automation.

**Request:** `{"type":"QSO.GET_CONTEXT"}`

**Response:**
```json
{"type":"QSO.CONTEXT",
 "params":{
   "_ID":<id>,
   "CALLSIGN":"WM8Q",
   "GRID":"DN61",
   "SUBMODE":0,
   "SUBMODE_NAME":"NORMAL",
   "DIAL":7078000,
   "OFFSET":1700,
   "PTT":false,
   "TX_QUEUE_DEPTH":0,
   "CALL_SELECTED":"",
   "ACTIVE_OFFSETS_2MIN":5,
   "UTC":<ms>}}
```

### `RX.CLEAR_OFFSET`

Remove a specific offset's band-activity history. Useful to drop stale
or unwanted rows without clearing the whole band pane.

**Request:**
```json
{"type":"RX.CLEAR_OFFSET","params":{"OFFSET":1500}}
```

**Response:**
```json
{"type":"RX.CLEAR_OFFSET",
 "params":{"_ID":<id>,"OFFSET":1500,"CLEARED":true}}
```

`CLEARED` is `false` if no activity existed at that offset.

### `TX.GET_ENABLED`

The TX enable button's state. An outside program that coordinates the
radio (a phone, an arbiter) reads this before it decides anything.

**Request:** `{"type":"TX.GET_ENABLED"}`

**Response:**
```json
{"type":"TX.ENABLED",
 "params":{"_ID":<id>,"ENABLED":true,"REMOTE":false}}
```

`ENABLED` is the button. `REMOTE` is `true` when the button was turned off
by a remote command (`TX.SET_ENABLED`, or `UI.ACTION` with value
`txenable`) and has not changed since; `REASON` is then the holder's
reason (see `TX.SET_ENABLED`), and the station's status bar shows
"TX disabled by <reason>". Any later change of the button, by the operator
or by a command, clears both. The same facts are in `UI.STATE` as
`TX_ENABLED` and `TX_DISABLED_REMOTE`.

### `TX.SET_ENABLED`

Sets the TX enable button. Use this, not `UI.ACTION txenable` (a toggle),
from any program that decides on its own: if the operator's click lands
between your query and your command, a toggle turns TX on when you meant
off. The value is applied only when it differs from the button, through
the button's own click, so turning it off is the full operator halt: the
current message is cut, queued auto-replies are dropped, ARQ sessions end.

**Request:**
```json
{"type":"TX.SET_ENABLED","params":{"ENABLED":false,"REASON":"VARA link"}}
```

`REASON` (optional, at most 40 characters) names why the holder turns TX
off, for the operator: the status bar shows "TX disabled by VARA link" and
a refused Send says "Send refused: TX disabled by VARA link". Without it
the reason is "remote command".

**Response:**
```json
{"type":"TX.ENABLED",
 "params":{"_ID":<id>,"ENABLED":false,"REMOTE":true,"REASON":"VARA link",
           "HOLD_S":300,"CHANGED":true,"OK":true}}
```

`CHANGED` is `false` when the button was already in that state. `OK` is
`false` with `REASON` `disabled` (the button is unavailable, for example
no rig) or `missing` (no `ENABLED` given); nothing is applied then.

**The hold is a lease.** A hold placed by a command ends on its own after
`HOLD_S` seconds (300, fixed in this build) and TX is turned back on, with
a `TX.ENABLED` event carrying `EXPIRED` true, a log line and a status bar
message. To keep holding, send the same `TX.SET_ENABLED false` again
before it runs out; while held it renews the lease (`CHANGED` false) and
may change the reason. A holder that dies or loses its connection
therefore holds nothing for longer than `HOLD_S`. Any change of the button
by anyone cancels the lease.

With TX off, a Send (the operator's button, a macro, or an API send) is
refused where it always was, and now says why in the status bar for five
seconds: the hold's reason when a remote command holds TX, else "TX is not
enabled". The outgoing box is still cleared, as before.

### `TX.ENABLED` (push event)

Sent with `_ID` -1 whenever the TX enable button changes, by the operator
or by a command, once per change. `ENABLED` is the new state, `REMOTE` is
`true` when it was turned off by a remote command, `REASON` is then the
holder's reason. `OVERRIDE` is `true` when the operator turned TX back on
over a remote hold: the operator always wins, the station logs the
override and tells the operator in the status bar, and the holder is
expected to back off until the radio is next clear rather than hold again
at once.

`EXPIRED` is `true` when the lease ran out and the station turned TX back
on itself (not an override).

```json
{"type":"TX.ENABLED",
 "params":{"_ID":-1,"ENABLED":true,"REMOTE":false,"REASON":"","OVERRIDE":true,
           "EXPIRED":false}}
```

### `TX.GET_AUTOREPLY`, `TX.SET_AUTOREPLY`, `TX.AUTOREPLY`

The Autoreply switch (Mode > Enable Autoreply, the AUTO gate), with the
same three shapes as TX enable: query, set, and a push event on every
change. This is the soft hold for a program that coordinates the radio:
with Autoreply off the station starts no new auto-replies, while the
operator's own sends are untouched and nothing already on the air is cut.
TX enable off is the hard stop.

**Request:** `{"type":"TX.GET_AUTOREPLY"}` or
`{"type":"TX.SET_AUTOREPLY","params":{"ENABLED":false}}`

**Response:**
```json
{"type":"TX.AUTOREPLY",
 "params":{"_ID":<id>,"ENABLED":false,"REMOTE":true,"CHANGED":true,
           "OK":true,"REASON":""}}
```

`CHANGED` and `OK` are in the set reply only; with `OK` false, `REASON` is
`disabled` (the switch is unavailable, JTTY mode) or `missing` (no
`ENABLED`). The set request takes the same optional `REASON` as
`TX.SET_ENABLED`, the hold is the same 300 s lease (`HOLD_S`, renewed by
the same request), and the event carries `OVERRIDE` and `EXPIRED` the
same way. `REMOTE`
is `true` while the switch is off by a remote command, `REASON` is then
the holder's reason, and the status bar shows "Autoreply disabled by
<reason>". The idle
watchdog also turns Autoreply off and restores it when its dialog is
dismissed; both are announced as `TX.AUTOREPLY` events with `REMOTE`
false, so a program holding Autoreply off sees the restore and may hold
again. In `UI.STATE` the two facts are `AUTOREPLY` and
`AUTO_DISABLED_REMOTE`.

### `STATION.GET_BUSY`

Whether transmitting right now would collide with something already in
progress: an ARQ transfer in either direction (file, web link or plain
super-message), its negotiation, or our own transmission.

**Request:** `{"type":"STATION.GET_BUSY"}`

**Response:**
```json
{"type":"STATION.BUSY","value":"arq_tx",
 "params":{"_ID":<id>,"BUSY":true,"BUSY_REASON":"arq_tx","PTT":false,
           "ARQ_TX_ACTIVE":true,"ARQ_RX_ACTIVE":false}}
```

`BUSY_REASON` (also the `value`) is the first that applies of
`arq_negotiating`, `arq_tx`, `arq_rx`, `transmitting`, or empty when not
busy. A client that cannot be changed can rely on `TX.GET_QUEUE_DEPTH`
instead, which reads at least 1 for as long as the station is busy.

### `MODE.GET_ARQ`, `MODE.SET_ARQ`

The chunked-ARQ switch (the ARQ button), so a client can turn ARQ on
before `TX.SEND_CHUNKED` and off after. Setting it works exactly like the
button.

**Request:** `{"type":"MODE.GET_ARQ"}` or
`{"type":"MODE.SET_ARQ","value":"true"}`

The new state is the request's `value` (`true` or `false`).

**Response:** `{"type":"MODE.ARQ","params":{"_ID":<id>,"VALUE":true}}`,
the state after the request. Refused in JTTY mode (see JTTY below).

### `TX.SEND_CHUNKED`

Sends a message to one station over chunked ARQ: the text is split into
chunks, each acknowledged by the receiver and repeated when lost.
Point-to-point only (no `@GROUP`).

**Request:**
```json
{"type":"TX.SEND_CHUNKED","value":"<message text>","params":{"PEER":"KD8SKZ"}}
```

**Response:**
```json
{"type":"TX.SEND_CHUNKED","value":"<message text>",
 "params":{"_ID":<id>,"OK":true,"ERROR":"","PEER":"KD8SKZ",
           "MSG_ID":<int>,"TOTAL":<chunks>}}
```

With `OK` false, `ERROR` says why (an invalid `PEER`, an empty message,
or the transfer could not start) and nothing is sent. Progress and the
outcome arrive as push events (`TX.CHUNKED_PROGRESS`,
`TX.CHUNKED_COMPLETE`, `TX.CHUNKED_FAILED`, below) carrying the same
`MSG_ID`. Refused in JTTY mode.

### `TX.REACH`, `TX.REACH_STOP`

Runs one reaching attempt: the station tries, in turn, the ways of
getting a message to the callsign in `value` (direct, then through
stations that hear it), and stops at the first that answers.

**Request:**
```json
{"type":"TX.REACH","value":"KD8SKZ","params":{"MAX_MOVES":6,"VIA":"N0KQX"}}
```

`MAX_MOVES` (optional, default 6) limits the attempts; `VIA` (optional)
names the relay to use. There is no reply and no event: each step is
written to the diagnostic log under `[REACH]`.
`{"type":"TX.REACH_STOP"}` stops the attempt and restores the speed in
use before it. Refused in JTTY mode.

### `TX.ATTEMPT_DONE`

Tells the station that the caller has stopped waiting for replies to the
calls it made, so the Spots Map blanks their dashed paths at once instead
of at the end of its own countdown. Paths that were answered stay.
Display only; no reply.

**Request:** `{"type":"TX.ATTEMPT_DONE"}`

### `RX.SET_CALL_SELECTED`

Selects a callsign or group, or deselects with an empty `value`, exactly
as a click in the station's lists does (pausing heartbeats during a QSO,
switching speed for the station, and so on).

**Request:** `{"type":"RX.SET_CALL_SELECTED","value":"KD8SKZ"}`

**Response:** `{"type":"RX.CALL_SELECTED","value":"KD8SKZ","params":{"_ID":<id>}}`,
the selection after the request (empty when deselected, or when the call
was not accepted).

### `RX.GET_SPOT_MAP`

A dump of the Spots Map's live state: who hears whom, with ages and SNRs,
every spot including those from internet sources with their reporting
station, and the grid of each station. Optional param `BAND` (default:
the band the map shows). Reply `RX.SPOT_MAP`, or `ERROR` "no spot map".
This is a diagnostic surface: its structure may change without notice.

---

## Phone Remote Commands (`UI.*`)

The phone remote (File > Phone remote) drives the station with these
commands, and they are accepted from any API client as well. Each one
works through the desktop's own button, menu or table, so it behaves
exactly as a click there. Replies carry `OK` and, when refused, `REASON`.

A reply goes back where its request came from: the answer to a request
from the phone goes to the phone only, and the answer to a request from a
UDP or TCP client goes to the UDP and TCP clients as before. Events
(`_ID` -1), such as `TX.ENABLED`, still go to every client.

### `UI.ACTION`

Presses one of the desktop's buttons, named in `value`: `send`, `halt`,
`txenable` (a toggle; programs should use `TX.SET_ENABLED`), `cq`, `snr`,
`reply`, `info`, `status`, `tu73` (TYPING in JTTY mode), `log` (logs the
selected station at once, no form), `directed`, `deselect`; and three
that are not desktop buttons: `agn` (`<CALL> AGN?`), `snrq`
(`<CALL> SNR?`) and `repeat` (restores the previous message).

**Request:** `{"type":"UI.ACTION","value":"cq"}`

**Response:**
`{"type":"UI.ACTION","value":"cq","params":{"_ID":<id>,"OK":true,"REASON":"","LOGGED":""}}`

`REASON` when refused: `unknown` (no such button), `disabled` (the button
is unavailable now), `no_call` (`log` with no station selected), `queued`
(`send` while a message already waits for its slot; `halt` cancels it),
`no_payload` (JTTY: the box holds only the address), `js8_menu`
(`directed` in JS8 mode, where it is a menu). `LOGGED` is the callsign
`log` logged.

### `UI.GET_SNAPSHOT`

Everything the phone shows, in one reply:

```json
{"type":"UI.SNAPSHOT","params":{"_ID":<id>,"LINES":[...],"ROWS":[...],"STATE":{...},"INBOX":[...]}}
```

- `LINES`: the conversation window's last 60 lines, each
  `{"TEXT":"...","TX":false}` (`TX` true for our own transmissions).
- `ROWS`: the band activity table as displayed, each
  `{"CALL","OFFSET","AGE","SNR","SPEED","TEXT","TOME","SELECTED"}`;
  `TEXT` is the row's latest messages, up to 10 or 300 characters.
- `STATE`: `BUTTONS` (each button name above: true when it may be
  pressed), `MODE` (`JS8` or `JTTY`), `SPEED`, `DIAL`, `OFFSET`,
  `SELECTED`, `TX_ENABLED`, `TX_DISABLED_REMOTE`, `AUTOREPLY`,
  `AUTO_DISABLED_REMOTE`, `TRANSMITTING`, `QUEUED` (a message waits for
  its slot), `TX_POWER_W` and `SWR` (-1 when not read), `DIRECTED_LATCH`,
  `BAND_BY_CALL`, `TU73_TEXT`, `OUTGOING` (the outgoing box).
- `INBOX`: the Message Inbox, as `UI.GET_INBOX` returns it.

While the phone remote is on, the phone (only) is sent these with `_ID`
-1 whenever they change (at most every 250 ms): `RX.TEXT.TAIL` (`LINES`),
`RX.BAND.TABLE` (`ROWS`), `UI.STATE` (`STATE`) and, after the inbox
changes, `UI.INBOX` (`ITEMS`). Other clients read them with
`UI.GET_SNAPSHOT`.

### `UI.GET_FREQ_LIST`, `UI.SET_FREQ_ENTRY`

The station's frequency list as the Frequency menu shows it, sorted by
frequency, and going to one of its entries.

**Request:** `{"type":"UI.GET_FREQ_LIST"}`

**Response:** `{"type":"UI.FREQ_LIST","params":{"_ID":<id>,"ITEMS":[...]}}`,
each item `{"FREQ":<Hz>,"MHZ":"7.078 000","BAND":"40m","MODE":"JS8","DESC":"...","CURRENT":true,"SHARED":false}`.
`SHARED` is true for a JS8+JTTY entry, where the operator chooses the mode.

**Request:** `{"type":"UI.SET_FREQ_ENTRY","params":{"FREQ":7078000,"MODE":"JS8"}}`

Only a frequency in the list. `MODE` (`JS8` or `JTTY`, optional) chooses
the mode on a JS8+JTTY entry once the radio has arrived there. Reply
`UI.SET_FREQ_ENTRY` with `OK`; `REASON` `not_listed` or `transmitting`
(a message is going out). The entry's remembered offset is restored as
with the Frequency menu (see `RIG.SET_FREQ` below).

### `UI.OFFSET_STEP`

Moves the offset one step up (`STEP` > 0) or down (`STEP` < 0): 100 Hz in
JS8 mode, 25 Hz in JTTY mode, landing on that grid (1237 Hz in JS8: up
1300, down 1200).

**Request:** `{"type":"UI.OFFSET_STEP","params":{"STEP":1}}`

**Response:**
`{"type":"UI.OFFSET_STEP","params":{"_ID":<id>,"OK":true,"REASON":"","OFFSET":1300}}`.
In JS8 mode a step into the heartbeat area (1000 Hz and below) is
refused with `REASON` `hb_zone`.

### `UI.GET_SPEEDS`, `UI.SET_SPEED`

The Mode menu's speeds as the menu shows them, and choosing one.

**Request:** `{"type":"UI.GET_SPEEDS"}`

**Response:** `{"type":"UI.SPEEDS","params":{"_ID":<id>,"ITEMS":[...]}}`,
each item `{"ID":"normal","TEXT":"<menu wording>","CHECKED":true,"ENABLED":true}`;
`ID` is one of `slow`, `normal`, `fast`, `turbo`, `subspace`, `jtty`, and
only the speeds the menu shows are listed.

**Request:** `{"type":"UI.SET_SPEED","value":"subspace"}`

Reply `UI.SET_SPEED` with `OK`; `REASON` `unknown`, `disabled`, or
`jtty_mode` (JTTY mode has a fixed speed).

### `UI.GET_GROUPS`, `UI.SELECT_GROUP`

The groups the call list shows (`@ALLCALL` unless avoided in Settings,
then My Groups, sorted), and selecting one as a click there does. JS8
mode only.

**Request:** `{"type":"UI.GET_GROUPS"}`

**Response:** `{"type":"UI.GROUPS","params":{"_ID":<id>,"ITEMS":[{"NAME":"@MAGNET","SELECTED":false}]}}`

**Request:** `{"type":"UI.SELECT_GROUP","value":"@MAGNET"}`

Reply `UI.SELECT_GROUP` with `OK`; `REASON` `unknown` or `jtty_mode`.

### `UI.BAND_CLICK`

A click on the band activity row of a station, through the table's own
click handlers; `DOUBLE` true makes it a double-click.

**Request:** `{"type":"UI.BAND_CLICK","params":{"CALL":"KD8SKZ","OFFSET":1500,"DOUBLE":true}}`

Reply `UI.BAND_CLICK` with `OK`; `REASON` `no_call` (rows without a
callsign are refused) or `gone` (the row is no longer shown). `NO_QSY`
`hb_zone` says the double-click selected the station but did not move the
offset into the heartbeat area.

### `UI.CONVO_CLICK`

A tap on a conversation line (`value` = the line's text): selects the
station that sent it (JS8: the first `CALL:`, or on our own line the
station addressed; JTTY: the sender form). `DOUBLE` true also moves to
the line's offset, under the same heartbeat-area rule.

**Request:** `{"type":"UI.CONVO_CLICK","value":"<line text>","params":{"DOUBLE":false}}`

**Response:**
`{"type":"UI.CONVO_CLICK","value":"KD8SKZ","params":{"_ID":<id>,"OK":true,"REASON":"","CALL":"KD8SKZ","OFFSET":1500,"NO_QSY":""}}`.
`REASON` `no_call` when the line names no station.

### `UI.GET_INBOX`, `UI.INBOX_READ`, `UI.INBOX_DELETE`

The Message Inbox panel's list (all calls, newest first), each field
written as the panel's column shows it: `ID`, `TYPE` (`UNREAD`, `READ`,
or `STORE` for a message waiting for its station), `DATE`, `FREQ`,
`FROM` (the relay path, "A via B"), `TO`, `TEXT`.

**Request:** `{"type":"UI.GET_INBOX"}`

**Response:** `{"type":"UI.INBOX","params":{"_ID":<id>,"ITEMS":[...]}}`

`UI.INBOX_READ` marks a message read, as opening it in the panel does
(an `UNREAD` message only); `UI.INBOX_DELETE` deletes it, as the panel's
Delete does. Both go through the panel, so the desktop's list and the
call list's flags follow at once.

**Request:** `{"type":"UI.INBOX_DELETE","params":{"ID":41}}`

**Response:** `{"type":"UI.INBOX_DELETE","params":{"_ID":<id>,"OK":true,"REASON":""}}`;
`REASON` `gone` when no message has that `ID`.

### `UI.GET_WATERFALL`

One picture of the waterfall as the screen shows it: the frequency
scale, waterfall, spectrum, offset marker and filter edges.

**Request:** `{"type":"UI.GET_WATERFALL"}`

**Response:**
`{"type":"UI.WATERFALL","params":{"_ID":<id>,"OK":true,"REASON":"","MIME":"image/jpeg","WIDTH":1200,"HEIGHT":300,"IMAGE":"<base64>"}}`

`MIME` is `image/png` when this Qt has no JPEG writer. `REASON` `hidden`
when the waterfall is not on the screen, `failed` when the picture could
not be made. The picture is far larger than a UDP datagram, so a UDP
client will not receive it; ask over TCP.

---

## Extended Commands

### `RIG.SET_FREQ` (Build 521)

Each entry of the station's frequency list remembers the offset last used
on it, and the station returns to that offset whenever the dial arrives at
the entry. For `RIG.SET_FREQ`:

- `DIAL` alone: on arrival at a listed frequency the offset becomes that
  entry's remembered offset, as with the Frequency menu.
- `DIAL` and `OFFSET` together: the given offset stays, and becomes the
  entry's remembered offset. The entry being left keeps the offset it had.
- `OFFSET` alone: sets the offset; the dial does not move.

A dial that is not in the list keeps whatever offset is in use.

### `MODE.SET_SPEED`

Now accepts the Subspace (FT2) submode in addition to Normal / Fast /
Turbo / Slow / Ultra. `SPEED` value for Subspace is `Varicode::JS8CallFT2`
(integer constant exported via `MODE.GET_SPEED` / `MODE.GET_SUBMODE_NAME`).

### `TX.SEND_MESSAGE`

Accepts an optional `PRIORITY` param:

- `"HIGH"` — default (matches prior behavior)
- `"NORMAL"` — fits between user-typed text and background traffic
- `"LOW"` — lowest priority; queue is drained in order

```json
{"type":"TX.SEND_MESSAGE","value":"WM8Q: CQ CQ CQ DN61","params":{"PRIORITY":"NORMAL"}}
```

Higher-priority items that arrive later will transmit before queued
lower-priority items.

---

## New Push Events

### `TX.COMPLETE`

Pushed when a queued transmission block finishes transmitting (all frames
sent, PTT released). Lets automation replace `TX.GET_QUEUE_DEPTH` polling
with an event-driven flow.

```json
{"type":"TX.COMPLETE",
 "value":"<sent message text>",
 "params":{"_ID":-1,"SUBMODE":<int>,"UTC":<ms>}}
```

Note: fires once per transmission block, not once per frame. `TX.FRAME`
events continue to fire per audio frame during TX.

### `TX.PENDING`

A slotted frame is waiting for its key-up. Pushed once per slot while it
waits (the first one as soon as the frame is ready), so a program sharing
the radio (the Arbiter) can hold the others before this station keys.
`KEYUP_IN_MS` is relative to the moment of sending: a client's clock is not
this station's. A client lets a `PENDING` true lapse after that time if no
`RIG.PTT` on follows. `PENDING` false comes when the frame stops waiting:
keyed (`RIG.PTT` on follows), halted, or TX turned off.

```json
{"type":"TX.PENDING","value":"","params":{"_ID":-1,"PENDING":true,"KEYUP_IN_MS":2791}}
{"type":"TX.PENDING","value":"","params":{"_ID":-1,"PENDING":false}}
```

Not sent for sends that key at once (ARQ transfers, JTTY, Tune): `RIG.PTT`
is their notice. Never sent while TX is off.

### `TX.CHUNKED_PROGRESS`, `TX.CHUNKED_COMPLETE`, `TX.CHUNKED_FAILED`

The course of a `TX.SEND_CHUNKED` transfer, matched by `MSG_ID`:

```json
{"type":"TX.CHUNKED_PROGRESS","params":{"PEER":"KD8SKZ","MSG_ID":<int>,"DELIVERED":3,"TOTAL":5}}
{"type":"TX.CHUNKED_COMPLETE","params":{"PEER":"KD8SKZ","MSG_ID":<int>,"TOTAL":5,"TOTAL_RETRIES":1}}
{"type":"TX.CHUNKED_FAILED","value":"<reason>",
 "params":{"PEER":"KD8SKZ","MSG_ID":<int>,"DELIVERED":3,"TOTAL":5,"TOTAL_RETRIES":4,"REASON":"<reason>"}}
```

### `RX.CHUNKED_DELIVERED`

A chunked message received complete, with the whole text as `value`:

```json
{"type":"RX.CHUNKED_DELIVERED","value":"<message text>",
 "params":{"FROM":"KD8SKZ","TO":"WM8Q","MSG_ID":<int>}}
```

---

## JTTY Mode

A station in JTTY mode (or at JTTY speed) accepts a request that would
transmit, change speed or move the dial only when the request says it
knows about JTTY: `"MODE":"JTTY"` in its params, or `"SPEED":"JTTY"` at
JTTY speed in JS8 mode. The same statement is needed to move the station
into JTTY (`MODE.SET_SPEED` to the JTTY speed, `RIG.SET_FREQ` to a JTTY
frequency). Without it the request is refused with `REFUSED` true and
`REASON` `mode_mismatch`, so an older client never lands in JTTY by
accident.

`TX.SEND_CHUNKED`, `TX.REACH` and `MODE.SET_ARQ` do not exist in JTTY and
are refused with `REASON` `jtty_mode`, as is a move to JTTY speed on a
MAGNET station. Reads are open to every client, and every reply carries
`MODE` (`JS8` or `JTTY`).

---

## Example: Persistent Python Client

```python
import socket, json

s = socket.create_connection(("127.0.0.1", 2442))
buf = b""

def send(cmd, value="", **params):
    msg = json.dumps({"type": cmd, "value": value, "params": params}) + "\n"
    s.sendall(msg.encode())

send("STATION.GET_CALLSIGN")

while True:
    chunk = s.recv(65536)
    if not chunk: break
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        if not line.strip(): continue
        msg = json.loads(line)
        t = msg.get("type", "")
        if t == "RX.DIRECTED":
            params = msg["params"]
            if params.get("TO") == "WM8Q":
                send("TX.SEND_MESSAGE",
                     f"WM8Q: {params['FROM']} SNR {params['SNR']:+03d}",
                     PRIORITY="HIGH")
        elif t == "TX.COMPLETE":
            print("tx done:", msg["value"])
```
