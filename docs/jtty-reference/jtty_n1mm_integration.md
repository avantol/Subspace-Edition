# JTTY and N1MM Logger+ Integration

## Summary

N1MM Logger+ can send either literal JTTY text or an explicit native action. Ordinary TXTEXT uses automatic minimum-frame packing after text and exchange-profile normalization, combining recognized compact forms with TEXT5. Explicit typed exchanges use a leading `[[JTTY:<ACTION>]]` marker in the transmitted text. The marker is consumed by WSJT-X and is never put on the air.

The bundled [`JTTY Messages.mc`](JTTY%20Messages.mc) uses tagged actions for its common Run and S&P messages. This provides compact Call8 and STRUCT30 transmission without asking WSJT-X to infer meaning from visible logger text. The action definitions and source-interface rules are kept in [jtty_source_encoding.txt](jtty_source_encoding.txt).

## Setup

- Configure N1MM Logger+ with WSJT-X as for FT8. A starting point is [Operating WW Digi with N1MM and WSJT-X](https://www.rttycontesting.com/tutorials/n1mm/operating-ww-digi-with-n1mm/).
- Verify radio control, PTT, audio, and the `forEW1` configuration in FT8 before selecting JTTY.
- Enter `JTTY` in the N1MM callsign field and press Enter to start WSJT-X in JTTY mode.
- Confirm that the WSJT-X footer shows JTTY and the window title contains `forEW1`.
- Use radio test mode, zero output power, or a dummy load for initial checks.
- Save the existing Digital Function Key file before loading the supplied `JTTY Messages.mc` example.

End-to-end validation with N1MM on Windows, including two-radio focus, call selection, queue movement, and Run/S&P state transitions, remains required before release.

## Literal TXTEXT

N1MM expands logger macros before passing TXTEXT to WSJT-X. An untagged result uses the literal source interface. Text is folded to uppercase, spaces are normalized, and unsupported characters become `#`. The existing special operating activity supplies a profile captured when each message is submitted. No activity selects Unknown, even though native macros default to serials.

`CQ N9ADG CQ`, `N9ADG`, `599 123`, `599 MA`, `599 FN42`, and `1D EMA` each use one frame without a tag. RTTY Roundup also normalizes report-prefixed serials: `599 05` becomes `599 005`, and `599 0123` becomes `599 123`, each in one frame. `599 001` takes one frame in RTTY Roundup and two otherwise. The canonical message is returned to the GUI for display and logging.

The full recognition, normalization, and spacing rules are in the "Automatic text packing" section of [jtty_source_encoding.txt](jtty_source_encoding.txt). More frame-count examples are in [jtty_design.md](jtty_design.md#contest-exchange-example).

## Writing tagged macros

The marker must begin at the first non-space transmitted character; tag and
action matching is case-insensitive. Non-transmitting N1MM commands may
execute between `{TX}` and the marker:

```text
{TX}[[JTTY:<ACTION>]]<payload>{RX}
```

N1MM expands `{MYCALL}`, `!` or `{CALL}`, and `{EXCH}`. WSJT-X removes the marker and validates the expanded payload under the selected action and active JTTY profile. The complete action and payload table is in the specification's "Tagged N1MM TXTEXT" section.

Payloads contain dynamic values, not the phrases implied by an action. For
example, `CQ` receives only the callsign, `CALL_TU_CQ` receives two callsigns,
and `TU_NOW_EXCH` receives a callsign and exchange. The action supplies `CQ`,
`TU`, `TU NOW`, and canonical full-exchange report text where applicable. Do
not add a hardcoded `599` to an exchange payload.

Under the RTTY profile, an exchange payload is one decimal serial or one
canonical two- or three-character state/province token. For tagged N1MM
input, `DX` is an expanded two-letter location token. The special `DX` and
`#` meanings that select the live serial apply only to the local WSJT-X RTTY
`%E` configuration.

An invalid tagged request is rejected rather than transmitted as literal text. A tag-shaped substring later in the message is just literal text. Check the action's payload and profile rules in the specification when editing a macro.

An all-rejected transaction receives `OUTPUTCOMPLETE` at `XMIT OFF`. When a transaction also contains accepted or pending audio, completion waits until that audio has drained. `ABORT` clears the transaction without reporting a successful output completion.

## Bundled action mappings

The supplied macro file deliberately omits hardcoded `599`. Full-role native exchanges render the report canonically when the selected profile calls for it; Field Day class/section does not.

| Context | Key | Tagged meaning |
| --- | --- | --- |
| Run | F1 | `CQ` |
| Run | F2 | `CALL_EXCH` |
| Run | F3 | `CALL_TU_CQ` |
| Run | F4 | `MYCALL` |
| Run | F5 | `HISCALL` |
| Run | F6 | `CONTROL NR?` |
| Run | F8 | `CONTROL AGN?` |
| S&P | F1 | `CQ` |
| S&P | F2 | `CALL_EXCH` |
| S&P | F3 | `CALL_TU_MY` |
| S&P | F4 | `CALL_MY` |
| S&P | F5 | `HISCALL` |
| S&P | F6 | `MYCALL` |
| S&P | F7 | `EXCH` |
| S&P | F8 | `CONTROL AGN?` |

Run F11 is intentionally left spare. N1MM call stacking can transmit a call
correction before `{LOGTHENPOP}` and change the station referenced by subsequent
callsign and exchange macros. That sequence cannot satisfy the leading-tag
contract reliably without validated N1MM TXTEXT ordering. `TU_NOW_EXCH` remains
available for an explicit leading tag whose already-expanded payload names the
intended station and exchange.

## Native WSJT-X function keys

The WSJT-X JTTY editor offers the same native subset without an N1MM tag. Its defaults and placeholder rules are in the "Native function keys" section of [jtty_source_encoding.txt](jtty_source_encoding.txt). Clickable F1-F8 buttons and keyboard shortcuts select the same actions.

For checks covering both local function keys and N1MM transactions, see [jtty_tx_manual_checks.md](jtty_tx_manual_checks.md).
