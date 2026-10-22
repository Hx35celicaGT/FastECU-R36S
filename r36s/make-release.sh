#!/bin/bash
# FastECU-R36S: make a tester release zip.
#
# Run on your Linux PC after building FastECU in the aarch64 chroot.
# It copies the built program plus every library it needs (taken from the
# chroot, so they match the build exactly), the license texts for all of
# them, and the matching source code, into one zip that works offline.
#
#   r36s/make-release.sh                 make dist/FastECU-R36S-<version>.zip
#   r36s/make-release.sh v0.2            use "v0.2" as the version
#   ALLOW_DIRTY=1 r36s/make-release.sh   allow uncommitted changes (test builds only)
#
# Settings (environment variables):
#   CHROOT   chroot folder            (default: ~/r36s-chroot)
#   APP      FastECU folder in chroot (default: /root/FastECU)
#   PLUGINS  Qt plugin folders to include, relative to /usr/lib/qt/plugins
#
# Needs on the PC: sudo, zip, git. Needs in the chroot: pacman, ldd.

set -euo pipefail

CHROOT="${CHROOT:-$HOME/r36s-chroot}"
APP="${APP:-/root/FastECU}"
PLUGINS="${PLUGINS:-platforms imageformats egldeviceintegrations generic platforminputcontexts}"
QT_PLUGIN_DIR=/usr/lib/qt/plugins

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

die() { echo "ERROR: $*" >&2; exit 1; }
in_chroot() { sudo chroot "$CHROOT" "$@"; }

# ---------- checks ----------
command -v zip >/dev/null || die "zip is not installed on the PC"
[ -x "$CHROOT$APP/FastECU" ] || die "no built program at $CHROOT$APP/FastECU (build it first)"
in_chroot /bin/true 2>/dev/null || die "cannot enter the chroot at $CHROOT (is qemu-user-static / binfmt set up?)"

if [ -n "$(git status --porcelain --untracked-files=no)" ] && [ "${ALLOW_DIRTY:-0}" != 1 ]; then
    die "uncommitted changes in $REPO. Commit them first, so the source in the zip matches the program.
       (For a private test build only: ALLOW_DIRTY=1 $0)"
fi

VERSION="${1:-$(git describe --tags --always --dirty)}"
NAME="FastECU-R36S-$VERSION"
DIST="$REPO/dist"
OUT="$DIST/$NAME"
PKG="$OUT/FastECU-R36S"          # the folder testers copy to the card

echo "Making $NAME"
rm -rf "$OUT" "$DIST/$NAME.zip"
mkdir -p "$PKG/app/lib" "$PKG/app/plugins" "$PKG/FastECU-Files" "$PKG/THIRD_PARTY/licenses"

# ---------- program and Qt plugins ----------
cp "$CHROOT$APP/FastECU" "$PKG/app/FastECU"
ELF_FILES=("$APP/FastECU")      # paths inside the chroot, for ldd

# Gamepad -> mouse/keyboard helper (the buttons don't work without it).
# Built here from r36s/gamepad_mouse, so it always matches the source in the zip.
GAMEPAD=/tmp/fastecu-gamepad_mouse
sudo cp "$REPO/r36s/gamepad_mouse/gamepad_mouse.cpp" "$CHROOT/tmp/fastecu-gamepad_mouse.cpp"
in_chroot g++ -O2 -std=c++17 -o "$GAMEPAD" /tmp/fastecu-gamepad_mouse.cpp \
    || die "gamepad_mouse did not compile in the chroot"
cp "$CHROOT$GAMEPAD" "$PKG/app/gamepad_mouse"
ELF_FILES+=("$GAMEPAD")

