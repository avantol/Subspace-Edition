! Golden-vector generator for the JTTY TBCC FEC core.
!
! Compiled and run ONLY in the scratchpad against the vendored reference
! Fortran (lib/jtty, tag v3.3.0-beta1). Never part of the project build.
! Writes vectors.txt, replayed by tests_jtty/jtty_tbcc_test.cpp to prove the
! C++ port bit-identical: encoder tones, list-decoder candidate lists for
! coherent lengths 1/2/4 (bits, identities, both metrics as raw IEEE bits,
! start states, CRC flags, pool counts), the outer ladder result, and the
! simple WAVA decoder as an independent cross-check.
program gen_vectors
  use, intrinsic :: iso_fortran_env, only: int32, int64, real32, real64
  use jtty_tbcc_code_profiles, only: JTTY_TBCC_PROFILE_1167_1545_80F
  use tbcc, only: tbcc_encode, tbcc_wava_fsk_decode, PAYLOAD_BITS, TOTAL_K
  use jtty_tbcc_list_decoder, only: jtty_tbcc_decoder_plan, &
       jtty_tbcc_decoder_workspace, jtty_tbcc_init_decoder_plan, &
       jtty_tbcc_init_decoder_workspace, jtty_tbcc_list_wava_optimized, &
       JTTY_TBCC_MAX_HYPOTHESES, JTTY_TBCC_INFORMATION_BITS
  use jtty_tbcc_decoder, only: jtty_tbcc_decode, jtty_tbcc_decode_result
  implicit none

  integer(int32), parameter :: NENC = 512, NDEC = 72
  integer(int32), parameter :: LENGTHS(3) = [1_int32, 2_int32, 4_int32]
  real(real32), parameter :: SIGMAS(6) = [0.0_real32, 0.4_real32, 0.7_real32, &
       0.9_real32, 1.1_real32, 1.4_real32]
  integer(int32) :: payload(PAYLOAD_BITS), tones(TOTAL_K), ebits(2, TOTAL_K)
  integer(int32) :: outpay(PAYLOAD_BITS), wpay(PAYLOAD_BITS)
  integer(int32) :: cbits(JTTY_TBCC_INFORMATION_BITS, JTTY_TBCC_MAX_HYPOTHESES)
  integer(int64) :: cid(JTTY_TBCC_MAX_HYPOTHESES)
  real(real64) :: ccm(JTTY_TBCC_MAX_HYPOTHESES), cwm(JTTY_TBCC_MAX_HYPOTHESES)
  integer(int32) :: css(JTTY_TBCC_MAX_HYPOTHESES)
  logical :: ccrc(JTTY_TBCC_MAX_HYPOTHESES)
  integer(int32) :: ccount, pcount, i, k, li
  complex(real32) :: corr(0:3, TOTAL_K), hcorr(0:3, TOTAL_K)
  real(real32) :: energies(0:3, TOTAL_K)
  type(jtty_tbcc_decoder_plan) :: plans(3)
  type(jtty_tbcc_decoder_workspace) :: ws(3)
  type(jtty_tbcc_decode_result) :: res
  logical :: ok, wok
  integer(int64) :: rng

  rng = 20261011_int64
  open(10, file='vectors.txt', status='replace', action='write')
  write(10, '(a)') 'JTTY_TBCC_VECTORS 1'

  write(10, '(a,i0)') 'ENC ', NENC
  do i = 1, NENC
     call next_payload(i, payload)
     call tbcc_encode(payload, tones, JTTY_TBCC_PROFILE_1167_1545_80F, ebits)
     write(10, '(34i1,1x,46i1,1x,92i1)') payload, tones, ebits
  end do

  do li = 1, 3
     call jtty_tbcc_init_decoder_plan(plans(li), JTTY_TBCC_PROFILE_1167_1545_80F, &
          LENGTHS(li), 4_int32, 2_int32, JTTY_TBCC_MAX_HYPOTHESES)
     call jtty_tbcc_init_decoder_workspace(ws(li), plans(li))
  end do

  write(10, '(a,i0)') 'DEC ', NDEC
  do i = 1, NDEC
     call next_payload(1000 + i, payload)
     payload(33) = 0_int32
     call tbcc_encode(payload, tones, JTTY_TBCC_PROFILE_1167_1545_80F)
     call make_corr(tones, SIGMAS(1 + mod(i - 1, size(SIGMAS))), corr)
     call make_corr(tones, 1.3_real32*SIGMAS(1 + mod(i - 1, size(SIGMAS))), hcorr)
     write(10, '(a,i0,1x,34i1)') 'FRAME ', i, payload
     write(10, '(a,46i1)') 'TONES ', tones
     call write_corr('CORR', corr)
     call write_corr('HCORR', hcorr)
     do li = 1, 3
        call jtty_tbcc_list_wava_optimized(plans(li), ws(li), corr, cbits, cid, &
             ccm, cwm, css, ccrc, ccount, pcount, prune_reserved_zero=.true.)
        write(10, '(a,i0,1x,i0,1x,i0)') 'LIST ', LENGTHS(li), ccount, pcount
        do k = 1, ccount
           write(10, '(a,46i1,1x,i0,1x,z16.16,1x,z16.16,1x,i0,1x,l1)') 'CAND ', &
                cbits(:, k), cid(k), transfer(ccm(k), 0_int64), &
                transfer(cwm(k), 0_int64), css(k), ccrc(k)
        end do
     end do
     call jtty_tbcc_decode(corr, hcorr, outpay, ok, res)
     write(10, '(a,l1,1x,34i1,1x,i0,1x,i0,1x,i0,1x,i0,1x,i0,1x,i0,1x,z16.16,1x,l1)') &
          'OUTER ', ok, outpay, res%accepted_hypothesis_rank, &
          res%exported_candidate_count, res%circular_candidate_count, &
          res%coherent_block_length, res%evaluated_rung_count, &
          res%accepted_identity, transfer(res%accepted_metric, 0_int64), &
          res%used_half_symbol_observation
     energies = real(corr*conjg(corr))
     call tbcc_wava_fsk_decode(energies, 4_int32, 2_int32, wpay, wok, &
          reserved_zero_bit=33_int32, code_profile=JTTY_TBCC_PROFILE_1167_1545_80F)
     write(10, '(a,l1,1x,34i1)') 'WAVA ', wok, wpay
  end do
  close(10)
  print *, 'wrote vectors.txt'

