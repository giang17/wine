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
#   3. The text rendering switches this branch reads at startup, and the system
#      font smoothing type they build on.  All of them are registry values and
#      therefore live in the PREFIX, not in the build: a fresh prefix does not
#      have them, and the text then renders the way stock Wine renders it, with
#      no hint that anything is switched off.
#
# Fonts are located through fontconfig, so distribution paths do not matter.
#
# NOTE: Wine rewrites the FontLink entries with its own defaults whenever the
# codepage record under HKCU\Software\Wine\Fonts\Codepages does not match the
# running process (fresh prefix, locale change; seen after a `wineboot -u` too).
# The code page follows LC_CTYPE, so one Wine process started under LC_ALL=C
# (or LANG=en_US on a de_DE desktop) is enough: it records 1252,437, the next
# process records 1252,850 again, and both rewrites drop step 2.  Since
# 2026-09-02 this branch carries the two fallback entries in win32u's own
# defaults, so the rewrite keeps them; on stock Wine re-run this script, it
# is idempotent.
#
# Usage:
#   wine-font-setup.sh [--prefix DIR] [--wine BINARY] [--check] [--no-mscore]
#                      [--no-rendering] [--contrast N]
#
#   --prefix DIR    Wine prefix to operate on.  Default: $WINEPREFIX, else ~/.wine
#   --wine BINARY   wine binary to use.  Default: wine
#   --check         report only, change nothing
#   --no-mscore     skip the MS Core Fonts part (step 1)
#   --no-rendering  skip the text rendering switches (step 3)
#   --contrast N    enhanced contrast, 0-100.  Default: 50, what Windows uses.
#                   70 suits dark interfaces; 0 turns the correction off.
#
# Exit codes: 0 done / 1 usage or prefix error / 2 required fonts not installed
#             3 setup incomplete (only with --check)

set -u

PREFIX="${WINEPREFIX:-$HOME/.wine}"
WINE="wine"
CHECK_ONLY=0
DO_MSCORE=1
DO_RENDERING=1
CONTRAST=50
CONTRAST_EXPLICIT=0

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
        --no-rendering) DO_RENDERING=0 ;;
        --contrast)  shift; CONTRAST="${1:?--contrast needs a number 0-100}"; CONTRAST_EXPLICIT=1 ;;
        -h|--help)   sed -n '2,37p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1  (try --help)" >&2; exit 1 ;;
    esac
    shift
done

FONTDIR="$PREFIX/drive_c/windows/Fonts"
SYSREG="$PREFIX/system.reg"
USERREG="$PREFIX/user.reg"

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

# A REG_MULTI_SZ whose entries are all one character long is a string that was
# encoded to UTF-16 twice: every character ended up followed by a NUL, so
# load_system_links() reads "SEGUISYM.TTF,Segoe UI Symbol" as 28 separate
# file,face entries and looks up every one of them.  Nothing fails loudly — the
# chain is simply empty, and each process start spends 28 lookups per value on
# it.  Ten values in this state went unnoticed in a prefix for six months, so
# report them; the correct content cannot be guessed, which is why they are only
# reported and never rewritten.
mangled_links=$(printf '%s\n' "$blk" \
    | grep -cE '^"[^"]+"=str\(7\):"([^\\]\\0){3}' 2>/dev/null) || mangled_links=0

