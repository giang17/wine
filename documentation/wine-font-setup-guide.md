# Wine Font Setup Guide

Fonts a Wine prefix needs for plugin GUIs: correct Unicode symbol rendering
(Serum 2 rating stars, menu arrows) and no crashes from plugins that expect
system font files to exist.

**[wine-font-setup.sh](wine-font-setup.sh)** does the routine part — MS Core Fonts,
the symbol fallback fonts with their FontLink entries, and this branch's text
rendering switches — and `--check` reports without changing anything:

```bash
scripts/wine-font-setup.sh --prefix ~/.wine
scripts/wine-font-setup.sh --prefix ~/.wine --check
```

This guide explains what the script sets and why, shows the manual commands, and
covers the one thing it deliberately leaves out (the Serum 2 tooltip font).

## Prerequisites

Required font files on the host:
- `DejaVuSans.ttf` — `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`
- `NotoSansSymbols2-Regular.ttf` — `/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf`
- **MS Core Fonts** (Arial, Verdana, Times New Roman, ...) — `/usr/share/fonts/truetype/msttcorefonts/`

If not present:
```bash
sudo apt install fonts-dejavu-core fonts-noto-core ttf-mscorefonts-installer
```

The script locates them through fontconfig, so other distribution paths work too.

## 0. MS Core Fonts (Arial, Verdana, etc.) — plugin stability

**Problem (discovered 2026-04-03):** some plugins ship their own D3D9/OpenGL rendering
engine (`engine_x64.dll`) that loads fonts directly by file path from
`C:\windows\Fonts\` during DLL initialisation. If the expected font file is missing,
the engine returns a NULL font object and crashes immediately with an access violation
(`c0000005`) before any rendering occurs. The plugin reports as "failed to initialize"
with no useful error message.

**Known affected plugin:** FL Studio "Fruity Delay 3" — `engine_x64.dll` checks for
`Arialbd.ttf` (Arial Bold) and `Verdana.ttf` with `PathFileExistsW` and `CreateFileW`,
direct file system access rather than GDI/DWrite enumeration. The files must physically
exist in the prefix' Fonts directory.

**Important:** Wine does not auto-register fonts from the Fonts directory into the
registry, and `wineboot -u` does not pick them up either. Programs using GDI font APIs
need the registry entries.

### Fix

```bash
# 1. Copy the MS Core Fonts into the prefix:
cp /usr/share/fonts/truetype/msttcorefonts/*.ttf ~/.wine/drive_c/windows/Fonts/

# 2. Register them for GDI/DWrite:
for f in /usr/share/fonts/truetype/msttcorefonts/*.ttf; do
  name=$(basename "$f" .ttf)
  wine reg add "HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts" \
    /v "$name (TrueType)" /t REG_SZ /d "$(basename "$f")" /f 2>/dev/null
done
```

### Debugging font-related plugin crashes

If a plugin fails to initialise with no error message, check for missing fonts:

```bash
# Run with relay trace, filter for the plugin's thread:
WINEDEBUG=+relay,+loaddll,+seh wine "/path/to/DAW.exe" 2>&1 | tee /tmp/debug.log

# After the crash, look for PathFileExistsW/CreateFileW calls to Fonts/:
grep "PathFileExistsW\|CreateFileW.*Fonts" /tmp/debug.log | grep "retval=00000000\|retval=fffff"
```

Signs of a font-related crash:
- `PathFileExistsW(... L"C:/windows/Fonts/SomeFont.ttf")` returning 0
- `CreateFileW(... L"C:\\windows\\Fonts\\SomeFont.ttf")` returning `ffffffffffffffff`
- Followed immediately by `EXCEPTION_ACCESS_VIOLATION` with `info[1]` near 0x00 (NULL+offset)

### Which font is the application actually using?

The section above finds a *missing* font file. This one answers the other
question: the application starts, but is it drawing with the typeface you
expect — and does anything on screen fall back to a substitute?

```bash
WINEDEBUG=+font,+dwrite wine "/path/to/App.exe" 2>&1 | tee /tmp/font.log
```

Three greps carry the answer:

```bash
# 1. Faces the application loaded from its own resources (no file path):
grep 'insert_face_in_family_list.*from (null)' /tmp/font.log

# 2. Every family DirectWrite was asked for:
grep FindFamilyName /tmp/font.log \
    | sed -E 's/.*FindFamilyName [0-9A-F]+, (L"[^"]*").*/\1/' | sort | uniq -c

