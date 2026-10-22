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

# ---------------------------------------------------------------------------
# DISPLAY AND BUTTONS
# TODO: copy these settings from the current R36S launcher (how the screen is
# opened and how the buttons reach FastECU). Until then: draw straight to the
# screen with linuxfb, which needs no graphics drivers.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-linuxfb}"
# ---------------------------------------------------------------------------

echo "Running from: $APPDIR"
echo "Qt platform: $QT_QPA_PLATFORM"
echo "----"

# Start through the bundled loader, so the bundled C library is used too.
# This is what makes it independent of the Arch R version on the card.
"$LOADER" --library-path "$APPDIR/lib" --argv0 FastECU "$APPDIR/FastECU" --debug
CODE=$?

echo "----"
echo "FastECU exited with code $CODE at $(date)"
sync
exit $CODE
