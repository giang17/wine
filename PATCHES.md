# Wine D2D1 / DComp Patches

This fork contains patches for Wine's Direct2D (d2d1), DirectComposition (dcomp),
DirectWrite (dwrite), and related subsystems. The patches fix rendering and
performance issues that prevented modern JUCE 8, VSTGUI and SynthEdit/GMPI based
Windows applications from running correctly under Wine — particularly audio
plugins (Serum2, Korg Trinity, Korg Prophecy, Pianoteq 9, VProm3) in DAWs like
Reaper.

These newer plugin frameworks rely on DirectComposition for window management
and D2D1 for GPU-accelerated 2D rendering — APIs that were largely unimplemented
in Wine. With these patches, plugins using DComp + D2D1 now render correctly
and run stable in production use.

**Base**: Wine 11.16 devel (`wine-11.16` tag) — this branch.

## Branches

| Branch | Description |
|--------|-------------|
| `d2d1-dcomp-11.0` | **Recommended for stable users.** Full stack on Wine 11.0 stable: D2D1 + DComp + DWrite + WineD3D performance + winex11. Actively maintained. |
| `d2d1-dcomp-11.<N>` (newest devel) | **Recommended for devel / rolling-release users** (e.g. `wine-tkg-dev`). The same full stack as `d2d1-dcomp-11.0` — D2D1 + DComp + DWrite + WineD3D + winex11, including the Tier 1-3 review hardening, the Trinity nested-popup z-order fix, the embedded-Trinity (IFX) popup flicker fix, the DComp thread-safety hardening, and the WineD3D process-heap fix — rebased onto the latest WineHQ devel tag and rolled forward roughly every 2 weeks. **Use the highest-numbered `d2d1-dcomp-11.*` branch**: it is always the current rolling base. Lower-numbered `d2d1-dcomp-11.x` devel branches are previous rolling snapshots, superseded. Plugin-tested by the maintainer (Serum 2, Korg Trinity, Pianoteq 9, WineSynth). |
| `d2d1-v6` | **Deprecated** (last update 2026-02-14). Was the upstream-targeted D2D1-only patch series; superseded by 41+ later commits in `d2d1-dcomp-11.0`. Kept for historical reference. |

## Full Stack (Branch: `d2d1-dcomp-11.0`)

This is the recommended branch. It includes all 15 D2D1 patches plus:

- **D2D1 layers on an opaque target**: a layer bitmap inherits the target's alpha mode, so
  on a render target created as `ALPHA_MODE_IGNORE` it was composited straight — the
  layer's own coverage was thrown away and everything drawn inside it turned the
  background colour, usually black. All three `PopLayer()` paths now composite the layer
  as premultiplied regardless of the target's alpha mode. Visible as rectangular black
  areas swallowing clip contents and volume curves in Fender Studio Pro 8
- **DComp**: IDCompositionDesktopDevice implementation with D2D1 bitmap rendering path,
  dirty-rect clipping, DIB+BitBlt presentation (9 phases); IDCompositionDevice3/4/5,
  composition and dynamic textures, D3D11 BeginDraw and surface handle export;
  rootless visual trees composited onto the target window at ~60 Hz, cross-process
  targets, backdrop capture
- **DComp leaf compositing into the presented frame**: a visual tree that covers only a
  sliver of its window — a transport playhead, a selection rectangle — is not allowed to
  take the window over, so its leaves used to be delivered *after* the application's
  present, by reading back the window, blending, and blitting the covered rectangles.
  That is a race no CPU-side blit can win: between a swap landing and the next delivery
  arriving the leaves are gone, and an application presenting at 40 Hz shows them in a
  minority of frames. Such leaves are now composited into the buffer that is about to
  become the frame — the same route content above the coverage threshold already took —
  in the GL present path as a texture drawn between the backbuffer blit and
  `wglSwapBuffers`. Measured on Fender Studio Pro 8: the playhead is missing in **14.9 %**
  of captured frames before and **0.0 %** after, and the selection rectangle stopped
  flickering with it, having travelled the same path. Safety net: three of seven
  swapchains in that application never present at all, and a published layer that nobody
  draws would hide the leaves in *every* frame instead of half of them, so wined3d counts
  each layer it draws and dcomp returns to the blit path on its own after ~3 s of silence.
  The Vulkan present path is untouched and keeps the previous behaviour
- **UIAnimation**: `UIAnimationManager2` and `UIAnimationTransitionLibrary2`. Wine ships
  only the version 1 classes, so an application that creates the version 2 pair while
  bringing up its Direct Composition engine fails there. Applications built on the CCL
  framework — Fender Studio Pro 8, PreSonus Studio One 6.6 and newer — then abort with
  "This application requires Windows 10 or later", which is the framework's generic
  alert for a graphics engine that did not start, not a version check
- **D2D1 effects and colour**: Color Management effect (registration plus the
  scRGB → sRGB transfer function applied in the shape pixel shader),
  ID2D1GradientStopCollection1, sRGB pixel formats for WIC-sourced bitmaps.
  Required by GMPI/SynthEdit plugins, which render linearly in 16-bit float and
  composite their GUI through the Color Management effect
