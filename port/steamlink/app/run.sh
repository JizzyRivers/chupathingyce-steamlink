#!/bin/sh
# the game as the steam link launches an app (shell.sh, with its interface
# closed). the box has room for the game but not its maps, so everything big
# goes on a usb stick: chupathingyce/ beside the disc image, the maps, the
# cache, the saves and run.log. the firmware mounts no stick for an app, so
# it is mounted here. with no stick, /mnt/scratch/chupathingyce
app=$(cd "$(dirname "$0")" && pwd)
# valve's shell takes wired pads from the kernel's driver and can leave them
# without it, where the game's sdl never sees them: given back here (the
# driver turns down whatever is not a pad)
for interface in /sys/bus/usb/devices/*:1.0; do
	[ -e "$interface/driver" ] || echo "$(basename "$interface")" > /sys/bus/usb/drivers/xpad/bind 2>/dev/null
done
stick=/mnt/chupathingyce-usb
mounted=
mkdir -p "$stick"
for device in /dev/block/sd[a-z][0-9]* /dev/block/sd[a-z]; do
	[ -b "$device" ] || continue
	mount -o noatime "$device" "$stick" 2>/dev/null || continue
	for found in "$stick"/chupathingyce "$stick"/*.iso "$stick"/*.ISO "$stick"/*.xiso "$stick"/*.XISO; do
		[ -e "$found" ] && mounted=1
	done
	[ -n "$mounted" ] && break
	umount "$stick"
done
if [ -n "$mounted" ]; then
	data=$stick/chupathingyce
else
	data=/mnt/scratch/chupathingyce
fi
mkdir -p "$data"
export HALO_DATA_ROOT="$data"
export HALO_SAVE_ROOT="$data/save"
# the shader compiler's load-time constants, which its driver worked out again
# at every draw: a sixth of a frame at bloodgulch's spawn
export VC_ENABLE_LTC=0
# the box has no certificate authorities of its own, for the game list's https
export SSL_CERT_FILE="$app/ca-certificates.crt"
# (with the app, which an update replaces, not with the maps on the stick)
export HALO_NET_BROKERS_FILE="$app/brokers.txt"
cd "$app"
[ -f launch.env ] && . ./launch.env
# the package on the stick, installed here as well as by the box at its boot
# (so a stick plugged in without a reboot counts), and then set aside: the box
# puts back whatever package is there each time it boots, which would undo
# every update. this script may be replaced by it, so it starts over
package=$stick/steamlink/apps/chupathingyce.tgz
if [ -n "$mounted" ] && [ -f "$package" ]; then
	# (a copy of the game puts the ring up meanwhile: unpacking takes a minute
	# and nothing else draws, so the television shows garbled video memory)
	running=/tmp/chupathingyce-installing
	screen=/tmp/chupathingyce-install-screen
	: > "$running"
	cp -f ./halo "$screen" && chmod +x "$screen"
	HALO_INSTALL_SCREEN="$running" "$screen" > /dev/null 2>&1 &
	shown=$!
	tar zxf "$package" -C "$app/.."
	unpacked=$?
	sync
	rm -f "$running"
	wait "$shown"
	rm -f "$screen"
	if [ "$unpacked" -eq 0 ]; then
		mv -f "$package" "$package.installed"
		sync
		umount "$stick"
		exec "$app/run.sh"
	fi
fi
# (42: an update is in place, and the new game wants starting)
: > "$data/run.log"
while :; do
	./halo >> "$data/run.log" 2>&1
	[ $? -eq 42 ] || break
	sync
done
sync
[ -n "$mounted" ] && umount "$stick"
exit 0
