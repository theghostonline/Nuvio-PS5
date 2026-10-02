#!/usr/bin/env bash
# =============================================================================
# scripts/setup-native-app-deps.sh - native-app build-tail dependency bootstrap.
#
# The host converter (tools/native-app/native_app_builder.cpp + self_container)
# links against static zlib. The PS5 payload SDK it also needs is already in the
# dev image at $PS5_PAYLOAD_SDK, so - unlike the upstream boilerplate bootstrap
# - this only has to provide zlib.
#
# Order of preference:
#   1. image-baked static zlib at $EVO_NATIVE_ZLIB   (Dockerfile, fast path)
#   2. SHA-pinned source build into .deps/native-app/zlib/root  (cached)
#
# Diagnostics go to stderr. stdout is exactly:
#   ZLIB_INCLUDE=<dir>
#   ZLIB_ARCHIVE=<path to libz.a>
# so callers can `eval "$(scripts/setup-native-app-deps.sh)"`.
# =============================================================================
set -euo pipefail

ZLIB_VERSION="1.3.2"
ZLIB_SHA256="bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"
ZLIB_URL="https://zlib.net/fossils/zlib-${ZLIB_VERSION}.tar.gz"

log() { echo "==> [native-app-deps] $*" >&2; }

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- 1. image-baked ---------------------------------------------------------
if [[ -n "${EVO_NATIVE_ZLIB:-}" && -f "${EVO_NATIVE_ZLIB}/lib/libz.a" \
      && -f "${EVO_NATIVE_ZLIB}/include/zlib.h" ]]; then
    log "using image-baked static zlib at ${EVO_NATIVE_ZLIB}"
    echo "ZLIB_INCLUDE=${EVO_NATIVE_ZLIB}/include"
    echo "ZLIB_ARCHIVE=${EVO_NATIVE_ZLIB}/lib/libz.a"
    exit 0
fi

# ---- 2. cached source build ----------------------------------------------------
for c in wget tar sha256sum make clang; do
    command -v "$c" >/dev/null 2>&1 || { echo "missing required command: $c" >&2; exit 2; }
done

cache="${repo_root}/.deps/native-app/zlib"
root="${cache}/root"
src="${cache}/zlib-${ZLIB_VERSION}"
archive="${cache}/zlib-${ZLIB_VERSION}.tar.gz"
stamp="${root}/.source-version"

existing="$(find "${root}" -type f -name libz.a -print -quit 2>/dev/null || true)"
if [[ -n "${existing}" && -f "${root}/usr/include/zlib.h" \
      && -f "${stamp}" && "$(<"${stamp}")" == "${ZLIB_VERSION}" ]]; then
    log "using cached static zlib ${ZLIB_VERSION} (${existing})"
    echo "ZLIB_INCLUDE=${root}/usr/include"
    echo "ZLIB_ARCHIVE=${existing}"
    exit 0
fi

log "building pinned static zlib ${ZLIB_VERSION} from source"
mkdir -p "${cache}"

if [[ -f "${archive}" ]] \
   && ! printf '%s  %s\n' "${ZLIB_SHA256}" "${archive}" | sha256sum --check --strict >/dev/null 2>&1; then
    rm -f -- "${archive}"
fi
if [[ ! -f "${archive}" ]]; then
    wget -q "${ZLIB_URL}" -O "${archive}.download"
    mv "${archive}.download" "${archive}"
fi
printf '%s  %s\n' "${ZLIB_SHA256}" "${archive}" | sha256sum --check --strict >/dev/null

rm -rf -- "${src}" "${root}"
tar -xzf "${archive}" -C "${cache}"
mkdir -p "${root}"

ar_tool="$(command -v llvm-ar || command -v ar)"
ranlib_tool="$(command -v llvm-ranlib || command -v ranlib)"
(
    cd "${src}"
    CC=clang AR="${ar_tool}" RANLIB="${ranlib_tool}" ./configure --static --prefix=/usr
    make -j"$(nproc 2>/dev/null || echo 2)" CC=clang AR="${ar_tool}" RANLIB="${ranlib_tool}"
    make DESTDIR="${root}" install
) >"${cache}/build.log" 2>&1
printf '%s\n' "${ZLIB_VERSION}" >"${stamp}"

built="$(find "${root}" -type f -name libz.a -print -quit)"
[[ -n "${built}" ]] || { echo "zlib build produced no libz.a (see ${cache}/build.log)" >&2; exit 2; }
log "built ${built}"
echo "ZLIB_INCLUDE=${root}/usr/include"
echo "ZLIB_ARCHIVE=${built}"
