#!/usr/bin/env python3
"""msg-rotator — send messages from a file to JS8Call one at a time,
cycling forever. Python port of msg-rotator.sh on the js8client
library ([TODO #155]): every completion wait is an event wait
(TX.COMPLETE), every readiness check is a sub-millisecond request —
the shell version's fixed sleeps and poll loops are gone.

Each cycle:
  1. send next message in Normal mode
  2. wait for TX.COMPLETE (the queue-drained stopTx — all frames)
  3. switch to Subspace (FT2), send single-frame hail "<MYCALL>: @ALLCALL ACK"
  4. wait for its TX.COMPLETE, switch back to Normal
  5. sleep INTERVAL (keepalive-pinging the socket every minute), repeat

Usage:  msg-rotator.py [MSG_FILE] [INTERVAL]
  MSG_FILE  default ~/Downloads/msgs.txt (one message per line;
            blanks and #comments ignored; hot-reloaded every cycle)
  INTERVAL  seconds: "N" fixed or "MIN-MAX" randomized per cycle
            (default 1500-2100 = 25-35 min)

Field lessons carried over from the shell version — all still load-
bearing, do not simplify away:
  * RIG.TX_HALT is an E-STOP: it aborts ARQ sessions in BOTH
    directions (killed a live file transfer, 2026-07-23). It is only
    ever fired through safe_tx_halt(), which checks STATION.GET_BUSY
    first.
  * MODE.SET_SPEED is fire-and-forget and SILENTLY REJECTED while any
    TX state lingers (observed 2026-05-04: a Normal @ALLCALL went out
    as multi-frame Subspace). set_mode_and_verify() re-reads the mode
    and retries with a safe halt between attempts.
  * An ARQ-busy skip retries after BUSY_RETRY_SEC (4 min), not the
    full interval (2026-07-28, Andy).
  * The operator's half-typed text in the outgoing box must never be
    commandeered — compose check before each cycle.
  * The hail must be a VALID directed frame ("<MYCALL>: @ALLCALL ACK");
    a bare "MYCALL:" silently fails to encode.
Gone by construction (library-level fixes):
  * JSON escaping bugs (json.dumps), RX events masquerading as
    responses (typed waiters), the queue-depth-counts-messages /
    empty-between-frames trap (TX.COMPLETE is the real drain event),
    the two-consecutive-settled-samples dance, and every fixed sleep.
"""

from __future__ import annotations

import random
import sys
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from js8client import Js8Client, Js8Error, Js8Timeout  # noqa: E402

NORMAL = 0
FT2 = 16
BUSY_RETRY_SEC = 240
# TX.COMPLETE ceiling for one plain message. Normal-mode multi-frame
# messages run well under this; an ARQ transfer that grabs the channel
# mid-cycle reports busy separately.
TX_COMPLETE_TIMEOUT = 300


def elapsed_clock() -> float:
    """Monotonic clock that COUNTS SUSPEND TIME.

    time.monotonic() freezes while the machine is suspended on Linux,
    so a 30-minute interval silently became 14 hours across a laptop
    sleep (field 2026-08-21: Mini suspended 10:29, woke 16:30, the
    rotator had been "sleeping" the whole time and skipped every
    scheduled cycle). CLOCK_BOOTTIME includes suspend, so the schedule
    resumes correctly on wake.
    """
    try:
        return time.clock_gettime(time.CLOCK_BOOTTIME)
    except (AttributeError, OSError):       # non-Linux fallback
        return time.monotonic()


