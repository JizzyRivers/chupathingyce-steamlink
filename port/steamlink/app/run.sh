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
cd "$app"
[ -f launch.env ] && . ./launch.env
# the package on the stick, installed here as well as by the box at its boot
# (so a stick plugged in without a reboot counts), and then set aside: the box
# puts back whatever package is there each time it boots, which would undo
# every update. this script may be replaced by it, so it starts over
package=$stick/steamlink/apps/chupathingyce.tgz
if [ -n "$mounted" ] && [ -f "$package" ]; then
	if tar zxf "$package" -C "$app/.."; then
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
