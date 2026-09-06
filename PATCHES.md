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

**Base**: Wine 11.0 stable (`wine-11.0` tag) — this branch.

## Branches

| Branch | Description |
|--------|-------------|
| `d2d1-dcomp-11.0` | **Recommended for stable users.** Full stack on Wine 11.0 stable: D2D1 + DComp + DWrite + WineD3D performance + winex11. Actively maintained. |
| `d2d1-dcomp-11.<N>` (newest devel) | **Recommended for devel / rolling-release users** (e.g. `wine-tkg-dev`). The same full stack as `d2d1-dcomp-11.0`, rebased onto the latest WineHQ devel tag and rolled forward roughly every 2 weeks (currently `d2d1-dcomp-11.16`). **Use the highest-numbered `d2d1-dcomp-11.*` branch**: it is always the current rolling base; lower-numbered ones are previous snapshots, superseded. Plugin-tested by the maintainer before each push. |
| `d2d1-v6` | **Deprecated** (last update 2026-02-14). The original upstream-targeted D2D1-only series; superseded by the D2D1 work in `d2d1-dcomp-11.0`. Kept for reference. |

## Full Stack (Branch: `d2d1-dcomp-11.0`)

This is the recommended branch. What it changes, by subsystem:

- **D2D1 rendering**: the geometry pipeline that started as `d2d1-v6` — path geometry
  arcs, iterative constrained Delaunay triangulation with cycle detection, miter limits,
  shader-based antialiasing for fills, outlines and text, correct stroke joins,
  premultiplied gradient stops — plus PushLayer stencil clipping, an ImageBrush UV fix,
  scratch-buffer reuse and a constant-buffer dirty check on the hot path, and a private
  heap that keeps high-frequency geometry allocations out of the process heap. Cubic
  Bézier segments are subdivided before they are approximated, so an S-shaped curve — a
  filter response, an envelope, an EQ display — keeps its inflection instead of
  collapsing into a plain arc, and `GetBounds()` bounds the same chain. Consecutive
  `DrawLine()` calls with a solid brush are batched into one upload and one draw per
  colour (Serum 2's 3D wavetable view draws 68 352 lines that way: 2D→3D toggle 1 060 ms
  → about 80 ms). Bitmap brushes honour WRAP and MIRROR instead of degrading to CLAMP,
  so a tiled background is tiled rather than one tile with its edge texels stretched
  (SynthEdit panels). `SetTarget(NULL)` drops the render target bindings, so a swapchain
  back buffer is not held across `ResizeBuffers()`
- **D2D1 layers on an opaque target**: a layer bitmap inherited the target's alpha mode,
  so on a render target created as `ALPHA_MODE_IGNORE` the layer's own coverage was thrown
  away and everything drawn inside it turned the background colour, usually black. All
  three `PopLayer()` paths now composite the layer as premultiplied. Visible as rectangular
  black areas swallowing clip contents and volume curves in Fender Studio Pro 8
- **D2D1 effects and colour**: Color Management effect (registration plus the
  scRGB → sRGB transfer function applied in the shape pixel shader),
  ID2D1GradientStopCollection1, sRGB pixel formats for WIC-sourced bitmaps — required by
  GMPI/SynthEdit plugins, which render linearly in 16-bit float and composite their GUI
  through the Color Management effect. `CLSID_D2D1GaussianBlur` was registered, so
  `CreateEffect()` succeeded, but `DrawImage()` had no case for it and drew nothing — JUCE's
  `GlowEffect` rendered empty under Direct2D; the blur is now evaluated over three standard
  deviations, for A8 and 8-bpc BGRA/RGBA
- **DComp**: IDCompositionDesktopDevice implementation with a D2D1 bitmap rendering path,
  dirty-rect clipping and DIB+BitBlt presentation; IDCompositionDevice3/4/5, composition
  and dynamic textures, D3D11 BeginDraw and surface handle export; rootless visual trees
  composited onto the target window at ~60 Hz, cross-process targets, backdrop capture
- **DComp leaves in the presented frame**: a visual tree that covers only a sliver of its
  window — a transport playhead, a selection rectangle — used to be delivered *after* the
  application's present by reading back the window and blitting, a race no CPU-side blit
  can win. Such leaves are now composited into the buffer that is about to become the
  frame, in the GL present path. Measured on Fender Studio Pro 8: the playhead was missing
  in 14.9 % of captured frames before and 0.0 % after. A layer nobody draws would hide the
  leaves in every frame, so dcomp returns to the blit path on its own after about 200
  undrawn deliveries. The Vulkan present path is untouched
