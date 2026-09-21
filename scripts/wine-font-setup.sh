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
#
#      Where the family cannot be packaged for licensing reasons — Fedora has no
#      mscore-fonts package and cannot have one — two separate gaps open up, and
#      they need different answers.  A file opened BY PATH only has to exist and
#      parse, so a metric-compatible free face copied under the MS file name
#      closes that one offline.  A family looked up BY NAME needs an entry in the
#      GDI FontSubstitutes key: the value name in the Fonts key is not the family,
#      win32u reads that from the file itself (load_registry_fonts() hands the
#      path to add_font_resource()), so a renamed file alone leaves "Arial"
#      unresolvable.  Both are applied when the genuine fonts are not on the host;
#      --winetricks fetches the real ones instead.
#   2. DejaVu Sans and Noto Sans Symbols2, plus the GDI FontLink entries that
#      make them the first fallback for symbol glyphs (star ratings, arrows in
#      Serum 2 show up as tofu boxes otherwise).
#   3. The text rendering switches this branch reads at startup, and the system
#      font smoothing type they build on.  All of them are registry values and
#      therefore live in the PREFIX, not in the build: a fresh prefix does not
#      have them, and the text then renders the way stock Wine renders it, with
#      no hint that anything is switched off.
#   4. The system UI font.  Wine's WindowMetrics defaults name Tahoma 8pt for
#      menus, dialogs, captions and status bars, and "Tahoma" resolves to
#      Wine's own clone unless the real one was installed; its outlines were
#      never drawn for 11-12 px, so with step 3 the interface text of every
#      application looks cramped.  Windows 10 reports Segoe UI 9pt there.  As
#      soon as the Segoe UI family is in the prefix, the six LOGFONTs are
#      switched to it; without the family the step is skipped.
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
#                      [--winetricks] [--no-rendering] [--no-uifont] [--contrast N]
#
#   --prefix DIR    Wine prefix to operate on.  Default: $WINEPREFIX, else ~/.wine
#   --wine BINARY   wine binary to use.  Default: wine
#   --check         report only, change nothing
#   --no-mscore     skip the MS Core Fonts part (step 1)
#   --winetricks    when the MS Core Fonts are not on the host, install the
#                   genuine ones with `winetricks -q corefonts` instead of the
#                   free stand-ins.  This downloads them and accepts the
#                   Microsoft EULA on your behalf, which is why it is opt-in.
#   --no-rendering  skip the text rendering switches (step 3)
#   --no-uifont     keep the WindowMetrics fonts, the system UI font (step 4)
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
DO_WINETRICKS=0
DO_RENDERING=1
DO_UIFONT=1
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

# Free stand-ins, used when the genuine family is not on the host.  Liberation
# Sans, Serif and Mono were drawn to the metrics of Arial, Times New Roman and
# Courier New, so line breaks and dialog layouts land where the application
# expects them; the rest of the table is a visual approximation and nothing more.
# Two groups are deliberately absent.  The symbol faces (Webdings, Wingdings):
# pointing them at a Latin face draws letters where the application asked for
# symbols, which is worse than the family being missing.  And Tahoma: Wine ships
# its own, and the FontLink chain of step 2 is keyed on that name.  Arial Narrow
# is absent for a third reason: "Liberation Sans Narrow" is a family on the host,
# but DirectWrite groups its faces under the WWS family "Liberation Sans", so the
# substitute would point at a name the collection does not carry and dwrite would
# fall back to the system message font — measurably worse than no entry at all.
MSCORE_FAMILY_SUBST=(
    "Arial|Liberation Sans"
    "Times New Roman|Liberation Serif"
    "Courier New|Liberation Mono"
    "Verdana|DejaVu Sans"
    "Georgia|DejaVu Serif"
    "Trebuchet MS|DejaVu Sans"
    "Comic Sans MS|DejaVu Sans"
    "Andale Mono|DejaVu Sans Mono"
    "Impact|DejaVu Sans"
)

