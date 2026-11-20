#!/bin/bash
# build-mft.sh - build the DroidVM virtio-media MFT, both directions.
#
# Runs on the Linux build server (<build-server>), llvm-mingw in /opt/llvm-mingw.
# No WSL, no Windows machine involved.
#
#   SRC=/srv/mft-fix bash tools/build-mft.sh                 # arm64 + x86, decoder + encoder
#   MODES=decoder ARCH=arm64 SRC=/srv/mft-fix bash tools/build-mft.sh
#
# MFT_BUILD_MODE is the compile-time switch:
#   decoder -> mft_h264_decoder_<arch>.dll   H.264 -> NV12   (working decode path)
#   encoder -> mft_h264_encoder_<arch>.dll   NV12 -> H.264   (IMFTransform/ICodecAPI skeleton)
#
# Output: $OUT/mft_h264_{decoder,encoder}_<arch>.dll, build-<mode>-<arch>.log,
#         the two ARM64 enum probes, the native h264_bits unit test, and
#         $OUT/pe-summary.txt.
#
# There is deliberately no .o/.d incremental build here: every DLL is compiled
# from all of its sources in one clang invocation, so the UMD ".d stale target"
# trap (AGENT-ENVIRONMENT 15) cannot apply.
set -uo pipefail

export PATH=/opt/llvm-mingw/bin:$PATH

SRC=${SRC:-/srv/mft-fix}
# Canonical vmedia_client/ABI source is kept at the build root and supplied to
# both MFT modes; the historical mft/ copies are not compiled.
CLIENT_ROOT=${CLIENT_ROOT:-$SRC}
OUT=${OUT:-$SRC/out}
MODES=${MODES:-"decoder encoder"}
ARCHES=${ARCH:-"arm64 x86"}
FAIL=0
mkdir -p "$OUT"

CFLAGS="-O2 -std=c99 -Wall -Wextra -Wno-unused-parameter \
        -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -fno-strict-aliasing"
LIBS="-lmfplat -lmfuuid -lole32 -luuid -ladvapi32"

prefix_for() {
  case "$1" in
    arm64)   echo "aarch64-w64-mingw32" ;;
    x86)     echo "i686-w64-mingw32" ;;
    arm64ec) echo "arm64ec-w64-mingw32" ;;
    *)       return 1 ;;
  esac
}

sources_for() {
  case "$1" in
    decoder)
      echo "mft/h264_bits.c mft/droidvm_mft_log.c mft/droidvm_mft_object.c \
            mft/droidvm_mft_class.c mft/droidvm_mft_reg.c $CLIENT_ROOT/vmedia_client.c" ;;
    encoder)
      echo "mft/droidvm_mft_log.c mft/droidvm_mft_encoder.c \
            mft/droidvm_mft_class.c mft/droidvm_mft_reg.c $CLIENT_ROOT/vmedia_client.c" ;;
  esac
}

extra_for() {
  case "$1" in
    decoder|encoder) echo "-DVMEDIA_CLIENT_STATIC" ;;
    *)       echo "" ;;
  esac
}

build_one() {
  mode=$1
  arch=$2
  prefix=$(prefix_for "$arch") || { echo "unknown arch $arch"; return 1; }
  dll="$OUT/mft_h264_${mode}_${arch}.dll"
  log="$OUT/build-${mode}-${arch}.log"
  srcs=$(sources_for "$mode")
  extra=$(extra_for "$mode")

  echo "=== $mode / $arch ($prefix) ==="
  rm -f "$dll"
  {
    echo "# MFT_BUILD_MODE=$mode arch=$arch"
    "$prefix-clang" --version | head -2
    echo "# command:"
    echo "$prefix-clang -shared -o $dll $CFLAGS -DMFT_BUILD_MODE=$mode $extra -I$SRC/mft <sources> mft_h264_decoder.def $LIBS -Wl,--no-insert-timestamp"
    echo
  } > "$log"

  # shellcheck disable=SC2086
  "$prefix-clang" -shared -o "$dll" \
      $CFLAGS -DMFT_BUILD_MODE="$mode" $extra \
      -I"$CLIENT_ROOT" -I"$SRC/mft" \
      $srcs \
      "$SRC/mft/mft_h264_decoder.def" \
      $LIBS -Wl,--no-insert-timestamp >> "$log" 2>&1
  rc=$?
  errors=$(grep -c 'error:' "$log")
  warnings=$(grep -c 'warning:' "$log")
  echo "  CC_RC=$rc errors=$errors warnings=$warnings"
  grep -E 'error:|undefined reference|multiple definition' "$log" | head -30
  if [ "$rc" -ne 0 ] || [ "$errors" -ne 0 ]; then
    FAIL=1
    return 1
  fi
  if [ -f "$dll" ]; then
    ls -l "$dll"
    sha256sum "$dll"
    llvm-readobj --file-headers "$dll" |
      grep -E 'Format:|Arch:|Machine:'
    echo "  exports:"
    llvm-readobj --coff-exports "$dll" |
      grep -E 'Name:' | sort -u
    echo "  vendor-string-count: $(strings -el "$dll" | grep -c 'VEN_1AF4')"
  fi
  return 0
}