- **DWrite**: Rendering mode 5 fix, IDWriteFontSet::GetMatchingFonts implementation,
  font fallback mapping for Miscellaneous Symbols and Arrows (U+2B00-2BFF, fixes star
  rating display in Serum2)
- **ClearType-style subpixel text** (from Cade / @shibco, `shibco/ableton-linux`):
  a glyph run asking for `DWRITE_TEXTURE_CLEARTYPE_3x1` used to be rendered once in
  greyscale and copied into all three subpixels, so DirectWrite text resolved
  horizontally at one sample per pixel where Windows uses three. DWrite now rasterises
  the outline scaled three times horizontally and filters each row with the same
  five-tap FIR FreeType uses for `FT_RENDER_MODE_LCD`; D2D1 blends such runs once per
  colour channel; GDI takes its antialiasing from the prefix rather than the host's
  fontconfig. Needs the registry keys under *Font Setup* below — without them nothing
  changes. See `Related` for the origin
- **DXGI**: Composition swapchain, FLIP_SEQUENTIAL preservation, DComp popup handling,
  micro-resize for stale UI, monitored fence support. The swapchain-to-window mapping is
  published on the desktop window, whose property list is shared across the window
  station, so it is keyed by owning process id — otherwise a host application and its
  WebView2 child can allocate a swapchain at the same address and resolve each other's
  window
- **D3D11**: ID3D11Fence with CPU timeline semantics
- **WineD3D**: Composition buffer with dirty rect accumulation, GL buffer recycling pool
  (70% RSS reduction), scratch buffer reuse (98.6% fewer allocs), constant buffer
  dirty-check (23% fewer DISCARDs), private heap to isolate alloc churn
- **ntdll**: MADV_FREE for MEM_RESET (improved page reclaim behavior)
- **D2D1 private heap**: Isolates high-frequency geometry alloc/free from process heap
- **Per-pixel alpha for GPU-painted layered windows**: `DwmExtendFrameIntoClientArea`
  with `margins = -1` asks for full glass, which on Windows makes the client area
  per-pixel alpha capable. Wine stubbed it, so a plugin dragging a bitmap around — Serum
  2's envelope, LFO and macro handles — showed an opaque **black box** instead of the
  bitmap. Alpha capability is now a property of the *window* rather than a side effect of
  the last call that touched it: dwmapi records the request, winex11 keeps it in the
  window data so a later `SetLayeredWindowAttributes` cannot drop it again, and both the
  window and its GL child end up on an ARGB visual.

  Removing the black box uncovered a second fault, and that one needed three more fixes
  before the bitmap was actually stable while being dragged:

  - The **format substitution ran per present** instead of per window. It is now decided
    once per drawable in `x11drv_egl_surface_create()`, and the drawable keeps the format
    that was *asked for* — only the X window and the EGL config move to the ARGB twin.
    Registering the substituted format instead makes the cache comparison in
    `get_window_unused_drawable()` fail on every call, which quietly rebuilds the client
    window each time.
  - The **ARGB visual switch destroyed the top level mid-drag**. `set_window_visual()`
    implements the switch by tearing the X window down and building it again; for a
    window that is created and dragged in one go, that landed inside the drag. Measured:
    27 of 80 samples with no window at all, about 1.3 s of not existing. The visual is now
    chosen when the window is created, so the transition never happens.
  - A **cache DC dropped its drawable on every `ReleaseDC`**, and anything drawing per
    frame through `GetDC`/`ReleaseDC` therefore lost its GL client window once a frame —
    54 new X client windows during a 20 s drag. A fresh one is empty, and at depth 32
    empty means fully transparent. The drawable is now parked on the window for the next
    DC to pick up, only while the cache slot is free, and it is taken back out when handed
    over so it can never belong to two consumers at once.

  Measured over a 200 px drag, 20 s, mouse moving: **one** GL client window instead of 8,
  one top level instead of 2, no sample without a window, and the brightness range of the
  dragged bitmap down from 6.62 to 0.90 with no jump above 1 where there had been 142.

  On by default. `WINE_ARGB_PIXFMT=0` turns the whole path off without a rebuild and is
  byte-identical to the previous behaviour — worth knowing because the drawable caching
  touches `dce.c`, which every application with a GL window goes through. Smoke tested
  across Korg Trinity, EPROM Memory Rites, FL Studio, Fender Studio Pro 8 and Ableton
  Live 12.
- **winex11**: DComp window support, backing store, micro-resize suppression;
  ownerless TOOLWINDOW popups are no longer folded into the active window's group,
  given a transient_for owner, or mapped as UTILITY — this fixes sticky, wrongly
  decorated and always-on-top plugin menus on KDE; the opaque black X expose
  background is suppressed while a GL/D3D client window is taken off the screen
  for offscreen rendering, so the client area no longer flashes black for a
  frame (visible in Ableton Live 12 when toggling the Learn View panel)
