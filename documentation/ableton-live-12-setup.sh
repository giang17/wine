#!/usr/bin/env bash
#
# ableton-live-12-setup.sh — prepare a Wine prefix for Ableton Live 12.
#
# Live runs its content indexer as a separate process (Ableton Index.exe).  On
# Wine's built-in msvcp140 that process dies immediately and Live restarts it in
# a loop, which leaves the browser without content and pops up an error dialog:
#
#     Indexer: process stopped prematurely [1]. Restart      (Preferences/Log.txt)
#
# Fixing it takes two things, and the second one is easy to miss:
#
#   1. a per-executable DllOverride so that only the indexer prefers the native
#      runtime, leaving the rest of the prefix on Wine's builtins,
#   2. the native runtime actually being installed.  "native,builtin" silently
#      falls back to the builtin when no native DLL is present — the registry
#      looks correct and the indexer keeps crashing.
#
# This script checks both, and applies the override.  It never installs the
# runtime itself (that is winetricks' job) and never touches anything outside
# the prefix you point it at.
#
# Usage:
#   ableton-live-12-setup.sh [--prefix DIR] [--wine BINARY] [--check]
#
#   --prefix DIR    Wine prefix to operate on.  Default: $WINEPREFIX, else ~/.wine
#   --wine BINARY   wine binary used to import the registry key.  Default: wine
#   --check         report only, change nothing.  Exit 0 if the prefix is ready.
#
# Exit codes: 0 ready / 1 usage or prefix error / 2 native runtime missing
#             3 override missing (only with --check)

set -u

PREFIX="${WINEPREFIX:-$HOME/.wine}"
WINE="wine"
CHECK_ONLY=0

# The runtime DLLs the indexer needs.  Keep in sync with the override below.
DLLS=(concrt140 msvcp140 msvcp140_1 msvcp140_2 msvcp140_atomic_wait
      msvcp140_codecvt_ids vcruntime140 vcruntime140_1)

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) shift; PREFIX="${1:?--prefix needs a directory}" ;;
        --wine)   shift; WINE="${1:?--wine needs a binary}" ;;
        --check)  CHECK_ONLY=1 ;;
        -h|--help) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1  (try --help)" >&2; exit 1 ;;
    esac
    shift
done

SYS32="$PREFIX/drive_c/windows/system32"
USERREG="$PREFIX/user.reg"

[ -d "$PREFIX" ]  || { echo "ERROR: prefix not found: $PREFIX" >&2; exit 1; }
[ -d "$SYS32" ]   || { echo "ERROR: not a 64-bit Wine prefix (no $SYS32)" >&2; exit 1; }

echo "Prefix: $PREFIX"
echo

# --- 1. is the native runtime present? ---------------------------------------
# Do NOT compare file sizes: Wine's builtin PE ships debug symbols and is around
# 5 MB, i.e. *larger* than the native DLL (~550 KB).  Check the origin instead.
echo "Checking the Visual C++ runtime..."
missing=0
for d in "${DLLS[@]}"; do
    f="$SYS32/$d.dll"
    if [ ! -f "$f" ]; then
        printf '  %-22s missing\n' "$d"
        missing=$((missing + 1))
    elif strings -a "$f" 2>/dev/null | grep -q "Wine builtin DLL"; then
        printf '  %-22s builtin  <- native needed\n' "$d"
        missing=$((missing + 1))
    else
        printf '  %-22s native\n' "$d"
    fi
done

if [ "$missing" -gt 0 ]; then
    echo
    echo "=> $missing of ${#DLLS[@]} runtime DLLs are not native."
    echo "   Install the redistributable first, then run this script again:"
    echo
    echo "       WINEPREFIX=\"$PREFIX\" winetricks vcrun2019"
    echo
    echo "   Without it the override below has no effect: Wine falls back to the"
    echo "   builtin DLL and the indexer keeps crashing."
    exit 2
fi
echo "=> native runtime present."
echo

# --- 2. is the override already in place? ------------------------------------
# Read user.reg directly so that --check needs no running Wine.
override_present=0
if [ -f "$USERREG" ] && \
   grep -q 'AppDefaults\\\\Ableton Index.exe\\\\DllOverrides' "$USERREG"; then
    override_present=1
fi

if [ "$override_present" -eq 1 ]; then
    echo "DllOverride for 'Ableton Index.exe' is already present."
    echo
    echo "This prefix is ready for Ableton Live 12."
    exit 0
fi

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "DllOverride for 'Ableton Index.exe' is MISSING."
    echo "Run without --check to apply it."
    exit 3
fi

# --- 3. apply the override ---------------------------------------------------
command -v "$WINE" >/dev/null 2>&1 || {
    echo "ERROR: wine binary not found: $WINE  (use --wine /path/to/wine)" >&2
    exit 1; }

# Back up the registry before touching it — cheap, and a broken user.reg is
# tedious to reconstruct.
stamp=$(date +%Y%m%d-%H%M%S)
for r in user.reg system.reg; do
    [ -f "$PREFIX/$r" ] && cp "$PREFIX/$r" "$PREFIX/$r.bak-$stamp"
done
echo "Registry backed up as *.reg.bak-$stamp"

# Write the .reg inside the prefix and hand regedit a WINDOWS path.  regedit is
# a Windows program: a Unix path like /tmp/x.reg is read as a Windows path and
# the import silently does nothing.
reg_file="$PREFIX/drive_c/ableton-index-override.reg"
trap 'rm -f "$reg_file"' EXIT
{
    echo "REGEDIT4"
    echo
    echo '[HKEY_CURRENT_USER\Software\Wine\AppDefaults\Ableton Index.exe\DllOverrides]'
    for d in "${DLLS[@]}"; do echo "\"$d\"=\"native,builtin\""; done
} > "$reg_file"

echo "Importing the DllOverride..."
WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" regedit /S 'C:\ableton-index-override.reg' \
    </dev/null >/dev/null 2>&1

# --- 4. verify ---------------------------------------------------------------
# Wine keeps the registry in the wineserver and flushes user.reg to disk with a
# delay, so checking immediately after the import reports a false failure.  Poll
# instead of sleeping blindly, and give up rather than block if it never lands.
for _ in $(seq 1 15); do
    grep -q 'AppDefaults\\\\Ableton Index.exe\\\\DllOverrides' "$USERREG" 2>/dev/null && {
        echo
        echo "This prefix is ready for Ableton Live 12."
        echo "If the browser still stays empty, check Preferences/Log.txt for"
        echo "'Indexer: process stopped prematurely'."
        exit 0
    }
    sleep 1
done

echo "ERROR: the override did not appear in $USERREG." >&2
echo "       Is '$WINE' the right binary for this prefix, and does it start at all?" >&2
echo "       Try:  WINEPREFIX=\"$PREFIX\" $WINE regedit   (and look for" >&2
echo "             HKCU\\Software\\Wine\\AppDefaults\\Ableton Index.exe)" >&2
exit 1
