<h1 align="center">Wine with DirectComposition and Direct2D</h1>

<p align="center">
  <b>Modern Windows audio software on Linux.</b><br>
  A Wine fork for the plugin and application GUIs that stock Wine cannot draw.
</p>

<p align="center">
  <img alt="Base" src="https://img.shields.io/badge/base-Wine%2011.0%20stable-blue">
  <img alt="Rolling devel" src="https://img.shields.io/badge/rolling%20devel-newest%2011.x%20tag-blue">
  <img alt="Subsystems" src="https://img.shields.io/badge/modified%20subsystems-40%2B-informational">
  <img alt="License" src="https://img.shields.io/badge/license-LGPL--2.1-green">
</p>

<p align="center">
  <a href="PATCHES.md">Full patch documentation</a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="#verifying-this-fork">Verifying this fork</a> ·
  <a href="#which-branch">Which branch?</a> ·
  <a href="#tested-applications">Tested applications</a>
</p>

> [!IMPORTANT]
> **This repository holds source code and nothing else.**
> There are no releases, no binary assets and no installers here — nothing to download and
> run. The patches are compiled locally ([Quick start](#quick-start)), and the complete
> difference from upstream Wine can be checked before building
> ([Verifying this fork](#verifying-this-fork)).
>
> **No application software is distributed here.** Every DAW, application and plugin shown
> or listed on this page runs from a purchased licence or from the vendor's own demo or
> trial version.
>
> **This is a private project**, published so that the work reaches the Wine community
> instead of staying on one machine. It is not affiliated with the Wine project, WineHQ or
> CodeWeavers, nor with Steinberg, Image-Line, Ableton or any other vendor named here; all
> product names are used only to say what was tested.

<table>
<tr>
<td width="50%" valign="top">
  <img src="https://raw.githubusercontent.com/giang17/wine/assets/readme-screenshots/steinberg-tools-collage.png" alt="Steinberg Download Assistant, Activation Manager and Library Manager running on this fork">
  <sub><i>Steinberg's own tools under Wine — the Download Assistant with Cubase Pro 15.0.30 and its content packages, the Activation Manager 1.9 with activated licences, and the Library Manager with 47 libraries (96.32 GB). Content updates in the Library Manager need the shell32 <code>IFileOperation::DeleteItem</code> implementation from this fork; with the stub a content update never completes.</i></sub>
</td>
<td width="50%" valign="top">
  <img src="https://raw.githubusercontent.com/giang17/wine/assets/readme-screenshots/cubase15-pro-mixconsole.png" alt="Cubase Pro 15 running on this fork">
  <sub><i>Cubase Pro 15.0.30 under Wine — the Cubase 14 demo project in playback: arrangement, MixConsole in the lower zone with live meters, MediaBay on the right, on this fork.</i></sub>
</td>
</tr>
</table>

## Why this fork exists

Windows audio software stopped drawing itself with GDI years ago. Plugin frameworks —
JUCE 8, VSTGUI, SynthEdit/GMPI — and the DAWs that host them now put their user interface
on **DirectComposition** visual trees rendered with **Direct2D**, text through
**DirectWrite**, presented through **DXGI** swapchains.

Stock Wine implements very little of that. The result is not a subtle rendering
difference: plugin windows come up black, artwork and icons are missing, GUIs flicker,
and some applications refuse to start at all because the graphics engine they build on
never comes up.

This fork implements the missing pieces. It adds **over 30 000 lines to stock Wine**,
reaching into more than 40 DLLs and programs, and it is used daily for music production
rather than kept as a proof of concept.

The other part of the work fixes code that stock Wine already has: an endless loop in d2d1's
Bézier handling that left Studio Pro 8 spinning at 100 % CPU after quitting, a crash in
msi's feature costing that stopped the Cubase 13 installer before its first dialog, a lost
wake-up in ntdll's address waits that stalled other threads for up to five seconds. Such fixes
help every Windows application that reaches the same code, not only the ones listed here;
seven fixes from this fork are already merged into upstream Wine.

```
d2d1 · dcomp · dwrite · dxgi · d3d11 · wined3d · winex11.drv · win32u · uianimation
dwmapi · ntdll · ole32 · msi · crypt32 · comdlg32 · shell32 · advapi32 · wintrust
pwrshsip (new) · powershell.exe · winemenubuilder.exe · and more than twenty others
```

## What it looks like

<table>
<tr>
<td width="50%" valign="top">
  <img src="https://raw.githubusercontent.com/giang17/wine/assets/readme-screenshots/serum2-fl-studio.png" alt="Serum 2 in FL Studio">
  <sub><b>Serum 2</b> (VSTGUI + DirectComposition) in FL Studio 2026 — three wavetable oscillators with their wavetable views, the filter response, the envelope, the Rössler chaos LFO and the arpeggiator all render.</sub>
</td>
<td width="50%" valign="top">
  <img src="https://raw.githubusercontent.com/giang17/wine/assets/readme-screenshots/studio-pro-8-video-splice.png" alt="Fender Studio Pro 8 with the video player and the Splice panel">
  <sub><b>Fender Studio Pro 8</b> (CCL: DXGI + DirectWrite + DComp) — the demo song in playback: arranger sections and the video track in the song view, the floating video player, the console with live meters and the Splice panel docked on the right. Without the <code>UIAnimationManager2</code> implementation from this fork it does not start at all.</sub>
</td>
</tr>
<tr>
<td width="50%" valign="top">
  <img src="https://raw.githubusercontent.com/giang17/wine/assets/readme-screenshots/fl-cloud-plugins.png" alt="FL Cloud plugin store in FL Studio">
  <sub><b>FL Cloud</b> in FL Studio — the plugin store browses with its artwork, products show as installed, and a download runs in place. Installing from it needs the AF_UNIX fixes from this fork.</sub>
</td>
<td width="50%" valign="top">
  <img src="https://raw.githubusercontent.com/giang17/wine/assets/readme-screenshots/eprom-fl-studio.png" alt="EPROM Memory Rites in FL Studio">
  <sub><b>EPROM — Memory Rites</b> (JUCE 8.0.13) in FL Studio 2026 — the preset header, the macro knobs, the artwork panel and the pad row render through JUCE; the splash and login screen before them go through WebView2.</sub>
</td>
</tr>
</table>

## Quick start

```bash
git clone https://github.com/giang17/wine.git
cd wine
git checkout d2d1-dcomp-11.0     # or the highest-numbered d2d1-dcomp-11.* for rolling release
./configure --prefix=/opt/wine-d2d1 --enable-archs=i386,x86_64
make -j$(nproc)
sudo make install
```

Use a separate `--prefix` so your distribution's Wine stays untouched. Build **both**
architectures: 32-bit hosts and plugins load `d2d1`, `dcomp` and `dxgi` directly, and a
stale 32-bit half keeps running old code without anything visibly failing. This needs the
`i686-w64-mingw32` cross compiler next to the x86_64 one.

Two things are worth knowing before the first run, both explained in
[PATCHES.md](PATCHES.md):

- **`wine.inf` is part of the install.** It carries this fork's registry defaults,
  including the `HideWineVersion` entries that keep JUCE 8.0.13+ plugins on the Direct2D
  path instead of its Wine-detecting GDI fallback. Copying DLLs by hand without it fails
  silently — the plugins simply keep rendering with GDI.
- **Leave DXVK out of the prefix.** It replaces `dxgi.dll` and `d3d11.dll` there, and this
  fork's composition-swapchain and DComp popup handling live in `dxgi` — an application
  that uses them then runs on DXVK's stack instead. Nothing here needs it, and it brought
  no speed-up in the setup this fork is developed on. If DXVK is needed for something else
  on the same machine, PATCHES.md has the per-application switch; the whole trio
  `d3d11,dxgi,d3d10core` has to move together.

Details, tuning and font setup: **[PATCHES.md](PATCHES.md)**.

### Prebuilt package for Fedora

Patrick packages this fork for Fedora in the COPR repository
[patrickl/wine-staging-dev](https://copr.fedorainfracloud.org/coprs/patrickl/wine-staging-dev/),
built from the spec at [codeberg.org/MusicProduction/wine](https://codeberg.org/MusicProduction/wine).
Two differences from a build of this repository are worth knowing:

- **It follows the rolling branch**, not `d2d1-dcomp-11.0`, and is synced with it as new
  commits land, so it can lag behind the newest devel tag.
- **It is built on wine-staging.** The staging patchsets that overlap with this fork —
  DirectComposition, AF_UNIX, two wined3d sets, `ntdll-Hide_Wine_Exports` and
  `user32-recursive-activation`, which edits the same `set_active_window()` this fork
  changes — are left out in favour of the fork's own handling, and the stack is applied on
  top of the remaining staging patchsets. Nothing is lost by that: the fork carries its own
  version of the recursive-activation fix (Wine bug 46274). A report from this package
  should say so.

The same repository carries yabridge, pipewire-wineasio, pipeasio and winetricks. Builds
are for Fedora 44 x86_64.

## Verifying this fork

GitHub records this repository as a fork of
[wine-mirror/wine](https://github.com/wine-mirror/wine), so nothing here has to be taken on
trust: the full history is checkable against the upstream release it was built on. Upstream
supplies the base tag, so add it as a remote first — this repository carries only the
tags up to `wine-11.5`.

```bash
git remote add upstream https://github.com/wine-mirror/wine.git
git fetch upstream --tags

# BASE is the branch's base tag: wine-11.0 for d2d1-dcomp-11.0,
# the matching wine-11.<N> for a rolling branch.
BASE=wine-11.0

# Does this branch descend from the released tag, or from a rewritten history?
git merge-base --is-ancestor $BASE HEAD && echo "descends from $BASE"

# Every commit added on top of it, and the complete diff
git log --oneline $BASE..HEAD
git diff --stat $BASE..HEAD
```

That diff is the whole of what this fork adds, and this command sorts it by area, so the
shape is visible without reading it line by line:

```bash
git diff --name-only $BASE..HEAD | cut -d/ -f1 | sort | uniq -c | sort -rn
```

On `d2d1-dcomp-11.0` the weight sits in `dlls/`, followed by `include/` and `scripts/`,
then `programs/` and `server/`, with single files for the loader's `wine.inf.in`, the font
setup guide, `.gitignore` and the documents at the top level.

The only remaining entries are `configure` and `configure.ac`, and those are worth reading
in full, because the build system is where a fork could hide something that runs before any
of the code does:

```bash
git diff $BASE..HEAD -- configure.ac
```

On `d2d1-dcomp-11.0` it holds three changes and no more: detection for the X Damage
extension, two `WINE_CONFIG_MAKEFILE` lines for the modules this fork adds (`pwrshsip` and
`windows.security.authentication.web.core`), and a `--match 'wine-*'` argument on the
`git describe` call that stamps the version string, so that a working or backup tag closer
to HEAD than the release tag cannot end up in a built binary's reported version. The
generated `configure` mirrors those same three and nothing else.

Rolling branches are rebased onto each new devel tag rather than merged, so their history
is rewritten by design; the same check against the newer `BASE` applies to each of them.

## Which branch?

| Branch | Base | For whom |
|---|---|---|
| **`d2d1-dcomp-11.0`** | `wine-11.0` stable | **Most users.** The full stack on the stable release, actively maintained. This is the default branch. |
| **`d2d1-dcomp-11.<N>`** | newest WineHQ devel tag | **Rolling-release users** (`wine-tkg-dev` and similar). The same stack rebased onto the newest devel tag roughly every two weeks. Always take the **highest-numbered** `d2d1-dcomp-11.*` branch; lower numbers are superseded snapshots. Plugin-tested before each push. |
| `d2d1-v6` | `wine-11.0` | **Deprecated** since 2026-05-03. The original upstream-targeted D2D1-only series, kept for reference. |

## Tested applications

Everything below is exercised on this fork, not inferred from code. The long form — what
exactly each one needs, and what breaks without it — is in
[PATCHES.md](PATCHES.md#tested-applications).

| Application | Framework | Status |
|---|---|---|
| **Steinberg Cubase Pro 15.0.30** | Custom (DComp + D2D1 + DirectWrite), WebView2 Hub | Installs through Steinberg's own bootstrapper and runs: project window, MixConsole with live meters, Hub. Installing needs the `msi` feature-cost fix (the setup crashed before its first dialog) and the Script SIP for signed PowerShell — without `pwrshsip`/`wintrust` the installer stops at "preinstall.ps1 … not trusted". Starting needs the `Windows.Globalization.Calendar` stub, without which `headtracking.dll` aborts behind the licence splash, and the `comdlg32` folder-dialog fix, without which the Hub reports the project folder as read-only. The window itself needs the dcomp virtual-surface resize and child-surface readback work — and the d2d1 WIC target fix, without which the MixConsole level meters stay empty |
| **Steinberg Groove Agent 5 / Groove Agent SE 6**, **HALion 7**, **Dorico 5**, **The Grand 3** | Steinberg framework (DComp + D2D1 + DXGI); Dorico: Qt 6.5 | Run as standalone applications. Groove Agent 5 needs the DXGI tooltip-popup and `Present1` scroll-rectangle fixes — without them its tooltips turn black and scrolling the kit list leaves stale thumbnails. The Grand 3 needs the dcomp restore of a surface-root frame lost before `ShowWindow`, without which its toolbar stays black until hovered. The Anima 3D wavetable view needs offscreen rendering for hidden top-level windows (measured in HALion Sonic 7). Desktop and start-menu shortcuts of the MSI-installed applications need the shell32 long-path fix, without which The Grand 3 reports "Skin file skins/skin.srf not found" |
| **Serum 2** (VST3 in Reaper) | VSTGUI + DComp | Fully functional — all waveform views, envelopes, presets, per-pixel-alpha drag bitmaps |
| **Korg Trinity / Prophecy** (VST3) | JUCE 8.0.13 / 8.0.12 + DComp | Fully functional on the DComp path |
| **Pianoteq 9** (standalone + VST3) | JUCE 8.0.10 + DComp | Fully functional |
| **Ableton Live 12** (Intro / Lite) | Custom (D3D11 + WebView2) | Fully functional — decorations, move/resize, fullscreen, Splice view |
| **VirtualDJ 2026** | Custom skin engine (D3D11 flip-model swapchains) | Runs — decks, browser, skin relayout while the window is being resized. Needs the wined3d GL present for flip-model swapchains without a BGR back buffer, without which both windows stay black, and the wined3d shared between the DXGI factories, without which the skin re-lays out only seconds after a resize |
| **FL Studio 2026** | Custom | Runs without xruns at 64 samples / 48 kHz; Cloud plugins install and stream |
| **Fender Studio Pro 8** | CCL (DXGI + DWrite + DComp) | Fully functional — needs the `UIAnimationManager2` implementation to start at all |
| **EPROM — Memory Rites**, **Minimal Audio Current / Evoke / Lucid** (VST3) | JUCE 8.0.13 (+ WebView2) | Fully functional with the `HideWineVersion` entry |
| **Native Access 3.25.2**, **Kontakt 8 Player** | Electron/Chromium, InstallAware/MSI | Install, sign in and run — need the `powershell` and `msi` fixes from this fork |
| **VProm3** (VST3) | SynthEdit/GMPI + D2D1 | Fully functional, correct colours (needs the Color Management effect) |
| **SynthEdit 1.5** | Custom engine (D2D1 + winex11 client surfaces) | Fully functional — the MDI canvas draws completely and stays stable through menus, resizes and moves |
| **UVI Portal**, **Minimal Hub** | WebView2, Tauri v2 | Install, sign in and update products; Minimal Hub needs the schannel `DecryptMessage` fix for incomplete messages, which this branch carries — without it its `oauth/token` request stops after a partial response and the app waits forever |

## What the fork changes

One line per area; [PATCHES.md](PATCHES.md) has the reasoning, the measurements and the
switches for each.

- **D2D1** — geometry pipeline (arcs, constrained Delaunay triangulation, miter limits,
  shader antialiasing, correct stroke joins, cubic Bézier subdivision), PushLayer stencil
  clipping, layers on opaque targets, Color Management effect and sRGB formats for
  SynthEdit/GMPI, Gaussian blur, bitmap-brush WRAP/MIRROR, line batching (Serum 2's
  wavetable view: 1 060 ms → about 80 ms)
- **DComp** — `IDCompositionDesktopDevice` with a D2D1 bitmap rendering path, dirty-rect
  clipping, `IDCompositionDevice3/4/5`, composition and dynamic textures, D3D11 BeginDraw,
  rootless visual trees composited at ~60 Hz, cross-process targets, backdrop capture, and
  leaf visuals composited into the frame instead of blitted after it (Studio Pro's
  playhead: missing in 14.9 % of frames before, 0.0 % after)
- **DirectWrite** — `NATURAL_SYMMETRIC` rendering mode, `IDWriteFontSet::GetMatchingFonts`,
  font fallback for Miscellaneous Symbols and Geometric Shapes, a glyph cache that
  outlives the fontface (a 25 s resize re-rasterised 196 distinct glyphs 111 598 times
  before), and ClearType-style subpixel text
- **DXGI / D3D11 / WineD3D** — composition swapchains, FLIP_SEQUENTIAL preservation, DComp
  popup handling, monitored fences, `ID3D11Fence` with CPU timeline semantics, GL buffer
  recycling (70 % RSS reduction), GL present for top-level windows
- **winex11 / dwmapi / win32u** — DComp window support, per-pixel alpha for GPU-painted
  layered windows, correct handling of ownerless tool-window popups (sticky, wrongly
  decorated plugin menus on KDE), no black expose flash while a client window renders
  offscreen
- **Everything an installer needs** — `msi` string pool and feature cost, Script SIP for
  signed PowerShell (`pwrshsip`, `wintrust`, `msisip`), a usable `powershell.exe`,
  `IShellLink::GetPath` short paths, `MiniDumpWriteDump` module lists.
  Unglamorous, but it is the difference between an application that installs and one that
  does not

## Running in the wild

Independent reports in this repository's issue tracker cover three GPU vendors — NVIDIA,
AMD (RADV) and Intel integrated graphics — Arch/CachyOS installations, Hyprland on Wayland
next to X11 sessions, and Bitwig hosting plugins through yabridge as well as standalone
applications.

One of them ([#5](https://github.com/giang17/wine/issues/5)) is worth reading as a
comparison: for that application stock Wine 11.12 produced a 1×1 black window, three DXVK
versions each aborted during graphics initialisation, and this fork's builtin
`dxgi`/`d3d11` brought up a working, responsive GUI. Another
([#3](https://github.com/giang17/wine/issues/3)) runs JUCE 8 plugins on an Intel iGPU with
no discrete GPU at all.

The fork is also the base of [shibco/ableton-linux](https://github.com/shibco/ableton-linux),
which builds Ableton Live 12 production setups on top of it.

## Relationship to upstream Wine

This is a fork of [wine-mirror/wine](https://github.com/wine-mirror/wine), not a
replacement for it. Small, self-contained fixes go to WineHQ as ordinary merge requests on
[gitlab.winehq.org](https://gitlab.winehq.org/wine/wine) and several have been merged
there. The large DirectComposition and Direct2D work stays here, where it can be rebased
onto every devel tag and tested against real plugins every two weeks.

The upstream Wine README — build requirements, configuration, documentation, where to get
support for Wine itself — is kept unchanged as **[README.wine](README.wine)**.

## Problems, questions, contributions

Open an [issue](https://github.com/giang17/wine/issues). For a rendering problem the two
most useful things are the application and plugin framework involved, and whether the
`HideWineVersion` entry applies to your host — with JUCE 8.0.13 and newer that one
switch decides whether the plugin renders with Direct2D or falls back to GDI, and the GDI
fallback is what a "the GUI is black" report usually turns out to be.

## Credits

- Subpixel text rendering: Cade ([@shibco](https://github.com/shibco)),
  `shibco/ableton-linux` pull request 155, reviewed there by @ClickSentinel
- `--enable-archs` packaging form for Bottles: @jibeape
- Related discussion: [yabridge#413](https://github.com/robbert-vdh/yabridge/issues/413)

## License

Same as Wine — GNU LGPL. See [LICENSE](LICENSE).
