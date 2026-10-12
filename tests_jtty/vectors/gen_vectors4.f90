! Golden-vector generator for the JTTY receiver (Stage 4).
!
! Compiled and run ONLY in the scratchpad against the vendored reference
! Fortran. Never part of the project build. Synthesises 28 s of 12 kHz
! audio carrying several JTTY messages in noise (written raw as
! jtty_rx_audio.pcm, little-endian int16), then drives the reference
! receive-context API through four call patterns and records every return
! value and every message update. tests_jtty/jtty_receiver_test.cpp replays
! the same call sequence through the C++ port.
program gen_vectors4
  use, intrinsic :: iso_c_binding, only: c_int, c_int16_t, c_int64_t, c_float, c_double, c_char
  use, intrinsic :: iso_fortran_env, only: int16, int32, int64
  use jtty_mod, only: pack_jtty, MAX_FRAMES
  use jtty_fec, only: is13, PAYLOAD_BITS, TOTAL_K
  use tbcc, only: tbcc_encode
  use jtty_tbcc_code_profiles, only: JTTY_TBCC_PROFILE_1167_1545_80F
  use jtty_receive_context
  implicit none

  integer, parameter :: NSPS = 384, FS = 12000, NFRAME = 59*NSPS
  integer, parameter :: NTOTAL = 28*FS
  integer(c_int), parameter :: NFA = 200, NFB = 2800
  real(c_float), parameter :: F0 = 1500.0_c_float, FTOL = 100.0_c_float
  real, allocatable :: acc(:)
  integer(c_int16_t), allocatable :: pcm(:)
  integer(int64) :: rng
  integer(c_int) :: h, ret
  integer :: i, n, first
  real :: g1, g2

  rng = 20261014_int64
  allocate(acc(NTOTAL), pcm(NTOTAL))
  do i = 1, NTOTAL
     call gauss(g1, g2)
     acc(i) = 300.0*g1
  end do

  call add_message('CQ KA1ABC CQ', 1500.0, 1.00, 3000.0, 0)
  call add_message('WB9XYZ 599 101', 1412.3, 3.50, 2500.0, 0)
  call add_message('TU KA1ABC CQ', 1750.0, 3.60, 2000.0, 0)
  call add_message('THE QUICK BROWN FOX JUMPED OVER THE LAZY DOG.', 1500.0, 8.00, 1500.0, 4)
  call add_message('599 FN42', 1520.0, 8.30, 800.0, 0)
  call add_message('AGN?', 1300.0, 14.0, 1200.0, 0)
  call add_message('K1JT 599 0004', 1580.0, 20.5, 1200.0, 0)
  call add_message('WM8Q: W5TTA SNR?', 1470.0, 24.0, 1800.0, 0)

  do i = 1, NTOTAL
     pcm(i) = int(nint(max(-32767.0, min(32767.0, acc(i)))), c_int16_t)
  end do
  open(20, file='jtty_rx_audio.pcm', access='stream', form='unformatted', status='replace')
  write(20) pcm
  close(20)

  open(10, file='vectors4.txt', status='replace', action='write')
  write(10, '(a)') 'JTTY_RX_VECTORS 1'
  write(10, '(a,1x,i0)') 'AUDIO jtty_rx_audio.pcm', NTOTAL
  write(10, '(a,1x,i0,1x,z8.8,1x,z8.8,1x,i0,1x,i0)') 'PARAMS', NSPS, transfer(F0, 0_int32), &
       transfer(FTOL, 0_int32), NFA, NFB

  ! A: everything in one call
  write(10, '(a)') 'SCENARIO A'
  h = jtty_rx_create()
  call do_begin(h, 1_c_int64_t, 0_c_int64_t, 0_c_int64_t)
  call do_process(h, 1, NTOTAL, 0_c_int64_t, int(NTOTAL, c_int64_t), 100000)
  call do_end(h, 3)
  call jtty_rx_destroy(h)

  ! B: buffer grows from sample 0 in 6000-sample increments
  write(10, '(a)') 'SCENARIO B'
  h = jtty_rx_create()
  call do_begin(h, 2_c_int64_t, 0_c_int64_t, 0_c_int64_t)
  do n = 6000, NTOTAL, 6000
     call do_process(h, 1, n, 0_c_int64_t, int(n, c_int64_t), 100000)
  end do
  call do_end(h, 3)
  call jtty_rx_destroy(h)

  ! C: sliding 60000-sample window, first_sample advancing
  write(10, '(a)') 'SCENARIO C'
  h = jtty_rx_create()
  call do_begin(h, 3_c_int64_t, 0_c_int64_t, 0_c_int64_t)
  do n = 6000, NTOTAL, 6000
     first = max(0, n - 60000)
     call do_process(h, first + 1, n - first, int(first, c_int64_t), int(n, c_int64_t), 100000)
  end do
  call do_end(h, 2)
  call jtty_rx_destroy(h)

  ! D: a window too short to reach back, exercising the -2 path, with a
  ! grid origin and first-search offset, three steps at a time
  write(10, '(a)') 'SCENARIO D'
  h = jtty_rx_create()
  call do_begin(h, 4_c_int64_t, 1000_c_int64_t, 12000_c_int64_t)
  do n = 6000, NTOTAL, 6000
     first = max(0, n - 20000)
     call do_process(h, first + 1, n - first, int(first, c_int64_t), int(n, c_int64_t), 3)
  end do
  call do_end(h, 3)
  call jtty_rx_destroy(h)

  close(10)
  print *, 'wrote vectors4.txt and jtty_rx_audio.pcm'

