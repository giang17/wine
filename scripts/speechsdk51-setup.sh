#!/usr/bin/env bash
#
# speechsdk51-setup.sh — make the Microsoft Speech SDK 5.1 voices (Mary, Mike, Sam) speak
# under Wine's builtin sapi, for .NET Framework's System.Speech and for ISpVoice.
#
#   1. Optionally install the SDK from SpeechSDK51.exe.  `winetricks speechsdk` refuses
#      64-bit prefixes and switches sapi to native, under which System.Speech does not
#      start (it needs SAPI 5.3 interfaces).  The SDK's MSI only installs its runtime
#      (spcommon.dll and the engine) when the prefix reports Windows 2000, so the script
#      switches to win2k for the MSI and back to the previous version afterwards.
#   2. The SDK engine (spttseng.dll) takes its pronunciations from Microsoft's lexicon and
#      phone converter classes (SpLexicon, SpUnCompressedLexicon, SpCompressedLexicon,
#      SpPhoneConverter), which read the SDK's .lxa files and which Wine's sapi does not
#      have; without them every word comes out as the same block of samples.  Wine loads
#      its builtin for any file named sapi.dll, so the script copies the SDK's sapi.dll to
#      sapi51.dll and points those four classes (32-bit registry view) at the copy.  Wine
#      itself never registers them, so prefix updates leave the entries alone.
#
# Everything else stays Wine's builtin sapi.  The default voice System.Speech reads from
# HKCU\Software\Microsoft\Speech\Voices\DefaultTokenId is stored by sapi itself on this
# branch (since 9a10b351cea / 27e3cc6394a), as on Windows.
#
# Usage:
#   speechsdk51-setup.sh [--prefix DIR] [--wine BINARY] [--check] [SpeechSDK51.exe]
#
#   --prefix DIR    Wine prefix to operate on.  Default: $WINEPREFIX, else ~/.wine
#   --wine BINARY   wine binary to use.  Default: wine
#   --check         report only, change nothing
#   SpeechSDK51.exe install the SDK from this file first (needs unzip), unless it is
#                   installed already
#
# Exit codes: 0 done / 1 usage or prefix error / 2 SDK not installed / 3 setup incomplete
#             (only with --check)

set -u

PREFIX="${WINEPREFIX:-$HOME/.wine}"
WINE="wine"
CHECK_ONLY=0
INSTALLER=""

# The four classes the SDK engine creates, see step 2 above.
CLSIDS=(
    "9185F743-1143-4C28-86B5-BFF14F20E5C8"  # SpPhoneConverter
    "0655E396-25D0-11D3-9C26-00C04F8EF87C"  # SpLexicon
    "C9E37C15-DF92-4727-85D6-72E5EEB6995A"  # SpUnCompressedLexicon
    "90903716-2F42-11D3-9C26-00C04F8EF87C"  # SpCompressedLexicon
)
SPEECH_DIR_WIN='C:\Program Files (x86)\Common Files\Microsoft Shared\Speech'

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --wine)   WINE="$2"; shift 2 ;;
        --check)  CHECK_ONLY=1; shift ;;
        -h|--help) sed -n '3,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 1 ;;
        *)  INSTALLER="$1"; shift ;;
    esac
done

[ -d "$PREFIX/drive_c" ] || { echo "no Wine prefix at $PREFIX" >&2; exit 1; }
[ -d "$PREFIX/drive_c/windows/syswow64" ] || { echo "$PREFIX is not a 64-bit prefix (no syswow64)" >&2; exit 1; }
export WINEPREFIX="$PREFIX"
export WINEDEBUG="${WINEDEBUG:--all}"

SPEECH_DIR="$PREFIX/drive_c/Program Files (x86)/Common Files/Microsoft Shared/Speech"
SDK_SAPI="$SPEECH_DIR/sapi.dll"
ENGINES_DIR="$PREFIX/drive_c/Program Files (x86)/Common Files/SpeechEngines/Microsoft"
COPY="$SPEECH_DIR/sapi51.dll"

wine_reg() { "$WINE" reg "$@" 2>/dev/null | tr -d '\r'; }

# Wine's own PE modules carry this marker in their DOS stub; Microsoft's sapi.dll does not.
is_wine_builtin() { grep -aq "Wine builtin DLL" "$1" 2>/dev/null; }

sdk_installed() { [ -f "$SDK_SAPI" ] && ! is_wine_builtin "$SDK_SAPI" && [ -f "$ENGINES_DIR/spcommon.dll" ]; }

