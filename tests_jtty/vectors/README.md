# JTTY TBCC golden vectors

`jtty_tbcc_vectors.txt` was produced by the **reference Fortran** in
`lib/jtty` (WSJT-X tag `v3.3.0-beta1`), compiled standalone with gfortran in a
scratch directory. It is the authority that `../jtty_tbcc_test.cpp` holds the
C++ port in `JS8_Mode/jtty_tbcc.cpp` to, bit for bit:

- 512 encoder frames: payload, 46 tones, 92 raw coded bits
- 72 decoder frames at six noise levels (sigma 0, 0.4, 0.7, 0.9, 1.1, 1.4;
  25 decode, 47 fail -- the failures are vectors too), each with:
  - the complex correlations (IEEE bit patterns, so nothing is rounded)
  - an independent half-symbol realisation
  - the list decoder's full candidate list for coherent lengths 1, 2 and 4:
    bits, identity, clean and WAVA metrics as raw IEEE doubles, start state,
    CRC flag, candidate and pool counts
  - the outer ladder result and every field of its result record
  - the simple energy-only WAVA decoder's verdict, as a cross-check

`jtty_modem_vectors.txt` (Stage 2, `../jtty_modem_test.cpp`) covers the
modem and sync layer, with every input the C++ needs dumped alongside the
expected output so the test never reproduces the generator's random numbers:

- `genJttyWave` complex and real at 6 kHz / nss 192, real at 12 kHz / 384
- `genSyncWave`
- `PayloadCorrelator` on a dumped baseband frame-in-noise buffer, plus
  `jttyBlockPow` for block lengths 2 and 4 on the reference correlations
- `twkfreq`, both the constant-shift fast path and the polynomial path
- `jttyPeakup`, two coarse starting points, on a dumped 8192-sample buffer
- `ana64a` on dumped int16 audio and `subtractJtty` on its result -- the two
  FFT-based routines, gated to a relative tolerance of 1e-5 because FFTW's
  plan choice is not guaranteed identical across machines (on the machine
  that produced them, both matched to the bit)

`jtty_source_vectors.txt` (Stage 3, `../jtty_source_test.cpp`) covers the
source grammar, every record exact. Text fields are delimited by `@`, which
is outside the JTTY alphabet:

- `ATOMIN`/`ATOMOUT`: atom -> `packJttyAtom` -> frame -> `unpackJttyAtom` ->
  `renderJttyAtom`, for every atom kind with valid and invalid field values
- `WORD`: `unpackJttyAtom` + render on raw 34-bit words -- the
  specification's ten golden vectors, their mutations, 900 random words,
  and every call action for four real callsigns including the two reserved
  forms that must be rejected
- `CALL` / `N28`: `pack28`, `unpack28`, `standardCall` over 43 callsign
  shapes and 622 n28 values across every range of the number space
- `MSGIN`/`MSGNORM`: `normalizeJttyMessage` with the 80-character field
- the first `ASM` after each message: `packJtty` (Stage 5, the automatic
  text packer under the Unknown profile) must reproduce the reference's
  frames exactly -- 88 messages including the specification's examples,
  every candidate family, and the tie-break cases
- `ASM`/`ASMOUT`: `unpackJtty` reassembly, including a mid-message frame
  with the reserved bit set, an all-zero sentinel frame, and a cleared EOM

The call28 authority for this file is **upstream's** `lib/77bit/packjt77.f90`
at the JTTY tag, vendored under `lib/jtty_deps/`, not the FT2-era copy in
`lib/ft2_deps/`: at this tag upstream rewrote `pack28`/`unpack28` over the
grammar module's c28 shape predicate and tightened `callok`, and the port
follows the new rules.

`jtty_receiver_vectors.txt` + `jtty_rx_audio.pcm` (Stage 4,
`../jtty_receiver_test.cpp`) cover the whole receiver end to end. The
generator synthesises 28 s of 12 kHz audio (raw little-endian int16) carrying
eight JTTY messages in Gaussian noise -- a one-frame CQ, two-, three- and
four-frame messages, two overlapping in time at different frequencies, a
nine-frame message with its fourth frame silenced (the gap renders as
" ... "), a message outside channel 0's window that only the fixed channels
can find, a weak signal 20 Hz under a strong one, and one cut off by the end
of the audio -- then drives the reference receive-context API through four
call patterns: one shot, a growing buffer, a sliding 60000-sample window, and
a 20000-sample window too short to reach back (the -2 path) with a grid
origin and first-search offset. Every return value, next-search and
next-required sample, and every message update are recorded.

This gate is on **outcomes**, not bits: ids, terminals and text exact;
frequency within 1 Hz, sync times within 5 ms, SNR within 1 dB. The
reference plans its sync-search FFTs with FFTW_MEASURE, whose plan choice is
timing dependent, so even the reference's own sync surface is not
bit-reproducible; the port uses FFTW_ESTIMATE and is deterministic.

Neither this project's build nor its CI touches Fortran. Regeneration is a
manual step, only needed if the port is re-synced to a newer upstream tag.

**2026-10-19: the generators (`gen_vectors*.f90`, `fftw3mod.f90`) and the
Fortran reference (`lib/`) were removed from this repository** -- the
project carries no Fortran. The vector files above are unchanged and remain
the authority the tests read. To regenerate, restore the generators and the
reference from git history (any commit up to `12cb238e`, Build 504), e.g.
`git show 12cb238e:tests_jtty/vectors/gen_vectors.f90 > gen_vectors.f90` and
`git archive 12cb238e lib | tar -x -C <scratch>`, or fetch `lib/jtty` from
WSJT-X at the tag. The commands below describe that layout.

## Regenerating

From any scratch directory, with `lib/jtty` as `$S` and `lib` as `$D`:

    gfortran -O2 -ffp-contract=off -J . -o gen_vectors \
        $S/jtty_tbcc_code_profile.f90 $S/tbcc.f90 \
        $S/jtty_tbcc_list_decoder.f90 $S/jtty_tbcc_decoder.f90 \
        $S/jtty_fec_mod.f90 gen_vectors.f90
    ./gen_vectors            # writes vectors.txt
    cp vectors.txt <repo>/tests_jtty/vectors/jtty_tbcc_vectors.txt

    gfortran -O2 -ffp-contract=off -fno-tree-vectorize -J . -o gen_vectors2 \
        $D/ft2_deps/fftw3.f90 $D/ft2_deps/four2a.f90 $D/ft2/gfsk_pulse.f90 \
        $D/jtty_deps/twkfreq.f90 \
        $S/jtty_tbcc_code_profile.f90 $S/tbcc.f90 \
        $S/jtty_tbcc_list_decoder.f90 $S/jtty_tbcc_decoder.f90 \
        $S/jtty_fec_mod.f90 $S/jtty_payload_correlators.f90 \
        $S/gen_jttywave.f90 $S/gen_syncwave.f90 $S/jtty_peakup.f90 \
        $S/jtty_block_pow.f90 $S/subtract_jtty.f90 $S/ana64a.f90 \
        gen_vectors2.f90 -lfftw3f
    ./gen_vectors2           # writes vectors2.txt
    cp vectors2.txt <repo>/tests_jtty/vectors/jtty_modem_vectors.txt

`-ffp-contract=off` on both the Fortran and the C++ side (see `../build.sh`)
keeps float-to-double promotion identical, which is what makes a last-bit
comparison meaningful; `-fno-tree-vectorize` keeps libm calls scalar on both
sides. The generators' noise comes from their own 31-bit LCG with fixed
seeds, so the files are reproducible.

Produced 11 October 2026 with GNU Fortran 13.3.0 and FFTW 3.3.10.