- **UIAnimation**: `UIAnimationManager2` and `UIAnimationTransitionLibrary2`. Wine ships
  only the version 1 classes, so an application that creates the version 2 pair while
  bringing up its Direct Composition engine fails there. Applications built on the CCL
  framework — Fender Studio Pro 8, PreSonus Studio One 6.6 and newer — then abort with
  "This application requires Windows 10 or later", the framework's generic alert for a
  graphics engine that did not start, not a version check
- **DWrite**: `DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC` is accepted instead of rejected,
  IDWriteFontSet::GetMatchingFonts is implemented, and the font fallback maps
  Miscellaneous Symbols and Arrows (U+2B00-2BFF), which fixes the star ratings in Serum 2,
  and Geometric Shapes (U+25A0-25FF), which fixes the password mask of Fender Studio Pro's
  login dialog (U+25CF drawn with a private font that lacks it).
  The per-fontface glyph cache used to die with the fontface, and text layout creates one
  per run: 17 208 fontfaces and 111 598 re-rasterisations of 196 distinct glyphs during a
  25 s SynthEdit resize. The cache now outlives the fontface, keyed by font file and face
  index
- **ClearType-style subpixel text** (from Cade / @shibco, `shibco/ableton-linux`):
  a glyph run asking for `DWRITE_TEXTURE_CLEARTYPE_3x1` used to be rendered once in
  greyscale and copied into all three subpixels. DWrite now rasterises the outline scaled
  three times horizontally and filters each row with the five-tap FIR FreeType uses for
  `FT_RENDER_MODE_LCD`; D2D1 blends such runs once per colour channel; GDI takes its
  antialiasing from the prefix rather than the host's fontconfig. Needs the registry keys
  under *Font Setup* below — without them nothing changes. See `Related` for the origin
- **DXGI**: composition swapchain, FLIP_SEQUENTIAL preservation, DComp popup handling,
  monitored fence support. The swapchain-to-window mapping is published on the desktop
  window, whose property list is shared across the window station, so it is keyed by
  owning process id — otherwise a host application and its WebView2 child can allocate a
  swapchain at the same address and resolve each other's window
- **D3D11**: ID3D11Fence with CPU timeline semantics. The `Discard*()` hints log at TRACE
  instead of FIXME — a WebView2 plug-in issued them about 120 times per second, 95 % of
  the log of a 9.5-minute Reaper session
- **WineD3D**: composition buffer with dirty rect accumulation, GL buffer recycling pool
  (70 % RSS reduction), and a `vs_out` initialisation that no longer trips NVIDIA's shader
  compiler warnings
- **ntdll**: MADV_FREE for MEM_RESET (improved page reclaim behaviour)
- **Per-pixel alpha for GPU-painted layered windows**: `DwmExtendFrameIntoClientArea`
  with `margins = -1` asks for full glass, which on Windows makes the client area
  per-pixel alpha capable. Wine stubbed it, so a plugin dragging a bitmap around — Serum
  2's envelope, LFO and macro handles — showed an opaque black box instead of the bitmap.
  Alpha capability is now a property of the window: dwmapi records the request, winex11
  keeps it in the window data, and both the window and its GL child end up on an ARGB
  visual, chosen once when the window is created so no visual switch happens mid-drag.
  A GL drawable released through `ReleaseDC` is parked on the window for the next DC
  instead of being rebuilt every frame. Measured over a 20 s drag: one GL client window
  instead of 8, no sample without a window, no brightness jump in the dragged bitmap.
  On by default; `WINE_ARGB_PIXFMT=0` turns the whole path off without a rebuild. Smoke
  tested across Korg Trinity, EPROM Memory Rites, FL Studio, Fender Studio Pro 8 and
  Ableton Live 12
- **winex11**: DComp window support, backing store; ownerless TOOLWINDOW popups are no
  longer folded into the active window's group, given a transient_for owner, or mapped as
  UTILITY — this fixes sticky, wrongly decorated and always-on-top plugin menus on KDE;
  the opaque black X expose background is suppressed while a GL/D3D client window is taken
  off the screen for offscreen rendering, so the client area no longer flashes black for a
  frame (visible in Ableton Live 12 when toggling the Learn View panel); a window hidden
  while its map was still unacknowledged no longer stays mapped and unpainted for the
  session (upstream fix for Wine bug 59932, cherry-picked; FL Studio's Browser panel at
  startup)
- **Window-surface repaints (win32u, winex11)**: a series of erase-and-repaint races in
  the window-surface path. Flushes are held back while an erase waits for its repaint,
  `XShmPutImage` is waited for before the surface is painted into again, a new surface
  inherits its predecessor's pixels, a plain move invalidates only child windows, the
  xrender PutImage path is flushed, and a client window that will render offscreen is
  created under the dummy parent. Seen as MDI captions going dark during a resize, a
  black toolbar after a menu and a stale MDI client after a drag in SynthEdit 1.5, and as
  a one-frame black flash on a fast resize in FL Studio
