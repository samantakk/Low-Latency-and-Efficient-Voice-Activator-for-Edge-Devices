#!/bin/sh
# Host unit tests for the ring buffer and sample conversions (no ESP32 needed).
# Usage: ./run_tests.sh      (from anywhere; needs gcc or clang)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE="$HERE/../../components/sih_audio_core"
CC=${CC:-cc}
mkdir -p "$HERE/build"
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -pthread -D_DEFAULT_SOURCE \
    -I"$CORE/include" "$HERE/test_ring_buffer.c" "$CORE/ring_buffer.c" \
    -o "$HERE/build/test_ring_buffer"
"$HERE/build/test_ring_buffer"