for mode in $MODES; do
  for arch in $ARCHES; do
    build_one "$mode" "$arch" || true
  done
done

# ---- enum probes (compile only; they run on the guest next session) ---------
build_probe() {
  mode=$1
  extra_def=""
  out="$OUT/mft_enum_probe_arm64.exe"
  if [ "$mode" = encoder ]; then
    extra_def="-DPROBE_MODE_ENCODER=1"
    out="$OUT/mft_enum_probe_encoder_arm64.exe"
  fi
  log="$OUT/build-probe-${mode}.log"
  {
    echo "# enum probe ($mode, arm64): MFTEnumEx flag matrix + activation"
    aarch64-w64-mingw32-clang --version | head -1
    echo "# command: aarch64-w64-mingw32-clang -O2 -std=c99 -Wall -Wextra $extra_def -o $out $SRC/tests/mft_enum_probe.c -lmfplat -lmfuuid -lole32 -luuid"
  } > "$log"
  aarch64-w64-mingw32-clang -O2 -std=c99 -Wall -Wextra \
      -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 $extra_def \
      -o "$out" "$SRC/tests/mft_enum_probe.c" \
      -lmfplat -lmfuuid -lole32 -luuid -Wl,--no-insert-timestamp >> "$log" 2>&1
  probe_rc=$?
  echo "  PROBE[$mode]_RC=$probe_rc errors=$(grep -c 'error:' "$log") warnings=$(grep -c 'warning:' "$log")"
  grep -E 'error:|warning:' "$log" | head -10
  [ "$probe_rc" -ne 0 ] && FAIL=1
  [ -f "$out" ] && ls -l "$out" && sha256sum "$out"
}

for mode in decoder encoder; do
  build_probe "$mode"
done

# ---- native h264_bits unit test (host gcc, Linux only) ----------------------
{
  echo "# host gcc build of the Linux-native unit test"
  gcc --version | head -1
  echo "# command: gcc -O2 -Wall -Wextra -I$SRC/mft -o $OUT/h264_bits_test $SRC/tests/h264_bits_test.c $SRC/mft/h264_bits.c"
} > "$OUT/unit-test-build.log"
gcc -O2 -Wall -Wextra -I"$CLIENT_ROOT" -I"$SRC/mft" -o "$OUT/h264_bits_test" \
    "$SRC/tests/h264_bits_test.c" "$SRC/mft/h264_bits.c" \
    >> "$OUT/unit-test-build.log" 2>&1
test_rc=$?
echo "UNIT_BUILD_RC=$test_rc errors=$(grep -c 'error:' "$OUT/unit-test-build.log") warnings=$(grep -c 'warning:' "$OUT/unit-test-build.log")"
[ "$test_rc" -ne 0 ] && FAIL=1

for stream in "input-1280x720:1280:720" "gen-640x360:640:360" "gen-1920x1080:1920:1080"; do
  name=${stream%%:*}
  rest=${stream#*:}
  w=${rest%%:*}
  h=${rest##*:}
  if [ -f "$SRC/tests/$name.h264" ]; then
    echo "--- $name (${w}x${h}) ---"
    "$OUT/h264_bits_test" "$SRC/tests/$name.h264" "$w" "$h" | tail -3
  fi
done

# ---- PE / export / string summary ------------------------------------------
{
  echo "# PE summary, $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  for f in "$OUT"/mft_h264_*.dll "$OUT"/mft_enum_probe*.exe; do
    [ -f "$f" ] || continue
    echo "== $(basename "$f") $(stat -c%s "$f") B $(sha256sum "$f" | cut -d' ' -f1)"
    llvm-readobj --file-headers "$f" | grep -E 'Format:|Arch:|Machine:|Characteristics'
    llvm-readobj --coff-exports "$f" 2>/dev/null | grep -E 'Name:' | sort -u
    echo "   vendor-string=$(strings -el "$f" | grep -c 'VEN_1AF4') \
log-string=$(strings "$f" | grep -c 'DROIDVM_MFT_LOG') \
vmedia-string=$(strings "$f" | grep -c 'VMediaDecoder')"
  done
} > "$OUT/pe-summary.txt" 2>&1
cat "$OUT/pe-summary.txt"

echo
echo "FINAL_RC=$FAIL"
exit "$FAIL"
