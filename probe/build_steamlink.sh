#!/bin/bash
# the probe for the steam link, packed as a steam link app:
# steamlink/apps/chupathingyce-probe.tgz, which the box installs off a usb
# drive. needs valve's sdk (MARVELL_SDK_PATH, ../steamlink-sdk by default)
# and SDL3 built w it (SDL3_PREFIX, ../build/sdl3 by default: what
# .github/workflows/probe.yml builds)
set -euo pipefail

TOP=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "${TOP}/.." && pwd)
export MARVELL_SDK_PATH="${MARVELL_SDK_PATH:-${ROOT}/steamlink-sdk}"
SDL3_PREFIX="${SDL3_PREFIX:-${ROOT}/build/sdl3}"

# (setenv.sh reads variables it never set)
set +u
source "${MARVELL_SDK_PATH}/setenv.sh"
set -u

OUT="${ROOT}/build/probe"
mkdir -p "${OUT}"
# SDL3 static, so the app is the one file, and what SDL3 links in turn
# from its sdl3.pc. no sysroot for that one: SDL3_PREFIX isn't in the sdk's
# root filesystem. its -L comes first, so -lSDL3 is its libSDL3.a and not
# valve's libSDL3.so
SDL3_FLAGS=$(PKG_CONFIG_SYSROOT_DIR="" PKG_CONFIG_LIBDIR="${SDL3_PREFIX}/lib/pkgconfig" pkg-config --static --cflags --libs sdl3)
# CC is the sdk's compiler and its flags, split on purpose
# shellcheck disable=SC2086
${CC} -std=gnu11 -O2 -Wall -marm -mfloat-abi=hard -mfpu=neon \
	"${TOP}/probe.c" -o "${OUT}/probe" ${SDL3_FLAGS}
${STRIP} "${OUT}/probe"
# (a probe that wants valve's SDL3 is a broken one: built w other headers)
if ${CROSS}readelf -d "${OUT}/probe" | grep -q 'NEEDED.*libSDL3'; then
	echo "the probe links a shared SDL3, not the static one" >&2
	exit 1
fi

APP="${ROOT}/steamlink/apps/chupathingyce-probe"
rm -rf "${APP}"
mkdir -p "${APP}"
cp "${OUT}/probe" "${APP}/probe"
cp "${ROOT}/art/icon.png" "${APP}/icon.png"
# the app as the box lists it (valve's examples/*/build_steamlink.sh)
cat >"${APP}/toc.txt" <<__EOF__
name=ChupathingyCE probe
icon=icon.png
run=probe
__EOF__

cd "$(dirname "${APP}")"
tar zcf chupathingyce-probe.tgz chupathingyce-probe
rm -rf chupathingyce-probe
echo "put the steamlink folder on a usb drive, plug it into the steam link and power cycle it"