- **win32u**: transparent (0x00) surface init for ARGB popups; the system arrow is shown
  again when an application hides the cursor and sets none; cursors are process-local in
  Wine, so a `WM_WINE_SETCURSOR` for an out-of-process child window (WebView2, bridged
  plug-ins) arrived with a handle the receiving process rejected and the previous cursor
  stayed — the owner now publishes each cursor's first frame in a named section and the
  receiver builds a proxy from it
- **wineserver**: a top-level's surface flush no longer overwrites child windows that
  belong to a *different process*. Such a child draws straight into the top-level's
  drawable while the owner flushes its own surface over it with a delay — a black
  one-frame flash over embedded panels whenever the two overlap. Its client rect is now
  subtracted from the surface region, the treatment GL and Vulkan children already
  receive. Affects any application that embeds out-of-process content: FL Studio's hub,
  Ableton Live 12 (WebView2), CEF-based plugin GUIs. Trade-off: a foreign child that stops
  painting without a geometry change leaves its last frame on screen until something
  moves. `WINE_DISABLE_FOREIGN_CHILD_CLIP=1` restores the previous behaviour
- **ole32**: RevokeDragDrop no longer touches drop targets owned by other processes
  (fixes a crash when closing plugin windows)
- **crypt32**: verifying a signature hashes the signed attributes as they are stored in
  the message instead of re-encoding them. The encoder sorts a SET OF into DER order, but a
  signer may emit the attributes unsorted and the signature covers the order it used, so
  re-encoding produced a different hash and verification failed with
  TRUST_E_CERT_SIGNATURE. Affects any application that checks its own Authenticode
  signature at startup; FL Studio 2026 reported that the program could not be verified and
  exited