# The winecfg name of the prefix' Windows version.  winecfg -v writes the version to the
# values under HKLM\Software\Microsoft\Windows NT\CurrentVersion, so it is read back from
# there (builds and product types as in programs/winecfg/appdefaults.c).
win_version() {
    local build type
    build=$(wine_reg query 'HKLM\Software\Microsoft\Windows NT\CurrentVersion' /v CurrentBuild | awk '/CurrentBuild/ {print $NF}')
    type=$(wine_reg query 'HKLM\System\CurrentControlSet\Control\ProductOptions' /v ProductType | awk '/ProductType/ {print $NF}')
    case "$build:$type" in
        22000:*) echo win11 ;;
        19045:*) echo win10 ;;
        9600:*)  echo win81 ;;
        9200:*)  echo win8 ;;
        7601:ServerNT) echo win2008r2 ;;
        7601:*)  echo win7 ;;
        6002:ServerNT) echo win2008 ;;
        6002:*)  echo vista ;;
        3790:ServerNT) echo win2003 ;;
        3790:*)  echo winxp64 ;;
        2600:*)  echo winxp ;;
        2195:*)  echo win2k ;;
    esac
}

install_sdk() {
    local tmp old
    command -v unzip >/dev/null || { echo "unzip is needed to unpack $INSTALLER" >&2; return 1; }
    tmp=$(mktemp -d) || return 1
    unzip -q -o "$INSTALLER" -d "$tmp" || { rm -rf "$tmp"; return 1; } # rm-guard: ok -- mktemp directory of this run
    [ -f "$tmp/Microsoft Speech SDK 5.1.msi" ] || { echo "$INSTALLER contains no Microsoft Speech SDK 5.1.msi" >&2; rm -rf "$tmp"; return 1; } # rm-guard: ok -- mktemp directory of this run

    old=$(win_version)
    [ -n "$old" ] || { echo "unknown Windows version in the prefix, set one with winecfg -v first" >&2; rm -rf "$tmp"; return 1; } # rm-guard: ok -- mktemp directory of this run
    echo "installing the SDK in Windows 2000 mode, then back to $old"
    "$WINE" winecfg -v win2k
    (cd "$tmp" && "$WINE" msiexec /i "Microsoft Speech SDK 5.1.msi" /q)
    local rc=$?
    "$WINE" winecfg -v "$old"
    [ "$(win_version)" = "$old" ] || echo "WARNING: the prefix did not return to $old, run: winecfg -v $old" >&2
    rm -rf "$tmp" # rm-guard: ok -- mktemp directory of this run
    [ $rc -eq 0 ] || { echo "msiexec failed ($rc)" >&2; return 1; }
}

clsid_target() {
    wine_reg query "HKLM\\Software\\Classes\\Wow6432Node\\CLSID\\{$1}\\InprocServer32" /ve |
        sed -n 's/^ *(.*) *REG_[A-Z_]* *//p'
}

report() {
    local ok=1 c t
    if sdk_installed; then echo "SDK runtime:  installed ($SDK_SAPI)"
    else echo "SDK runtime:  MISSING"; ok=0; fi
    if [ -f "$COPY" ] && cmp -s "$COPY" "$SDK_SAPI"; then echo "sapi51.dll:   present"
    else echo "sapi51.dll:   MISSING or differs from the SDK's sapi.dll"; ok=0; fi
    for c in "${CLSIDS[@]}"; do
        t=$(clsid_target "$c")
        if [ "$t" = "$SPEECH_DIR_WIN\\sapi51.dll" ]; then echo "{$c}  -> sapi51.dll"
        else echo "{$c}  -> ${t:-<not registered>}  (should be sapi51.dll)"; ok=0; fi
    done
    echo "voices:       $(wine_reg query 'HKLM\Software\Microsoft\Speech\Voices\Tokens' /reg:32 | grep -o 'Tokens\\[^\\]*$' | sed 's/Tokens\\//' | tr '\n' ' ')"
    [ $ok -eq 1 ]
}

if [ $CHECK_ONLY -eq 1 ]; then
    report && exit 0 || exit 3
fi

if ! sdk_installed; then
    if [ -z "$INSTALLER" ]; then
        echo "the Speech SDK 5.1 runtime is not installed; pass SpeechSDK51.exe to install it" >&2
        exit 2
    fi
    install_sdk || exit 2
    sdk_installed || { echo "the SDK installed, but its runtime (sapi.dll, SpeechEngines\\Microsoft\\spcommon.dll) is missing" >&2; exit 2; }
fi

cp -f "$SDK_SAPI" "$COPY" || exit 1
for c in "${CLSIDS[@]}"; do
    wine_reg add "HKLM\\Software\\Classes\\Wow6432Node\\CLSID\\{$c}\\InprocServer32" /ve \
        /d "$SPEECH_DIR_WIN\\sapi51.dll" /f >/dev/null || { echo "registering {$c} failed" >&2; exit 1; }
done

report