contains

  subroutine do_begin(handle, session, grid, first_search)
    integer(c_int), intent(in) :: handle
    integer(c_int64_t), intent(in) :: session, grid, first_search
    call jtty_rx_begin(handle, session, grid, first_search, NSPS)
    write(10, '(a,1x,i0,1x,i0,1x,i0,1x,i0,1x,i0,1x,i0)') 'BEGIN', session, grid, first_search, NSPS, &
         jtty_rx_next_search_sample(handle), jtty_rx_next_required_sample(handle)
  end subroutine do_begin

  subroutine do_process(handle, offset, count, first_sample, stop_sample, max_steps)
    integer(c_int), intent(in) :: handle, count, max_steps
    integer, intent(in) :: offset
    integer(c_int64_t), intent(in) :: first_sample, stop_sample
    integer(c_int) :: r
    r = jtty_rx_process(handle, pcm(offset), count, first_sample, stop_sample, max_steps, NFA, NFB, F0, FTOL)
    write(10, '(a,1x,i0,1x,i0,1x,i0,1x,i0,1x,a,1x,i0,1x,i0,1x,i0)') 'PROCESS', first_sample, count, &
         stop_sample, max_steps, '->', r, jtty_rx_next_search_sample(handle), &
         jtty_rx_next_required_sample(handle)
    call drain(handle)
  end subroutine do_process

  subroutine do_end(handle, reason)
    integer(c_int), intent(in) :: handle, reason
    call jtty_rx_end(handle, reason)
    write(10, '(a,1x,i0)') 'END', reason
    call drain(handle)
  end subroutine do_end

  subroutine drain(handle)
    integer(c_int), intent(in) :: handle
    integer(c_int), parameter :: CAP = 8
    character(kind=c_char) :: text(80*CAP)
    integer(c_int64_t) :: ids(CAP)
    real(c_float) :: freqs(CAP)
    real(c_double) :: starts(CAP), latest(CAP)
    integer(c_int) :: terms(CAP), snrs(CAP)
    integer(c_int) :: k
    integer :: i, j
    character(len=80) :: line
    do
       k = jtty_rx_take_updates(handle, CAP, text, ids, freqs, starts, latest, terms, snrs)
       write(10, '(a,1x,i0)') 'UPDATES', k
       do i = 1, k
          do j = 1, 80
             line(j:j) = text((i - 1)*80 + j)
          end do
          write(10, '(a,1x,i0,1x,z8.8,1x,z16.16,1x,z16.16,1x,i0,1x,i0,1x,a,a,a)') 'U', ids(i), &
               transfer(freqs(i), 0_int32), transfer(starts(i), 0_int64), &
               transfer(latest(i), 0_int64), terms(i), snrs(i), '@', line, '@'
       end do
       if (k == 0) exit
    end do
  end subroutine drain

  subroutine add_message(msg, f1, t0, amp, dropframe)
    character(len=*), intent(in) :: msg
    real, intent(in) :: f1, t0, amp
    integer, intent(in) :: dropframe
    character(len=80) :: message80
    character(len=34) :: c32(MAX_FRAMES)
    integer :: nframes, k, nsym, nw, istart, payload(PAYLOAD_BITS), chan(TOTAL_K)
    integer, allocatable :: tones(:)
    real, allocatable :: wave(:)
    complex :: cjunk(1)
    message80 = msg
    call pack_jtty(message80, c32, nframes)
    if (nframes < 1) stop 'pack failed'
    nsym = 59*nframes
    allocate(tones(nsym))
    do k = 1, nframes
       tones((k - 1)*59 + 1:(k - 1)*59 + 13) = is13
       read(c32(k), '(34i1)') payload
       call tbcc_encode(payload, chan, JTTY_TBCC_PROFILE_1167_1545_80F)
       tones((k - 1)*59 + 14:k*59) = chan
    end do
    nw = nsym*NSPS
    allocate(wave(nw))
    call gen_jttywave(tones, nsym, NSPS, 2.0, 12000.0, f1, cjunk, wave, 0, nw)
    if (dropframe >= 1 .and. dropframe <= nframes) wave((dropframe - 1)*NFRAME + 1:dropframe*NFRAME) = 0.0
    istart = nint(t0*FS)
    do k = 1, nw
       if (istart + k > NTOTAL) exit
       acc(istart + k) = acc(istart + k) + amp*wave(k)
    end do
    write(*, '(a,1x,i0,1x,a)') 'message frames:', nframes, trim(message80)
  end subroutine add_message

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

end program gen_vectors4
