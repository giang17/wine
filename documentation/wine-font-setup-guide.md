# Wine Font Setup Guide (Container + Host)

Guide for setting up fonts for correct Unicode symbol rendering in Wine
(Serum2 rating stars, menu arrows, etc.) and for preventing plugin crashes
caused by missing system fonts.

## Prerequisites

Required font files (available on the host):
- `DejaVuSans.ttf` — `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`
- `NotoSansSymbols2-Regular.ttf` — `/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf`
- **MS Core Fonts** (Arial, Verdana, Times New Roman, etc.) — `/usr/share/fonts/truetype/msttcorefonts/`

If not present:
```bash
sudo apt install fonts-dejavu-core fonts-noto-core ttf-mscorefonts-installer
```

## 0. MS Core Fonts (Arial, Verdana, etc.) — CRITICAL for Plugin Stability

**Problem (discovered 2026-04-03):** Some plugins ship their own D3D9/OpenGL rendering
engine (`engine_x64.dll`) that loads fonts directly by file path from
`C:\windows\Fonts\` during DLL initialization. If the expected font file is missing,
the engine returns a NULL font object and crashes immediately with an Access Violation
(`c0000005`) before any rendering occurs. The plugin reports as "failed to initialize"
with no useful error message.

**Known affected plugin:** FL Studio "Fruity Delay 3" — `engine_x64.dll` requires
`Arialbd.ttf` (Arial Bold) and `Verdana.ttf`. Without these fonts, the plugin crashes
on load.

**Root cause:** The engine uses `PathFileExistsW("C:/windows/Fonts/Arialbd.ttf")` and
`CreateFileW("C:\\windows\\Fonts\\Verdana.ttf")` — direct file system access, not
GDI/DWrite font enumeration. The font files must physically exist in the Wine Fonts
directory.

**Important:** Wine does NOT auto-register fonts from the Fonts directory into the
registry. `wineboot -u` does not pick them up either. Manual registry entries are needed
for programs using GDI font APIs.

### Fix (Host)

```bash
# 1. Copy all MS Core Fonts to Wine prefix:
cp /usr/share/fonts/truetype/msttcorefonts/*.ttf ~/.wine/drive_c/windows/Fonts/

# 2. Register in Windows Font registry (for GDI/DWrite access):
for f in /usr/share/fonts/truetype/msttcorefonts/*.ttf; do
  name=$(basename "$f" .ttf)
  wine reg add "HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts" \
    /v "$name (TrueType)" /t REG_SZ /d "$(basename "$f")" /f 2>/dev/null
done
```

### Fix (Container)

```bash
# 1. Copy fonts into container:
for f in /usr/share/fonts/truetype/msttcorefonts/*.ttf; do
  docker cp "$f" wine-test-11.0-container:/home/wine/.wine/drive_c/windows/Fonts/
done

# 2. Register in registry:
docker exec wine-test-11.0-container bash -c '
  for f in /home/wine/.wine/drive_c/windows/Fonts/[Aa]rial*.ttf \
           /home/wine/.wine/drive_c/windows/Fonts/[Vv]erdana*.ttf \
           /home/wine/.wine/drive_c/windows/Fonts/[Tt]imes*.ttf \
           /home/wine/.wine/drive_c/windows/Fonts/[Cc]our*.ttf; do
    name=$(basename "$f" .ttf)
    wine reg add "HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts" \
      /v "$name (TrueType)" /t REG_SZ /d "$(basename "$f")" /f 2>/dev/null
  done
'
```

### Debugging font-related plugin crashes

If a plugin fails to initialize with no error message, check for missing fonts:

```bash
# Run with relay trace, filter for the plugin's thread:
WINEDEBUG=+relay,+loaddll,+seh wine "/path/to/DAW.exe" 2>&1 | tee /tmp/debug.log

# After crash, look for PathFileExistsW/CreateFileW calls to Fonts/:
grep "PathFileExistsW\|CreateFileW.*Fonts" /tmp/debug.log | grep "retval=00000000\|retval=fffff"
```

Signs of a font-related crash:
- `PathFileExistsW(... L"C:/windows/Fonts/SomeFont.ttf")` returning 0
- `CreateFileW(... L"C:\\windows\\Fonts\\SomeFont.ttf")` returning `ffffffffffffffff`
- Followed immediately by `EXCEPTION_ACCESS_VIOLATION` with `info[1]` near 0x00 (NULL+offset)

## 1. Installing Fonts into the Test Container

### fontconfig path (for DWrite/D2D1)

```bash
docker cp /usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf \
  wine-test-11.0-container:/home/wine/.local/share/fonts/
docker exec wine-test-11.0-container fc-cache -fv
```

### BarlowSemiCondensed — No manual copying needed anymore

Previously, BarlowSemiCondensed had to be copied manually to `~/.local/share/fonts/`
and `fc-cache -fv` had to be run (documented as "Serum 2 font fix" in yabridge guides).

**Since commit `5ade4a2`** (`dwrite: Implement IDWriteFontSet::GetMatchingFonts`) this is
**no longer necessary**. Wine now finds BarlowSemiCondensed directly from the Serum2
skin folder (`Skins/Default/Fonts/`) because VSTGUI's Custom Font Collection works
correctly. Tested: fonts deleted from `~/.local/share/fonts/` → Serum2 still displays
them correctly.

### BitPDisp-10 Tooltip Font (Serum2-specific)

Serum2 uses the proprietary bitmap font `BitPDisp-10` for tooltips. This font is
provided by the Windows installer and is missing in a manual installation. Without it,
DWrite falls back to Tahoma (pixelated/aliased).

**Workaround:** DejaVu Sans Mono with a renamed family name as a substitute:

```bash
# 1. Generate font (once on the host):
python3 -c "
from fontTools.ttLib import TTFont
font = TTFont('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf')
for rec in font['name'].names:
    if rec.nameID in (1, 4, 6):  # Family, Full, PostScript
        rec.string = 'BitPDisp-10'
font.save('/tmp/BitPDisp-10.ttf')
"

# 2. Copy to Serum2 skin fonts folder (VSTGUI Custom Collection):
SKIN_FONTS="/home/wine/.wine/drive_c/users/wine/Documents/Xfer/Serum 2 Presets/Skins/Default/Fonts"
docker cp /tmp/BitPDisp-10.ttf "wine-test-11.0-container:${SKIN_FONTS}/BitPDisp-10.ttf"

# 3. Copy to fontconfig path (DWrite System Collection):
docker cp /tmp/BitPDisp-10.ttf \
  wine-test-11.0-container:/home/wine/.local/share/fonts/BitPDisp-10.ttf
docker exec wine-test-11.0-container fc-cache -fv
```

Background and root-cause analysis: see `docs/wine-serum2-tooltip-gray-box.md` (Session 11).

### Windows Fonts directory (for GDI FontLink)

```bash
docker exec wine-test-11.0-container bash -c '
  cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
     /home/wine/.wine/drive_c/windows/Fonts/
  cp /home/wine/.local/share/fonts/NotoSansSymbols2-Regular.ttf \
     /home/wine/.wine/drive_c/windows/Fonts/
'
```

## 2. Setting GDI FontLink Registry Entries

> **WARNING: `wineboot -u` overwrites FontLink entries!**
> Wine's `wineboot -u` resets the Tahoma FontLink to Windows defaults (CJK fallbacks
> only), removing our DejaVu Sans / Noto Sans Symbols2 entries. This causes Serum2
> rating stars to revert to tofu boxes. **Re-run the commands below after every
> `wineboot -u`** (discovered 2026-04-03 on the host after MS Core Font registration
> triggered a wineboot).

The commands below place our custom entries **before** the standard CJK fallbacks,
so they are tried first for symbol glyphs (stars, arrows).

### Container

```bash
docker exec wine-test-11.0-container bash -c '
  # Tahoma (system menu font) → DejaVu Sans (arrows) + Noto Sans Symbols2 (stars)
  # followed by standard CJK fallbacks
  wine reg add \
    "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink" \
    /v "Tahoma" /t REG_MULTI_SZ \
    /d "DejaVuSans.ttf,DejaVu Sans\0NotoSansSymbols2-Regular.ttf,Noto Sans Symbols2\0MSGOTHIC.TTC,MS UI Gothic\0MINGLIU.TTC,PMingLiU\0SIMSUN.TTC,SimSun\0GULIM.TTC,Gulim\0YUGOTHM.TTC,Yu Gothic UI\0MSJH.TTC,Microsoft JhengHei UI\0MSYH.TTC,Microsoft YaHei UI\0MALGUN.TTF,Malgun Gothic\0SEGUISYM.TTF,Segoe UI Symbol" /f

  # DejaVu Sans itself → Noto Sans Symbols2 (for missing symbols)
  wine reg add \
    "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink" \
    /v "DejaVu Sans" /t REG_MULTI_SZ \
    /d "NotoSansSymbols2-Regular.ttf,Noto Sans Symbols2" /f
'
```

### Host

```bash
wine reg add \
  "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink" \
  /v "Tahoma" /t REG_MULTI_SZ \
  /d "DejaVuSans.ttf,DejaVu Sans\0NotoSansSymbols2-Regular.ttf,Noto Sans Symbols2\0MSGOTHIC.TTC,MS UI Gothic\0MINGLIU.TTC,PMingLiU\0SIMSUN.TTC,SimSun\0GULIM.TTC,Gulim\0YUGOTHM.TTC,Yu Gothic UI\0MSJH.TTC,Microsoft JhengHei UI\0MSYH.TTC,Microsoft YaHei UI\0MALGUN.TTF,Malgun Gothic\0SEGUISYM.TTF,Segoe UI Symbol" /f

wine reg add \
  "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink" \
  /v "DejaVu Sans" /t REG_MULTI_SZ \
  /d "NotoSansSymbols2-Regular.ttf,Noto Sans Symbols2" /f
```

## 3. Verification

```bash
# Check FontLink entries:
docker exec wine-test-11.0-container bash -c '
  echo "=== Tahoma FontLink ==="
  wine reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink" /v "Tahoma"
  echo "=== DejaVu Sans FontLink ==="
  wine reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink" /v "DejaVu Sans"
  echo "=== Fonts in Windows/Fonts ==="
  ls -la /home/wine/.wine/drive_c/windows/Fonts/{DejaVuSans,NotoSansSymbols2}*.ttf
  echo "=== fontconfig ==="
  fc-list | grep -i "noto.*symbol"
'
```

## 4. Testing

```bash
# Start Serum2 in Reaper (WineD3D, no DXVK):
docker exec wine-test-11.0-container bash -c \
  'WINEDLLOVERRIDES="d3d11,dxgi,d3d10core,d2d1=b" wine \
  "/home/wine/.wine/drive_c/Program Files/REAPER (x64)/reaper.exe"'

# Check:
# 1. Open Preset Browser → RATING column: stars instead of tofu boxes
# 2. Open rating dropdown "(any rating)" → stars in the list
# 3. MENU button → "Note Exp.: XYZ → Macro 1, 2, 3": arrows instead of tofu
```

## Host Setup

On the host, fonts are typically already installed. Only the DWrite patch
(`dlls/dwrite/analyzer.c`, commit `31a8676`) needs to be included in the Wine
installation.

FontLink entries for the host: see Section 2 above (Host subsection).
The same `wineboot -u` caveat applies — re-run after any wineboot.

## Rollback (in Case of Problems)

```bash
# Remove FontLink entries:
docker exec wine-test-11.0-container bash -c '
  wine reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink" /v "Tahoma" /f
  wine reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\FontLink\SystemLink" /v "DejaVu Sans" /f
'

# Remove fonts from Windows/Fonts:
docker exec wine-test-11.0-container bash -c '
  rm -f /home/wine/.wine/drive_c/windows/Fonts/DejaVuSans.ttf
  rm -f /home/wine/.wine/drive_c/windows/Fonts/NotoSansSymbols2-Regular.ttf
'
```


**Summary:** Three separate rendering paths, three separate fixes:

| Path | Font | Fix |
|------|------|-----|
| Direct file access (plugin engines) | Arial, Verdana, etc. | Copy MS Core Fonts to `Fonts/` + registry |
| DWrite/D2D1 (VSTGUI GUI) | BarlowSemiCondensed | `analyzer.c` fallback mapping 2B00–2BFF |
| GDI (native Win32 menus) | Tahoma → DejaVu Sans | FontLink registry → Noto Sans Symbols2 |
| Serum2 tooltips | BitPDisp-10 (proprietary) | DejaVu Sans Mono with renamed family name |
