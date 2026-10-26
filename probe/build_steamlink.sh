#!/bin/bash
# Builds the probe for the Steam Link and packs it as a Steam Link app:
# steamlink/apps/chupathingyce-probe.tgz, which the box installs from a USB
# drive. Needs Valve's SDK (MARVELL_SDK_PATH, default ../steamlink-sdk) and
# SDL3 built with it (SDL3_PREFIX, default ../build/sdl3).
set -euo pipefail

TOP=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "${TOP}/.." && pwd)
export MARVELL_SDK_PATH="${MARVELL_SDK_PATH:-${ROOT}/steamlink-sdk}"
SDL3_PREFIX="${SDL3_PREFIX:-${ROOT}/build/sdl3}"

set +u
source "${MARVELL_SDK_PATH}/setenv.sh"
set -u

OUT="${ROOT}/build/probe"
mkdir -p "${OUT}"
# shellcheck disable=SC2086 # CC is the SDK's compiler and its flags
${CC} -std=gnu11 -O2 -Wall -marm -mfloat-abi=hard -mfpu=neon \
	-I"${SDL3_PREFIX}/include" "${TOP}/probe.c" -o "${OUT}/probe" \
	-L"${SDL3_PREFIX}/lib" -Wl,-Bstatic -lSDL3 -Wl,-Bdynamic -lm -ldl -lpthread -lrt
${STRIP} "${OUT}/probe"

APP="${ROOT}/steamlink/apps/chupathingyce-probe"
rm -rf "${APP}"
mkdir -p "${APP}"
cp "${OUT}/probe" "${APP}/probe"
cp "${ROOT}/art/icon.png" "${APP}/icon.png"
cat >"${APP}/toc.txt" <<__EOF__
name=ChupathingyCE probe
icon=icon.png
run=probe
__EOF__

cd "$(dirname "${APP}")"
tar zcf chupathingyce-probe.tgz chupathingyce-probe
rm -rf chupathingyce-probe
echo "put the steamlink folder on a usb drive, plug it into the steam link and power cycle it"
