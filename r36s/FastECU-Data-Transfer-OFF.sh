#!/bin/sh
# FastECU-R36S: one-button USB data transfer (MTP) toggle for the Ports menu.
# The file renames itself to show the current state:
#   FastECU-Data-Transfer-OFF = off (select to turn ON)
#   FastECU-Data-Transfer-ON  = on  (select to turn OFF)
# Then the menu refreshes (about 10-20 seconds) so the new name shows.
#
# When ON, connect a USB-C DATA cable to a PC: it shows "ArchR Device" ->
# Roms -> ports -> FastECU-R36S -> FastECU-Files. Turn it OFF before using
# the OBD cable.
#
# The USB change and the menu refresh run in their own systemd units,
# so restarting the menu does not kill the MTP server.

DIR="$(cd "$(dirname "$0")" && pwd)"

if [ "$(/usr/bin/usbgadget)" = "file_transfer" ]; then
	systemd-run --scope --collect -q /usr/bin/usbgadget disabled >/dev/null 2>&1 </dev/null
	NEW=FastECU-Data-Transfer-OFF.sh
else
	systemd-run --scope --collect -q /usr/bin/usbgadget file_transfer >/dev/null 2>&1 </dev/null
	NEW=FastECU-Data-Transfer-ON.sh
fi

# Rename this entry to match the new state
for f in "$DIR"/FastECU-Data-Transfer-*.sh; do
	[ "$f" != "$DIR/$NEW" ] && mv "$f" "$DIR/$NEW"
done

# Refresh the menu 1 second from now, from outside the menu's own service
systemd-run --collect -q --on-active=1 systemctl restart essway.service >/dev/null 2>&1 </dev/null
exit 0
