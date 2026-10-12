# JTTY Design

JTTY is a new digital mode designed for fast RTTY-like contest exchanges and other keyboard-to-keyboard communication on the amateur radio bands. It has an operational feel similar to standard RTTY, but far better weak-signal performance and a lower error rate. JTTY transmissions can start at any time and typically last a few seconds. Any arbitrary message can be sent using letters, digits, spaces, and punctuation. Source encoding is especially well optimized for the short, fixed-format messages generally used in RTTY-style radio contesting.

This overview describes the design and gives examples of its use. The detailed protocol and source-interface rules are defined in [jtty_source_encoding.txt](jtty_source_encoding.txt). N1MM setup and macro examples are in [jtty_n1mm_integration.md](jtty_n1mm_integration.md); transmit checks are in [jtty_tx_manual_checks.md](jtty_tx_manual_checks.md).

## Frame and waveform

A transmission contains one or more 1.888-second frames. The waveform is Gaussian-smoothed four-tone frequency-shift keying (4-GFSK) at 31.25 baud. The occupied bandwidth (99% of transmitted power) is 127 Hz. Each frame carries one source atom, protected by a CRC and a tail-biting convolutional code. An end-of-message (EOM) bit marks the final atom.

The receiver verifies FEC, CRC, the reserved-zero bit, and the complete source grammar before using a decoded atom. An invalid atom is discarded before display, EOM handling, slot creation, or signal subtraction.

## Source contracts

JTTY separates automatic text packing from typed native actions.

Ordinary keyboard text, `sjtty` input, externally queued strings, and untagged N1MM/MMTTY text use the literal source interface. Text is folded to uppercase, spaces are normalized, and unsupported characters become `#`. The explicit RTTY Roundup profile also canonicalizes report-prefixed serials. The encoder chooses the minimum-frame combination of recognized compact atoms and five-character TEXT5 frames that preserves the resulting normalized text exactly. "Literal" identifies the text interface, not a requirement to use TEXT5 or bypass profile normalization.

The eight shipped JTTY function-key templates use a NativeMacro contract. When a default template is selected, it is compiled to typed call and exchange atoms before placeholder expansion. Native atoms provide compact transmission and unambiguous fields for future logger integration. A customized template that does not match a native form falls back to automatic text packing after normal placeholder expansion. A recognized native template with invalid runtime data is rejected rather than silently transmitted with different semantics.

Restricting ordinary input to TEXT5 would avoid guessing whether `05` meant a serial, zone, or check, but would also lose unambiguous callsign compaction. Automatic packing uses a minimum-frame algorithm, with context-free recognition and an explicit exchange profile that can normalize serials and enable additional typed candidates.

## Automatic text packing

Automatic packing recognizes calls, registered control phrases, generic numbers and locations, grids, and Field Day class/section exchanges at complete token boundaries. A dynamic program over character offsets selects the fewest frames under this recognition policy, rather than greedily taking the first compact form.

The GUI captures an exchange profile from the existing special operating activity when each message is submitted: Unknown, Field Day, or RTTY Roundup. No activity means Unknown even though native macros default to serial exchanges. RTTY Roundup also recognizes report-prefixed serial and state/province exchanges and normalizes serial spelling. The detailed recognition, normalization, spacing, and tie-breaking rules are in the specification's "Automatic text packing" section.

The result need not match the theoretical minimum with perfect knowledge of exchange semantics. Under Unknown or Field Day, `599 001` and `599 05` still take two frames. An explicit codec caller can encode `599 05` as a CQ zone in one frame, but the current GUI macros do not expose that numeric kind; RTTY Roundup explicitly selects serial meaning instead.

`sjtty` defaults to Unknown. An optional leading `--exchange-profile=unknown|field-day|rtty-roundup` selects the profile for either its one-message packing invocation or its eight-argument waveform invocation. Invalid profile values are errors. For example, `sjtty --exchange-profile=rtty-roundup "599 001"` selects the one-frame serial interpretation.

