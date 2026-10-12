! Golden-vector generator for the JTTY modem / sync layer (Stage 2).
!
! Compiled and run ONLY in the scratchpad against the vendored reference
! Fortran (lib/jtty, tag v3.3.0-beta1) plus lib/jtty_deps/twkfreq.f90,
! lib/ft2/gfsk_pulse.f90 and lib/ft2_deps/{fftw3,four2a}.f90. Never part of
! the project build. Writes vectors2.txt, replayed by
! tests_jtty/jtty_modem_test.cpp.
!
! Every input the C++ needs is dumped too (noise buffers, int16 audio), so
! the test never has to reproduce this program's random numbers.
program gen_vectors2
  use, intrinsic :: iso_fortran_env, only: int16, int32, int64
  use jtty_fec, only: is13
  use tbcc, only: tbcc_encode, PAYLOAD_BITS, TOTAL_K
  use jtty_tbcc_code_profiles, only: JTTY_TBCC_PROFILE_1167_1545_80F
  use jtty_payload_correlators, only: jtty_payload_correlator, &
       jtty_payload_correlator_prepare, jtty_correlate_payload_symbols
  implicit none

  integer, parameter :: NSS = 192, NSYNC = 13, NCHAN = 46, NFRAME = 59
  integer, parameter :: NBUF = 16384, NPEAK = 8192, NANA = 32768
  integer :: tones(NFRAME), chan(TOTAL_K), payload(PAYLOAD_BITS), i, b, nw
  integer(int64) :: x, rng
  complex :: cwave(NFRAME*NSS), cwave0(NFRAME*NSS), cwavep(NFRAME*NSS)
  complex :: csync(NSYNC*NSS), cjunk(1)
  real :: wave(NFRAME*NSS), wave12(NFRAME*384), xjunk(1)
  complex, allocatable :: buf(:), c1(:), c0(:), c0a(:)
  complex :: zsym(0:3, TOTAL_K), zhalf(0:3, TOTAL_K)
  real :: pow2(0:3, TOTAL_K), pow4(0:3, TOTAL_K)
  real :: a(3), xdt, f1, snr, g1, g2
  integer(int16), allocatable :: iwave(:)
  type(jtty_payload_correlator) :: pc

  rng = 20261012_int64

  x = 777_int64
  do b = 1, PAYLOAD_BITS
     x = mod(x*48271_int64, 2147483647_int64)
     payload(b) = int(iand(x/65536_int64, 1_int64), int32)
  end do
  payload(33) = 0
  call tbcc_encode(payload, chan, JTTY_TBCC_PROFILE_1167_1545_80F)
  tones(1:NSYNC) = is13
  tones(NSYNC+1:NFRAME) = chan

  open(10, file='vectors2.txt', status='replace', action='write')
  write(10, '(a)') 'JTTY_MODEM_VECTORS 1'
  write(10, '(a,59i1)') 'TONES ', tones

  ! --- waveforms --------------------------------------------------------
  nw = NFRAME*NSS
  call gen_jttywave(tones, NFRAME, NSS, 2.0, 6000.0, 1500.0, cwave, xjunk, 1, nw)
  call wc('WAVE6C', cwave, nw)
  nw = NFRAME*NSS
  call gen_jttywave(tones, NFRAME, NSS, 2.0, 6000.0, 1500.0, cjunk, wave, 0, nw)
  call wr('WAVE6R', wave, nw)
  nw = NFRAME*384
  call gen_jttywave(tones, NFRAME, 384, 2.0, 12000.0, 1500.0, cjunk, wave12, 0, nw)
  call wr('WAVE12R', wave12, nw)

  call gen_syncwave(csync, NSS)
  call wc('SYNC', csync, NSYNC*NSS)

  ! --- correlators on a baseband frame in noise --------------------------
  nw = NFRAME*NSS
  call gen_jttywave(tones, NFRAME, NSS, 2.0, 6000.0, 0.0, cwave0, xjunk, 1, nw)
  allocate(buf(0:NBUF-1))
  do i = 0, NBUF-1
     call gauss(g1, g2)
     buf(i) = cmplx(0.3*g1, 0.3*g2)
  end do
  buf(1000:1000+nw-1) = buf(1000:1000+nw-1) + cwave0
  call wc('CORRIN', buf(1000+NSYNC*NSS:1000+nw-1), NCHAN*NSS)
  call jtty_payload_correlator_prepare(pc, NSS)
  call jtty_correlate_payload_symbols(pc, buf(1000+NSYNC*NSS:1000+nw-1), 0, zsym, zhalf)
  call wc('ZSYM', zsym, 4*TOTAL_K)
  call wc('ZHALF', zhalf, 4*TOTAL_K)
  call jtty_block_pow(zsym, NCHAN, 2, pow2)
  call wr('BLOCKPOW2', pow2, 4*NCHAN)
  call jtty_block_pow(zsym, NCHAN, 4, pow4)
  call wr('BLOCKPOW4', pow4, 4*NCHAN)

  ! --- twkfreq, both code paths -----------------------------------------
  allocate(c1(0:NBUF-1))
  call wc('TWKIN', buf(0:2047), 2048)
  a = 0.0
  a(1) = -1500.0
  call twkfreq(buf, c1, 2048, 6000.0, a)
  call wc('TWKA', c1, 2048)
  a(1) = 3.7
  a(2) = 0.2
  a(3) = -0.1
  call twkfreq(buf, c1, 2048, 6000.0, a)
  call wc('TWKB', c1, 2048)
  deallocate(c1)

  ! --- peakup: frame at 1512.3 Hz starting at sample 2000 ----------------
  nw = NFRAME*NSS
  call gen_jttywave(tones, NFRAME, NSS, 2.0, 6000.0, 1512.3, cwavep, xjunk, 1, nw)
  allocate(c0(0:NPEAK-1), c1(0:NPEAK-1))
  do i = 0, NPEAK-1
     call gauss(g1, g2)
     c0(i) = cmplx(0.5*g1, 0.5*g2)
  end do
  c0(2000:NPEAK-1) = c0(2000:NPEAK-1) + cwavep(1:NPEAK-2000)
  call wc('PEAKIN', c0, NPEAK)
  call jtty_peakup(c0, c1, csync, NPEAK, NSS, 0.335, 1512.0, xdt, f1, snr)
  call wr3('PEAKUP', xdt, f1, snr)
  call jtty_peakup(c0, c1, csync, NPEAK, NSS, 2000.0/6000.0, 1512.3, xdt, f1, snr)
  call wr3('PEAKUP2', xdt, f1, snr)

  ! --- analytic conversion and subtraction (FFT based, tolerance gate) ---
  allocate(iwave(NANA), c0a(0:NANA-1))
  do i = 1, NANA
     call gauss(g1, g2)
     iwave(i) = int(nint(200.0*g1), int16)
  end do
  do i = 1, NFRAME*384
     iwave(2000+i) = int(iwave(2000+i) + nint(8000.0*wave12(i)), int16)
  end do
  call wi('IWAVE', iwave, NANA)
  call ana64a(iwave, NANA, c0a, NANA)
  call wc('ANA', c0a, 4096)
  call subtract_jtty(c0a, NANA, NANA/2, tones, NFRAME, NSS, 1500.0, 2000.0/12000.0)
  call wc('SUB', c0a, 4096)
  close(10)
  print *, 'wrote vectors2.txt'

