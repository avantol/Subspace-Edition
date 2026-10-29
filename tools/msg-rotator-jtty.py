#!/usr/bin/env python3
"""msg-rotator-jtty -- the JTTY-mode counterpart of msg-rotator.py.

Sends messages from a file one at a time, cycling forever, while the
station is in JTTY mode. Shares msg-rotator.py's helpers (busy check,
outgoing-box check, keepalive, suspend-proof clock) -- one authority
for those field lessons.

Differences from the JS8 rotator:
  * NO mode or speed changes and no Subspace hail. The rotator never
    moves the station into JTTY; it only transmits when the station is
    ALREADY in JTTY mode (the MODE field on any API reply). Otherwise
    the cycle is skipped and retried after BUSY_RETRY_SEC.
  * Every send states "MODE":"JTTY" -- the API refuses an unstated
    send in JTTY mode (REASON mode_mismatch, dev 501.1-apijtty).
  * A line longer than one JTTY message (80 characters) is split by
    the app into several JTTY messages; the TX.SEND_MESSAGE reply says
    how many (SPLIT / JTTY_MESSAGES) and is logged. TX.COMPLETE fires
    after the last one.

Each message is sent once per cycle (SENDS_PER_MESSAGE; it was 2 until
2026-10-27, operator: "not repeat the message each run").

Usage:  msg-rotator-jtty.py [MSG_FILE] [INTERVAL] [START]
  START     1-based message number to begin with (default 1)
  MSG_FILE  default ~/Downloads/msgs_jtty.txt (one message per line;
            blanks and #comments ignored; hot-reloaded every cycle)
  INTERVAL  seconds: "N" fixed or "MIN-MAX" randomized per cycle
            (default 660-1140 = 15 min +- 4 min)
"""

from __future__ import annotations

import importlib.util
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("msg_rotator",
                                               HERE / "msg-rotator.py")
_base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_base)

Js8Error = _base.Js8Error
Js8Timeout = _base.Js8Timeout
log = _base.log
BUSY_RETRY_SEC = _base.BUSY_RETRY_SEC
TX_COMPLETE_TIMEOUT = _base.TX_COMPLETE_TIMEOUT
SENDS_PER_MESSAGE = 1


