#!/usr/bin/env bash
#
# wine-font-setup.sh — install the fonts a Wine prefix needs for plugin GUIs.
#
# Covers the host part of documentation/wine-font-setup-guide.md:
#
#   1. MS Core Fonts (Arial, Verdana, ...) into the prefix' Fonts directory and
#      into the GDI font registry.  Some plugins load font FILES directly during
#      DLL init (FL Studio's "Fruity Delay 3" opens C:\windows\Fonts\Arialbd.ttf)
#      and crash with an access violation if they are missing — with no useful
#      error message.  Wine does not register fonts from that directory by
#      itself, and `wineboot -u` does not either.
#   2. DejaVu Sans and Noto Sans Symbols2, plus the GDI FontLink entries that
#      make them the first fallback for symbol glyphs (star ratings, arrows in
#      Serum 2 show up as tofu boxes otherwise).
#
# Fonts are located through fontconfig, so distribution paths do not matter.
#
# NOTE: `wineboot -u` resets the FontLink entries to Wine's defaults and undoes
# step 2.  Re-run this script after a prefix update; it is idempotent.
#
# Usage:
#   wine-font-setup.sh [--prefix DIR] [--wine BINARY] [--check] [--no-mscore]
#
#   --prefix DIR    Wine prefix to operate on.  Default: $WINEPREFIX, else ~/.wine
#   --wine BINARY   wine binary to use.  Default: wine
#   --check         report only, change nothing
#   --no-mscore     skip the MS Core Fonts part (step 1)
#
# Exit codes: 0 done / 1 usage or prefix error / 2 required fonts not installed
#             3 setup incomplete (only with --check)

set -u

PREFIX="${WINEPREFIX:-$HOME/.wine}"
WINE="wine"
CHECK_ONLY=0
DO_MSCORE=1

# Files the FontLink entries reference. Without these two the symbol fallback
# cannot work, so they are hard requirements.
REQUIRED=(DejaVuSans.ttf NotoSansSymbols2-Regular.ttf)

# The standard MS Core Fonts set. Whatever of it is installed gets copied.
MSCORE=(andale arial arialbd arialbi ariali ariblk comic comicbd cour courbd
        courbi couri georgia georgiab georgiai georgiaz impact times timesbd
        timesbi timesi trebuc trebucbd trebucbi trebucit verdana verdanab
        verdanai verdanaz webdings)

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)    shift; PREFIX="${1:?--prefix needs a directory}" ;;
        --wine)      shift; WINE="${1:?--wine needs a binary}" ;;
        --check)     CHECK_ONLY=1 ;;
        --no-mscore) DO_MSCORE=0 ;;
        -h|--help)   sed -n '2,37p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1  (try --help)" >&2; exit 1 ;;
    esac
    shift
done

FONTDIR="$PREFIX/drive_c/windows/Fonts"
SYSREG="$PREFIX/system.reg"

[ -d "$PREFIX" ]  || { echo "ERROR: prefix not found: $PREFIX" >&2; exit 1; }
[ -d "$FONTDIR" ] || { echo "ERROR: no Fonts directory in $PREFIX" >&2; exit 1; }
command -v fc-list >/dev/null 2>&1 || {
    echo "ERROR: fc-list not found — install fontconfig." >&2; exit 1; }

# Locate a font file by name, case-insensitively, wherever the distro keeps it.
find_font() {
    fc-list --format='%{file}\n' 2>/dev/null | grep -iE "/$1\$" | head -1
}

echo "Prefix: $PREFIX"
echo

# --- 1. are the required fonts available on the host? ------------------------
echo "Looking for the required fonts..."
missing=0
for f in "${REQUIRED[@]}"; do
    src=$(find_font "$f")
    if [ -n "$src" ]; then
        printf '  %-30s %s\n' "$f" "$src"
    else
        printf '  %-30s NOT INSTALLED\n' "$f"
        missing=$((missing + 1))
    fi
done

if [ "$missing" -gt 0 ]; then
    echo
    echo "=> Install them first (package names vary by distribution):"
    echo "     Debian/Ubuntu : fonts-dejavu-core  fonts-noto-core"
    echo "     Fedora        : dejavu-sans-fonts  google-noto-sans-symbols2-fonts"
    echo "     Arch          : ttf-dejavu         noto-fonts"
    exit 2
fi
echo

# --- 2. what is already in place? --------------------------------------------
have_fonts=1
for f in "${REQUIRED[@]}"; do
    [ -f "$FONTDIR/$f" ] || have_fonts=0
