# JTTY reference dependencies outside lib/jtty

Read-only reference, never compiled by this project — same rule as
`lib/jtty/` (see `lib/jtty/UPSTREAM_DIFF.md`).

- `twkfreq.f90` — WSJT-X `lib/twkfreq.f90` at tag `v3.3.0-beta1`, called by
  `jtty_peakup.f90` and the decode drivers to shift a candidate to baseband.
  Not the same routine as `lib/ft2_deps/twkfreq1.f90` (five coefficients,
  no fast path); JTTY uses the three-coefficient form with its constant-
  shift fast path.

- `packjt77.f90`, `packjt77_schema.f90`, `packjt77_grammar.f90` — WSJT-X
  `lib/77bit/` at tag `v3.3.0-beta1`: the call28 codec (`pack28`,
  `unpack28`, `callok`) and the ARRL section registry that JTTY's Call8
  and CLASS_SECTION atoms depend on. **These, not `lib/ft2_deps/packjt77.f90`,
  are the JTTY authority**: at this tag upstream rewrote `pack28`/`unpack28`
  over the grammar module's `pack77_c28_standard_shape` predicate (3-6
  alphanumerics, call-area digit in position 2 or 3, at most three suffix
  letters), added `pack77_valid_cq_modifier` and
  `pack77_valid_hash_call_token`, and tightened `callok` (prefix must be
  alphanumeric; a long suffix is rejected rather than truncated). The
  FT2-era copy predates all of that.

Also used by the JTTY reference but already vendored elsewhere:

- `lib/ft2/gfsk_pulse.f90` — the Gaussian frequency pulse used by
  `gen_jttywave.f90`
- `lib/ft2_deps/four2a.f90`, `lib/ft2_deps/fftw3.f90` — the FFTW wrapper
  used by `ana64a.f90` and `subtract_jtty.f90`
- `lib/ft2_deps/chkcall.f90` — identical to upstream at this tag

Ported to C++ in `JS8_Mode/jtty_modem.cpp` (twkfreq) and
`JS8_Mode/jtty_source.cpp` (the call28 codec, standard calls only, no hash
table).
