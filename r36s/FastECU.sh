#!/bin/bash
# FastECU-R36S launcher. Lives in the Arch R ports folder, next to the
# FastECU-R36S folder, and shows up in the Ports menu.
#
# Everything FastECU needs is inside FastECU-R36S/app (its own copy of Qt and
# the other libraries), so nothing has to be installed and no internet is needed.
#
# It stops the Arch R menu to free the screen, runs FastECU with the gamepad
# working as mouse + keyboard, then brings the menu back.

HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$HERE/FastECU-R36S"
APPDIR="$PKG/app"
LOG="$PKG/fastecu-debug.log"

# The menu (sway + EmulationStation) starts this script. Stopping sway would
# also stop anything the menu started, so hand off to a separate background
# service first (the same way the original FastECU menu entry does), and let
# the menu's copy end right away.
if [ -z "${FASTECU_DETACHED:-}" ]; then
    systemd-run --unit=fastecu-r36s --collect --setenv=FASTECU_DETACHED=1 \
        /bin/bash "$HERE/$(basename "$0")" > "$PKG/fastecu-start.log" 2>&1
    exit 0
fi

# Keep the last two runs' output for bug reports
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.old"
exec >"$LOG" 2>&1
echo "FastECU-R36S $(cat "$PKG/VERSION.txt" 2>/dev/null) started $(date)"
echo "Board: $( { tr -d '\0' < /proc/device-tree/model; } 2>/dev/null)"
echo "Kernel: $(uname -r)"

if [ ! -f "$APPDIR/FastECU" ]; then
    echo "FastECU-R36S folder not found next to this script ($PKG)."
    exit 1
fi

# The card's ROMS partition may not allow running programs from it.
# If it doesn't, copy the program once to internal storage and run it from there.
LOADER="$APPDIR/lib/ld-linux-aarch64.so.1"
chmod +x "$LOADER" "$APPDIR/FastECU" "$APPDIR/gamepad_mouse" 2>/dev/null
if ! "$LOADER" --version >/dev/null 2>&1; then
    VERSION="$(cat "$PKG/VERSION.txt" 2>/dev/null || echo unknown)"
    RUNDIR="${HOME:-/root}/.local/share/FastECU-R36S/$VERSION"
    if [ ! -f "$RUNDIR/FastECU" ]; then
        echo "Card does not allow running programs; copying to $RUNDIR"
        rm -rf "${HOME:-/root}/.local/share/FastECU-R36S"
        mkdir -p "$RUNDIR" && cp -r "$APPDIR/." "$RUNDIR/" && chmod +x "$RUNDIR/lib/ld-linux-aarch64.so.1"
    fi
    APPDIR="$RUNDIR"
    LOADER="$APPDIR/lib/ld-linux-aarch64.so.1"
fi

# ---------- free the screen, and give it back when done ----------
# Arch R draws its menu with sway. FastECU draws straight to the screen, so
# sway has to stop first. The trap brings the menu back however FastECU ends.
systemctl stop sway.service
trap 'echo "Restoring menu $(date)"; sync; systemctl start essway.service' EXIT
sleep 1

# ---------- settings ----------
# Settings, ECU definitions and datalogs stay on the card, next to the program,
# so testers can reach them from a PC.
export HOME="$PKG/userdata"
export FASTECU_FILES="$PKG/FastECU-Files"
mkdir -p "$HOME" "$FASTECU_FILES"

# Use only the bundled Qt and libraries, not anything installed on the system
export QT_PLUGIN_PATH="$APPDIR/plugins"
unset QT_QPA_PLATFORM_PLUGIN_PATH LD_LIBRARY_PATH LD_PRELOAD

# Display: draw straight to the screen (no graphics drivers needed)
export QT_QPA_PLATFORM=linuxfb
export QT_FONT_DPI=70
umask 000

run() {   # run a bundled program with the bundled libraries only
    "$LOADER" --library-path "$APPDIR/lib" --argv0 "$(basename "$1")" "$@"
}

# ---------- find the gamepad ----------
# The built-in gamepad is the input device with a joystick handler (jsN).
# Its event number can differ between boards, so look it up instead of
# assuming event3.
GAMEPAD_DEV=$(awk '
    /^H: Handlers=/ && /js[0-9]/ {
        sub(/^H: Handlers=/, "")
        for (i = 1; i <= NF; i++) if ($i ~ /^event[0-9]+$/) { print "/dev/input/" $i; exit }
    }' /proc/bus/input/devices 2>/dev/null)
GAMEPAD_DEV="${GAMEPAD_DEV:-/dev/input/event3}"
echo "Gamepad: $GAMEPAD_DEV"

# gamepad_mouse needs the uinput driver to make the virtual mouse/keyboard
[ -e /dev/uinput ] || modprobe uinput

# ---------- USB check (for bug reports) ----------
# Which cables the R36S sees and which serial drivers this Arch R has.
echo "=== USB check"
echo "- serial ports:"; ls -l /dev/ttyUSB* /dev/ttyACM* 2>&1
echo "- USB devices (vendor:product name):"
for d in /sys/bus/usb/devices/*; do
    [ -f "$d/idVendor" ] && echo "  $(cat "$d/idVendor"):$(cat "$d/idProduct") $(cat "$d/manufacturer" 2>/dev/null) $(cat "$d/product" 2>/dev/null)"
done
echo "- kernel messages:"; dmesg 2>/dev/null | grep -i -E 'usb|ch34|ttyUSB|ttyACM|cdc|ftdi|cp210' | tail -25
echo "- serial drivers available:"
ls "/lib/modules/$(uname -r)/kernel/drivers/usb/serial/" 2>&1
zcat /proc/config.gz 2>/dev/null | grep -E 'USB_SERIAL_CH341|USB_SERIAL_FTDI|USB_SERIAL_CP210|USB_ACM'
echo "=== end USB check"

echo "Running from: $APPDIR"
echo "----"

# ---------- run ----------
# Gamepad -> mouse/keyboard (hands off the sticks for the first second)
run "$APPDIR/gamepad_mouse" --device "$GAMEPAD_DEV" --speed 550 &
GP=$!
sleep 1

cd "$APPDIR" || exit 1
run "$APPDIR/FastECU" &
FE=$!

# Stop when either one ends:
#  - FastECU closed normally (File -> Quit), or
#  - SELECT + START held (quits gamepad_mouse) = emergency exit
wait -n $GP $FE
CODE=$?

kill $GP $FE 2>/dev/null
sleep 2
kill -9 $FE 2>/dev/null
wait 2>/dev/null

echo "----"
echo "Stopped with code $CODE at $(date)"
exit $CODE
