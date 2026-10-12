#!/bin/sh
# Offline bit-exact gate for the JTTY TBCC FEC port (Stage 1 of the JTTY port).
# Run from this directory:
#   ./build.sh && ./jtty_tbcc_test
#
# vectors/jtty_tbcc_vectors.txt was written by the reference Fortran in
# lib/jtty compiled STANDALONE (see vectors/README.md). This build never
# touches Fortran; the project build never touches this directory.
# -ffp-contract=off keeps float->double promotion identical to the reference
# build, so metrics are compared to the last IEEE bit.
set -e
cd "$(dirname "$0")"

FLAGS="-std=c++20 -O2 -Wall -Wextra -ffp-contract=off -fno-tree-vectorize -DJS8_ENABLE_JTTY=1 -I.."

g++ $FLAGS \
    jtty_tbcc_test.cpp \
    ../JS8_Mode/jtty_tbcc.cpp \
    -o jtty_tbcc_test
echo "built: ./jtty_tbcc_test"

# Stage 2: modem / sync layer. Needs FFTW3 (single precision), as the
# application does, for ana64a and subtractJtty.
g++ $FLAGS \
    jtty_modem_test.cpp \
    ../JS8_Mode/jtty_modem.cpp \
    ../JS8_Mode/jtty_tbcc.cpp \
    $(pkg-config --cflags --libs fftw3f) \
    -o jtty_modem_test
echo "built: ./jtty_modem_test"

# Stage 3: source grammar, call28 codec, message assembly; Stage 5's text
# packer and frame generator are gated here too (the generator encodes).
g++ $FLAGS \
    jtty_source_test.cpp \
    ../JS8_Mode/jtty_source.cpp \
    ../JS8_Mode/jtty_tbcc.cpp \
    -o jtty_source_test
echo "built: ./jtty_source_test"

# Stage 4: the receiver (decode orchestrator + stepping driver), end to end
# on synthetic audio. Outcome gate, see the test header.
g++ $FLAGS \
    jtty_receiver_test.cpp \
    ../JS8_Mode/jtty_receiver.cpp \
    ../JS8_Mode/jtty_modem.cpp \
    ../JS8_Mode/jtty_source.cpp \
    ../JS8_Mode/jtty_tbcc.cpp \
    $(pkg-config --cflags --libs fftw3f) \
    -o jtty_receiver_test
echo "built: ./jtty_receiver_test"

# Diagnostic tool, not a gate: the receiver over a raw 12 kHz int16 capture
# (parecord --raw), driven as the application drives it. See its header.
g++ $FLAGS \
    jtty_rx_file.cpp \
    ../JS8_Mode/jtty_receiver.cpp \
    ../JS8_Mode/jtty_modem.cpp \
    ../JS8_Mode/jtty_source.cpp \
    ../JS8_Mode/jtty_tbcc.cpp \
    $(pkg-config --cflags --libs fftw3f) \
    -o jtty_rx_file
echo "built: ./jtty_rx_file"