contains

  subroutine wc(tag, c, n)
    character(len=*), intent(in) :: tag
    integer, intent(in) :: n
    complex, intent(in) :: c(n)
    integer :: i, j, m, w(8)
    write(10, '(a,1x,i0)') tag, n
    do i = 1, n, 4
       m = min(4, n - i + 1)
       do j = 1, m
          w(2*j - 1) = transfer(real(c(i + j - 1)), 0_int32)
          w(2*j) = transfer(aimag(c(i + j - 1)), 0_int32)
       end do
       write(10, '(*(z8.8,1x))') w(1:2*m)
    end do
  end subroutine wc

  subroutine wr(tag, r, n)
    character(len=*), intent(in) :: tag
    integer, intent(in) :: n
    real, intent(in) :: r(n)
    integer :: i, j, m, w(8)
    write(10, '(a,1x,i0)') tag, n
    do i = 1, n, 8
       m = min(8, n - i + 1)
       do j = 1, m
          w(j) = transfer(r(i + j - 1), 0_int32)
       end do
       write(10, '(*(z8.8,1x))') w(1:m)
    end do
  end subroutine wr

  subroutine wi(tag, iv, n)
    character(len=*), intent(in) :: tag
    integer, intent(in) :: n
    integer(int16), intent(in) :: iv(n)
    integer :: i
    write(10, '(a,1x,i0)') tag, n
    do i = 1, n, 16
       write(10, '(*(i0,1x))') int(iv(i:min(i + 15, n)))
    end do
  end subroutine wi

  subroutine wr3(tag, p, q, r)
    character(len=*), intent(in) :: tag
    real, intent(in) :: p, q, r
    write(10, '(a,1x,z8.8,1x,z8.8,1x,z8.8)') tag, transfer(p, 0_int32), &
         transfer(q, 0_int32), transfer(r, 0_int32)
  end subroutine wr3

  function uniform() result(u)
    real :: u
    rng = mod(rng*48271_int64, 2147483647_int64)
    u = real(rng)/2147483647.0
  end function uniform

  subroutine gauss(g1, g2)
    real, intent(out) :: g1, g2
    real :: u1, u2, r
    u1 = uniform()
    u2 = uniform()
    if (u1 < 1.0e-7) u1 = 1.0e-7
    r = sqrt(-2.0*log(u1))
    g1 = r*cos(6.2831853*u2)
    g2 = r*sin(6.2831853*u2)
  end subroutine gauss

end program gen_vectors2