- **ws2_32**: `SIO_ADDRESS_LIST_SORT` was undefined and every call got WSAEOPNOTSUPP;
  Chromium issues it for every resolution that returns an IPv6 address and discards the
  result on failure (FL Studio 2026's WebView2 hit it 35 times in 18 minutes). Addresses
  are sorted per RFC 6724 §6 with the default policy table
- **msvcp**: the stream classes' move constructors and move assignment were exported as
  stubs, so a C++ application calling one aborted with "Call to unimplemented function" —
  Ableton Live's indexer did, on startup in a fresh prefix, so Live restarted it in a loop
  and its browser stayed empty. Implemented for msvcp110, msvcp120, msvcp140 and msvcp_win;
  Live no longer needs a native VC runtime in the prefix
- **Virtual-desktop compositor (winex11)**: windows inside a Wine virtual desktop get
  real per-pixel alpha, which the plain desktop drawable cannot provide. A small
  XDamage-driven compositor assembles frames off screen and composites them through an
  ARGB overlay onto an opaque child window, so translucent plugin popups and drop shadows
  blend correctly instead of showing black or leftover pixels
- **AF_UNIX sockets (ws2_32, wineserver, ntdll)**: Unix-domain socket support, based on
  the long-standing wine-staging patch set plus hardening and five conformance fixes of
  our own — a socket is given its family before bind, a bound socket is reported as a
  reparse point by GetFileAttributes and can be opened and queried through CreateFile with
  FSCTL_GET_REPARSE_POINT, an unbound socket is auto-bound with a path, and short addresses
  are handled. With these the ws2_32 AF_UNIX conformance tests run at all (upstream they
  skip) and pass. Needed by applications that talk to a local helper over a Unix socket;
  FL Studio's Cloud plugins install normally instead of spinning on a socket that never
  appears
- **Keyboard input into out-of-process content (wineserver, winex11)**: a process that
  translates keys for a window owned by *another* process now has the whole thread input
  family attached, derives the AltGr modifier from the passed key state rather than from a
  process-global cache of the last X11 event, and posts a finished IME result string as
  WM_CHAR as well. Together these make typing work in embedded WebView2 fields, including
  AltGr characters such as `@`, `\` and `€` on non-US layouts
- **Media Foundation video playback (mf, evr, winegstreamer)**: playing and scrubbing
  video on a DAW timeline exercises paths a straight play-to-the-end never reaches. The
  session asked for a new sample on *every* call of its delivery routine instead of one at
  a time, so a topology rebuild produced 5155 requests in one second and the samples piled
  up on a transform input that was not consuming yet, until the picture stopped; sample
  requests now run on a work queue of their own per stream. The renderer accepts samples
  that arrive while paused, the presenter reinstalls its allocator notification after an
  output type renegotiation and no longer pulls mixer output through a NULL mixer pointer
  during teardown, and the session is told when a scrub has been carried out. Together
  these fix seeking, looping, timeline jumps, playback stalling after 20-35 seconds,
  stuttering after a large seek, and a black picture after a window toggle in Fender
  Studio Pro 8
- **windows.security.authentication.web.core**: WebAuthenticationCoreManager
  implementation, for applications that probe the WinRT web-account API on startup
- **Direct2D for JUCE 8.0.13+ (ntdll, wine.inf)**: JUCE 8.0.13 and later pick their
  renderer with `GetProcAddress(GetModuleHandleA("ntdll"), "wine_get_version") != nullptr`
  and fall back to GDI whenever that succeeds, which bypasses this entire stack — a JUCE
  plugin then never creates a device context and never gets a composition swapchain. This
  branch hides that one export inside the plug-in hosts, so those plugins take the Direct2D
  path again. **On by default for the DAWs listed in `wine.inf`**, off elsewhere — see the
  note below

### Note: JUCE 8.0.13+ and the hidden `wine_get_version` export

Since JUCE 8.0.13 (upstream commit `5179690ff7`, "Restore Wine functionality") every JUCE
plugin falls back to the GDI renderer as soon as it detects Wine. That fallback exists
because stock Wine does not implement the Direct2D 1.3 / DirectComposition surface JUCE
needs — but this branch does, so the fallback only costs functionality here. Measured with
one plugin: 23 d2d1 calls with the fallback, 2.6 million without it. What the GDI path
loses is bitmaps: background artwork, icons and keyboard graphics vanish while text and
vector shapes survive, which is the fingerprint to look for.

This branch therefore hides `wine_get_version` from `GetProcAddress`. Doing so restored
the complete UI of a WebView2-based JUCE plugin whose artwork, icons and content had been
missing, and removed flicker that had been chased for weeks in the wrong place.

The switch is a per-process registry value. `wine.inf` (section `[JuceHosts]`) sets it for
the plug-in hosts that have been checked here, so a prefix created or updated with this
branch has it without further setup:

```
reaper.exe · FL64.exe · Studio Pro.exe · Studio One.exe · Ableton Live 12 {Intro,Lite,Standard,Suite,Trial}.exe
yabridge-host.exe · yabridge-host-32.exe
BitwigAudioEngine-X64-{AVX2,SSE41}.exe · BitwigPluginHost-{X64-AVX2,X64-SSE41,X86-SSE41}.exe
Cubase{12,13,14,15}.exe
```

The entries use the INF "do not overwrite" flag, so a value you set yourself — including
`"N"` to turn the switch off for one of these hosts — survives every prefix update.
Existing prefixes pick the defaults up on their next start after the branch is installed
(the usual `wineboot` update). For any other host, or for a single run:

```bash
# single run
WINE_HIDE_WINE_VERSION=1 wine your-host.exe
```

```
# per application
HKCU\Software\Wine\AppDefaults\your-host.exe\HideWineVersion = "Y"
# globally, if you know what runs in this prefix (not recommended, see below)
HKCU\Software\Wine\HideWineVersion = "Y"
```

The per-application value wins over the global one, so `"N"` can carve out an exception
where the global switch is on. **It keys on the process name, not on the plug-in**: a
plug-in that renders correctly in one host and loses its artwork in another has simply
landed in a host without the value. Bridged hosting runs the plug-in in a different
process, which needs its own entry: yabridge's hosts are in the list (checked with JUCE
8.0.13 plug-ins in a Linux DAW), FL Studio's `ilbridge.exe` is not. The Bitwig entries
name the processes Bitwig runs plug-ins in; they were added by name and have not been
exercised here yet.

**Why not system-wide:** the switch applies to the whole process, so anything else probing
for Wine stops finding it — and some software depends on the answer. PACE/iLok protected
*standalone* applications fail to start with *"Error 2000: An iLok background component
required to validate the license for this product is not running"*; PACE-protected
plug-ins inside a host are not affected (checked with UVI Workstation as a plug-in under
yabridge). Ableton Live's embedded Splice view stopped loading with the global switch,
which also hides Wine from the `msedgewebview2.exe` child processes Splice renders in —
with the per-host entry that `wine.inf` now sets, Live runs and Splice loads. Standalone
JUCE applications are deliberately not in the list; add them per application.

An upstream fix is proposed as [juce-framework/JUCE#1701](https://github.com/juce-framework/JUCE/pull/1701),
which would add the same opt-in inside JUCE and make this workaround unnecessary.

### Building

```bash
git clone https://github.com/giang17/wine.git
cd wine
git checkout d2d1-dcomp-11.0
./configure --prefix=/opt/wine-d2d1 --enable-archs=i386,x86_64
make -j$(nproc)
sudo make install
```

**Build both architectures.** `--enable-archs=i386,x86_64` builds the 64-bit and the
32-bit PE side (new WoW64, the Unix side stays 64-bit) and needs the `i686-w64-mingw32`
cross compiler next to the x86_64 one. It is what this branch is built and tested with,
and it matters for plug-ins: 32-bit hosts and plug-ins load `d2d1`, `dcomp` and `dxgi`
directly, and a 32-bit half left over from an older build keeps running old code without
anything failing. `--enable-win64` still works and gives a 64-bit-only build. Use a
separate `--prefix` to avoid overwriting your distro's Wine installation. (Thanks to
@jibeape for working out the `--enable-archs` form for Bottles.)

**`wine.inf` is part of the install.** `make install` also puts `loader/wine.inf` into
`<prefix>/share/wine/`; it carries this branch's registry defaults — the `HideWineVersion`
entries for the plug-in hosts, see the JUCE note above. New prefixes get them at creation,
existing ones at their next start. If you update an installation by copying DLLs rather
than running `make install`, copy `loader/wine.inf` as well: nothing fails when it is
missing, the plug-in hosts simply keep rendering with GDI.

**ntsync** (recommended, upstream Wine feature): with a kernel that provides the
`ntsync` driver (`/dev/ntsync`), make sure `/usr/include/linux/ntsync.h` exists before
running `./configure`; check with `grep HAVE_LINUX_NTSYNC_H include/config.h` afterwards.
NT synchronisation then runs in the kernel instead of through the wineserver, and the
audio thread keeps up at 64 samples / 48 kHz without xruns (measured here with Serum 2 and
FL Studio).

**DXVK**: not a hard conflict, but the two do not combine — DXVK replaces the builtin
`dxgi.dll` and `d3d11.dll`, and the composition-swapchain and DComp popup handling of this
branch live in `dxgi` (the GL present in `wined3d`). The simplest setup is to not install
DXVK at all. If you keep it, switch the whole trio per application
(`WINEDLLOVERRIDES="d3d11,dxgi,d3d10core=n"` for DXVK, `=b` for this branch) — overriding
part of it mixes DXVK's D3D11 with Wine's DXGI, which use different internal COM interfaces
and crash. DComp-based plug-ins (Serum 2, Korg Trinity, Pianoteq 9) gain nothing from DXVK:
their D2D1 draws go through a bitmap+BitBlt path, not the DXGI swapchain.

**GL present for top-level windows** (default ON): D3D11 swapchains on top-level windows
present through the driver's SwapBuffers (EGL by default in Wine 11) directly from the GPU
instead of the GDI readback path — this removes a large per-frame GPU→CPU copy (order of
650 MB/s display-server traffic during continuous UI activity in Ableton Live) and fixes
main-window flicker when an app hosts WebView2 content (Ableton Live's Learn View).
`WS_CHILD` and `WS_POPUP` windows keep the GDI path. Adopted from shibco/ableton-linux
patch 0055 (diagnosis: ClickSentinel). If you see misplaced frames, set
`WINE_DISABLE_GL_PRESENT=1` to restore the GDI path for every window.

**Serum2 settings** (recommended — DComp gives the best performance):
- `"Disable DirectComposition": false`
- `"Disable Partial Redraw": false`

The GDI fallback (`"Disable DirectComposition": true`) is supported as well: the plugin's
HWND is marked so `needs_offscreen_rendering()` returns FALSE and its X11 child is attached
directly to the host's toplevel, instead of an offscreen-redirected child that wined3d's
GDI present never composited.

**SynthEdit / GMPI plugins**: plugins built with SynthEdit — recognisable by the `.sem`
modules inside the bundle — render through GMPI's DirectX backend, which maps straight
onto the modern, colour-space aware D2D1 interfaces (`ID2D1GradientStopCollection1`, the
Color Management effect, sRGB bitmaps). Without those patches such a plugin crashes on
load or draws its GUI far too dark. No configuration is required. VProm3 is the plugin
these were developed against, but they are not specific to it.

## Tested Applications

| Application | Framework | Status |
|-------------|-----------|--------|
| Serum2 (VST3 in Reaper) | VSTGUI + DComp | Fully functional, all waveform views + envelopes + presets; dragging the envelope, LFO and macro handles shows the drag bitmap with per-pixel alpha and no flicker |
| VProm3 (VST3 in Reaper) | SynthEdit/GMPI + D2D1 | Fully functional, correct colours (needs the Color Management effect patches) |
| Korg Trinity (VST3 in Reaper) | JUCE 8.0.13 + DComp | Fully functional on the DComp path — with the `HideWineVersion` entry, which `wine.inf` sets for Reaper; the standalone needs its own entry |
| Korg Prophecy (VST3 in Reaper) | JUCE 8.0.12 + DComp | Fully functional |
| Pianoteq 9 (standalone + VST3) | JUCE 8.0.10 + DComp | Fully functional |
| FL Studio 2026 (Wine + ntsync) | Custom | Runs without xruns at 64 samples / 48 kHz; Cloud plugins install (needs the AF_UNIX patches) and stream |
| UVI Portal | WebView2 | Installs and signs in, including special characters typed into the login fields |
| Ableton Live 12 (Intro / Lite) | Custom (D3D11 + WebView2) | Fully functional — window decorations, stable move/resize, F11 fullscreen both ways, menu bar hit testing, Splice view; the content indexer (`Ableton Index.exe`) runs on the built-in VC runtime |
| Native Access 3.25.2 | Electron/Chromium (D3D11 + DComp) | Installs, signs in and installs products (verified with a Kontakt 7 update, Supercharger and the bundled NTKDaemon). Needs the `powershell` patch from this branch: without it the installer loops forever on "Native Access is running" and the bundled NTKDaemon is never installed, leaving the app stuck at "grant permission to install dependencies". Kontakt 8's own installer aborts under Wine for unrelated reasons (InstallAware) |
| Kontakt 8 Player 8.12.1 | Custom; installer: InstallAware/MSI | Installs and runs. The official installer needs the `msi` string-pool patch from this branch — without it it aborts after 77 s with "Setup has failed: FALSE", writes no installer log and leaves the product missing (WineHQ bug 59056). Both routes verified with the patch: running the setup executable directly, and a reinstall through Native Access 3; each produces the desktop shortcut and registry entries. Both the standalone and the VST3 in Reaper run: the Player browser lists its 1940 presets and instruments load and play, alongside Kontakt 6 and 7 |
| SynthEdit 1.5 | Custom engine (D2D1 + winex11 client surfaces) | Fully functional — the MDI canvas draws completely and stays stable: no permanently black regions after a menu closes, no caption flicker while resizing or moving the top-level window, and canvas children no longer vanish mid-resize. Needs the winex11 client-surface and d2d1 bitmap-brush fixes from this branch |
| EPROM - Memory Rites (VST3 in Reaper) | JUCE 8.0.13; splash/login via WebView2 | Fully functional — the splash and login window render through WebView2, and once signed in the GUI is entirely JUCE. Requires the `HideWineVersion` entry (`wine.inf` sets it for the plug-in hosts); without it JUCE 8.0.13 detects Wine, falls back from Direct2D to GDI and the plug-in comes up with a black background and missing GUI elements |
| Minimal Audio Current / Evoke / Lucid (VST3 in Reaper) | JUCE 8.0.13 | Fully functional, same `HideWineVersion` requirement as above — without it the GDI fallback leaves the background black and parts of the interface missing |
| Minimal Hub | Tauri v2 + SvelteKit + WebView2 | Starts, signs in and installs products (an 11.8 MB update completed). Needs the `secur32`/schannel `DecryptMessage` fix from this branch: without it the `oauth/token` request stops after a partial response and the app waits indefinitely, because reqwest has no response timeout. Its installer step also shells out to `powershell Start-Process`, but in a form the `powershell` patch does not recognise — it ran both with and without that patch |
| Fender Studio Pro 8 | CCL (DXGI + DWrite + DComp) | Fully functional — the song view draws completely and stays stable, no stale tool bar or transport and no flicker; the transport playhead and the selection rectangle no longer flicker while the transport runs, and video on the timeline plays, seeks, loops and jumps without stalling or going black. Starting at all needs the `UIAnimationManager2` and `UIAnimationTransitionLibrary2` implementation from this branch; without it the CCL framework aborts with "requires Windows 10 or later" |
| Steinberg Cubase Pro 15.0.30 | Custom (DComp + D2D1 + DirectWrite) + WebView2 | Installs through Steinberg's own bootstrapper and runs: project window, MixConsole with live meters, Hub. Installing needs the `msi` feature-cost fix (the setup crashed before its first dialog) and the Script SIP for signed PowerShell — without `pwrshsip`/`wintrust` the installer stops at "preinstall.ps1 … not trusted". Starting needs the `Windows.Globalization.Calendar` stub, without which `headtracking.dll` aborts behind the licence splash, and the `comdlg32` folder-dialog fix, without which the Hub reports the project folder as read-only. The window itself needs the dcomp virtual-surface resize and child-surface readback work — and the d2d1 WIC target fix, without which the MixConsole level meters stay empty |

## Font Setup

The DWrite patches fix font rendering in VSTGUI-based plugins, but some applications
also require system fonts to be configured correctly (Unicode symbols, GDI menus).

**[scripts/wine-font-setup.sh](scripts/wine-font-setup.sh)** does the
host part for you:

```bash
scripts/wine-font-setup.sh --prefix ~/.wine
scripts/wine-font-setup.sh --prefix ~/.wine --check   # report only
```

It locates the fonts through fontconfig (so distribution paths do not matter),
copies them into the prefix, registers the MS Core Fonts for GDI, writes the
`FontLink` fallback chain and switches on this branch's text rendering. It is
idempotent, and `--check` shows whether everything is still in place — worth running
after a prefix update. Wine rewrites the FontLink chain with its own defaults whenever
its stored codepage record does not match the running process, and one process
started under `LC_ALL=C` already triggers that; since 2026-09-02 this branch therefore
carries the two fallback entries in win32u's own defaults, so the rewrite no longer
loses them. The fonts themselves still have to be in the prefix, which is this
script's job.

Three of those steps matter more than they look:

- **MS Core Fonts** are not cosmetic. Some plugins check for font *files* during DLL
  initialisation — FL Studio's "Fruity Delay 3" looks for `C:\windows\Fonts\Arialbd.ttf`
  and Verdana — and crash with an access violation and no usable error message when
  they are missing.
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
  scripts/wine-font-setup.sh --prefix ~/.wine --check
  ```

  The values themselves are `text_enhanced_contrast` and `text_linear_blend`
  under `HKCU\Software\Wine\Direct2D`, and `outline_in_natural_modes` under
  `HKCU\Software\Wine\DirectWrite`. Enhanced contrast is also in winecfg's
  graphics tab (*Off* / *Medium (50)* / *Strong (70)*). winecfg stores *Off* as
  the absence of the value, so the script writes a contrast only on its first run
  (when no switch is set yet) or when `--contrast N` is given; a choice made
  there, *Off* included, survives a re-run, and `--check` does not count a
  missing contrast as a gap.

See **[documentation/wine-font-setup-guide.md](documentation/wine-font-setup-guide.md)**
for the full guide, including working around the missing `BitPDisp-10` tooltip font in
Serum 2, which the script deliberately leaves out.

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
colour fringe: 0.00 % without the patches, 29.05 % (body text) and 8.47 % (file
list) with them; in a test program the GRAYSCALE and ALIASED lines stay at exactly
0.00 %, which is what makes the measurement trustworthy. FL Studio's Sounds tab is
a good place to see the difference by eye.

A font's embedded bitmap strikes used to defeat this: where a face carries one,
FreeType returns the strike, which has a single coverage sample per pixel. Wine's
bundled Tahoma carries strikes for 8 to 16 ppem and `MS Shell Dlg` resolves to Tahoma,
so the default interface font lost its subpixel resolution across the whole size range
interface text uses. DWrite now asks for the outline when the caller wants a ClearType
texture; a face with no outline for a glyph keeps its strike.

### Greyscale text and embedded strikes

Greyscale text still takes the strike, and Tahoma does not carry one for *every* size
in its range, so at 96 dpi the character of the text flips between neighbouring sizes —
12, 15 and 16 px come out as hard 1-bpp bitmaps while 14 px and everything from 17 px
up is antialiased. Windows selects by **rendering mode** instead: the GDI-compatible
modes take the strike, the NATURAL modes take the outline — and applications measured
here request NATURAL exclusively (Fender Studio Pro 8 in 1625 of 1625 glyph-run
analyses).

```bash
wine reg add 'HKCU\Software\Wine\DirectWrite' /v outline_in_natural_modes /t REG_DWORD /d 1 /f
wineserver -k
```

With the key set, greyscale follows the same rule as Windows and the size-to-size
inconsistency disappears; unset, rendering is unchanged. It is opt-in because the
strike is hand-tuned and crisp: switching to the outline makes small greyscale text
softer, which is a matter of taste. With `FontSmoothingType=2` it barely matters,
since almost everything then goes through the ClearType path anyway.

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

Measured against identical text drawn in both polarities, the polarity bias goes
from up to -0.57 (half coverage) to 8-bit quantisation noise. On a dark audio
interface black-on-white becomes more restrained, white-on-black slightly heavier —
the expected direction. **It costs about 500 µs per `DrawText`** (+12 to +13 % of
the call), which is why it is off by default; with the key unset rendering is
bit-identical. Two things worth knowing: it deliberately departs from Direct2D on a
plain UNORM target, where Windows blends in encoded values; and the cheaper route —
an `_SRGB` render target view — does not work in Wine, because wined3d honours the
sRGB cast only for swapchains with a single back buffer, so the blend is finished in
the shader instead.

### Enhanced contrast — worth setting on a dark interface

DirectWrite reports an *enhanced contrast* value alongside gamma and ClearType
level; Wine hardcodes it to zero, so nothing is applied. This branch honours it and
lets the prefix override it — in winecfg (*Graphics* tab, *Direct2D text*, three named
settings) or by hand:

```bash
wine reg add 'HKCU\Software\Wine\Direct2D' /v text_enhanced_contrast /t REG_DWORD /d 70 /f
wineserver -k    # the value is read when d2d1 loads, so restart the application
```

The value is in hundredths; deleting it restores the default. It raises partial
coverage while leaving fully covered and empty samples alone, so at 0 — the default
— rendering is bit-identical to not having the feature at all. `AppDefaults` works
as for the other keys, so it can be set for a single application.

| value | when |
|---|---|
| `0` | default. Clean as it stands; leave it unless you have a reason |
| `50` | what Windows runs. A safe middle if you want a little more weight |
| `70` | dark interfaces with dense body text — light text on dark looks thinner than it geometrically is, and this puts the weight back |

On a **light** background the effect works against you — there the same setting makes
text look heavy. Watch small sizes if you go high: at 11px a strong setting can start
closing the counters in `e`, `a` and `g`.

Unrelated to the above, FL Studio's Piano Roll needs one more font fix to show
flat/sharp symbols (♭ ♯) instead of tofu boxes — FL bypasses Wine's font
fallback through `GetGlyphIndices`. That one has its own project:
[giang17/flstudio-wine-font-fix](https://github.com/giang17/flstudio-wine-font-fix).

## Steinberg installers: PowerShell hosting in Wine-Mono

Steinberg's `Setup.exe` (the bootstrapper behind every Download Assistant package) runs
the `preinstall.ps1` scripts from `setup.xml` by hosting PowerShell in-process through
`System.Management.Automation`, an assembly Wine-Mono does not ship. The load fails,
Setup.exe swallows the exception, logs "Finished Installation" and exits with 0 —
without installing a single MSI. Nothing in the setup log says so. The Script SIP from
this branch (`pwrshsip`/`wintrust`) gets such scripts past the "not trusted" check;
this is the step after it.

**[scripts/mono-sma-shim](scripts/mono-sma-shim/README.md)** is a
`System.Management.Automation.dll` with the identity Setup.exe asks for (version
3.0.0.0, delay-signed with Microsoft's public key; Mono does not verify strong-name
signatures) that implements the hosting API the installer uses and runs the script
with a small interpreter for the PowerShell subset those preruns are written in.
Constructs outside that subset raise an error, so a script the shim cannot run is
reported as failed rather than as done. It is a prefix-side component, not part of
the Wine build: Wine-Mono ships the C# compiler it needs, and it goes into the
prefix' Wine-Mono GAC.

```bash
scripts/mono-sma-shim/build.sh
scripts/mono-sma-shim/install.sh --prefix ~/.wine          # then a probe through the GAC
scripts/mono-sma-shim/install.sh --prefix ~/.wine --status # after a Wine-Mono update
```

A `wineboot -u` with a newer Wine replaces Wine-Mono and the shim with it; `--status`
reports that (exit 3). Verified with MediaBay 1.3.100: removed with `msiexec /x`, then
installed again through `Setup.exe --silent`, the setup log showing the prerun's
`Write-Host` output and `preinstall.ps1 executed successfully`.

## D2D1 Patches Only (Branch: `d2d1-v6`) — DEPRECATED

Deprecated since 2026-05-03; last update 2026-02-14, 15 patches against vanilla Wine 11.0
(`git format-patch wine-11.0..d2d1-v6`). It was the upstream-targeted D2D1-only series
and no longer reflects the current D2D1 work, all of which lives in `d2d1-dcomp-11.0`.
No bundled successor series is planned: small targeted fixes go upstream as ordinary
merge requests on gitlab.winehq.org, the fork itself stays a rolling devel-tracking
distribution.

## Related

- Discussion: [yabridge#413](https://github.com/robbert-vdh/yabridge/issues/413)
- Upstream: [wine-mirror/wine](https://github.com/wine-mirror/wine)
- Subpixel text: [shibco/ableton-linux](https://github.com/shibco/ableton-linux),
  pull request 155 by Cade (@shibco), who wrote the nine patches and wants to take
  them to WineHQ. Reviewed there by @ClickSentinel, whose measurements are worth
  reading before touching this code

## License

Same as Wine -- GNU LGPL. See [LICENSE](LICENSE) for details.