# The file names plugins are known to open by path, and the face to put there.
# Only the four metric-compatible groups: a stand-in that is not metric
# compatible still stops the crash, but it may not be what the caller draws with.
MSCORE_FILE_SUBST=(
    "arial.ttf|LiberationSans-Regular.ttf"
    "arialbd.ttf|LiberationSans-Bold.ttf"
    "ariali.ttf|LiberationSans-Italic.ttf"
    "arialbi.ttf|LiberationSans-BoldItalic.ttf"
    "times.ttf|LiberationSerif-Regular.ttf"
    "timesbd.ttf|LiberationSerif-Bold.ttf"
    "timesi.ttf|LiberationSerif-Italic.ttf"
    "timesbi.ttf|LiberationSerif-BoldItalic.ttf"
    "cour.ttf|LiberationMono-Regular.ttf"
    "courbd.ttf|LiberationMono-Bold.ttf"
    "couri.ttf|LiberationMono-Italic.ttf"
    "courbi.ttf|LiberationMono-BoldItalic.ttf"
    "verdana.ttf|DejaVuSans.ttf"
    "verdanab.ttf|DejaVuSans-Bold.ttf"
    "verdanai.ttf|DejaVuSans-Oblique.ttf"
    "verdanaz.ttf|DejaVuSans-BoldOblique.ttf"
)

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)    shift; PREFIX="${1:?--prefix needs a directory}" ;;
        --wine)      shift; WINE="${1:?--wine needs a binary}" ;;
        --check)     CHECK_ONLY=1 ;;
        --no-mscore) DO_MSCORE=0 ;;
        --winetricks) DO_WINETRICKS=1 ;;
        --no-rendering) DO_RENDERING=0 ;;
        --no-uifont) DO_UIFONT=0 ;;
        --contrast)  shift; CONTRAST="${1:?--contrast needs a number 0-100}"; CONTRAST_EXPLICIT=1 ;;
        -h|--help)   awk 'NR > 1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
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

# Is a FAMILY (not a file) on the host?  fc-list reports the localised names of
# one family as a comma separated list, so split before matching.
have_family() {
    fc-list --format='%{family}\n' 2>/dev/null | tr ',' '\n' | grep -qxiF "$1"
}

# A file in the prefix' Fonts directory, matched without regard to case: the
# distributions ship "Arial.ttf", the plugins ask for "arialbd.ttf".
prefix_font() {
    local f base want=${1,,}
    for f in "$FONTDIR"/*; do
        base=${f##*/}
        [ -f "$f" ] && [ "${base,,}" = "$want" ] && { printf '%s\n' "$base"; return 0; }
    done
    return 1
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

# DirectWrite family substitutes (issue 384).  Applications hardcode the UI
# families every Windows ships — KORG Legacy Cell asks DirectWrite for Meiryo UI —
# and JUCE 5 to 7 answer a missing family with family 0 of the collection, i.e.
# whatever sorts first.  This branch's dwrite resolves a miss through the GDI
# FontSubstitutes key, so the entries below point those families at Segoe UI
# (dwrite falls back to the system message font when Segoe UI is absent too).
SUBST_FAMILIES=("Malgun Gothic" "Meiryo" "Meiryo UI" "Microsoft JhengHei UI" "Microsoft YaHei UI" "Yu Gothic UI")
SUBST_KEY='Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes'
n_subst=0
for fam in "${SUBST_FAMILIES[@]}"; do
    [ -n "$(reg_value "$SYSREG" "$SUBST_KEY" "$fam")" ] && n_subst=$((n_subst + 1))
done
have_subst=0; [ "$n_subst" -eq "${#SUBST_FAMILIES[@]}" ] && have_subst=1

# MS Core Fonts: genuine files, free stand-ins, or nothing at all.  "Arial" is
# the representative — it is the family the known crash cases open by path and
# the one applications ask for by name most often.
have_arial_file=0; [ -n "$(prefix_font arial.ttf)" ] && have_arial_file=1
subst_arial=0
[ -n "$(reg_value "$SYSREG" "$SUBST_KEY" "Arial")" ] && subst_arial=1
if [ "$subst_arial" -eq 1 ]; then
    mscore_state="free stand-ins (Liberation/DejaVu) — --winetricks installs the genuine set"
elif [ "$have_arial_file" -eq 1 ]; then
    mscore_state="present in the prefix"
else
    mscore_state="absent — plugins that open font files by path crash"
fi
have_mscore=0
{ [ "$have_arial_file" -eq 1 ] || [ "$subst_arial" -eq 1 ]; } && have_mscore=1
[ "$DO_MSCORE" -eq 1 ] || have_mscore=1   # not asked for, do not report it missing