def utc(ts: float | None = None) -> str:
    dt = datetime.fromtimestamp(ts if ts is not None else time.time(),
                                timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def log(msg: str) -> None:
    print(f"[{utc()}] {msg}", flush=True)


class Rotator:
    def __init__(self, msg_file: Path, int_min: int, int_max: int):
        self.msg_file = msg_file
        self.int_min = int_min
        self.int_max = int_max
        self.js8 = Js8Client()
        self.idx = 0

    # ---- helpers ----------------------------------------------------

    def next_interval(self) -> int:
        if self.int_min == self.int_max:
            return self.int_min
        return random.randint(self.int_min, self.int_max)

    @staticmethod
    def announce(secs: int, label: str) -> None:
        at = (datetime.now(timezone.utc) +
              timedelta(seconds=secs)).strftime("%Y-%m-%dT%H:%M:%SZ")
        print(f"   next {label} in {secs}s"
              f" (~{secs // 60}m {secs % 60}s), at {at}", flush=True)

    def interval_sleep(self, secs: int) -> None:
        """Inter-cycle wait. This is scheduled CADENCE, not API
        waiting — but the persistent socket gets a keepalive ping each
        minute so a dead connection surfaces here, not mid-cycle."""
        deadline = elapsed_clock() + secs
        while (remain := deadline - elapsed_clock()) > 0:
            time.sleep(min(60.0, remain))
            if remain > 60.0:
                try:
                    self.js8.request("EVENTS.KEEPALIVE",
                                     reply_type="EVENTS.PONG",
                                     timeout=10)
                except Js8Error:
                    # Catches BOTH flavors of a dead connection:
                    # Js8Timeout (no PONG) and the base Js8Error
                    # ("disconnected while waiting" -- socket dropped,
                    # e.g. app restart or another API client taking
                    # the single TCP slot). Field 2026-09-06: the
                    # disconnect flavor escaped as a raw traceback.
                    log("keepalive lost — exiting for supervisor "
                        "restart")
                    raise SystemExit(1)

    def busy_reason(self) -> str | None:
        """STATION.GET_BUSY — ARQ in either direction, or keyed."""
        r = self.js8.request("STATION.GET_BUSY", timeout=10)
        p = r.get("params", {})
        if p.get("BUSY"):
            return p.get("BUSY_REASON") or "busy"
        return None

    def outgoing_text(self) -> str:
        return self.js8.request("TX.GET_TEXT",
                                timeout=10).get("value", "") or ""

    def safe_tx_halt(self) -> bool:
        """E-stop ONLY when nothing is in flight (see module doc)."""
        why = self.busy_reason()
        if why:
            print(f"   skipping RIG.TX_HALT — busy ({why})",
                  file=sys.stderr, flush=True)
            return False
        self.js8.send("RIG.TX_HALT")
        return True

    def set_mode_and_verify(self, speed: int, name: str) -> bool:
        """MODE.SET_SPEED + verify via MODE.GET_SUBMODE_NAME; the set
        is silently rejected during residual TX state, so retry with a
        safe halt between attempts (shell lesson, 2026-05-04)."""
        for attempt in range(1, 6):
            self.js8.send("MODE.SET_SPEED", "", {"SPEED": speed})
            got = None
            # Verify loop: the handler applies synchronously, so the
            # first read usually settles it — bounded retries replace
            # the shell's fixed 1.5 s sleep.
            for _ in range(6):
                r = self.js8.request("MODE.GET_SUBMODE_NAME",
                                     timeout=10)
                got = r.get("params", {}).get("SPEED")
                if got == speed:
                    return True
                time.sleep(0.5)
            print(f"   set_mode_and_verify: attempt {attempt} got"
                  f" SPEED={got}, want {speed}; retry",
                  file=sys.stderr, flush=True)
            if not self.safe_tx_halt():
                return False
            time.sleep(1.0)
        print(f"   set_mode_and_verify: FAILED to reach {name}"
              f" (SPEED={speed})", file=sys.stderr, flush=True)
        return False

    def send_and_drain(self, text: str) -> bool:
        """Queue text and wait for the REAL drain event. Returns False
        when ARQ busy-ness means the caller should back off."""
        try:
            self.js8.send_and_wait_complete(
                text, timeout=TX_COMPLETE_TIMEOUT)
            return True
        except Js8Timeout:
            why = self.busy_reason()
            if why:
                print(f"   TX.COMPLETE overdue and busy ({why}) — "
                      "not barging in", file=sys.stderr, flush=True)
            else:
                print("   TX.COMPLETE overdue with app idle — "
                      "continuing", file=sys.stderr, flush=True)
                return True
            return False

    def load_messages(self) -> list[str]:
        try:
            lines = self.msg_file.read_text().splitlines()
        except OSError as e:
            print(f"msg-rotator: cannot read {self.msg_file}: {e}",
                  file=sys.stderr, flush=True)
            return []
        return [ln for ln in (l.strip() for l in lines)
                if ln and not ln.startswith("#")]

    # ---- main loop --------------------------------------------------

    def run(self) -> int:
        mycall = self.js8.request("STATION.GET_CALLSIGN",
                                  timeout=10).get("value", "")
        if not mycall:
            print("msg-rotator: STATION.GET_CALLSIGN returned "
                  "nothing. Is the TCP API enabled on :2442?",
                  file=sys.stderr)
            return 1
        hail = f"{mycall}: @ALLCALL ACK"
        span = (f"{self.int_min}s" if self.int_min == self.int_max
                else f"randomized {self.int_min}-{self.int_max}s")
        print(f"msg-rotator: {self.msg_file}, interval={span}, "
              f"callsign={mycall}")
        print(f"msg-rotator: each cycle = Normal message, then "
              f"Subspace hail '{hail}'; list hot-reloaded per cycle")
        print("msg-rotator: Ctrl+C to stop\n", flush=True)

        self.set_mode_and_verify(NORMAL, "Normal")

        while True:
            msgs = self.load_messages()
            if not msgs:
                secs = self.next_interval()
                log(f"{self.msg_file} is empty; waiting {secs}s")
                self.interval_sleep(secs)
                continue
            self.idx %= len(msgs)

            # BUSY check FIRST — before any halt could abort a live
            # transfer. idx not advanced: same message next time.
            why = self.busy_reason()
            if why:
                log(f"JS8Call busy ({why}); skipping cycle")
                self.announce(BUSY_RETRY_SEC, "attempt")
                self.interval_sleep(BUSY_RETRY_SEC)
                continue

            # Operator compose check — never commandeer typed text.
            typed = self.outgoing_text()
            if typed:
                log(f'outgoing box not empty ("{typed[:40]}...");'
                    " skipping cycle")
                secs = self.next_interval()
                self.announce(secs, "attempt")
                self.interval_sleep(secs)
                continue

            msg = msgs[self.idx]
            log(f"[{self.idx + 1}/{len(msgs)}] Normal: {msg}")

            # Clear stuck startTxButton state from any interrupted
            # prior cycle (toggleTx no-ops on already-checked).
            self.safe_tx_halt()

            if not self.set_mode_and_verify(NORMAL, "Normal"):
                print("   skipping cycle: could not enter Normal",
                      file=sys.stderr, flush=True)
                self.idx += 1
                secs = self.next_interval()
                self.announce(secs, "attempt")
                self.interval_sleep(secs)
                continue

            if not self.send_and_drain(msg):
                # Transfer grabbed the channel mid-send: message DID
                # queue/air — advance idx, skip the hail entirely.
                log("message sent, but ARQ now in progress; "
                    "skipping hail")
                self.idx = (self.idx + 1) % len(msgs)
                self.set_mode_and_verify(NORMAL, "Normal")
                self.announce(BUSY_RETRY_SEC, "send")
                self.interval_sleep(BUSY_RETRY_SEC)
                continue

            log(f"     Subspace hail: {hail}")
            if self.set_mode_and_verify(FT2, "Subspace"):
                self.safe_tx_halt()
                self.send_and_drain(hail)
                # The TX-drain path KEEPS the outgoing text in FT2
                # (re-send affordance) — the completed hail would
                # strand in the box, read as operator compose next
                # cycle, and skip the rotation forever (field
                # 2026-08-20: Andy found the "ACK" and deleted it by
                # hand). Clear it — but ONLY if the box still holds
                # exactly our hail; operator text is never touched.
                if self.outgoing_text().strip() == hail:
                    self.js8.send("TX.SET_TEXT", "")
            else:
                print("   couldn't enter Subspace, skipping hail",
                      file=sys.stderr, flush=True)

            # Back to Normal so auto-replies during the long interval
            # go out in the common mode.
            self.set_mode_and_verify(NORMAL, "Normal")

            self.idx = (self.idx + 1) % len(msgs)
            secs = self.next_interval()
            self.announce(secs, "send")
            self.interval_sleep(secs)


def main() -> int:
    msg_file = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path.home() / "Downloads/msgs.txt"
    interval = sys.argv[2] if len(sys.argv) > 2 else "1500-2100"
    if "-" in interval:
        lo, _, hi = interval.partition("-")
        try:
            int_min, int_max = int(lo), int(hi)
        except ValueError:
            print(f"msg-rotator: bad INTERVAL {interval!r}",
                  file=sys.stderr)
            return 1
        if int_max < int_min:
            print("msg-rotator: INTERVAL MAX < MIN", file=sys.stderr)
            return 1
    else:
        try:
            int_min = int_max = int(interval)
        except ValueError:
            print(f"msg-rotator: bad INTERVAL {interval!r}",
                  file=sys.stderr)
            return 1
    if not msg_file.is_file():
        print(f"msg-rotator: message file not found: {msg_file}",
              file=sys.stderr)
        return 1
    rot = Rotator(msg_file, int_min, int_max)
    try:
        return rot.run()
    except KeyboardInterrupt:
        print("\nmsg-rotator: stopped")
        return 0
    except Js8Error as e:
        # ONE owner for "the connection died" wherever it surfaces
        # (send, mode set, busy probe, keepalive): clean exit 1 for
        # the supervisor, never a raw traceback (field 2026-09-06:
        # a socket drop mid-keepalive escaped as a traceback).
        print(f"msg-rotator: connection lost ({e}) — exiting for "
              "supervisor restart", file=sys.stderr, flush=True)
        return 1
    finally:
        rot.js8.close()


if __name__ == "__main__":
    raise SystemExit(main())
