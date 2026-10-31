#!/usr/bin/env bash
#
# build.sh — build the System.Management.Automation shim for Wine-Mono.
#
# Produces System.Management.Automation.dll (version 3.0.0.0, delay-signed with
# Microsoft's public key so that the reference `PublicKeyToken=31bf3856ad364e35`
# resolves; Mono does not verify strong-name signatures) and, unless --no-test is
# given, the test host test/smahost.exe.  See README.md for what the shim does.
#
# No host Mono is required: Wine-Mono ships its own C# compiler, and the script
# runs it through Wine when there is no `mcs` on the host.
#
# Usage:
#   scripts/mono-sma-shim/build.sh [--prefix DIR] [--no-test]
#
#   --prefix DIR   Wine prefix whose Wine-Mono provides mcs.exe
#                  (default: $WINEPREFIX, else ~/.wine)
#   --no-test      do not build test/smahost.exe
#
# Environment:
#   WINE           Wine binary used to run mcs.exe (default: `wine` from PATH).
#                  Use the Wine that owns the prefix — starting a prefix with a
#                  different Wine build triggers a prefix update.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX=${WINEPREFIX:-$HOME/.wine}
WINE=${WINE:-wine}
BUILD_TEST=1
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PREFIX=$2; shift 2 ;;
        --no-test) BUILD_TEST=0; shift ;;
        -h|--help) sed -n '3,23p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

SRC=(Api.cs Values.cs Parser.cs Interpreter.cs Cmdlets.cs)
OUT=System.Management.Automation.dll

cd "$HERE"
if command -v mcs >/dev/null 2>&1; then
    MCS=(mcs)
    echo "compiler: host mcs ($(mcs --version 2>/dev/null | head -1))"
else
    MCS_EXE="$PREFIX/drive_c/windows/mono/mono-2.0/lib/mono/4.5/mcs.exe"
    if [ ! -f "$MCS_EXE" ]; then
        echo "error: no C# compiler: neither a host mcs nor $MCS_EXE" >&2
        echo "       (is Wine-Mono installed in the prefix? run \`WINEPREFIX=$PREFIX $WINE wineboot -u\`)" >&2
        exit 1
    fi
    MCS=(env WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" 'C:\windows\mono\mono-2.0\lib\mono\4.5\mcs.exe')
    echo "compiler: Wine-Mono mcs.exe from $PREFIX (via $WINE)"
fi

"${MCS[@]}" -nologo -target:library -out:"$OUT" -delaysign+ -keyfile:ms-pub.snk -optimize+ "${SRC[@]}" | tr -d '\r'
[ -s "$OUT" ] || { echo "error: $OUT was not produced" >&2; exit 1; }
echo "built: $HERE/$OUT ($(stat -c %s "$OUT") bytes, md5 $(md5sum "$OUT" | cut -c1-8))"

if [ "$BUILD_TEST" = 1 ]; then
    "${MCS[@]}" -nologo -out:test/smahost.exe -r:"$OUT" test/smahost.cs | tr -d '\r'
    cp -f "$OUT" test/
    echo "built: test/smahost.exe (run the tests with test/run-tests.sh)"
fi