- **win32u**: transparent (0x00) surface init for ARGB popups; the system arrow is
  shown again when an application hides the cursor and sets none
- **wineserver**: a top-level's surface flush no longer overwrites child windows that
  belong to a *different process*. Such a child never gets a window surface of its own
  and draws straight into the top-level's drawable, while the owner collects into its
  surface and flushes over it with a delay — the result is a black one-frame flash over
  embedded panels whenever the two overlap. Its client rect is now subtracted from the
  surface region, exactly the treatment GL and Vulkan children already receive. Affects
  any application that embeds out-of-process content: FL Studio's hub and Ableton
  Live 12 (WebView2), CEF-based plugin GUIs. Trade-off: an area the owner no longer
  flushes keeps its last frame, so a foreign child that stops painting without a
  geometry change leaves that frame on screen until something moves — the same exposure
  GL children have today. `WINE_DISABLE_FOREIGN_CHILD_CLIP=1` restores the previous
  behaviour
- **ole32**: RevokeDragDrop no longer touches drop targets owned by other processes
  (fixes a use-after-free crash when closing plugin windows)
- **crypt32**: verifying a signature hashes the signed attributes as they are stored in
  the message instead of re-encoding them. The encoder sorts a SET OF into DER order,
  but a signer may emit the attributes unsorted and the signature covers the order it
  used, so re-encoding produced a different hash and verification failed with
  TRUST_E_CERT_SIGNATURE. Affects any application that checks its own Authenticode
  signature at startup; FL Studio 2026 reports that the program could not be verified
  and exits
- **Virtual-desktop compositor (winex11)**: windows inside a Wine virtual desktop get
  real per-pixel alpha, which the plain desktop drawable cannot provide. A small
  XDamage-driven compositor assembles frames off screen and composites them through an
  ARGB overlay onto an opaque child window, so translucent plugin popups and drop
  shadows blend correctly instead of showing black or leftover pixels. It is set up on
  demand, tracks every virtual-desktop root, survives a desktop window that has been
  replaced, and restores or blanks a child's backing store when the child is remapped
  or leaves the desktop
- **AF_UNIX sockets (ws2_32, wineserver, ntdll)**: Unix-domain socket support, based on
  the long-standing wine-staging patch set plus hardening and three conformance fixes of
  our own — a socket is given its family before bind, a bound socket is reported as a
  reparse point by GetFileAttributes, and it can be opened and queried through CreateFile
  with FSCTL_GET_REPARSE_POINT. With these the ws2_32 AF_UNIX conformance tests run at
  all (upstream they skip) and pass without a failure. Needed by applications that talk
  to a local helper over a Unix socket; FL Studio's Cloud plugins install normally
  instead of spinning on a socket that never appears
