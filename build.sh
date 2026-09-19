#!/usr/bin/env bash
# Build FlarialZoom (LeviLauncher native mod) for arm64-v8a.
#
# Requirements:
#   - clang-17 / lld-17 (NDK r28c's libc++ needs clang >= 16)
#   - Android NDK r28c sysroot extracted to $NDK_SYSROOT
#
# Usage:
#   NDK_SYSROOT=/path/to/sysroot ./build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${ROOT}/build"

_user_cc="${CC:-}"
_user_cxx="${CXX:-}"
CC="${CC:-clang-17}"
CXX="${CXX:-clang++-17}"
STRIP="${STRIP:-llvm-strip-17}"

# --- prefer the compiler payload shipped with the kit (if extracted) ---------
# tools/setup_toolchain.sh unpacks it to <kit>/toolchain/clang17/. Using it
# needs no env vars and no system clang: the binaries' RUNPATH is $ORIGIN/../lib,
# so they find their own LLVM libraries next to themselves.
# An explicit CC/CXX in the environment always wins.
KIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The toolchain may sit beside this project (handoff kit) or inside a sibling kit
# in the workspace (working copy). Try each; first hit wins.
TOOLCHAIN_DIR=""
for cand in "$KIT_ROOT/toolchain" \
            "$KIT_ROOT/BetterZoom-DevKit/toolchain" \
            "$KIT_ROOT/GlassChunkBorder-DevKit/toolchain" \
            "$KIT_ROOT/../BetterZoom-DevKit/toolchain" \
            "$KIT_ROOT/../GlassChunkBorder-DevKit/toolchain"; do
    if [ -x "${cand}/clang17/bin/clang-17" ] || [ -d "${cand}/ndk-sysroot/usr/include" ]; then
        TOOLCHAIN_DIR="$cand"; break
    fi
done
[ -n "$TOOLCHAIN_DIR" ] || TOOLCHAIN_DIR="$KIT_ROOT/toolchain"
BUNDLED_CC="${TOOLCHAIN_DIR}/clang17/bin"
# platform-aware install hints (bionic uses pkg, glibc uses apt)
if [ -f "${KIT_ROOT:-.}/tools/lib/env.sh" ]; then . "${KIT_ROOT:-.}/tools/lib/env.sh"
elif [ -f "$(dirname "${BASH_SOURCE[0]}")/lib/env.sh" ]; then . "$(dirname "${BASH_SOURCE[0]}")/lib/env.sh"
fi
command -v kit_hint_clang >/dev/null 2>&1 || kit_hint_clang() { echo "apt-get install -y clang-17 lld-17 llvm-17"; }
if [ -x "${BUNDLED_CC}/clang-17" ] && [ -z "$_user_cxx" ] && ! command -v "$CXX" >/dev/null 2>&1; then
    if "${BUNDLED_CC}/clang-17" --version >/dev/null 2>&1; then
        echo "note: using the bundled compiler in ${BUNDLED_CC}" >&2
        CC="${BUNDLED_CC}/clang-17"
        CXX="${BUNDLED_CC}/clang++-17"
        STRIP="${BUNDLED_CC}/llvm-strip-17"
    else
        echo "warning: bundled compiler at ${BUNDLED_CC} cannot run here;" >&2
        echo "         check: ldd ${BUNDLED_CC}/clang-17 | grep 'not found'" >&2
        if [ -e /system/bin/linker64 ] || [ "$(uname -o 2>/dev/null)" = "Android" ]; then
            echo "         (this is Android/bionic: the bundled clang is a glibc binary;" >&2
            echo "          use a system clang >= 16 -> export CC=clang CXX=clang++ STRIP=llvm-strip)" >&2
        fi
    fi
fi

# --- resolve the compiler, and FAIL LOUDLY if it is missing or too old -------
# The old code fell back to bare "clang++"/"clang" whenever the versioned name
# was absent. On a machine whose PATH clang is old (e.g. Ubuntu's clang 14) that
# silently selects an incompatible compiler, and NDK r28c's libc++ then dies
# DEEP INSIDE ITS OWN HEADERS with messages like
#     error: 'std' is not a class, namespace, or enumeration
# which looks like a broken project instead of a wrong toolchain.
for spec in CC CXX STRIP; do
    eval "val=\$$spec"
    if command -v "$val" >/dev/null 2>&1; then continue; fi
    base="${val%-17}"                       # clang-17 -> clang
    if [ "$base" != "$val" ] && command -v "$base" >/dev/null 2>&1; then
        echo "note: '$val' not found, using '$base'" >&2
        eval "$spec=$base"
    elif [ "$spec" = STRIP ]; then
        echo "warning: no strip tool ('$val' / '$base') -> the .so keeps its debug info (~7MB)" >&2
    else
        echo "error: $spec='$val' not found (and '$base' is missing too)." >&2
        echo "       Need clang-17/clang++-17. Options: $(kit_hint_clang)," >&2
        echo "       or the NDK r28 toolchain, or an already-prepared compiler payload." >&2
        echo "       See docs/15-换机器与容器移植.md and tools/kit_doctor.sh" >&2
        exit 1
    fi
done

