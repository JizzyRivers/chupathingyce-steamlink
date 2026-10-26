#!/bin/bash
# the probe for the steam link, packed as a steam link app:
# steamlink/apps/chupathingyce-probe.tgz, which the box installs off a usb
# drive. needs valve's sdk (MARVELL_SDK_PATH, ../steamlink-sdk by default)
# w SDL3 built and installed into its root filesystem, the way
# .github/workflows/probe.yml does it
set -euo pipefail

TOP=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "${TOP}/.." && pwd)
export MARVELL_SDK_PATH="${MARVELL_SDK_PATH:-${ROOT}/steamlink-sdk}"

# (setenv.sh reads variables it never set)
set +u
source "${MARVELL_SDK_PATH}/setenv.sh"
set -u

OUT="${ROOT}/build/probe"
mkdir -p "${OUT}"
# SDL3 static, so the app is the one file, and what SDL3 links in turn
# (pulseaudio, its libraries off the box) from its sdl3.pc
SDL3_FLAGS=$(pkg-config --static --cflags --libs sdl3)
# CC is the sdk's compiler and its flags, split on purpose
# shellcheck disable=SC2086
${CC} -std=gnu11 -O2 -Wall -marm -mfloat-abi=hard -mfpu=neon \
	"${TOP}/probe.c" -o "${OUT}/probe" ${SDL3_FLAGS}
${STRIP} "${OUT}/probe"

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