contains

  subroutine next_payload(idx, p)
    integer(int32), intent(in) :: idx
    integer(int32), intent(out) :: p(PAYLOAD_BITS)
    integer(int32) :: b
    integer(int64) :: x
    select case (idx)
    case (1)
       p = 0_int32
    case (2)
       p = 1_int32
    case (3)
       p = 0_int32
       p(1) = 1_int32
    case (4)
       p = 0_int32
       p(PAYLOAD_BITS) = 1_int32
    case default
       x = mod(int(idx, int64)*7919_int64 + 104729_int64, 2147483647_int64)
       do b = 1, PAYLOAD_BITS
          x = mod(x*48271_int64, 2147483647_int64)
          p(b) = int(iand(x/65536_int64, 1_int64), int32)
       end do
    end select
  end subroutine next_payload

  function uniform() result(u)
    real(real32) :: u
    rng = mod(rng*48271_int64, 2147483647_int64)
    u = real(rng, real32)/2147483647.0_real32
  end function uniform

  subroutine gauss(g1, g2)
    real(real32), intent(out) :: g1, g2
    real(real32) :: u1, u2, r
    u1 = uniform()
    u2 = uniform()
    if (u1 < 1.0e-7_real32) u1 = 1.0e-7_real32
    r = sqrt(-2.0_real32*log(u1))
    g1 = r*cos(6.2831853_real32*u2)
    g2 = r*sin(6.2831853_real32*u2)
  end subroutine gauss

  subroutine make_corr(t, sigma, c)
    integer(int32), intent(in) :: t(TOTAL_K)
    real(real32), intent(in) :: sigma
    complex(real32), intent(out) :: c(0:3, TOTAL_K)
    integer(int32) :: s, tone
    real(real32) :: g1, g2
    do s = 1, TOTAL_K
       do tone = 0, 3
          g1 = 0.0_real32
          g2 = 0.0_real32
          if (sigma > 0.0_real32) call gauss(g1, g2)
          c(tone, s) = cmplx(sigma*g1, sigma*g2, real32)
          if (tone == t(s)) c(tone, s) = c(tone, s) + cmplx(1.0_real32, 0.0_real32, real32)
       end do
    end do
  end subroutine make_corr

  subroutine write_corr(tag, c)
    character(len=*), intent(in) :: tag
    complex(real32), intent(in) :: c(0:3, TOTAL_K)
    integer(int32) :: s, tone, w(8)
    write(10, '(a)') tag
    do s = 1, TOTAL_K
       do tone = 0, 3
          w(2*tone + 1) = transfer(real(c(tone, s)), 0_int32)
          w(2*tone + 2) = transfer(aimag(c(tone, s)), 0_int32)
       end do
       write(10, '(8(z8.8,1x))') w
    end do
  end subroutine write_corr

end program gen_vectors
