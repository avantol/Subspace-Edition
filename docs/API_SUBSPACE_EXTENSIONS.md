# JS8Call Subspace API Extensions

Additions and clarifications to the JS8Call TCP/UDP JSON API, introduced
in Subspace Edition. All existing commands continue to work unchanged.

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

---

## Extended Commands

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
