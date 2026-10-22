#!/bin/bash
# FastECU-R36S launcher. Lives in the Arch R ports folder, next to the
# FastECU-R36S folder, and shows up in the Ports menu.
#
# Everything FastECU needs is inside FastECU-R36S/app (its own copy of Qt and
# the other libraries), so nothing has to be installed and no internet is needed.

HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$HERE/FastECU-R36S"
APPDIR="$PKG/app"
LOG="$PKG/fastecu-debug.log"

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
chmod +x "$LOADER" "$APPDIR/FastECU" 2>/dev/null
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

# Settings, ECU definitions and datalogs stay on the card, next to the program,
# so testers can reach them from a PC.
export HOME="$PKG/userdata"
export FASTECU_FILES="$PKG/FastECU-Files"
mkdir -p "$HOME" "$FASTECU_FILES"

# Use only the bundled Qt, not anything installed on the system
export QT_PLUGIN_PATH="$APPDIR/plugins"
unset QT_QPA_PLATFORM_PLUGIN_PATH LD_LIBRARY_PATH LD_PRELOAD


# Display: draw straight to the screen (no graphics drivers needed)
export QT_QPA_PLATFORM=linuxfb
export QT_FONT_DPI=70
umask 000

run() {   # run a bundled program with the bundled libraries only
    "$LOADER" --library-path "$APPDIR/lib" --argv0 "$(basename "$1")" "$@"
}

echo "Running from: $APPDIR"
echo "----"

# Buttons: gamepad -> mouse/keyboard (hands off the sticks for the first second)
GP=""
if [ -f "$APPDIR/gamepad_mouse" ]; then
    run "$APPDIR/gamepad_mouse" --speed 550 &
    GP=$!
    sleep 1
else
    echo "gamepad_mouse not in package: buttons will not work"
fi

cd "$APPDIR" || exit 1
run "$APPDIR/FastECU" &
FE=$!

# Stop when either one ends:
#  - FastECU closed normally (File -> Quit), or
#  - SELECT + START held (quits gamepad_mouse) = emergency exit
if [ -n "$GP" ]; then
    wait -n $GP $FE
else
    wait $FE
fi
CODE=$?

kill $GP $FE 2>/dev/null
sleep 2
kill -9 $FE 2>/dev/null
wait 2>/dev/null

echo "----"
echo "Stopped with code $CODE at $(date)"
sync
exit $CODE