# libc++ in NDK r28c requires clang >= 16. Check it instead of producing
# hundreds of in-header errors.
cxx_ver="$("$CXX" -dumpversion 2>/dev/null | cut -d. -f1 || true)"
case "$cxx_ver" in
    ''|*[!0-9]*) echo "warning: cannot determine the version of $CXX" >&2 ;;
    *) if [ "$cxx_ver" -lt 16 ]; then
           echo "error: $CXX is clang $cxx_ver, but NDK r28c's libc++ requires >= 16." >&2
           echo "       Continuing would fail inside libc++ headers with" >&2
           echo "       \"'std' is not a class, namespace, or enumeration\"." >&2
           exit 1
       fi ;;
esac

NDK_SYSROOT="${NDK_SYSROOT:-${ANDROID_NDK_SYSROOT:-}}"
# fall back to the sysroot the kit ships (tools/setup_toolchain.sh unpacks it)
if [ -z "$NDK_SYSROOT" ] && [ -d "${TOOLCHAIN_DIR}/ndk-sysroot/usr/include" ]; then
    NDK_SYSROOT="${TOOLCHAIN_DIR}/ndk-sysroot"
    echo "note: using the bundled sysroot ${NDK_SYSROOT}" >&2
fi
if [ -z "$NDK_SYSROOT" ] || [ ! -d "$NDK_SYSROOT/usr/include" ]; then
    echo "error: NDK sysroot not found." >&2
    echo "       Run: bash ${KIT_ROOT}/tools/setup_toolchain.sh" >&2
    echo "       Or set NDK_SYSROOT=/path/to/ndk/sysroot" >&2
    exit 1
fi

API="${API:-26}"
SYSROOT_INC="$NDK_SYSROOT/usr/include"
SYSROOT_LIB="$NDK_SYSROOT/usr/lib/aarch64-linux-android/$API"
SYSROOT_LIB_TOP="$NDK_SYSROOT/usr/lib/aarch64-linux-android"
LIBCXX_INC="$NDK_SYSROOT/usr/include/c++/v1"
ARCH_INC="$NDK_SYSROOT/usr/include/aarch64-linux-android"
RT_LIB="$NDK_SYSROOT/libclang_rt.builtins-aarch64-android.a"
UNWIND_LIB="$NDK_SYSROOT/libunwind.a"

TARGET="aarch64-linux-android${API}"

CXXFLAGS=(
    --target="$TARGET"
    --sysroot="$NDK_SYSROOT"
    -std=c++20
    -fPIC
    -Os
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-rtti
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -fno-stack-protector
    -fmerge-all-constants
    -fno-exceptions
    -DJSON_NOEXCEPTION
    -ffunction-sections
    -fdata-sections
    -w
    -isystem"${LIBCXX_INC}"
    -isystem"${ARCH_INC}"
    -isystem"${SYSROOT_INC}"
    -I"${ROOT}/include"
    -I"${ROOT}/src"
    -I"${ROOT}/third_party" -I"${ROOT}/third_party/fmt"
    -DFMT_HEADER_ONLY
)

LDFLAGS=(
    --target="$TARGET"
    -shared
    -nostdlib
    -Wl,-z,max-page-size=16384
    -Wl,--hash-style=gnu
    -Wl,--gc-sections
    -Wl,--icf=all
    -Wl,--exclude-libs,ALL
    -L"${SYSROOT_LIB}"
    -L"${SYSROOT_LIB_TOP}"
    -L"${ROOT}/lib"
    -Wl,--no-as-needed -l:libpreloader-1.5.16.so -Wl,--as-needed
    "${UNWIND_LIB}"
)

mkdir -p "$OUT"
echo "==> compiling with $CXX (target $TARGET)"
$CXX "${CXXFLAGS[@]}" -c "${ROOT}/src/main.cpp"          -o "$OUT/main.o"
$CXX "${CXXFLAGS[@]}" -c "${ROOT}/src/ZoomConfig.cpp"    -o "$OUT/ZoomConfig.o"
$CXX "${CXXFLAGS[@]}" -c "${ROOT}/src/ZoomMod.cpp"       -o "$OUT/ZoomMod.o"

echo "==> linking"
$CXX "${LDFLAGS[@]}" \
    "${SYSROOT_LIB}/crtbegin_so.o" \
    "$OUT/main.o" "$OUT/ZoomConfig.o" "$OUT/ZoomMod.o" \
    -lc++_static -lc++abi -ldl -llog -lm -lc \
    "${RT_LIB}" \
    "${SYSROOT_LIB}/crtend_so.o" \
    -o "$OUT/libbetterzoom.so"

# Strip debug info + symbols to match the shipped release build (the .so is
# ~7 MB unstripped vs ~0.5 MB stripped). STRIP was resolved at the top.
if command -v "$STRIP" >/dev/null 2>&1; then
    "$STRIP" --strip-all "$OUT/libbetterzoom.so"
fi

echo "==> done: $OUT/libbetterzoom.so"
# `file` is not present in every minimal container - do not fail on its absence.
if command -v file >/dev/null 2>&1; then
    file "$OUT/libbetterzoom.so"
else
    printf '    %s bytes\n' "$(stat -c%s "$OUT/libbetterzoom.so" 2>/dev/null || echo '?')"
fi
