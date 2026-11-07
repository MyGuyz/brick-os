#!/usr/bin/env bash
# Deploy to the device over WiFi (SSH) — the same destinations `make deploy` uses over adb.
# Enable on device: Settings > Developer > Enable SSH / SFTP (the hint shows the IP).
# One-time key setup (asks for the device password):  ssh-copy-id root@<ip>
#
#   scripts/deploy-ssh.sh full [device]          build/BASE/MinUI-<device>.zip -> /mnt/SDCARD/MinUI.zip, reboot
#   scripts/deploy-ssh.sh pak <local.pak dir>    -> /mnt/SDCARD/.system/paks/Tools/<name>.pak/ (relaunch, no reboot)
#   scripts/deploy-ssh.sh bin <file>             -> /mnt/SDCARD/.system/bin/<file>, reboot
#
# Host comes from BRICK_HOST (e.g. BRICK_HOST=192.168.1.50).
set -euo pipefail

host="${BRICK_HOST:?set BRICK_HOST to the device IP}"
ssh_cmd=(ssh -o ConnectTimeout=5 "root@$host")

# push <local file> <remote path>: stream through ssh (no scp/sftp needed), then compare md5
push() {
	local src=$1 dst=$2
	"${ssh_cmd[@]}" "mkdir -p \"$(dirname "$dst")\" && cat > \"$dst.part\" && mv \"$dst.part\" \"$dst\"" < "$src"
	local want got
	want=$(md5sum "$src" | cut -d' ' -f1)
	got=$("${ssh_cmd[@]}" "md5sum \"$dst\"" | cut -d' ' -f1)
	if [ "$want" != "$got" ]; then echo "md5 mismatch: $dst" >&2; exit 1; fi
	echo "ok  $dst"
}

case "${1:-}" in
full)
	dev="${2:-brickpro}"
	push "build/BASE/MinUI-$dev.zip" /mnt/SDCARD/MinUI.zip
	"${ssh_cmd[@]}" reboot || true
	;;
pak)
	dir="${2:?local .pak directory}"
	dest="/mnt/SDCARD/.system/paks/Tools/$(basename "$dir")"
	"${ssh_cmd[@]}" "rm -rf \"$dest\""
	(cd "$dir" && find . -type f) | while read -r f; do
		push "$dir/${f#./}" "$dest/${f#./}"
	done
	;;
bin)
	file="${2:?binary to push}"
	push "$file" "/mnt/SDCARD/.system/bin/$(basename "$file")"
	"${ssh_cmd[@]}" reboot || true
	;;
*)
	sed -n '2,12p' "$0"
	exit 1
	;;
esac