class JttyRotator(_base.Rotator):

    def station_mode(self) -> str:
        """JTTY or JS8, from the MODE field every reply carries."""
        r = self.js8.request("RIG.GET_FREQ", reply_type="RIG.FREQ",
                             timeout=10)
        return r.get("params", {}).get("MODE", "")

    def send_jtty_and_drain(self, text: str) -> bool:
        """TX.SEND_MESSAGE with the JTTY statement, then the real drain
        event. A refusal (REFUSED) or a split notice arrives as a
        TX.SEND_MESSAGE reply; TX.COMPLETE ends the transmission."""
        rid = int(time.time() * 1000)
        reply = self.js8._add_waiter(
            lambda m: m.get("type") == "TX.SEND_MESSAGE" and
            m.get("params", {}).get("_ID") == rid)
        done = self.js8._add_waiter(
            lambda m: m.get("type") == "TX.COMPLETE")
        try:
            self.js8.send("TX.SEND_MESSAGE", text,
                          {"_ID": rid, "MODE": "JTTY"})
            # The reply comes at once or not at all (an unsplit message
            # gets none), so a short wait here is a check, not cadence.
            try:
                r = self.js8._await(reply, what="TX.SEND_MESSAGE",
                                    timeout=3)
                p = r.get("params", {})
                if p.get("REFUSED"):
                    log(f"     refused: {p.get('REASON')}")
                    return True        # nothing aired; move on
                if p.get("SPLIT"):
                    log(f"     split into {p.get('JTTY_MESSAGES')} "
                        "JTTY messages")
            except Js8Timeout:
                pass
            try:
                self.js8._await(done, what="TX.COMPLETE",
                                timeout=TX_COMPLETE_TIMEOUT)
                return True
            except Js8Timeout:
                why = self.busy_reason()
                print(f"   TX.COMPLETE overdue"
                      f"{f' and busy ({why})' if why else ' with app idle'}",
                      file=sys.stderr, flush=True)
                return not why
        finally:
            self.js8._drop_waiter(reply)
            self.js8._drop_waiter(done)

    def run(self) -> int:
        mycall = self.js8.request("STATION.GET_CALLSIGN",
                                  timeout=10).get("value", "")
        if not mycall:
            print("msg-rotator-jtty: STATION.GET_CALLSIGN returned "
                  "nothing. Is the TCP API enabled on :2442?",
                  file=sys.stderr)
            return 1
        span = (f"{self.int_min}s" if self.int_min == self.int_max
                else f"randomized {self.int_min}-{self.int_max}s")
        print(f"msg-rotator-jtty: {self.msg_file}, interval={span}, "
              f"callsign={mycall}")
        print("msg-rotator-jtty: sends only while the station is in "
              "JTTY mode; never changes mode or frequency")
        print("msg-rotator-jtty: Ctrl+C to stop\n", flush=True)

        while True:
            msgs = self.load_messages()
            if not msgs:
                secs = self.next_interval()
                log(f"{self.msg_file} is empty; waiting {secs}s")
                self.interval_sleep(secs)
                continue
            self.idx %= len(msgs)

            mode = self.station_mode()
            if mode != "JTTY":
                log(f"station in {mode or 'unknown'} mode, not JTTY; "
                    "skipping cycle")
                self.announce(BUSY_RETRY_SEC, "attempt")
                self.interval_sleep(BUSY_RETRY_SEC)
                continue

            why = self.busy_reason()
            if why:
                log(f"JS8Call busy ({why}); skipping cycle")
                self.announce(BUSY_RETRY_SEC, "attempt")
                self.interval_sleep(BUSY_RETRY_SEC)
                continue

            typed = self.outgoing_text()
            if typed:
                log(f'outgoing box not empty ("{typed[:40]}...");'
                    " skipping cycle")
                secs = self.next_interval()
                self.announce(secs, "attempt")
                self.interval_sleep(secs)
                continue

            msg = msgs[self.idx]
            ok = True
            # [operator 2026-10-18] Each message twice, back to back: a
            # WSJT-X operator decodes only in their own window and
            # 1200-1800 Hz, and easily misses a single pass.
            # [operator 2026-10-27] ...back to once: SENDS_PER_MESSAGE = 1.
            # The loop stays so the count is one constant.
            for n in range(1, SENDS_PER_MESSAGE + 1):
                if n > 1 and (self.busy_reason() or self.outgoing_text()):
                    log("     repeat skipped: busy or operator typing")
                    break
                log(f"[{self.idx + 1}/{len(msgs)}] JTTY "
                    f"({n}/{SENDS_PER_MESSAGE}): {msg}")
                if not self.send_jtty_and_drain(msg):
                    ok = False
                    break
            if not ok:
                log("transmission did not finish (busy); retrying later")
                self.announce(BUSY_RETRY_SEC, "attempt")
                self.interval_sleep(BUSY_RETRY_SEC)
                continue

            self.idx = (self.idx + 1) % len(msgs)
            secs = self.next_interval()
            self.announce(secs, "send")
            self.interval_sleep(secs)


def main() -> int:
    msg_file = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path.home() / "Downloads/msgs_jtty.txt"
    interval = sys.argv[2] if len(sys.argv) > 2 else "660-1140"
    lo, _, hi = interval.partition("-")
    try:
        int_min = int(lo)
        int_max = int(hi) if hi else int_min
    except ValueError:
        print(f"msg-rotator-jtty: bad INTERVAL {interval!r}",
              file=sys.stderr)
        return 1
    if int_max < int_min:
        print("msg-rotator-jtty: INTERVAL MAX < MIN", file=sys.stderr)
        return 1
    if not msg_file.is_file():
        print(f"msg-rotator-jtty: message file not found: {msg_file}",
              file=sys.stderr)
        return 1
    rot = JttyRotator(msg_file, int_min, int_max)
    if len(sys.argv) > 3:                      # START: 1-based line to begin at
        rot.idx = max(0, int(sys.argv[3]) - 1)
    try:
        return rot.run()
    except KeyboardInterrupt:
        print("\nmsg-rotator-jtty: stopped")
        return 0
    except Js8Error as e:
        print(f"msg-rotator-jtty: connection lost ({e}) -- exiting for "
              "supervisor restart", file=sys.stderr, flush=True)
        return 1
    finally:
        rot.js8.close()


if __name__ == "__main__":
    raise SystemExit(main())