- **Keyboard input into out-of-process content (wineserver, winex11)**: a process that
  translates keys for a window owned by *another* process now has the whole thread input
  family attached, derives the AltGr modifier from the passed key state rather than from
  a process-global cache of the last X11 event, and posts a finished IME result string as
  WM_CHAR as well. Together these make typing work in embedded WebView2 fields, including
  AltGr characters such as `@`, `\` and `€` on non-US layouts
- **Media Foundation video playback (mf, evr, winegstreamer)**: playing and scrubbing
  video on a DAW timeline exercises paths that a straight play-to-the-end never reaches,
  and several of them were broken. The session asked upstream for a new sample on *every*
  call of its delivery routine instead of one at a time, so a topology rebuild — closing
  and reopening a video window, wrapping a loop — produced **5155 requests in one second**
  against 23-26 in steady playback; a source with a filled read-ahead answers all of them,
  and the samples pile up on a transform input that is not consuming yet, gigabytes of
  them, after which the picture stops. Sample requests now also run on a work queue of
  their own per stream instead of sharing the source's command queue, where a request that
  blocks waiting for the next buffer used to hold up transport commands and the other
  streams. Alongside: the renderer accepts samples that arrive while it is paused, so a
  jump in a stopped transport updates the picture; the presenter reinstalls its allocator
  notification after an output type renegotiation; the session is told when a scrub has
  been carried out; and the presenter no longer pulls mixer output through a NULL mixer
  pointer while the sink is being torn down. Together these fix seeking, looping, timeline
  jumps, playback stalling after 20-35 seconds, stuttering after a large seek, and a black
  picture after a window toggle in Fender Studio Pro 8
- **windows.security.authentication.web.core**: WebAuthenticationCoreManager
  implementation, for applications that probe the WinRT web-account API on startup
- **Direct2D for JUCE 8.0.13+ (ntdll)**: JUCE 8.0.13 and later pick their renderer with
  `GetProcAddress(GetModuleHandleA("ntdll"), "wine_get_version") != nullptr` and fall back
  to GDI whenever that succeeds, which bypasses this entire stack — a JUCE plugin then
  never creates a device context and never gets a composition swapchain. This branch can
  hide that one export so those plugins take the Direct2D path again. **Opt-in**, see the
  note below

### Note: JUCE 8.0.13+ and the hidden `wine_get_version` export

Since JUCE 8.0.13 (upstream commit `5179690ff7`, "Restore Wine functionality") every JUCE
plugin falls back to the GDI renderer as soon as it detects Wine. That fallback exists
because stock Wine does not implement the Direct2D 1.3 / DirectComposition surface JUCE
needs — but this branch does, so the fallback only costs functionality here. Measured with
one plugin: 23 d2d1 calls with the fallback, 2.6 million without it.

This branch can therefore hide `wine_get_version` from `GetProcAddress`. Doing so restored
the complete UI of a WebView2-based JUCE plugin whose artwork, icons and content had been
missing, and removed flicker that had been chased for weeks in the wrong place.

It is **off by default** and meant to be enabled for the hosts that actually run JUCE
plug-ins, not system-wide — see the caveat below.

```bash
# single run
WINE_HIDE_WINE_VERSION=1 wine your-host.exe
```

```
# per application, the usual case
HKCU\Software\Wine\AppDefaults\your-host.exe\HideWineVersion = "Y"
# globally, if you know what runs in this prefix
HKCU\Software\Wine\HideWineVersion = "Y"
```

The per-application value wins over the global one, so `"N"` can carve out an exception
where the global switch is on.

**The switch lives in the prefix, not in the build.** It is a registry value, so a fresh
prefix does not have it: installing or rebuilding this branch does not carry it over, and
neither does creating a second prefix for testing. When a JUCE plug-in that used to work
renders nothing in a new prefix, check this first — the symptom looks like a broken build
or a regression in the stack, but the renderer has simply fallen back to GDI again. Seen
here on a freshly set up test prefix, where a WebView2-based plug-in stayed blank until
the value was added for the host.

**Why not system-wide:** the switch applies to the whole process, so anything else probing
for Wine stops finding it — and some software depends on the answer. Two cases seen here:
PACE/iLok protected software fails to start with *"Error 2000: An iLok background component
required to validate the license for this product is not running"*, and Ableton Live's
embedded Splice view stops loading. Both fail in a way that points anywhere but at Wine,
which is exactly why the default is off.

An upstream fix is proposed as [juce-framework/JUCE#1701](https://github.com/juce-framework/JUCE/pull/1701),
which would add the same opt-in inside JUCE and make this workaround unnecessary.

### Building

```bash
git clone https://github.com/giang17/wine.git
cd wine
git checkout d2d1-dcomp-11.0
./configure --prefix=/opt/wine-d2d1 --enable-win64
make -j$(nproc)
sudo make install
```

**Important**: The `--enable-win64` flag is required — without it only 32-bit is built and
64-bit DLLs (including d2d1) will be missing. Use a separate `--prefix` to avoid overwriting
your distro's Wine installation.

**ntsync** (recommended): If your kernel has the `ntsync` module (Linux 6.12+), make sure
`/usr/include/linux/ntsync.h` exists before running `./configure`. This enables kernel-level
NT synchronization primitives, significantly reducing audio latency (stable at 64 samples /
48 kHz). Check with `grep HAVE_LINUX_NTSYNC_H include/config.h` after configure.

**Bottles users**: If you need 32-bit support (e.g. for Bottles), use
`--enable-archs=i386,x86_64` instead of `--enable-win64` (thanks to @jibeape for
figuring this out).

**DXVK compatibility**: DXVK and this patch set are **not a hard conflict**, but they
don't combine. The DComp/DXGI patches (DComp popup handling, GL SwapBuffers) live in
Wine's builtin `dxgi.dll` — DXVK *replaces* that DLL with its own Vulkan-based
implementation and so silently bypasses the composition-swapchain/DComp patches.
The two can coexist on disk and be switched per-application via `WINEDLLOVERRIDES`:
`d3d11,dxgi,d3d10core=n` activates DXVK, `=b` uses the patched builtin with the DComp
path. **Switch the whole trio together** — overriding only part of it mixes DXVK's
`d3d11.dll` with Wine's `dxgi.dll`, which use different internal COM interfaces
(`DxgiSwap*` / `D3D11DXGI*` vs `IWineDXGI*`) and crash. For DComp-based plugins
(Serum 2, Korg Trinity, Pianoteq 9) DXVK offers no benefit anyway — D2D1 draws go
through a bitmap+BitBlt path, not the DXGI swapchain, so WineD3D performs equally
well. The simplest, recommended setup is to **not install DXVK at all**, so the
builtin DComp path is always used and no overrides are needed.

**GL present for top-level windows** (default ON): D3D11 swapchains on top-level
windows present through `glXSwapBuffers` directly from the GPU instead of the
GDI readback path — this removes a large per-frame GPU→CPU copy (order of
650 MB/s display-server traffic during continuous UI activity in Ableton Live)
and fixes main-window flicker when an app hosts WebView2 content (Ableton
Live's Learn View). `WS_CHILD` and `WS_POPUP` windows keep the GDI path.
Adopted from shibco/ableton-linux patch 0055 (diagnosis: ClickSentinel).
If you see misplaced frames (reported once on niri/Wayland: content shifted
down, black band on top), set `WINE_DISABLE_GL_PRESENT=1` to restore the old
GDI path for every window.

**Serum2 settings** (recommended — DComp gives the best performance):
- `"Disable DirectComposition": false`
- `"Disable Partial Redraw": false`

The GDI fallback path (`"Disable DirectComposition": true`) is also supported
since commit `e04e6dfd` (`d2d1/winex11.drv: Skip offscreen XComposite for
ID2D1HwndRenderTarget windows`). Earlier versions of this branch left the
plugin window black until the user moved it or hovered the mouse over it,
because `wined3d`'s GDI present path never triggered the X11 client surface
composite for offscreen-redirected child windows. The fix marks the HWND so
`needs_offscreen_rendering()` returns FALSE, attaching the plugin's X11 child
directly to the host's toplevel.

**SynthEdit / GMPI plugins**: Plugins built with SynthEdit — recognisable by the
`.sem` modules inside the bundle — render through GMPI's DirectX backend, which
maps straight onto the modern, colour-space aware D2D1 interfaces
(`ID2D1GradientStopCollection1`, the Color Management effect, sRGB bitmaps).
Without those patches such a plugin crashes on load, or comes up with a black
window, or draws its GUI far too dark. No configuration is required — the fixes
apply automatically. VProm3 is the plugin these were developed against, but they
are not specific to it.

## Tested Applications

| Application | Framework | Status |
|-------------|-----------|--------|
| Serum2 (VST3 in Reaper) | VSTGUI + DComp | Fully functional, all waveform views + envelopes + presets; dragging the envelope, LFO and macro handles shows the drag bitmap with per-pixel alpha and no flicker |
| VProm3 (VST3 in Reaper) | SynthEdit/GMPI + D2D1 | Fully functional, correct colours (needs the Color Management effect patches) |
| Korg Trinity (VST3 in Reaper) | JUCE 8 + DComp | Fully functional, DComp rendering path |
| Korg Prophecy (VST3 in Reaper) | JUCE 8 + DComp | Fully functional |
| Pianoteq 9 (standalone + VST3) | JUCE 8 + DComp | Fully functional |
| FL Studio (Wine + ntsync) | Custom | Runs without xruns at 64 samples / 48 kHz; Cloud plugins install and stream (needs the AF_UNIX patches) |
| UVI Portal | WebView2 | Installs and signs in, including special characters typed into the login fields |
| WineSynth (custom VSTGUI plugin) | VSTGUI + DComp | 18k+ partial redraws without crash |
| Ableton Live 12 (Intro / Lite) | Custom (D3D11 + WebView2) | Fully functional — window decorations, stable move/resize, F11 fullscreen both ways, menu bar hit testing. See *Ableton Live 12 Setup* below |
| Fender Studio Pro 8 | CCL (DXGI + DWrite + DComp) | Fully functional — the song view draws completely and stays stable, no stale tool bar or transport and no flicker; the transport playhead and the selection rectangle no longer flicker while the transport runs, and video on the timeline plays, seeks, loops and jumps without stalling or going black. Starting at all needs the `UIAnimationManager2` and `UIAnimationTransitionLibrary2` stubs from this branch; without them the CCL framework aborts with "requires Windows 10 or newer" |

## Font Setup

The DWrite patches fix font rendering in VSTGUI-based plugins, but some applications
also require system fonts to be configured correctly (Unicode symbols, GDI menus).

**[documentation/wine-font-setup.sh](documentation/wine-font-setup.sh)** does the
host part for you:

```bash
documentation/wine-font-setup.sh --prefix ~/.wine
documentation/wine-font-setup.sh --prefix ~/.wine --check   # report only
```

It locates the fonts through fontconfig (so distribution paths do not matter),
copies them into the prefix, registers the MS Core Fonts for GDI, writes the
`FontLink` fallback chain and switches on this branch's text rendering. Re-run it
after `wineboot -u`, which resets the FontLink entries to Wine's defaults — the
script is idempotent.

Three of those steps matter more than they look:

- **MS Core Fonts** are not cosmetic. Some plugins open font *files* directly
  during DLL initialisation — FL Studio's "Fruity Delay 3" opens
  `C:\windows\Fonts\Arialbd.ttf` — and crash with an access violation and no
  usable error message when they are missing.
- **FontLink** is what makes symbol glyphs resolve; without it Serum 2's star
  ratings render as tofu boxes.
- **The text rendering switches live in the prefix, not in the build.** Enhanced
  contrast, linear blending and outline rasterisation are all read from the
  registry once at startup, and each falls back to stock behaviour when its value
  is absent. A prefix that never saw this script therefore renders text the way
  stock Wine does, however carefully the branch was built — and nothing in the
  log says so. If text looks unchanged after installing this branch, check here
  before suspecting the build:

  ```bash
  documentation/wine-font-setup.sh --prefix ~/.wine --check
  ```

  The values themselves are `text_enhanced_contrast` and `text_linear_blend`
  under `HKCU\Software\Wine\Direct2D`, and `outline_in_natural_modes` under
  `HKCU\Software\Wine\DirectWrite`. Enhanced contrast is also in winecfg's
  graphics tab (*Off* / *Medium (50)* / *Strong (70)*); the script leaves an
  existing value alone unless `--contrast N` is given, so a choice made there
  survives a re-run.

See **[documentation/wine-font-setup-guide.md](documentation/wine-font-setup-guide.md)**
for the full guide, including the container setup and working around the missing
`BitPDisp-10` tooltip font in Serum 2, which the script deliberately leaves out.

Not every application needs any of this. Some ship their typefaces as binary
resources and load them with `AddFontMemResourceEx` into a private DirectWrite
collection, never touching the prefix' font directory for their own UI — Fender
Studio Pro 8 loads 25 faces that way, from its "Nimbus Sans Novus" interface
family to the notation fonts. Running the setup script changes nothing for those,
and text that looks wrong in such an application is not a missing system font.
The guide's *Which font is the application actually using?* section tells the two
cases apart from a single `WINEDEBUG=+font,+dwrite` log.

### Subpixel (ClearType-style) text

The subpixel text patches are inert until the prefix says it wants them. A fresh
prefix carries no `FontSmoothingType`, and its absence means "follow the host", so
these three values are what turns the feature on:

```bash
wine reg add 'HKCU\Control Panel\Desktop' /v FontSmoothing            /t REG_SZ    /d 2 /f
wine reg add 'HKCU\Control Panel\Desktop' /v FontSmoothingType        /t REG_DWORD /d 2 /f
wine reg add 'HKCU\Control Panel\Desktop' /v FontSmoothingOrientation /t REG_DWORD /d 1 /f
```

`FontSmoothingType` is the switch: 2 is ClearType, 1 is greyscale. Orientation is
1 for an RGB panel and 0 for BGR — a BGR panel driven as RGB fringes the wrong way.
Match it to the host (`XftSubPixel` in `kdeglobals`, or
`gsettings get org.gnome.desktop.interface font-rgba-order`).

What to expect: DirectWrite text is rendered with three coverage samples per pixel
and reaches the screen that way. Measured on Fender Studio Pro 8, counting pixels
whose channels differ by more than 15 of 255 — that is, pixels carrying a visible
colour fringe:

| | body text | file list |
|---|---|---|
| without the patches | 0.00% | 0.00% |
| with them | 29.05% | 8.47% |

In `d2d-font-test` the CLEARTYPE and DEFAULT lines come out at 22–25%, while the
GRAYSCALE and ALIASED lines stay at exactly 0.00% — those two are the control, and
their staying at zero is what makes the measurement trustworthy. FL Studio's Sounds
tab is a good place to see the difference by eye.

Note that a font's embedded bitmap strikes used to defeat this: where a face carries
one, FreeType returns the strike, which has a single coverage sample per pixel. Wine's
bundled Tahoma carries strikes for 8 to 16 ppem and `MS Shell Dlg` resolves to Tahoma,
so the default interface font lost its subpixel resolution across the whole size range
interface text uses — and, the strike being 1bpp, was not even greyscale antialiased.
DWrite now asks for the outline when the caller wants a ClearType texture; a face with
no outline for a glyph keeps its strike, so the fallback is what the result would have
been anyway.

### Greyscale text and embedded strikes

The above fixed the ClearType half. Greyscale text still takes the strike, and the
result is inconsistent in a way that is easy to see once you look for it: Tahoma does
not carry a strike for *every* size in its range, so at 96 dpi the character of the
text flips between neighbouring sizes — 12, 15 and 16 px come out as hard 1-bpp
bitmaps while 14 px and everything from 17 px up is antialiased.

Windows draws this line differently: it selects by **rendering mode**, not by whether
subpixel output was requested. The GDI-compatible modes take the strike, the NATURAL
modes take the outline — and applications measured here request NATURAL exclusively
(Fender Studio Pro 8 in 1625 of 1625 glyph-run analyses).

```bash
wine reg add 'HKCU\Software\Wine\DirectWrite' /v outline_in_natural_modes /t REG_DWORD /d 1 /f
wineserver -k
```

With the key set, greyscale follows the same rule as Windows and the size-to-size
inconsistency disappears. Leaving it unset keeps the current behaviour exactly —
verified at 0 of 291580 pixels difference. It is opt-in because the strike is
hand-tuned and crisp: switching to the outline makes small greyscale text softer,
which is a matter of taste. If you run with `FontSmoothingType=2` it barely matters,
since almost everything then goes through the ClearType path anyway.

`projects/d2d-font-test/d2d_strike_test.cpp` renders the size ladder in both modes
side by side if you want to judge it yourself.

GDI text is unaffected on hosts whose fontconfig already resolves subpixel per font,
which is the common case on KDE and GNOME.

### Linear-space text blending

Coverage is a geometric area, so a correct composite linearises source and
destination, mixes there, and encodes the result back. Direct2D's text path mixes
in the target's *encoded* space instead, which makes the same text carry a
different amount of ink depending on its polarity — light text on dark comes out
thinner than dark text on light, from the arithmetic alone.

```bash
wine reg add 'HKCU\Software\Wine\Direct2D' /v text_linear_blend /t REG_DWORD /d 1 /f
wineserver -k
```

Measured against identical text drawn in both polarities, where a correct blend
gives zero: the mean deviation is **-0.30 at 11px and -0.20 at 24px** without the
key, worst case -0.5719 — exactly the value half coverage predicts. With the key
the mean falls to **+0.0004** and the worst case to 0.0058, which is 8-bit
quantisation. The blend is then as accurate as the target format permits.

Reported effect on a dark audio interface: black on white becomes noticeably more
restrained and cleaner in its stroke, white on black slightly heavier, which reads
better at small sizes. Both are the expected direction — the correction removes the
polarity bias rather than adding weight everywhere.

**It costs about 500 µs per `DrawText`**, tripling the ClearType surcharge from
roughly 215 µs to 700 µs, or +12 to +13% of the total cost of a `DrawText` call.
That is why it is off by default. With the key unset, rendering is bit-identical
to not having the feature: 0 of 341156 pixels differ.

Two things worth knowing before enabling it:

- It **deliberately departs from Direct2D** on a plain UNORM target, where Windows
  blends linearly in encoded values. This is "more correct than the original",
  which is not automatically what an application expects.
- The obvious cheaper route — let an `_SRGB` render target view make the hardware
  do the conversion — **does not work in Wine**, and fails silently. wined3d
  honours the sRGB cast only for swapchains with a single back buffer; for flip
  models and composition swapchains `CreateRenderTargetView` succeeds and the
  encoding does not happen. The implementation therefore copies the destination
  under the glyph run and finishes the blend in the shader.

### Enhanced contrast — worth setting on a dark interface

DirectWrite reports an *enhanced contrast* value alongside gamma and ClearType
level; Wine hardcodes it to zero, so nothing is applied. This branch honours it and
lets the prefix override it:

**In winecfg:** *Graphics* tab, *Direct2D text*, "Enhanced contrast" — three named
settings plus whatever a registry edit put there. Or by hand:

```bash
wine reg add 'HKCU\Software\Wine\Direct2D' /v text_enhanced_contrast /t REG_DWORD /d 70 /f
wineserver -k    # the value is read when d2d1 loads, so restart the application
```

The value is in hundredths; deleting it restores the default. It raises partial
coverage while leaving fully covered and empty samples alone, so at 0 — the default
— rendering is bit-identical to not having the feature at all. `AppDefaults` works
as for the other keys, so it can be set for a single application.

**The default needs no defending.** At 0 the text is already clean — assessed on a
dark audio interface by a graphics designer, who described the untouched rendering
as reading "like print, not bold", with clean edges in both polarities under
magnification. Nothing here is a repair; the key is a preference, not a fix.

**Why you may still want a non-zero value:** light text on a dark background looks
thinner than it geometrically is, because stray light in the eye eats into the edges
(irradiation). Audio software is almost entirely dark-themed, and tightly set body
text suffers most. Raising the contrast puts that weight back.

| value | when |
|---|---|
| `0` | default. Clean as it stands; leave it unless you have a reason |
| `50` | what Windows runs. A safe middle if you want a little more weight |
| `70` | dark interfaces with dense body text. Measured to give the clearest reading of the three on such a UI |

Reported result on a dark interface at 92 DPI with `hintslight`: at 0.70 text reads
"clearer, cleaner and easier to read, minimally thicker" on both an ordinary and a
rotated monitor, most visibly in tightly-spaced body text such as a news feed.

On a **light** background the effect works against you — there the same setting makes
text look heavy. That, and the fact that 0 already looks right, is why the default
stays 0 and this is opt-in. Watch small sizes if you go high: at 11px a strong
setting can start closing the counters in `e`, `a` and `g`.

Unrelated to the above, FL Studio's Piano Roll needs one more font fix to show
flat/sharp symbols (♭ ♯) instead of tofu boxes — FL bypasses Wine's font
fallback through `GetGlyphIndices`. That one has its own project:
[giang17/flstudio-wine-font-fix](https://github.com/giang17/flstudio-wine-font-fix).

## Ableton Live 12 Setup

Live runs its content indexer as a separate process (`Ableton Index.exe`). On
Wine's built-in `msvcp140` that process dies immediately and Live restarts it in
a loop, which leaves the browser without content and pops up an error dialog.
This is a prefix setup issue, not something the patches in this branch address.

**[documentation/ableton-live-12-setup.sh](documentation/ableton-live-12-setup.sh)**
does both checks and applies the override:

```bash
documentation/ableton-live-12-setup.sh --prefix ~/.wine-ableton
documentation/ableton-live-12-setup.sh --prefix ~/.wine-ableton --check  # report only
```

It installs nothing by itself: if the native runtime is missing it prints the
`winetricks` command and stops, and `--check` reads the prefix without starting
Wine. Use `--wine /path/to/wine` if this fork is not the `wine` in your `PATH`.

The two steps it performs, in case you prefer doing it by hand:

1. Prefer the native runtime **for that executable only**, so the rest of the
   prefix keeps using Wine's builtins. Under
   `HKCU\Software\Wine\AppDefaults\Ableton Index.exe\DllOverrides`, set
   `concrt140`, `msvcp140`, `msvcp140_1`, `msvcp140_2`, `msvcp140_atomic_wait`,
   `msvcp140_codecvt_ids`, `vcruntime140` and `vcruntime140_1` to
   `native,builtin`.
2. **Install the native runtime itself** (e.g. `winetricks vcrun2019`).
   `native,builtin` silently falls back to the built-in DLL when no native one
   is present — the override looks correct in the registry and the indexer still
   crashes.

Symptom to look for in `Preferences/Log.txt`:

```
Indexer: process stopped prematurely [1]. Restart
```

When checking which DLL a prefix actually has, **do not compare file sizes**:
Wine's built-in PE carries debug symbols and is roughly 5 MB, i.e. *larger* than
the native DLL (~550 KB). Check the origin instead:

```bash
strings -a "$WINEPREFIX/drive_c/windows/system32/msvcp140.dll" \
    | grep -m1 -E "Wine builtin DLL|Microsoft Corporation"
