# JTTY upstream reference — provenance and churn

> **2026-10-19: the Fortran is no longer in this repository.** `lib/jtty`,
> `lib/jtty_deps`, `lib/ft2` and `lib/ft2_deps` were removed so the project
> carries no Fortran at all. What stays, here in `docs/jtty-reference/`, is
> the non-Fortran material that came with them: this file, the
> specification (`jtty_source_encoding.txt`, `jtty_design.md`), the
> upstream test material (`decodes.txt`, `jtty_msgs.txt`), the TX manual
> checks, and `DEPS.md`. To diff or re-sync, fetch `lib/jtty` from
> `github.com/WSJTX/wsjtx` at the tag below (or a newer one); the copy this
> project ported from is also in git history -- any commit up to
> `12cb238e` (Build 504) has it under `lib/`. Paths below that say
> `lib/...` describe that former layout.

This directory WAS a **read-only reference copy** of WSJT-X's `lib/jtty`, kept
for diffing and for porting to C++. **Nothing here was compiled.** The build
stays `LANGUAGES C CXX` (`CMakeLists.txt:61`) and no gfortran is added to any
CI workflow.

Do not edit these files. Fixes belong in the C++ port.

## Provenance

- Source: `github.com/WSJTX/wsjtx`, path `lib/jtty`
- Vendored tag: **`v3.3.0-beta1`** (released 7 October 2026)
- Compared against: **`v3.2.0-rc1`** (released 24 September 2026)
- Fetched 11 October 2026 via the GitHub contents API; every file verified
  byte-for-byte against the upstream size manifest (37 files, 0 mismatches)
- Licence: GPL-3.0, same as this project

Omitted from the 41 upstream files, as host plumbing with no porting value:
`portaudio.h` (42 KB third-party header), `jttyaudio.c` (standalone audio
app), `JTTY Messages.mc` (Windows message compiler), `wava` (empty).

## Where JTTY came from

JTTY is not a parallel invention — it occupies the slot FT2 used to:

| Tag | `lib/ft2` | `lib/jtty` |
| :--- | :--- | :--- |
| `v3.0.2` | present | absent |
| `v3.2.0-rc1` | **removed** | **added** |
| `v3.3.0-beta1` | absent | present |

(`master` still carries `lib/ft2` and no `lib/jtty`; the 3.2/3.3 line is where
the substitution happened.)

## Measured churn, rc1 to beta1 — 13 days apart

**The headline: no wire-format constant changed.** Frame length, baud rate,
bit layout, CRC and the convolutional code polynomials are identical across
both tags. What moved is encoder ergonomics and receiver structure. This is
why Stage 1 and Stage 2 of the port are safe to start now.

Line-level diff (removed / added):

| File | − | + | What changed |
| :--- | ---: | ---: | :--- |
| `jtty_mdecode.f90` | 29 | 78 | decoder orchestration |
| `genjtty.f90` | 4 | 18 | generator |
| `jtty_mod.f90` | 3 | 10 | **encode API only** — see below |
| `sjtty.f90` | 1 | 7 | standalone sender |
| `jtty_source_codec.f90` | 2 | 6 | source codec |
| `rjtty_sub.f90` | 1 | 4 | standalone receiver |
| `jtty_receive_context.f90` | — | NEW | 11.6 KB new module |
| `jtty_source_encoding.txt` | 46 | 61 | **the specification itself** |

Total code churn excluding the new module and the spec: **−40 / +123 lines
across six files.** The byte deltas look larger than the real change.

### The whole of `jtty_mod.f90`'s change

`pack_jtty` gained two optional arguments and one `trim()` was dropped:

- `frame_starts(MAX_FRAMES)`, out, optional — 1-indexed start column of each
  frame's source text in the normalized message
- `is_final`, in, optional — 0 suppresses the "last frame of message" flag
  even on the final frame, which lets a second message be queued behind the
  first without a spacing atom
- `pack77_arrl_section_index(trim(words(2)))` → `(words(2))`, a bug fix

Both new arguments are TX-side display and queuing conveniences. Neither
affects the bits on the air.

### What the new module is

`jtty_receive_context.f90` is a reentrant receiver-state container with an
`iso_c_binding` interface: `message_assembly` for in-flight multi-frame
messages, `frame_fingerprint` over `MAX_RECENT_FRAMES` for upstream's own
recent-frame dedup, a pending-update queue, and `receive_retro_state` for
already-subtracted signals. It carries `nsps=384`, confirming 12000/384 =
31.25 baud from source.

Its arrival helps the port rather than hindering it: it is the receiver state
model already factored behind a C-callable seam, which is the shape a C++
port wants. Port against this, not against rc1's in-line state.

## Protocol parameters, confirmed from source

These replace the third-party figures used in earlier analysis
(`jl1nie/mfsk-core` issue 477), which were correct but second-hand:

- 1.888 s frames, one or more per message, maximum 16
- 4-GFSK, Gaussian-smoothed, 31.25 baud, `nsps=384` at 12 kHz
- occupied bandwidth 127 Hz at **99 % of transmitted power** — note this is a
  power measurement, not `tones x spacing`; the 4 x 31.25 = 125 Hz figure used
  in our spectral-efficiency comparison is the other convention, and the two
  must not be mixed in one table
- 34 bits per frame: 32 source bits + reserved-zero bit (33) + EOM bit (34)
- protected by a CRC and a tail-biting convolutional code
- receiver truncates the accumulated display at 80 characters
- TEXT5 carries five characters per frame, 6 bits each, from this 64-symbol
  alphabet (character values 0-63 index it):

      0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?!"#$%,&*()_'=[]{}<>|:;

  Every character our ARQ and relay wire forms use is present: `/ + - . # * >
  :` and all of base32.
- Call8 atoms reuse "the existing call28 representation" for standard calls
  only; nonstandard calls fall back to TEXT5, so **no callsign hash table is
  required**
- the receiver must verify FEC, CRC, the reserved-zero bit **and the complete
  source grammar** before using an atom — a text-only partial decoder is
  non-conforming

## Re-sync procedure

JTTY is still moving and 3.2.0 has not reached GA. Before the TX path keys
anything on the air:

1. List `lib/jtty` at the new tag and diff the file manifest against the table
   above.
2. Diff `jtty_mod.f90` and `jtty_source_encoding.txt` first — those two decide
   whether the wire changed.
3. If no wire constant moved, only the volatile layer needs review (Stage 4
   and Stage 5 of the port). If one did, the FEC core must be re-verified
   against `decodes.txt` before anything transmits.

## Gate material shipped with the reference

- `decodes.txt` — 23 KB of upstream decode vectors
- `jtty_msgs.txt` — test messages
- `jtty_source_encoding.txt` — normative bit layouts, field ranges, enum
  assignments, validity rules and golden vectors
- `jtty_design.md` — design overview
- `jtty_tx_manual_checks.md`, `jtty_n1mm_integration.md`

These let the port be validated without building any Fortran.