for dir in $PLUGINS; do
    src="$CHROOT$QT_PLUGIN_DIR/$dir"
    [ -d "$src" ] || { echo "  (no Qt plugin folder $dir, skipped)"; continue; }
    mkdir -p "$PKG/app/plugins/$dir"
    for so in "$src"/*.so; do
        [ -e "$so" ] || continue
        cp "$so" "$PKG/app/plugins/$dir/"
        ELF_FILES+=("$QT_PLUGIN_DIR/$dir/$(basename "$so")")
    done
done

# ---------- every library those files need ----------
# ldd runs inside the chroot. Libraries are stored under the name the program
# asks for (e.g. libQt5Core.so.5) with symlinks resolved, because exFAT/FAT
# card partitions cannot hold symlinks.
declare -A LIBS=()
for f in "${ELF_FILES[@]}"; do
    while read -r soname path; do
        LIBS["$soname"]="$path"
    done < <(in_chroot ldd "$f" 2>/dev/null | awk '$2 == "=>" && $3 ~ /^\// {print $1, $3}')
done

LOADER_NAME="${LOADER_NAME:-ld-linux-aarch64}"   # only changed for testing on a PC
LOADER=$(in_chroot ldd "$APP/FastECU" | awk -v n="$LOADER_NAME" 'index($1, n) {print $1}')
[ -n "$LOADER" ] || die "could not find the program loader ($LOADER_NAME) in ldd output"

for soname in "${!LIBS[@]}"; do
    cp -L "$CHROOT${LIBS[$soname]}" "$PKG/app/lib/$soname"
done
cp -L "$CHROOT$LOADER" "$PKG/app/lib/ld-linux-aarch64.so.1"
echo "  ${#LIBS[@]} libraries + loader bundled"

# ---------- licenses for everything bundled ----------
# Ask pacman which package each bundled file came from, then copy that
# package's license files and record its exact version.
{
    echo "$LOADER"
    printf '%s\n' "${LIBS[@]}"
    for f in "${ELF_FILES[@]:1}"; do echo "$f"; done   # Qt plugins (FastECU itself is not a package)
} | sort -u > "$OUT/.owned-files"

sudo cp "$OUT/.owned-files" "$CHROOT/tmp/fastecu-owned-files"
in_chroot sh -c 'while read -r f; do pacman -Qqo "$f" 2>/dev/null; done < /tmp/fastecu-owned-files' \
    | sort -u > "$OUT/.packages"
sudo rm -f "$CHROOT/tmp/fastecu-owned-files"

{
    echo "Third-party software bundled in FastECU-R36S $VERSION"
    echo "All of it was taken unmodified from Arch Linux ARM (aarch64) packages."
    echo
    printf '%-28s %-24s %s\n' "PACKAGE" "VERSION" "LICENSE"
    while read -r pkg; do
        ver=$(in_chroot pacman -Q "$pkg" | awk '{print $2}')
        lic=$(in_chroot pacman -Qi "$pkg" | awk -F': ' '/^Licenses/ {print $2}')
        printf '%-28s %-24s %s\n' "$pkg" "$ver" "$lic"
        if [ -d "$CHROOT/usr/share/licenses/$pkg" ]; then
            mkdir -p "$PKG/THIRD_PARTY/licenses/$pkg"
            cp -rL "$CHROOT/usr/share/licenses/$pkg/." "$PKG/THIRD_PARTY/licenses/$pkg/"
        fi
    done < "$OUT/.packages"
} > "$PKG/THIRD_PARTY/PACKAGES.txt"

# Full texts of the common licenses (Arch keeps these in one shared place)
for d in spdx common; do
    for lic in GPL-3.0-only GPL-3.0-or-later LGPL-3.0-only LGPL-3.0-or-later LGPL-2.1-or-later GPL3 LGPL3 LGPL2.1; do
        for ext in .txt "/license.txt" ""; do
            src="$CHROOT/usr/share/licenses/$d/$lic$ext"
            if [ -f "$src" ]; then
                cp -L "$src" "$PKG/THIRD_PARTY/licenses/$lic.txt"
                break
            fi
        done
    done
done
cp "$REPO/r36s/THIRD_PARTY-SOURCES.txt" "$PKG/THIRD_PARTY/SOURCES.txt"
echo "  licenses for $(wc -l < "$OUT/.packages") packages"

# ---------- FastECU's own files ----------
cp "$REPO/LICENSE" "$PKG/LICENSE.txt"
cp "$REPO/CHANGES-R36S.md" "$PKG/CHANGES-R36S.md"
cp "$REPO/r36s/README.txt" "$PKG/README.txt"
cp "$REPO/r36s/FastECU.sh" "$OUT/FastECU.sh"
if [ -d "$REPO/r36s/FastECU-Files" ]; then
    cp -r "$REPO/r36s/FastECU-Files/." "$PKG/FastECU-Files/"
fi

# Source code for exactly this build (GPLv3 section 6). Inside the zip, so
# every copy that gets passed around carries its own source.
git archive --format=tar.gz --prefix="$NAME-source/" -o "$PKG/$NAME-source.tar.gz" HEAD
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    # ALLOW_DIRTY build: add the uncommitted changes too
    git diff HEAD > "$PKG/uncommitted-changes.patch"
fi

echo "$VERSION" > "$PKG/VERSION.txt"
rm -f "$OUT/.owned-files" "$OUT/.packages"
sudo rm -f "$CHROOT$GAMEPAD" "$CHROOT/tmp/fastecu-gamepad_mouse.cpp"

# ---------- zip ----------
(cd "$OUT" && zip -qr9 "$DIST/$NAME.zip" FastECU.sh FastECU-R36S)
(cd "$DIST" && sha256sum "$NAME.zip" > "$NAME.zip.sha256")

echo
echo "Done: dist/$NAME.zip ($(du -h "$DIST/$NAME.zip" | cut -f1))"
echo "      dist/$NAME.zip.sha256"
echo "Upload both to a GitHub release."
