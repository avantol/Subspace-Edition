! Golden-vector generator for the JTTY source grammar (Stage 3).
!
! Compiled and run ONLY in the scratchpad against the vendored reference
! Fortran (lib/jtty jtty_source_codec.f90 + jtty_mod.f90, lib/ft2_deps
! packjt77.f90 + chkcall.f90, upstream lib/77bit/packjt77_grammar.f90).
! Never part of the project build. Writes vectors3.txt, replayed by
! tests_jtty/jtty_source_test.cpp.
!
! Text fields are delimited by '@', which is not in the JTTY alphabet, is
! never produced by normalisation ('#' is) and never appears in a decoded
! message ('~' does).
program gen_vectors3
  use, intrinsic :: iso_fortran_env, only: int32, int64
  use jtty_mod
  use packjt77, only: pack28, unpack28
  implicit none

  integer, parameter :: NTOKENS = 2063592, MAX22 = 4194304
  character(len=34) :: frame, c32(MAX_FRAMES)
  character(len=80) :: line
  integer(int64) :: rng, n34
  integer :: i, k, ios, nchunks, nfr
  character(len=64), parameter :: ALPHA = &
       '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?!"#$%,&*()_''=[]{}<>|:;'

  rng = 20261013_int64
  open(10, file='vectors3.txt', status='replace', action='write')
  write(10, '(a)') 'JTTY_SOURCE_VECTORS 1'

  ! ---- atoms: every kind, valid and invalid ------------------------------
  do i = JTTY_CALL_CQ, JTTY_CALL_TU_NOW
     call emit(jtty_call_atom(i, 'K1ABC'), i == JTTY_CALL_TU_NOW)
     call emit(jtty_call_atom(i, 'KA1ABC'), .false.)
  end do
  call emit(jtty_call_atom(JTTY_CALL_CQ, 'W1AW'), .true.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'PJ4/KA1ABC'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'QU1RK'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, '3DA0XX'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, '2E0ABC'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'VK9NS'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'A1A'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'KH6ABC'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'E73ABC'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, '4U1UN'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'KK7LMI'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'WM8Q'), .true.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'K7RIE'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'W5TTA'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'K1J'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'K1JTXYZ'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, '12ABC'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'CQ'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'DE'), .false.)
  call emit(jtty_call_atom(JTTY_CALL_CALL, 'k1abc'), .false.)
  call emit(jtty_call_atom(6, 'K1ABC'), .false.)
  call emit(jtty_call_atom(-1, 'K1ABC'), .false.)

  do k = JTTY_ROLE_FIELD_ONLY, JTTY_ROLE_FULL
     do i = JTTY_NUM_SERIAL, JTTY_NUM_GENERIC
        call emit(jtty_exch_num_atom(k, i, 0), .false.)
        call emit(jtty_exch_num_atom(k, i, 1), .false.)
        call emit(jtty_exch_num_atom(k, i, 40), .false.)
        call emit(jtty_exch_num_atom(k, i, 123), .true.)
        call emit(jtty_exch_num_atom(k, i, 999), .false.)
        call emit(jtty_exch_num_atom(k, i, 1000), .false.)
        call emit(jtty_exch_num_atom(k, i, 9999), .false.)
        call emit(jtty_exch_num_atom(k, i, 10000), .false.)
        call emit(jtty_exch_num_atom(k, i, 131071), .false.)
        call emit(jtty_exch_num_atom(k, i, 131072), .false.)
     end do
  end do
  call emit(jtty_exch_num_atom(JTTY_ROLE_FULL, JTTY_NUM_CQ_ZONE, 41), .false.)
  call emit(jtty_exch_num_atom(JTTY_ROLE_FULL, JTTY_NUM_ITU_ZONE, 90), .false.)
  call emit(jtty_exch_num_atom(JTTY_ROLE_FULL, JTTY_NUM_ITU_ZONE, 91), .false.)
  call emit(jtty_exch_num_atom(JTTY_ROLE_FULL, JTTY_NUM_LICENSE_YEAR, 2026), .false.)
  call emit(jtty_exch_num_atom(JTTY_ROLE_FULL, 8, 5), .false.)
  call emit(jtty_exch_num_atom(2, JTTY_NUM_SERIAL, 5), .false.)
  call emit(jtty_exch_num_atom(JTTY_ROLE_FULL, JTTY_NUM_SERIAL, -1), .false.)

  do k = JTTY_ROLE_FIELD_ONLY, JTTY_ROLE_FULL
     do i = JTTY_LOC_STATE_PROVINCE, JTTY_LOC_ADMIN_CODE
        call emit(jtty_exch_loc_atom(k, i, 'CA'), .false.)
        call emit(jtty_exch_loc_atom(k, i, 'NWT'), .true.)
        call emit(jtty_exch_loc_atom(k, i, 'EMA'), .false.)
        call emit(jtty_exch_loc_atom(k, i, '0A'), .false.)
        call emit(jtty_exch_loc_atom(k, i, 'ZZZ'), .false.)
     end do
  end do
  call emit(jtty_exch_loc_atom(JTTY_ROLE_FULL, JTTY_LOC_QTH, '0AB'), .false.)
  call emit(jtty_exch_loc_atom(JTTY_ROLE_FULL, JTTY_LOC_QTH, 'ABCD'), .false.)
  call emit(jtty_exch_loc_atom(JTTY_ROLE_FULL, JTTY_LOC_QTH, 'A'), .false.)
  call emit(jtty_exch_loc_atom(JTTY_ROLE_FULL, JTTY_LOC_QTH, 'a1'), .false.)
  call emit(jtty_exch_loc_atom(JTTY_ROLE_FULL, 5, 'CA'), .false.)

  call emit(jtty_zone_loc_atom(5, 'NWT'), .true.)
  call emit(jtty_zone_loc_atom(1, 'CA'), .false.)
  call emit(jtty_zone_loc_atom(40, 'ZZZ'), .false.)
  call emit(jtty_zone_loc_atom(0, 'CA'), .false.)
  call emit(jtty_zone_loc_atom(41, 'CA'), .false.)
  call emit(jtty_zone_loc_atom(5, '0AB'), .false.)
  call emit(jtty_class_section_atom(1, 'D', 11), .true.)
  call emit(jtty_class_section_atom(32, 'F', 86), .false.)
  call emit(jtty_class_section_atom(1, 'A', 1), .false.)
  call emit(jtty_class_section_atom(0, 'D', 11), .false.)
  call emit(jtty_class_section_atom(33, 'D', 11), .false.)
  call emit(jtty_class_section_atom(1, 'G', 11), .false.)
  call emit(jtty_class_section_atom(1, 'D', 0), .false.)
  call emit(jtty_class_section_atom(1, 'D', 87), .false.)
  call emit(jtty_class_section_atom(1, 'DX', 11), .false.)

  call emit(jtty_exch_num_time_atom(JTTY_ROLE_FULL, 156, 17*60 + 49), .true.)
  call emit(jtty_exch_num_time_atom(JTTY_ROLE_FIELD_ONLY, 0, 0), .false.)
  call emit(jtty_exch_num_time_atom(JTTY_ROLE_FULL, 16383, 1439), .false.)
  call emit(jtty_exch_num_time_atom(JTTY_ROLE_FULL, 16384, 0), .false.)
  call emit(jtty_exch_num_time_atom(JTTY_ROLE_FULL, 1, 1440), .false.)
  call emit(jtty_exch_num_time_atom(JTTY_ROLE_FULL, 1234, 60), .false.)

  do i = 0, 18
     call emit(jtty_control_atom(i), i == 17)
  end do
  call emit(jtty_control_atom(-1), .false.)

  do k = JTTY_ROLE_FIELD_ONLY, JTTY_ROLE_FULL
     call emit(jtty_grid4_atom(k, 'FN42'), k == 1)
     call emit(jtty_grid4_atom(k, 'AA00'), .false.)
     call emit(jtty_grid4_atom(k, 'RR99'), .false.)
     call emit(jtty_grid4_atom(k, 'SS00'), .false.)
     call emit(jtty_grid4_atom(k, 'FN4'), .false.)
     call emit(jtty_grid4_atom(k, 'fn42'), .false.)
     call emit(jtty_grid4_atom(k, 'DN61'), .false.)
  end do
  call emit(jtty_grid4_atom(2, 'FN42'), .false.)

  call emit(jtty_text5_atom('HELLO'), .true.)
  call emit(jtty_text5_atom('HI'), .false.)
  call emit(jtty_text5_atom(''), .false.)
  call emit(jtty_text5_atom('     '), .true.)
  call emit(jtty_text5_atom('a'), .false.)
  call emit(jtty_text5_atom('~~~~~'), .false.)
  call emit(jtty_text5_atom('HELLO WORLD'), .false.)
  do i = 1, 61, 5
     call emit(jtty_text5_atom(ALPHA(i:min(i + 4, 64))), .false.)
  end do

  ! ---- raw 34-bit words: the specification's golden vectors, mutations, noise
  call emit_hex(int(z'026F78D41', int64))
  call emit_hex(int(z'20007B008', int64))
  call emit_hex(int(z'20007B009', int64))
  call emit_hex(int(z'2001BA019', int64))
  call emit_hex(int(z'00B790D29', int64))
  call emit_hex(int(z'082C58029', int64))
  call emit_hex(int(z'204E42D39', int64))
  call emit_hex(int(z'000000049', int64))
  call emit_hex(int(z'04A198049', int64))
  call emit_hex(int(z'11395558D', int64))
  call emit_hex(0_int64)
  call emit_hex(1_int64)
  call emit_hex(2_int64)
  call emit_hex(3_int64)
  call emit_hex(int(z'026F78D43', int64))   ! CQ K1ABC CQ with the reserved bit set
  call emit_hex(int(z'3FFFFFFFF', int64))
  call emit_hex(int(z'3FFFFFFFC', int64))
  do i = 1, 600
     n34 = ior(ishft(lcg32(), 2), iand(lcg32(), 1_int64))
     if (mod(i, 7) == 0) n34 = ior(n34, 2_int64)   ! reserved bit set: must be invalid
     call emit_hex(n34)
  end do
  ! words biased to the structured and call families
  do i = 1, 300
     n34 = ior(ishft(lcg32(), 2), iand(lcg32(), 1_int64))
     n34 = iand(n34, not(12_int64))                 ! clear i2
     if (mod(i, 3) == 0) n34 = ior(n34, 8_int64)     ! i2 = 2 (STRUCT30)
     if (mod(i, 3) == 1) n34 = ior(n34, 4_int64)     ! i2 = 1 (call, AGN / TU NOW)
     call emit_hex(n34)
  end do
  ! standard-call words: encoder n28 of real calls with every action
  call emit_call_words('K1JT')
  call emit_call_words('N9JOW')
  call emit_call_words('VE4TIM')
  call emit_call_words('YO8PGI')

  ! ---- call28 codec ------------------------------------------------------
  call emit_call('K1JT')
  call emit_call('W1AW')
  call emit_call('KA1ABC')
  call emit_call('K1ABC')
  call emit_call('WM8Q')
  call emit_call('K7RIE')
  call emit_call('W5TTA')
  call emit_call('PJ4/KA1ABC')
  call emit_call('KA1ABC/P')
  call emit_call('3DA0XX')
  call emit_call('3XA1AB')
  call emit_call('QU1RK')
  call emit_call('Q1ABC')
  call emit_call('2E0ABC')
  call emit_call('VK9NS')
  call emit_call('A1A')
  call emit_call('KH6ABC')
  call emit_call('E73ABC')
  call emit_call('4U1UN')
  call emit_call('KK7LMI')
  call emit_call('K1J')
  call emit_call('K1JTXYZ')
  call emit_call('12ABC')
  call emit_call('CQ')
  call emit_call('DE')
  call emit_call('QRZ')
  call emit_call('CQ_123')
  call emit_call('CQ_DX')
  call emit_call('CQ_POTA')
  call emit_call('K1ABC1')
  call emit_call('KA12ABC')
  call emit_call('AB1')
  call emit_call('ZZ9ZZZ')
  call emit_call('A0A')
  call emit_call('9A1A')
  call emit_call('RA1AAA')
  call emit_call('TA1ABC')
  call emit_call('G4ABC')
  call emit_call('M0ABC')
  call emit_call('2M0ABC')
  call emit_call('')
  call emit_call('K1.ABC')
  call emit_call('K1A?')

  call emit_n28(0)
  call emit_n28(1)
  call emit_n28(2)
  call emit_n28(3)
  call emit_n28(500)
  call emit_n28(1002)
  call emit_n28(1003)
  call emit_n28(1003 + 27)
  call emit_n28(532443)
  call emit_n28(532444)
  call emit_n28(NTOKENS - 1)
  call emit_n28(NTOKENS)
  call emit_n28(NTOKENS + 12345)
  call emit_n28(NTOKENS + MAX22 - 1)
  call emit_n28(NTOKENS + MAX22)
  call emit_n28(NTOKENS + MAX22 + 1)
  call emit_n28(NTOKENS + MAX22 + 27)
  call emit_n28(NTOKENS + MAX22 + 27*27*27)
  call emit_n28(NTOKENS + MAX22 + 10*27*27*27)
  call emit_n28(NTOKENS + MAX22 + 36*10*27*27*27)
  call emit_n28(NTOKENS + MAX22 + 36*10*27*27*27*36)
  call emit_n28(2**28 - 1)
  do i = 1, 400
     call emit_n28(int(iand(lcg32(), int(2**28 - 1, int64)), int32))
  end do
  do i = 1, 200
     call emit_n28(NTOKENS + MAX22 + int(mod(lcg32(), int(36*10*27*27*27*37, int64)), int32))
  end do

  ! ---- messages: normalise, pack with the reference packer, reassemble ----
  open(11, file='/home/john/js8call-improved/lib/jtty/jtty_msgs.txt', status='old', action='read')
  do
     read(11, '(a)', iostat=ios) line
     if (ios /= 0) exit
     call emit_msg(line)
  end do
  close(11)
  call emit_msg('hello world')
  call emit_msg('CQ k1abc CQ')
  call emit_msg('TEST~TEST')
  call emit_msg('A  B   C')
  call emit_msg('   LEADING AND TRAILING   ')
  call emit_msg('599 001')
  call emit_msg('599 05 NWT')
  call emit_msg('1D EMA')
  call emit_msg('K1JT 599 0004')
  call emit_msg('AGN?')
  call emit_msg('WM8Q: W5TTA SNR?')
  call emit_msg('W5TTA MSG TO: K7RIE HELLO FROM THE JS8 WORLD')
  call emit_msg('#51.02/02.44F9 LO/ -K- XFK')
  call emit_msg('lower case and {braces} <angles> |bars| back\slash ^caret `tick')
  call emit_msg(repeat('0123456789', 9))
  call emit_msg('')
  call emit_msg(' ')
  ! packer tie-breaks and candidate families under the Unknown profile
  call emit_msg('599 FN42')
  call emit_msg('FN42')
  call emit_msg('599 MA')
  call emit_msg('599 CA 599 MA')
  call emit_msg('K1ABC 599 001')
  call emit_msg('CQ K1ABC CQ CQ K1ABC CQ')
  call emit_msg('1D EMA 2A NFL')
  call emit_msg('1D EMAX')
  call emit_msg('TU')
  call emit_msg('QRZ?')
  call emit_msg('599 12345')
  call emit_msg('599 131071')
  call emit_msg('599 131072')
  call emit_msg('0')
  call emit_msg('00')
  call emit_msg('599 0')
  call emit_msg('AGN? AGN? AGN?')
  call emit_msg('K1ABC TU CQ K1ABC CQ')
  call emit_msg('WB9XYZ TU')
  call emit_msg('TU NOW JA6DEF 599 123')
  call emit_msg('QSO B4')
  call emit_msg('QSL TU')
  call emit_msg('OK? NIL? WAIT')
  call emit_msg('599 05 NWT')
  call emit_msg('599 ABC')
  call emit_msg('599 0AB')
  call emit_msg('599 A')
  call emit_msg('5993 FN42')
  call emit_msg('K1JT K1JT K1JT K1JT K1JT K1JT K1JT K1JT K1JT K1JT K1JT K1JT K1JT K1JT K1JT K1JT K1JT')
  call emit_msg('HELLO WORLD THIS IS A LONGER TEXT MESSAGE THAT SPANS MANY FRAMES AND THEN SOME MORE')
  call emit_msg('PJ4/KA1ABC 599 124')
  call emit_msg('CQ PJ4/KA1ABC CQ')
  call emit_msg('TU NOW K1ABC')
  call emit_msg('TU NOW K1ABC AGN?')
  call emit_msg('599 1234 599 1234')
  nchunks = 0
  do i = 1, 64, 5
     line = ALPHA(i:min(i + 4, 64))
     call emit_msg(line)
  end do

  close(10)
  print *, 'wrote vectors3.txt'

