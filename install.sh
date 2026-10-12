#!/bin/sh
# Install hp_envy_fan: the driver through DKMS (as root) and the Plasma widget (as your user).
#
#   sudo ./install.sh driver
#   ./install.sh widget
set -eu
cd "$(dirname "$0")"
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)

case "${1:-}" in
driver)
	[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0 driver" >&2; exit 1; }
	dkms remove "hp-envy-fan/$VER" --all 2>/dev/null || true
	rm -rf "/usr/src/hp-envy-fan-$VER"
	mkdir -p "/usr/src/hp-envy-fan-$VER"
	cp hp_envy_fan.c Makefile dkms.conf "/usr/src/hp-envy-fan-$VER/"
	dkms install "hp-envy-fan/$VER"
	install -m 644 udev/61-hp-envy-fan.rules /etc/udev/rules.d/
	echo hp_envy_fan > /etc/modules-load.d/hp-envy-fan.conf
	udevadm control --reload
	modprobe -r hp_envy_fan 2>/dev/null || true
	modprobe hp_envy_fan
	;;
widget)
	[ "$(id -u)" -ne 0 ] || { echo "run as your user, not root" >&2; exit 1; }
	dest="${XDG_DATA_HOME:-$HOME/.local/share}/plasma/plasmoids/com.github.hpenvyfan"
	if [ -d "$dest" ]; then
		# Copy in place: kpackagetool6 -u removes the package first, and the system tray
		# then drops the widget from its entries.
		cp -r plasmoid/com.github.hpenvyfan/. "$dest/"
		echo "Updated; restart Plasma (systemctl --user restart plasma-plasmashell) to load it."
		exit 0
	fi
	kpackagetool6 -t Plasma/Applet -i plasmoid/com.github.hpenvyfan
	echo "Enable \"Envy Fan\" under System Tray Settings > Entries, or add it to a panel."
	;;
*)
	echo "usage: sudo $0 driver | $0 widget" >&2
	exit 2
	;;
esac