done
# Look inside the FontLink\SystemLink key and check for BOTH entries this script
# writes.  Grepping the whole of system.reg for a font name does not work: Wine
# registers every system font under Windows\CurrentVersion\Fonts, so even an
# untouched prefix matches "NotoSansSymbols2" and the check would always pass.
fontlink_block() {
    awk '/^\[Software\\\\Microsoft\\\\Windows NT\\\\CurrentVersion\\\\FontLink\\\\SystemLink\]/{f=1;next}
         f && /^\[/{exit} f' "$SYSREG" 2>/dev/null
}
have_link=0
{
    blk=$(fontlink_block)
    printf '%s\n' "$blk" | grep -q '^"Tahoma"=.*DejaVuSans\.ttf' &&
    printf '%s\n' "$blk" | grep -q '^"DejaVu Sans"=.*NotoSansSymbols2' &&
    have_link=1
} 2>/dev/null || true

echo "Current state of the prefix:"
printf '  %-34s %s\n' "fonts in windows/Fonts" \
    "$( [ "$have_fonts" -eq 1 ] && echo present || echo missing )"
printf '  %-34s %s\n' "FontLink symbol fallback" \
    "$( [ "$have_link" -eq 1 ] && echo present || echo missing )"

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo
    if [ "$have_fonts" -eq 1 ] && [ "$have_link" -eq 1 ]; then
        echo "Font setup is complete."
        exit 0
    fi
    echo "Font setup is incomplete. Run without --check to apply it."
    exit 3
fi

command -v "$WINE" >/dev/null 2>&1 || {
    echo "ERROR: wine binary not found: $WINE  (use --wine /path/to/wine)" >&2
    exit 1; }

# --- 3. copy the fonts into the prefix ---------------------------------------
echo
echo "Copying fonts into the prefix..."
copied=0
for f in "${REQUIRED[@]}"; do
    src=$(find_font "$f")
    cp -f "$src" "$FONTDIR/$f" && copied=$((copied + 1))
done

# MS Core Fonts live in one directory per distribution; derive it from arial.
reg_lines=()
if [ "$DO_MSCORE" -eq 1 ]; then
    arial=$(find_font "arial.ttf")
    if [ -n "$arial" ]; then
        dir=$(dirname "$arial")
        for base in "${MSCORE[@]}"; do
            # match case-insensitively, the packaging differs between distros
            for cand in "$dir"/*.ttf; do
                [ -f "$cand" ] || continue
                b=$(basename "$cand"); b_lc=$(echo "$b" | tr 'A-Z' 'a-z')
                [ "$b_lc" = "$base.ttf" ] || continue
                cp -f "$cand" "$FONTDIR/$b"
                copied=$((copied + 1))
                reg_lines+=("\"${base} (TrueType)\"=\"${b}\"")
                break
            done
        done
        echo "  MS Core Fonts from $dir"
    else
        echo "  MS Core Fonts not installed — skipping (install msttcorefonts to"
        echo "  avoid crashes in plugins that open font files directly)"
    fi
fi
echo "  $copied font file(s) copied"

# --- 4. registry ---------------------------------------------------------------
stamp=$(date +%Y%m%d-%H%M%S)
for r in system.reg user.reg; do
    [ -f "$PREFIX/$r" ] && cp "$PREFIX/$r" "$PREFIX/$r.bak-$stamp"
done
echo "  Registry backed up as *.reg.bak-$stamp"

# regedit is a Windows program: hand it a WINDOWS path. A Unix path is read as a
# Windows path and the import silently does nothing.
if [ ${#reg_lines[@]} -gt 0 ]; then
    reg_file="$PREFIX/drive_c/wine-font-setup.reg"
    {
        echo "REGEDIT4"
        echo
        echo '[HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\Fonts]'
        printf '%s\n' "${reg_lines[@]}"
    } > "$reg_file"
    WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" regedit /S 'C:\wine-font-setup.reg' \
        </dev/null >/dev/null 2>&1
    rm -f "$reg_file"
    echo "  registered ${#reg_lines[@]} MS Core Font(s) for GDI"
fi

# FontLink is REG_MULTI_SZ; `reg add` takes \0 separators directly, which is far
# easier than hex-encoding it into a .reg file.
echo "  setting the FontLink symbol fallback..."
WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" reg add \
    'HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink' \
    /v "Tahoma" /t REG_MULTI_SZ \
    /d "DejaVuSans.ttf,DejaVu Sans\0NotoSansSymbols2-Regular.ttf,Noto Sans Symbols2\0MSGOTHIC.TTC,MS UI Gothic\0MINGLIU.TTC,PMingLiU\0SIMSUN.TTC,SimSun\0GULIM.TTC,Gulim\0YUGOTHM.TTC,Yu Gothic UI\0MSJH.TTC,Microsoft JhengHei UI\0MSYH.TTC,Microsoft YaHei UI\0MALGUN.TTF,Malgun Gothic\0SEGUISYM.TTF,Segoe UI Symbol" /f \
    </dev/null >/dev/null 2>&1

WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" reg add \
    'HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink' \
    /v "DejaVu Sans" /t REG_MULTI_SZ \
    /d "NotoSansSymbols2-Regular.ttf,Noto Sans Symbols2" /f \
    </dev/null >/dev/null 2>&1

# --- 5. verify ---------------------------------------------------------------
# Wine keeps the registry in the wineserver and flushes system.reg to disk with
# a delay — checking immediately reports a false failure. Poll with a cap.
for _ in $(seq 1 15); do
    printf '%s\n' "$(fontlink_block)" | grep -q '^"DejaVu Sans"=.*NotoSansSymbols2' && break
    sleep 1
done

ok=1
for f in "${REQUIRED[@]}"; do
    [ -f "$FONTDIR/$f" ] || { echo "ERROR: $f was not copied into $FONTDIR" >&2; ok=0; }
done
printf '%s\n' "$(fontlink_block)" | grep -q '^"DejaVu Sans"=.*NotoSansSymbols2' || {
    echo "ERROR: the FontLink entry did not appear in $SYSREG." >&2
    echo "       Is '$WINE' the right binary for this prefix?" >&2
    ok=0; }

echo
if [ "$ok" -eq 1 ]; then
    echo "Font setup complete."
    echo "Re-run this after 'wineboot -u' — it resets the FontLink entries."
    exit 0
fi
exit 1