# 3. Families actually selected through GDI:
grep 'font_SelectFont L"' /tmp/font.log \
    | sed -E 's/.*SelectFont (L"[^"]*").*/\1/' | sort | uniq -c
```

A lookup in (2) succeeded when — and only when — a `GetFontFamily` follows it
immediately: `create_matching_font()` in `dlls/dwrite/analyzer.c` returns
`E_FAIL` directly after `FindFamilyName` when the family does not exist, so a
lookup *without* a following `GetFontFamily` is a miss. If (2) lists nothing but
the family the application asked for, the fallback path was never entered and no
font is missing. A second, unrelated family name appearing there is the fallback
stepping in — that is what a font setup problem looks like from the log.

Two warnings in that log read like causes and are not:

- `warn:dwrite:factory_create_system_fontset Failed to add font file, hr 0x88985003`
  — registry entries under the Fonts key that hold a bare file name instead of a
  path. DirectWrite expands those to `C:\windows\fonts\<name>`
  (`dlls/dwrite/main.c`), which is not where Wine keeps its own bitmap fonts.
  They are `.fon` files DirectWrite cannot use anyway.
- Hundreds of `warn:font:... unable to parse font, falling back to FreeType`
  for `.fon` files — Wine parses those on every start, in every prefix.

## 1. Symbol fallback fonts and GDI FontLink

Serum 2's rating stars (U+2B50 range) and menu arrows are drawn through two different
paths: the VSTGUI interface goes through DirectWrite, where this branch's fallback
mapping for Miscellaneous Symbols and Arrows (U+2B00-2BFF, commit `31a86766fe5`) finds a
symbol font by itself; the native Win32 menus go through GDI, which resolves missing
glyphs through the `FontLink\SystemLink` registry chain of the menu font, Tahoma. That
chain needs the fallback fonts in the prefix and two registry entries.

```bash
# Fonts into the prefix (GDI reads them from here):
cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf ~/.wine/drive_c/windows/Fonts/
cp /usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf ~/.wine/drive_c/windows/Fonts/

# Tahoma (menu font) -> DejaVu Sans (arrows) + Noto Sans Symbols2 (stars),
# placed BEFORE Wine's standard CJK fallbacks so they are tried first:
wine reg add \
  "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink" \
  /v "Tahoma" /t REG_MULTI_SZ \
  /d "DejaVuSans.ttf,DejaVu Sans\0NotoSansSymbols2-Regular.ttf,Noto Sans Symbols2\0MSGOTHIC.TTC,MS UI Gothic\0MINGLIU.TTC,PMingLiU\0SIMSUN.TTC,SimSun\0GULIM.TTC,Gulim\0YUGOTHM.TTC,Yu Gothic UI\0MSJH.TTC,Microsoft JhengHei UI\0MSYH.TTC,Microsoft YaHei UI\0MALGUN.TTF,Malgun Gothic\0SEGUISYM.TTF,Segoe UI Symbol" /f

# DejaVu Sans itself -> Noto Sans Symbols2 (for the symbols it lacks):
wine reg add \
  "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink" \
  /v "DejaVu Sans" /t REG_MULTI_SZ \
  /d "NotoSansSymbols2-Regular.ttf,Noto Sans Symbols2" /f