# The system UI font.  The six WindowMetrics values are LOGFONTW blobs of 92
# bytes: the height at bytes 0-3 (negative = pixels at the prefix' system DPI,
# positive = points), the face name at byte 28 as UTF-16LE.  Applications read
# them once at start, so a change needs a restart, not a wineserver restart.
UIFONT_VALUES=(CaptionFont SmCaptionFont MenuFont StatusFont MessageFont IconFont)
windowmetrics_hex() {  # <value> — the hex of one WindowMetrics blob, comma-separated
    RV_VAL="$1" awk '
        index($0, "[Control Panel\\\\Desktop\\\\WindowMetrics]") == 1 {f=1; next}
        f && /^\[/ {exit}
        f && index($0, "\"" ENVIRON["RV_VAL"] "\"=hex:") == 1 {sub(/^[^:]*:/, ""); v=$0; c=1; next}
        c && /^[ \t]*[0-9a-f][0-9a-f],/ {v = v $0; next}
        c {exit}
        END {gsub(/[\\ \t]/, "", v); print v}' "$USERREG" 2>/dev/null
}
uifont_face() {  # <value> — face name of one WindowMetrics blob, empty if unset
    windowmetrics_hex "$1" | awk -F, '
        BEGIN { for (i = 32; i < 127; i++) chr[sprintf("%02x", i)] = sprintf("%c", i) }
        NF >= 30 { s = ""
            for (i = 29; i + 1 <= NF; i += 2) {
                if ($i == "00" && $(i+1) == "00") break
                s = s (($(i+1) == "00" && ($i in chr)) ? chr[$i] : "?") }
            print s }'
}
prefix_dpi() {  # the prefix' system DPI (LogPixels), 96 when unset
    local v; v=$(reg_value "$USERREG" 'Control Panel\\Desktop' LogPixels)
    case "$v" in *dword:*) echo $(( 16#${v##*:} )) ;; *) echo 96 ;; esac
}
uifont_points() {  # <value> — point size of one WindowMetrics blob, empty if unset
    windowmetrics_hex "$1" | DPI="$(prefix_dpi)" awk -F, '
        BEGIN { for (i = 0; i < 256; i++) val[sprintf("%02x", i)] = i }
        NF >= 4 { h = val[$1] + val[$2] * 256 + val[$3] * 65536 + val[$4] * 16777216
            if (h >= 2147483648) h -= 4294967296
            if (h < 0) printf "%d\n", (-h * 72 + ENVIRON["DPI"] / 2) / ENVIRON["DPI"]
            else printf "%d\n", h }'
}
menu_face=$(uifont_face MenuFont); menu_pt=$(uifont_points MenuFont)
have_uifont=0
[ "$menu_face" = "Segoe UI" ] && have_uifont=1
# Not asked for, or not possible without the family: do not report it missing.
{ [ "$DO_UIFONT" -eq 1 ] && [ "$have_segoe" -eq 1 ]; } || have_uifont=1

echo "Current state of the prefix:"
printf '  %-34s %s\n' "fonts in windows/Fonts" \
    "$( [ "$have_fonts" -eq 1 ] && echo present || echo missing )"
printf '  %-34s %s\n' "MS Core Fonts" "$mscore_state"
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
printf '  %-34s %s\n' "DirectWrite family substitutes" \
    "$( [ "$have_subst" -eq 1 ] && echo "present (${#SUBST_FAMILIES[@]} UI families -> Segoe UI)" \
        || echo "missing ($n_subst of ${#SUBST_FAMILIES[@]})" )"
printf '  %-34s %s\n' "system UI font (WindowMetrics)" \
    "$( if [ "$menu_face" = "Segoe UI" ]; then echo "Segoe UI ${menu_pt}pt"; \
        elif [ "$have_segoe" -eq 1 ]; then echo "${menu_face:-unset} ${menu_pt:+${menu_pt}pt }— Wine default; Segoe UI 9pt on apply"; \
        else echo "${menu_face:-unset} ${menu_pt:+${menu_pt}pt }— Wine default; needs the Segoe UI family first"; fi )"
# Fonts that applications registered with AddFontMemResourceEx and that win32u
# kept in the prefix (issue 383): informational, never counted as a gap.
n_embedded=$(ls "$FONTDIR/wine-embedded" 2>/dev/null | wc -l)
printf '  %-34s %s\n' "embedded fonts kept by win32u" \
    "$( [ "$n_embedded" -gt 0 ] && echo "$n_embedded file(s) — listed under HKCU\\Software\\Wine\\Fonts\\Embedded Fonts" || echo none )"
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
       [ "$mangled_links" -eq 0 ] && [ "$have_uifont" -eq 1 ] && [ "$have_subst" -eq 1 ] &&
       [ "$have_mscore" -eq 1 ]; then
        echo "Font setup is complete."
        exit 0
    fi
    echo "Font setup is incomplete. Run without --check to apply it."
    exit 3
fi

command -v "$WINE" >/dev/null 2>&1 || {
    echo "ERROR: wine binary not found: $WINE  (use --wine /path/to/wine)" >&2
    exit 1; }

# The backup comes before the first write, not before the registry section:
# --winetricks writes to the registry itself, from inside the copy step.
echo
echo "Backing up the registry..."
stamp=$(date +%Y%m%d-%H%M%S)
for r in system.reg user.reg; do
    [ -f "$PREFIX/$r" ] && cp "$PREFIX/$r" "$PREFIX/$r.bak-$stamp"
done
echo "  saved as *.reg.bak-$stamp"

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
subst_lines=()
arial=""
if [ "$DO_MSCORE" -eq 1 ]; then
    arial=$(find_font "arial.ttf")

    # --winetricks runs here, and it has to run HERE: winetricks starts wine
    # several times, and a start whose code page does not match the record under
    # HKCU\Software\Wine\Fonts\Codepages makes Wine rewrite the FontLink
    # SystemLink key with its own defaults.  Ahead of step 4a that costs nothing;
    # after it, it would drop the symbol fallback on stock Wine.  winetricks
    # takes the wine binary from $WINE, so --wine is honoured.
    if [ -z "$arial" ] && [ "$DO_WINETRICKS" -eq 1 ]; then
        if command -v winetricks >/dev/null 2>&1; then
            echo "  fetching the genuine MS Core Fonts with winetricks..."
            WINE="$WINE" WINEPREFIX="$PREFIX" winetricks -q corefonts </dev/null
            wt_rc=$?
            if [ "$wt_rc" -eq 0 ] && [ -n "$(prefix_font arial.ttf)" ]; then
                echo "  MS Core Fonts installed by winetricks (registered by it too)"
                DO_MSCORE=0     # nothing left for the branches below
            else
                # Both the network and the local downloader are in that path:
                # winetricks fetches from third party mirrors with the checksums
                # pinned in its own source, and picks up whatever downloader is
                # installed.  A snap confined aria2c, for instance, cannot write
                # to ~/.cache/winetricks and fails every download (seen
                # 2026-09-21); WINETRICKS_DOWNLOADER=curl rules that one out.
                echo "  WARNING: winetricks did not install them (exit $wt_rc) — the"
                echo "  reason is in its output above.  WINETRICKS_DOWNLOADER=curl rules"
                echo "  out a broken local downloader.  Falling back to the stand-ins."
            fi
        else
            echo "  WARNING: --winetricks given, but winetricks is not installed"
        fi
    fi
fi
if [ "$DO_MSCORE" -eq 1 ] && [ -n "$arial" ]; then
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
elif [ "$DO_MSCORE" -eq 1 ]; then
    # Nothing on the host and no download asked for: close both gaps with what
    # the distribution does ship.  The files stop the crashes of plugins that
    # open a path, the FontSubstitutes entries make the families resolvable for
    # GDI and for this branch's dwrite — neither replaces the other.
    n_file_subst=0
    for e in "${MSCORE_FILE_SUBST[@]}"; do
        tgt=${e%%|*}; srcname=${e##*|}
        [ -n "$(prefix_font "$tgt")" ] && continue        # never shadow a real one
        src=$(find_font "$srcname")
        [ -n "$src" ] || continue
        cp -f "$src" "$FONTDIR/$tgt" || continue
        copied=$((copied + 1)); n_file_subst=$((n_file_subst + 1))
        # Registering them is not optional.  A file that only sits in the Fonts
        # directory is enough for PathFileExistsW, but Wine drops the host copy
        # of the same face from its external font list over it and never loads
        # the prefix one, so the family disappears from the collection
        # altogether: measured 2026-09-21, Liberation Serif and Liberation Mono
        # gone and "Times New Roman" resolving to Tahoma, the system message
        # font dwrite falls back to.  The value name is cosmetic — win32u takes
        # the family from the file — but it must not collide with a full name
        # that already exists, or load_registry_fonts() skips the entry.
        reg_lines+=("\"${tgt%.ttf} (TrueType)\"=\"${tgt}\"")
    done
    for e in "${MSCORE_FAMILY_SUBST[@]}"; do
        fam=${e%%|*}; to=${e##*|}
        [ -n "$(reg_value "$SYSREG" "$SUBST_KEY" "$fam")" ] && continue
        have_family "$to" || continue
        subst_lines+=("$fam|$to")
    done
    if [ "$n_file_subst" -eq 0 ] && [ ${#subst_lines[@]} -eq 0 ]; then
        echo "  MS Core Fonts: free stand-ins already in place"
    else
        echo "  MS Core Fonts not installed on the host — using free stand-ins:"
        echo "    $n_file_subst file(s) under the MS names, ${#subst_lines[@]} family substitute(s)"
        echo "    metrics match for Arial, Times New Roman and Courier New only;"
        echo "    Verdana, Georgia, Impact, Comic Sans, Trebuchet and Andale shift."
        echo "    Segoe UI cannot be covered this way and is not in winetricks either."
        echo "  For the genuine files instead:"
        echo "    re-run with --winetricks, or:  WINEPREFIX=$PREFIX winetricks -q corefonts"
        echo "    Debian/Ubuntu: ttf-mscorefonts-installer   Arch: ttf-ms-fonts"
        echo "    Fedora ships no package — the family is not redistributable."
    fi
fi
echo "  $copied font file(s) copied"

# --- 4. registry ---------------------------------------------------------------
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

# --- 4a1. MS Core Font family substitutes (only when the genuine ones are absent)
# Collected in step 3, written here, where the registry backup is already taken.
if [ ${#subst_lines[@]} -gt 0 ]; then
    echo "  setting the MS Core Font substitutes (-> Liberation/DejaVu)..."
    for e in "${subst_lines[@]}"; do
        WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" reg add \
            'HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontSubstitutes' \
            /v "${e%%|*}" /t REG_SZ /d "${e##*|}" /f </dev/null >/dev/null 2>&1
    done
fi

# --- 4a2. DirectWrite family substitutes (issue 384) ---------------------------
# Written only when absent, so a deliberate different target survives a re-run.
if [ "$have_subst" -eq 0 ]; then
    echo "  setting the DirectWrite family substitutes (-> Segoe UI)..."
    for fam in "${SUBST_FAMILIES[@]}"; do
        [ -n "$(reg_value "$SYSREG" "$SUBST_KEY" "$fam")" ] && continue
        WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" reg add \
            'HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontSubstitutes' \
            /v "$fam" /t REG_SZ /d "Segoe UI" /f </dev/null >/dev/null 2>&1
    done
fi

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

# --- 4c. system UI font --------------------------------------------------------
# Wine's WindowMetrics defaults come from DEFAULT_GUI_FONT (MS Shell Dlg ->
# Tahoma, 8pt), and "Tahoma" is Wine's own clone in every prefix that never got
# the real one.  Plain Win32, Qt and VSTGUI menus and dialogs all draw with
# these six LOGFONTs, so with step 3 the interface text of every application in
# the prefix is the clone's outline at 11-12 px, which looks cramped (Dorico 5,
# 2026-09-08).  Windows 10 reports Segoe UI 9pt for all six.  The height is
# stored at the prefix' system DPI, so it is derived from LogPixels; the other
# fields are what Wine writes itself (weight 400, DEFAULT_CHARSET).
if [ "$DO_UIFONT" -eq 1 ] && [ "$have_segoe" -eq 1 ]; then
    dpi=$(prefix_dpi)
    h=$(( (9 * dpi + 36) / 72 ))                                   # 9pt in pixels
    h_le=$(printf '%08x' $(( (-h) & 0xffffffff )) | sed -E 's/(..)(..)(..)(..)/\4\3\2\1/')
    face_hex=5300650067006f006500200055004900                      # "Segoe UI", UTF-16LE
    while [ ${#face_hex} -lt 128 ]; do face_hex="${face_hex}00"; done
    #   height     width/escapement/orientation  weight  ital/ul/so/charset  prec/qual/pitch  face
    lf="${h_le}000000000000000000000000900100000000000100000000${face_hex}"
    echo "  setting the system UI font: Segoe UI 9pt (${h}px at ${dpi} dpi)..."
    for v in "${UIFONT_VALUES[@]}"; do
        WINEPREFIX="$PREFIX" WINEDEBUG=-all "$WINE" reg add 'HKCU\Control Panel\Desktop\WindowMetrics' \
            /v "$v" /t REG_BINARY /d "$lf" /f </dev/null >/dev/null 2>&1
    done
elif [ "$DO_UIFONT" -eq 1 ]; then
    echo "  system UI font: left at Wine's default — the Segoe UI family is not in this prefix"
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

if [ "$DO_UIFONT" -eq 1 ] && [ "$have_segoe" -eq 1 ]; then
    for _ in $(seq 1 15); do
        [ "$(uifont_face MenuFont)" = "Segoe UI" ] && break
        sleep 1
    done
    [ "$(uifont_face MenuFont)" = "Segoe UI" ] || {
        echo "ERROR: the WindowMetrics fonts did not switch to Segoe UI in $USERREG." >&2; ok=0; }
fi

echo
if [ "$ok" -eq 1 ]; then
    echo "Font setup complete."
    echo "On stock Wine re-run this after 'wineboot -u' — it resets the FontLink entries."
    exit 0
fi
exit 1
