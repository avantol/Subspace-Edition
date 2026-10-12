! Scratchpad-only: the FFTW3 Fortran 2003 interface as a module named
! fftw3, which is what lib/jtty/jtty_mdecode.f90 and lib/ft2_deps/four2a.f90
! expect. Never part of the project build.
module fftw3
  use, intrinsic :: iso_c_binding
  include 'fftw3.f03'
end module fftw3
