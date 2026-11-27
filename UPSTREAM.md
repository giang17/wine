# Upstream status of fork fixes

Defects in Wine code inherited from upstream that this fork fixes, with the
matching WineHQ bug report where one exists and the state of upstream Wine.
The list covers fixes that were checked against upstream; it is not complete.
PATCHES.md describes every change of the fork.

As of 2026-11-20: branch `d2d1-dcomp-11.0` at `7c5c7c23446`, upstream master
at `wine-11.22-107-g76f8e947954`.

None of the fork's own fixes is submitted to WineHQ: Wine's Clean-Room
Guidelines do not accept LLM-generated code, and this fork is developed with
LLM assistance (see the upstream section of the README). The page is
information for users of the fork, not a request to take the code upstream.

## Fixed upstream in the meantime

| Component | Defect | Fork commit | WineHQ bug | Upstream |
|---|---|---|---|---|
| crypt32 | Verifying a signature re-sorts the signed attributes, so a valid signature whose attributes are not in DER order fails; FL Studio 2026 does not start | `05965da7736` (backport of `a358d7ecf13`, replaces the fork's own fix) | [60273](https://bugs.winehq.org/show_bug.cgi?id=60273) | Wine 11.21, `a358d7ecf13` |
| msi | The saved string pool counts unused slots at its end, crosses 65535 entries and switches to 3-byte string references; the Kontakt 8 installer aborts | `89612d2640e` (backport of `fd1a09b8702`, replaces the fork's own fix) | [59056](https://bugs.winehq.org/show_bug.cgi?id=59056) | master after Wine 11.22, `fd1a09b8702` |
| ntdll | A debugger's break-in thread in a WoW64 process starts at a truncated 64-bit address | `aa300232981` (backport of `a9dd4878b14`, replaces the fork's own fix) | [59803](https://bugs.winehq.org/show_bug.cgi?id=59803) | Wine 11.22, `a9dd4878b14` |
| mf | The audio renderer never reports a completed scrub | `1cd7e8c5883` (backport of `324a7da836e`) | - | Wine 11.5, `324a7da836e` |

## Open upstream

"Open" means the defect is still in upstream master: the fork's change applies
there, or, where it does not or upstream changed the functions involved since
`wine-11.0`, the upstream code was read.

| Component | Defect | Fork commit | WineHQ bug |
|---|---|---|---|
| d2d1 | `CombineWithGeometry()` is a stub, and anything drawn through its empty result is clipped away; a JUCE 8 component stops updating until a full repaint | `2e939c3e701`, `77b9f3cadc3`, `74cc53881df` | - |
| d2d1 | `ID2D1Geometry::Outline()` is a stub; Cubase 15 leaves the corners of its start-up splash transparent | `84bad3dff46` | - |
| d2d1 | Every cubic Bezier is approximated by a single quadratic; S-shaped curves are drawn as arcs | `0f42192d02e`, `c823f3c363f` | - |
| d2d1 | Rounded rectangle `Simplify()` places the corner arcs wrongly | `a6976602e20` | - |
| d2d1 | Bezier overlap resolution never ends for a control triangle with a non-finite area; the process runs out of memory | `2e075d515e1` | - |
| d2d1 | Triangulation vertices are compared without a total order; one NaN vertex can make the triangulation read from address zero, and Studio Pro 8 crashed at the end of its shutdown | `4331b70158d` | - |
| d2d1 | `PushAxisAlignedClip()` repeats its FIXME on every call (HALion Sonic 7) | `0f635c87cc0` | - |
| dwrite | The glyph cache dies with its font face, and text layout creates a font face per run; resizing a SynthEdit window for 25 s: 111064 bitmap misses, 209 with the fix | `ad0a071cb29` | - |
| dwrite | The shared factory keeps only a weak reference to the system font collection, so every fetch rebuilds it (Pianoteq 9 through JUCE) | `924dbe3fdf7` | [56717](https://bugs.winehq.org/show_bug.cgi?id=56717) |
| dxgi | `IDXGIDevice2::EnqueueSetEvent()` is a stub; Chromium skips its wait for the GPU after the first present of a swap chain and writes a dump of about 112 MiB into the Crashpad folder of a WebView2 host | `8cc735e8869` | - |
| dxgi | `IDXGIOutput::WaitForVBlank()` returns E_NOTIMPL; JUCE 8 dispatches its repaints only after a successful call and never refreshes its window, and its VBlank thread checks its exit flag only then, so destroying the window waits forever | `0743742cde5`, `8649fee37e8`, `b06e67e1cb7` | [56799](https://bugs.winehq.org/show_bug.cgi?id=56799) |
| dwmapi | `DwmGetCompositionTimingInfo()` reports the last vblank instead of the upcoming one; Ableton Live 12 rejects every timing sample | `32f39163e25` | - |
| evr | The presenter loses its allocator notification when the output type is renegotiated; video stops while audio continues | `94ff059321a` | - |
| mf | The video renderer (EVR) never reports a completed scrub; Studio Pro 8 does not start playback after a position jump | `529d0045dab` | - |
| msvcp | Move construction and assignment of the standard stream classes are missing; Ableton Live's indexer aborts without a native VC runtime | `85ea78fad61` | - |
| msvcp | `msvcp100` to `msvcp140` do not export the `locale::id` objects of the money facets; the PACE wrapper of oeksound soothe3 stops with "Could not find imported function in specified DLL" | `a2204f1c70a` | - |
| ntdll | `RtlWakeAddressSingle()` loses its wake-up when the waiter it goes to has been terminated; a released critical section stays unowned for 5 s (Studio Pro 8 at exit) | `4940adccac5` | - |
| ntdll | The first-fit walk in `find_free_block()` is linear; Dorico 6: 399 steps per call, 9.9 with the fix | `e55692cfad7` | - |
| ntdll | The DWARF decoder does not read the operand of `DW_CFA_GNU_args_size` and decodes the rest of the frame as nonsense; an exception through a C++ frame of a Winelib program (yabridge) overflows the stack | `8d2e41a959d` | [60350](https://bugs.winehq.org/show_bug.cgi?id=60350) |
| ntdll | The personality routine of a C++ frame in a Winelib module (`__gxx_personality_v0`) is called as an exception handler, and the dispatcher faults on its answer | `dc37c197a28` | - |
| iphlpapi | `GetAdaptersInfo()` reports an estimated buffer size; Nuendo 15's ADR API network setup fails | `ef6039d796a` | - |
| include | On i386, `IP_ADAPTER_INFO` is 648 bytes instead of 640 since `time_t` became 64-bit in Wine 11.19 | `028829b7497` | - |
| server | The pointer stays invisible after `ShowCursor(TRUE)` when a null cursor was set while it was hidden (WebView2 sliders) | `bd76fa41cc6` | - |
| server | A top-level surface flush paints over child windows of another process; single black frames over embedded WebView2 and CEF views | `74639e37b06` | - |
| server | `AttachThreadInput()` drops the capture and the pending hardware messages; a JUCE button reacts only to every second click | `ba42cb8b820` | - |
| uianimation | `UIAnimationManager2` and `UIAnimationTransitionLibrary2` are missing; Studio Pro 8 and Studio One 6.6 and later do not start | `a4e09a0e012` | [59824](https://bugs.winehq.org/show_bug.cgi?id=59824) |
| win32u | 32-bpp menu item bitmaps are drawn without their alpha channel; transparent pixels come out black | `90ce004991f` | [59756](https://bugs.winehq.org/show_bug.cgi?id=59756), [58290](https://bugs.winehq.org/show_bug.cgi?id=58290): same symptom, cause not checked |
| win32u | A DC fetched between setting a pixel format and the server update draws into a surface that is clipped away; nothing it paints appears (SynthEdit) | `f23204942c5`, `33d23685e49`, `f2ef136f6ab` | - |
| win32u | Window-manager geometry that arrives together with a maximize is dropped; drawing and hit testing are offset | `c4868e373a1` | - |
| win32u | `WM_UAHMEASUREMENUITEM` and `WM_UAHDRAWMENU*` are never sent; the Ableton Live 12 main window grows | `c06e05bad30`, `9b800ea262f` | [57955](https://bugs.winehq.org/show_bug.cgi?id=57955) |
| win32u | `ShowWindowAsync()` returns the previous visibility for a window of the calling thread, FALSE for a hidden one that it shows; Waves Flow Motion takes it for a failure and crashes its host when its editor opens | `ad805c66719` | - |
| wined3d | A present rotates the back buffers without copying the frame back; an application that draws incrementally gets an older frame (Studio Pro 8 alternates between two frames) | `7070fef6397` | - |
| winex11 | The opaque black X background shows while a client window leaves the screen (Ableton Live 12, Learn View) | `aa3b2b8cf5d` | - |
| winex11 | A `_NET_WM_STATE` request the window manager ignores blocks every later state and configuration update (KWin, Ableton Live 12 leaving fullscreen) | `b377aff406b` | - |
| winex11 | A fullscreen position the window manager has already applied is not taken over; drawing and hit testing are offset (Ableton Live 12 menu bar) | `f8c735f42dd` | - |
| winex11 | A process starting while the virtual desktop is created ends up outside of it | `0cafb0c7120` | - |
| winex11 | A click on the virtual desktop background ends the session (`BadWindow` on `_NET_WM_USER_TIME`) | `8c6b232482a` | - |
| winex11 | An offscreen EGL swap is presented before it has completed; Studio Pro 8's Studio Assistant popup comes up black | `e8ed3cc4c61` | [60555](https://bugs.winehq.org/show_bug.cgi?id=60555) |
| winex11 | GL rendering into a hidden top-level has no backing store; reading it back returns stale content (HALion Sonic, Anima 3D view) | `6989d99b9fa` | - |
| winealsa | System exclusive input that arrives while the application has no buffer queued is dropped; FM-Alive DX Manager receives Yamaha DX11 voice banks short and its Patch Viewer never finishes | `c3b7c790f2b` | - |
| winmm, winealsa | MIDI devices connected after start-up are not enumerated | `280603c82e0`, `e1e936d1843`, `d07446559cb` | - |