# The rendering switches sit in HKCU, so they are in user.reg.  All three are
# read once when d2d1/dwrite load, so a running application does not pick up a
# change — it has to be restarted.
reg_value() {  # <file> <section> <value>
    # Passed through the environment, not with -v: awk expands escape sequences
    # in -v assignments, which would turn the doubled backslashes of a registry
    # path into single ones and never match.
    RV_SEC="$2" RV_VAL="$3" awk '
        index($0, "[" ENVIRON["RV_SEC"] "]") == 1 {f=1; next}   # Wine appends a timestamp
        f && /^\[/ {exit}
        f && index($0, "\"" ENVIRON["RV_VAL"] "\"=") == 1 {print; exit}' "$1" 2>/dev/null
}
# text_enhanced_contrast is deliberately not required here: winecfg stores
# "Off" as the absence of the value (d2d1 then keeps DirectWrite's default),
# so a missing value is a choice as often as it is a gap.
have_rendering=0
{
    [ -n "$(reg_value "$USERREG" 'Software\\Wine\\Direct2D'    text_linear_blend)" ] &&
    [ -n "$(reg_value "$USERREG" 'Software\\Wine\\DirectWrite' outline_in_natural_modes)" ] &&
    [ "$(reg_value "$USERREG" 'Control Panel\\Desktop' FontSmoothingType)" \
        = '"FontSmoothingType"=dword:00000002' ] &&
    have_rendering=1
} 2>/dev/null || true
[ "$DO_RENDERING" -eq 1 ] || have_rendering=1   # not asked for, do not report it missing

# Segoe UI is proprietary and ships with no distribution, so this script cannot
# install it.  It is reported anyway, because one application family depends on
# it in a way that does not fail gracefully: Steinberg's framework looks the
# family up by name in the DirectWrite system collection and dereferences the
# result without a check.  Cubase 15 then shows its Hub without any Direct2D
# text and crashes in hubservice.dll (NULL read) a second after the Hub content
# has downloaded (issue 345, 2026-09-05).  A prefix cloned from one that never
# had the family inherits the gap silently.  Copy segoeui*.ttf and segui*.ttf
# from a prefix that has them and register them under the Fonts key.
have_segoe=0
[ -f "$FONTDIR/segoeui.ttf" ] && grep -q '^"Segoe UI (TrueType)"=' "$SYSREG" 2>/dev/null \
    && have_segoe=1

echo "Current state of the prefix:"
printf '  %-34s %s\n' "fonts in windows/Fonts" \
    "$( [ "$have_fonts" -eq 1 ] && echo present || echo missing )"
printf '  %-34s %s\n' "FontLink symbol fallback" \
    "$( [ "$have_link" -eq 1 ] && echo present || echo missing )"
printf '  %-34s %s\n' "text rendering switches" \
    "$( [ "$have_rendering" -eq 1 ] && echo present || echo missing )"
printf '  %-34s %s\n' "SystemLink values well-formed" \
    "$( [ "$mangled_links" -eq 0 ] && echo yes \
        || echo "no — $mangled_links value(s) encoded twice" )"
printf '  %-34s %s\n' "Segoe UI (Steinberg applications)" \
    "$( [ "$have_segoe" -eq 1 ] && echo present \
        || echo "absent — Cubase 15 Hub crashes without it; not installable by this script" )"
if [ "$mangled_links" -gt 0 ]; then
    echo
    echo "  These SystemLink values hold one character per entry:"
    printf '%s\n' "$blk" | grep -E '^"[^"]+"=str\(7\):"([^\\]\\0){3}' \
        | cut -d'"' -f2 | sed 's/^/    /'
    echo "  Delete them, or rewrite them with"
    echo "    wine reg add 'HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink' \\"
    echo "      /v '<family>' /t REG_MULTI_SZ /d '<file>,<face>' /f"
fi

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo
    if [ "$have_fonts" -eq 1 ] && [ "$have_link" -eq 1 ] && [ "$have_rendering" -eq 1 ] &&
       [ "$mangled_links" -eq 0 ]; then
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

# --- 4b. text rendering switches ---------------------------------------------
# This branch renders ClearType text closer to what Windows does, but every part
# of it is opt-in: the code reads these values once at startup and falls back to
# stock behaviour when they are absent.  Without them a plug-in GUI looks like
# the fix was never built, which is indistinguishable from a broken build.
#   text_enhanced_contrast    stem darkening, in hundredths.  Windows uses 50.
#   text_linear_blend         blend subpixel coverage in linear space
#   outline_in_natural_modes  rasterise from the outline instead of an embedded
#                             bitmap strike, so hinted fonts keep their shape
if [ "$DO_RENDERING" -eq 1 ]; then
    echo "  setting the text rendering switches..."
    # Contrast is a matter of taste — winecfg offers Off/50/70 in the graphics
    # tab, and stores Off as the absence of the value, which looks exactly like
    # a prefix this script has never seen.  The other switch tells the two
    # apart: with text_linear_blend already set this script has run before, and
    # an absent contrast is somebody's choice.  Never overwrite that silently;
    # write the contrast on the first run or when --contrast says so explicitly.
    cur_contrast=$(reg_value "$USERREG" 'Software\\Wine\\Direct2D' text_enhanced_contrast)
    cur_blend=$(reg_value "$USERREG" 'Software\\Wine\\Direct2D' text_linear_blend)
    if [ "$CONTRAST_EXPLICIT" -eq 1 ] || { [ -z "$cur_contrast" ] && [ -z "$cur_blend" ]; }; then
        echo "    enhanced contrast: $CONTRAST"
        WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" reg add 'HKCU\Software\Wine\Direct2D' \
            /v text_enhanced_contrast /t REG_DWORD /d "$CONTRAST" /f </dev/null >/dev/null 2>&1
    elif [ -n "$cur_contrast" ]; then
        echo "    enhanced contrast: keeping $(printf '%d' "0x${cur_contrast##*:}" 2>/dev/null || echo '?') (already set; --contrast N overrides)"
    else
        echo "    enhanced contrast: left unset (winecfg Off; --contrast N overrides)"
    fi
    WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" reg add 'HKCU\Software\Wine\Direct2D' \
        /v text_linear_blend /t REG_DWORD /d 1 /f </dev/null >/dev/null 2>&1
    WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" reg add 'HKCU\Software\Wine\DirectWrite' \
        /v outline_in_natural_modes /t REG_DWORD /d 1 /f </dev/null >/dev/null 2>&1
    # Everything above builds on the system font smoothing type.  Wine defaults
    # it to FE_FONTSMOOTHINGSTANDARD, i.e. greyscale, and dwrite then reports a
    # ClearType level of zero — d2d1 does not promote DWRITE_TEXT_ANTIALIAS_MODE_
    # DEFAULT to subpixel, and every switch above has no visible effect.  The
    # d2d1 switches still announce themselves in the log while the text stays
    # greyscale, which is a confusing place to end up.
    echo "    font smoothing: ClearType"
    WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" reg add 'HKCU\Control Panel\Desktop' \
        /v FontSmoothingType /t REG_DWORD /d 2 /f </dev/null >/dev/null 2>&1
fi

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
if [ "$DO_RENDERING" -eq 1 ]; then
    for _ in $(seq 1 15); do
        [ -n "$(reg_value "$USERREG" 'Software\\Wine\\DirectWrite' outline_in_natural_modes)" ] && break
        sleep 1
    done
    # Single quotes: in double quotes the shell would collapse the doubled
    # backslashes of a registry path into single ones and nothing would match.
    for kv in 'Software\\Wine\\Direct2D:text_linear_blend' \
              'Software\\Wine\\DirectWrite:outline_in_natural_modes' \
              'Control Panel\\Desktop:FontSmoothingType'; do
        [ -n "$(reg_value "$USERREG" "${kv%%:*}" "${kv##*:}")" ] || {
            echo "ERROR: ${kv##*:} did not appear in $USERREG." >&2; ok=0; }
    done
fi

echo
if [ "$ok" -eq 1 ]; then
    echo "Font setup complete."
    echo "On stock Wine re-run this after 'wineboot -u' — it resets the FontLink entries."
    exit 0
fi
exit 1