```

> **The entries can be reset by Wine itself.** `win32u` rewrites the `SystemLink`
> chain with its own defaults whenever the codepage record it keeps under
> `HKCU\Software\Wine\Fonts\Codepages` does not match the running process
> (`update_codepage()` in `dlls/win32u/font.c`) — in a fresh prefix, after a locale
> change, and it was observed here after a `wineboot -u` on 2026-04-03. The code page
> follows `LC_CTYPE`, so a single Wine process started under `LC_ALL=C` (or `LANG=en_US`
> on a `de_DE` desktop) is enough: it records `1252,437`, the next process records
> `1252,850` again, and each rewrite drops the entries (measured 2026-09-02 in a scratch
> prefix; the culprits here were measurement scripts exporting `LC_ALL=C` for `awk`).
> Since 2026-09-02 this branch therefore carries the two entries in win32u's own
> defaults (`system_link_tahoma_symbols_non_cjk`), so the rewrite keeps them. On stock
> Wine the stars revert to tofu boxes; `wine-font-setup.sh --check` tells whether the
> entries are still in place, and re-running the script restores them, it is
> idempotent.

### BarlowSemiCondensed — no manual copying needed

Older guides had BarlowSemiCondensed copied to `~/.local/share/fonts/` by hand (the
"Serum 2 font fix" in yabridge guides). Since commit `5ade4a26d7d` (`dwrite: Implement
IDWriteFontSet::GetMatchingFonts`) this is not necessary: Wine finds the font in Serum 2's
skin folder (`Skins/Default/Fonts/`) through VSTGUI's custom font collection. Verified
with the fonts deleted from `~/.local/share/fonts/` — Serum 2 still displays them.

### BitPDisp-10 tooltip font (Serum 2)

Serum 2 draws its tooltips in the proprietary bitmap font `BitPDisp-10`, which the
Windows installer provides and a manual installation lacks. Without it DirectWrite falls
back to Tahoma, pixelated and aliased. The setup script leaves this one out on purpose:
it is a renamed substitute, not a font the prefix is missing. Workaround — DejaVu Sans
Mono with the family name changed:

```bash
# 1. Generate the font (needs python3-fonttools):
python3 -c "
from fontTools.ttLib import TTFont
font = TTFont('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf')
for rec in font['name'].names:
    if rec.nameID in (1, 4, 6):  # Family, Full, PostScript
        rec.string = 'BitPDisp-10'
font.save('/tmp/BitPDisp-10.ttf')
"

# 2. Into Serum 2's skin fonts folder (VSTGUI custom collection):
SKIN_FONTS="$HOME/.wine/drive_c/users/$USER/Documents/Xfer/Serum 2 Presets/Skins/Default/Fonts"
cp /tmp/BitPDisp-10.ttf "$SKIN_FONTS/BitPDisp-10.ttf"

# 3. And into fontconfig (DirectWrite's system collection):
cp /tmp/BitPDisp-10.ttf ~/.local/share/fonts/BitPDisp-10.ttf
fc-cache -f
```

## 2. Text rendering switches

The subpixel, linear-blend, outline and enhanced-contrast switches of this branch are
registry values read once at startup and therefore live in the prefix; the script sets
them, and `--contrast N` picks the enhanced-contrast value. What each does, with the
measurements, is in `PATCHES.md` under *Font Setup*.

## 3. Verification

```bash
scripts/wine-font-setup.sh --prefix ~/.wine --check

# or by hand:
wine reg query "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink" /v "Tahoma"
wine reg query "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink" /v "DejaVu Sans"
ls -la ~/.wine/drive_c/windows/Fonts/{DejaVuSans,NotoSansSymbols2}*.ttf
fc-list | grep -i "noto.*symbol"
```

## 4. Testing

Start Serum 2 in Reaper and check:

1. Preset Browser → RATING column: stars instead of tofu boxes
2. Rating dropdown "(any rating)" → stars in the list
3. MENU button → "Note Exp.: XYZ → Macro 1, 2, 3": arrows instead of tofu

## Rollback

```bash
wine reg delete "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink" /v "Tahoma" /f
wine reg delete "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink" /v "DejaVu Sans" /f
rm -f ~/.wine/drive_c/windows/Fonts/DejaVuSans.ttf ~/.wine/drive_c/windows/Fonts/NotoSansSymbols2-Regular.ttf
```

**Summary:** four rendering paths, four separate fixes:

| Path | Font | Fix |
|------|------|-----|
| Direct file access (plugin engines) | Arial, Verdana, ... | MS Core Fonts into `Fonts/` + registry |
| DWrite/D2D1 (VSTGUI GUI) | BarlowSemiCondensed, symbols | `analyzer.c` fallback mapping U+2B00-2BFF (in the branch) |
| GDI (native Win32 menus) | Tahoma → DejaVu Sans | FontLink registry → Noto Sans Symbols2 |
| Serum 2 tooltips | BitPDisp-10 (proprietary) | DejaVu Sans Mono with a renamed family |
