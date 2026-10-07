#!/usr/bin/env bash
#
# install.sh — put the System.Management.Automation shim into a prefix' Wine-Mono GAC.
#
# .NET installers that host PowerShell in-process (Steinberg's Setup.exe) look the
# assembly up through Mono, and Mono looks in the application directory, the GAC
# and MONO_PATH.  The GAC inside the prefix is the one place that works for every
# installer, so the shim goes to
#
#   <prefix>/drive_c/windows/mono/mono-2.0/lib/mono/gac/
#       System.Management.Automation/3.0.0.0__31bf3856ad364e35/System.Management.Automation.dll
#
# and the script then proves, with test/smahost.exe started from a directory that
# holds no copy of its own, that Mono resolves the assembly from there.
#
# Usage:
#   scripts/mono-sma-shim/install.sh [--prefix DIR] [--build] [--status] [--remove]
#
#   --prefix DIR   target prefix (default: $WINEPREFIX, else ~/.wine)
#   --build        run ./build.sh first
#   --status       report only: is the shim installed, and is it the built one?
#                  Exit 3 when it is missing or differs.
#   --remove       remove the shim from the GAC
#
# Environment:
#   WINE           Wine binary for the probe (default: `wine` from PATH).  Use the
#                  Wine that owns the prefix.
#
# A Wine-Mono update (wineboot -u with a newer Wine) replaces the mono-2.0
# directory and the shim with it; run this script again afterwards.  --status
# tells.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DLL=$HERE/System.Management.Automation.dll
HOST=$HERE/test/smahost.exe
PREFIX=${WINEPREFIX:-$HOME/.wine}
WINE=${WINE:-wine}
MODE=install
BUILD=0
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --build) BUILD=1; shift ;;
        --status) MODE=status; shift ;;
        --remove) MODE=remove; shift ;;
        -h|--help) sed -n '3,31p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ -d "$PREFIX" ] || { echo "error: prefix $PREFIX does not exist" >&2; exit 1; }
MONO_DIR=$PREFIX/drive_c/windows/mono
GAC=$(find "$MONO_DIR" -maxdepth 4 -type d -name gac 2>/dev/null | head -1 || true)
if [ -z "$GAC" ]; then
    echo "error: no Wine-Mono GAC under $MONO_DIR (Wine-Mono not installed in this prefix?)" >&2
    exit 1
fi
DEST_DIR=$GAC/System.Management.Automation/3.0.0.0__31bf3856ad364e35
DEST=$DEST_DIR/System.Management.Automation.dll

case $MODE in
    status)
        if [ ! -f "$DEST" ]; then
            echo "sma-shim $PREFIX: MISSING  -> $0 --prefix $PREFIX"
            exit 3
        fi
        if [ ! -f "$DLL" ]; then
            echo "sma-shim $PREFIX: installed (md5 $(md5sum "$DEST" | cut -c1-8)); nothing built here to compare against (./build.sh)"
            exit 0
        fi
        if cmp -s "$DLL" "$DEST"; then
            echo "sma-shim $PREFIX: OK  (GAC md5 $(md5sum "$DEST" | cut -c1-8) = built copy)"
            exit 0
        fi
        echo "sma-shim $PREFIX: DIFFERS  (GAC md5 $(md5sum "$DEST" | cut -c1-8), built $(md5sum "$DLL" | cut -c1-8)) -> $0 --prefix $PREFIX"
        exit 3 ;;
    remove)
        if [ -f "$DEST" ]; then
            rm -f "$DEST"
            rmdir "$DEST_DIR" "$(dirname "$DEST_DIR")" 2>/dev/null || true
            echo "removed: $DEST"
        else
            echo "nothing to remove in $PREFIX"
        fi
        exit 0 ;;
esac

if [ "$BUILD" = 1 ] || [ ! -f "$DLL" ]; then
    WINE="$WINE" "$HERE/build.sh" --prefix "$PREFIX" || { echo "error: build failed" >&2; exit 1; }
fi

mkdir -p "$DEST_DIR"
cp -f "$DLL" "$DEST"
cmp -s "$DLL" "$DEST" || { echo "error: copy to $DEST does not match" >&2; exit 1; }
echo "installed: $DEST (md5 $(md5sum "$DEST" | cut -c1-8))"

# Proof: a host started from a directory without its own copy must get the
# assembly from the GAC.
if [ -f "$HOST" ]; then
    TMP=$(mktemp -d)
    cp "$HOST" "$TMP/"
    out=$(cd "$TMP" && WINEPREFIX=$PREFIX WINEDEBUG=-all timeout 120 "$WINE" smahost.exe -c 'Write-Host "gac-probe"; EXIT 42' 2>/dev/null | tr -d '\r' || true)
    rm -rf "$TMP"
    if printf '%s\n' "$out" | grep -q 'INFO:   gac-probe' && printf '%s\n' "$out" | grep -q 'EXIT:   42'; then
        echo "verified: Mono ($WINE) resolves System.Management.Automation 3.0.0.0 from the GAC"
    else
        echo "warning: GAC probe failed in $PREFIX; output was:" >&2
        printf '%s\n' "$out" >&2
        exit 1
    fi
else
    echo "note: $HOST not built, GAC probe skipped (build.sh without --no-test builds it)"
fi