contains

  function lcg32() result(v)
    integer(int64) :: v
    rng = mod(rng*48271_int64, 2147483647_int64)
    v = iand(rng, 2147483647_int64)
    rng = mod(rng*48271_int64, 2147483647_int64)
    v = ieor(v, ishft(iand(rng, 1_int64), 31))
  end function lcg32

  subroutine emit(atom, eom)
    type(jtty_source_atom), intent(in) :: atom
    logical, intent(in) :: eom
    character(len=34) :: f
    type(jtty_source_atom) :: out
    logical :: pv, uv, ue, rv
    character(len=80) :: rendered
    write(10, '(a,1x,i0,1x,i0,1x,i0,1x,i0,1x,i0,1x,a,a,a,1x,l1)') 'ATOMIN', atom%kind, &
         atom%subtype, atom%role, atom%value, atom%value2, '@', atom%text, '@', eom
    call pack_jtty_atom(atom, f, eom, pv)
    if (.not. pv) then
       write(10, '(a)') 'ATOMOUT F'
       return
    end if
    call unpack_jtty_atom(f, out, uv, ue)
    rendered = ''
    rv = .false.
    if (uv) call render_jtty_atom(out, rendered, rv)
    write(10, '(a,1x,a,1x,l1,1x,l1,1x,i0,1x,i0,1x,i0,1x,i0,1x,i0,1x,a,a,a,1x,l1,1x,a,a,a)') &
         'ATOMOUT T', f, uv, ue, out%kind, out%subtype, out%role, out%value, out%value2, &
         '@', out%text, '@', rv, '@', trim(rendered), '@'
  end subroutine emit

  subroutine emit_word(f)
    character(len=34), intent(in) :: f
    type(jtty_source_atom) :: out
    logical :: uv, ue, rv
    character(len=80) :: rendered
    call unpack_jtty_atom(f, out, uv, ue)
    if (.not. uv) then
       write(10, '(a,1x,a,1x,a)') 'WORD', f, 'F'
       return
    end if
    rendered = ''
    call render_jtty_atom(out, rendered, rv)
    write(10, '(a,1x,a,1x,a,1x,l1,1x,i0,1x,i0,1x,i0,1x,i0,1x,i0,1x,a,a,a,1x,l1,1x,a,a,a)') &
         'WORD', f, 'T', ue, out%kind, out%subtype, out%role, out%value, out%value2, &
         '@', out%text, '@', rv, '@', trim(rendered), '@'
  end subroutine emit_word

  subroutine emit_hex(n)
    integer(int64), intent(in) :: n
    character(len=34) :: f
    write(f, '(b34.34)') n
    call emit_word(f)
  end subroutine emit_hex

  subroutine emit_call_words(c)
    character(len=*), intent(in) :: c
    character(len=13) :: c13
    integer :: n28, action, i2, n2
    integer(int64) :: n
    c13 = c
    call pack28(c13, n28)
    do action = 0, 5
       if (action <= 3) then
          i2 = 0
          n2 = action
       else
          i2 = 1
          n2 = action - 4
       end if
       n = ishft(int(n28, int64), 4) + 4*n2 + i2
       call emit_hex(ishft(n, 2))
       call emit_hex(ishft(n, 2) + 1)
    end do
    ! the two reserved call forms 1.2 and 1.3 must be invalid
    n = ishft(int(n28, int64), 4) + 4*2 + 1
    call emit_hex(ishft(n, 2))
    n = ishft(int(n28, int64), 4) + 4*3 + 1
    call emit_hex(ishft(n, 2))
  end subroutine emit_call_words

  subroutine emit_call(c)
    character(len=*), intent(in) :: c
    character(len=13) :: c13, work, unp
    integer :: n28
    logical :: success, std
    c13 = c
    work = c13
    call pack28(work, n28)
    call unpack28(n28, unp, success)
    std = jtty_standard_call(c13)
    write(10, '(a,1x,a,a,a,1x,i0,1x,a,a,a,1x,l1,1x,l1)') 'CALL', '@', c13, '@', n28, &
         '@', unp, '@', success, std
  end subroutine emit_call

  subroutine emit_n28(n)
    integer, intent(in) :: n
    character(len=13) :: unp
    logical :: success
    call unpack28(n, unp, success)
    write(10, '(a,1x,i0,1x,a,a,a,1x,l1)') 'N28', n, '@', unp, '@', success
  end subroutine emit_n28

  subroutine emit_msg(raw)
    character(len=*), intent(in) :: raw
    character(len=80) :: msg80, work, norm, decoded
    logical :: tsep, last, svalid
    integer :: n, j
    msg80 = raw
    call normalize_jtty_message(msg80, norm)
    write(10, '(a,1x,a,a,a)') 'MSGIN', '@', msg80, '@'
    write(10, '(a,1x,a,a,a)') 'MSGNORM', '@', norm, '@'
    work = msg80
    call pack_jtty(work, c32, n)
    if (n < 1) then
       write(10, '(a,1x,i0)') 'ASM', n
       return
    end if
    call emit_asm(c32, n)
    if (n >= 2) then
       c32(2)(33:33) = '1'                  ! reserved bit set on frame 2
       call emit_asm(c32, n)
       c32(2)(33:33) = '0'
       c32(1) = repeat('0', 34)             ! all-zero sentinel as frame 1
       call emit_asm(c32, n)
    end if
    if (n >= 1 .and. n < MAX_FRAMES) then
       c32(n)(34:34) = '0'                  ! clear EOM on the last frame
       call emit_asm(c32, n)
    end if
  end subroutine emit_msg

  subroutine emit_asm(frames, n)
    character(len=34), intent(in) :: frames(MAX_FRAMES)
    integer, intent(in) :: n
    character(len=80) :: decoded
    logical :: tsep, last, svalid
    integer :: j
    write(10, '(a,1x,i0,*(1x,a))') 'ASM', n, (frames(j), j = 1, n)
    call unpack_jtty(frames, n, decoded, tsep, last, svalid)
    write(10, '(a,1x,a,a,a,1x,l1,1x,l1,1x,l1)') 'ASMOUT', '@', decoded, '@', tsep, last, svalid
  end subroutine emit_asm

end program gen_vectors3