## Source grammar

A source atom carries a compact callsign action, a typed STRUCT30 exchange or control, or five TEXT5 characters. STRUCT30 covers numeric and location fields, paired exchanges, UTC time, grids, and common control phrases. Typed atoms have canonical renderings; formatting choices and visible repetition do not consume wire bits.

The bit layouts, field ranges, enum assignments, validity rules, and golden vectors are kept in [jtty_source_encoding.txt](jtty_source_encoding.txt).

## Native function keys

Keyboard shortcuts and the clickable F1-F8 buttons select the same actions. `%E` supplies the configured contest exchange and `%G` a grid. The default templates, profile rules, and saved-template migration are defined in the specification's "Native function keys" section.

The practical native GUI and N1MM transmit subset covers Call8, serial or RTTY state/province exchanges, Field Day class/section, GRID4, and registered control phrases. Automatic text packing additionally uses GENERIC_NUMERIC and GENERIC_QTH without assigning contest-specific meanings. Other normative STRUCT30 types are decoded, validated, and rendered canonically but are not inferred from text or exposed as general-purpose macros yet.

## Tagged N1MM actions

N1MM may explicitly request native encoding by starting TXTEXT with `[[JTTY:<ACTION>]]`. N1MM expands its macros before WSJT-X validates the action payload under the active profile.

Untagged N1MM text remains literal. A malformed tag, unknown action, invalid call, invalid exchange or grid, unregistered control phrase, or profile mismatch is rejected rather than transmitted as literal bracket text. The exact grammar and action payloads are in [jtty_source_encoding.txt](jtty_source_encoding.txt), with operating examples in [jtty_n1mm_integration.md](jtty_n1mm_integration.md).

## Contest exchange example

The native defaults keep the usual run sequence compact while retaining explicit meanings:

| Run station | S+P stations | Frames |
| --- | --- | ---: |
| `CQ KA1ABC CQ` | | 1 |
| | `WB9XYZ` | 1 |
| | `JA6DEF` | 1 |
| `WB9XYZ 599 101` | | 2 |
| | `599 057` | 1 |
| `TU NOW JA6DEF 599 102` | | 2 |
| | *(no decode)* | 1 |
| `JA6DEF AGN?` | | 1 |
| | `599 292` | 1 |
| `TU KA1ABC CQ` | | 1 |

Typed exchanges such as `599 05 NWT`, `599 156 1749`, and `1D EMA` fit one STRUCT30 frame. Automatic text packing recognizes `1D EMA`, but does not infer the zone/location or serial/time meanings of the other two examples. Strong FEC makes repeated visible fields unnecessary; when RF redundancy is desired, repeating the protected atom provides another independent synchronization, FEC, and CRC opportunity.

These frame counts apply to ordinary text, without a native template or contest profile:

| Input | Frames | Reason |
| --- | ---: | --- |
| `CQ K1ABC CQ` or `CQ KA1ABC CQ` | 1 | CQ call atom |
| `WB9XYZ` | 1 | Call atom |
| `WB9XYZ TU CQ KA1ABC CQ` | 2 | Two call atoms |
| `WB9XYZ 599 123` | 2 | Call and generic numeric exchange |
| `599 123` | 1 | Generic numeric exchange |
| `599 MA` | 1 | Generic QTH exchange |
| `599 FN42` | 1 | Full-role GRID4 |
| `1D EMA` | 1 | Class/section pair |
| `599 001` or `599 05` | 2 | No numeric kind is inferred to preserve leading zeros |
| `599 BRUCE` | 2 | No current generic five-character exchange atom |

With the RTTY Roundup profile, `599 001` takes one frame and `K1ABC 599 001` takes two. `599 05` becomes `599 005` in one frame. The other examples retain their frame counts, although `599 123` and `599 MA` use typed SERIAL and STATE_PROVINCE atoms instead of generic atoms.