```

## D2D1 Patches Only (Branch: `d2d1-v6`) — DEPRECATED

> **Status (2026-05-03)**: This branch is **deprecated**. Last update was 2026-02-14
> (15 patches against vanilla Wine 11.0). Since then, 41+ further D2D1 commits have
> landed in `d2d1-dcomp-11.0` (PushLayer stencil clipping, ImageBrush UV fix, scratch
> buffers, intersect_self grid, FillRectangle cache, Sivov's Stream-Path + Bezier-Arc,
> and 15 misc upstream bug fixes pre-baked from 11.x devel). `d2d1-v6` no longer
> reflects the current state of the D2D1 work.
>
> **Replacement plan**: No new bundled upstream-submission series (`d2d1-v7`) is
> planned. WineHQ's Clean-Room Guidelines disallow LLM-generated code, so large
> AI-assisted patch series are out of scope for upstream. Instead, small targeted
> bug fixes go upstream opportunistically via a Codeweavers maintainer sign-off
> (precedent: Wine bug 59718 — three arc fixes merged with Sivov's `Signed-off-by`).
> The fork itself stays as a rolling devel-tracking distribution.
>
> The `d2d1-v6` branch is kept on the fork for historical reference only. The
> patch ZIPs in `patches/v6-full/` and `patches/v6-full-11.0/` are similarly frozen.

This branch contains only the 15 D2D1 rendering patches without DComp, DWrite, or
performance fixes. It was originally maintained as a clean upstream reference.
**For normal use, prefer `d2d1-dcomp-11.0` above.**

The 15 D2D1 patches in this branch:

1. **AddArc implementation** -- path geometry arc segments (later replaced upstream by Sivov's Bezier-Arc, see `d2d1-dcomp-11.0`)
2. **Font rendering** -- force NATURAL rendering mode for better quality
3. **CDT iterative conversion** -- convert recursive constrained Delaunay triangulation to iterative
4. **Miter limit clamping** -- stroke outline miter limit support
5. **Shader-based antialiasing** -- smooth edges for geometry rendering (fill + outline)
6. **Text rendering fix** -- fix interaction between shader AA and text rendering
7. **CDT cycle detection** -- replace blind iteration guard with proper cycle detection
8. **Stroke outline join fix** -- fix spike artifacts from collinear/hairpin joins
9. **Debug marker cleanup** -- remove debug markers from geometry sink
10. **Stroke style rate-limit** -- rate-limit stroke style FIXME messages
11. **CDT bailout removal** -- remove 512-vertex CDT bailout (protected by cycle detection)
12. **CDT segment skip** -- skip failed constraints instead of aborting triangulation
13. **CDT diagnostic rate-limit** -- rate-limit collinear cycle detection diagnostics
14. **Outline edge AA** -- anti-aliasing for straight line outline edges
15. **Gradient premultiply** -- premultiply alpha in gradient stop colors for correct blending

These patches are self-contained and applicable to vanilla Wine 11.0 with `git am` or `patch -p1`.

## Related

- Discussion: [yabridge#413](https://github.com/robbert-vdh/yabridge/issues/413)
- Upstream: [wine-mirror/wine](https://github.com/wine-mirror/wine)
- Subpixel text: [shibco/ableton-linux](https://github.com/shibco/ableton-linux),
  pull request 155 by Cade (@shibco), who wrote the nine patches and wants to take
  them to WineHQ. Reviewed there by @ClickSentinel, whose measurements are worth
  reading before touching this code

## License

Same as Wine -- GNU LGPL. See [LICENSE](LICENSE) for details.
