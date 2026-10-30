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
| `d2d1-dcomp-11.<N>` (newest devel) | **Recommended for devel / rolling-release users** (e.g. `wine-tkg-dev`). The same full stack as `d2d1-dcomp-11.0`, rebased onto the latest WineHQ devel tag and rolled forward roughly every 2 weeks. **Use the highest-numbered `d2d1-dcomp-11.*` branch**: it is always the current rolling base; lower-numbered ones are previous snapshots, superseded. Plugin-tested by the maintainer before each push. |
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
  → about 80 ms). Consecutive solid-colour fills — `FillRectangle()`,
  `FillRoundedRectangle()`, `FillEllipse()`, `FillGeometry()` — are batched the same way:
  the antialiased fill meshes and the curve triangles of all of them go up in one vertex
  buffer each and are drawn per colour run, instead of two uploads and two draws with a
  full state setup per geometry, and `FillRoundedRectangle()` reuses one retained geometry
  per device context (fill only, no outline tessellation) instead of building a factory
  object per call. Cymatics Hotline draws its LCD dot matrix as 1 931 rounded rectangles
  per frame: 13.9 → 25.1 fps on a 2-core laptop, `wined3d_cs` 57 % → 12 %, the main
  thread at 65 % for nearly twice the frames, pixels identical except for 8-bit rounding
  where fills of one colour overlap. Closing a path geometry tests the control triangles of
  its Bézier pieces pairwise for overlap; pairs whose bounding boxes lie apart skip the test
  (half of `Close()` for glyph outlines, where it never split anything), and the result of
  `Close()` is kept by the figure data and copied back for an identical path
  (`WINE_D2D_FILL_CACHE=0` disables it): JUCE 8 draws text as path outlines rebuilt every
  frame — Hotline closes 27 glyph paths per frame, 37 distinct ones in all — so a frame's
  `Close()` time goes from 8.4 ms to 0.2–0.4 ms and the main thread from 62 % to 46 %,
  pixels identical. Bitmap brushes honour WRAP and MIRROR instead of degrading to CLAMP,
  so a tiled background is tiled rather than one tile with its edge texels stretched
  (SynthEdit panels). `SetTarget(NULL)` drops the render target bindings, so a swapchain
  back buffer is not held across `ResizeBuffers()`. The scratch vertex and index buffers are dynamic and written through a
  `WRITE_DISCARD` map instead of `UpdateSubresource()` — wined3d cannot rename a
  DEFAULT buffer from the client thread and drained the command stream once per
  primitive (1 090–1 940 synchronisations per 20 px resize step of the SynthEdit canvas,
  212 now) — and the private device context state is swapped in once per
  `BeginDraw()`/`EndDraw()` span rather than around every primitive (3 485 swaps per
  step, each a full state emission both ways, 28 now); a nested `BeginDraw()` on
  another context of the same device takes the state over and hands it back. The geometry
  of a layer mask goes through the same scratch buffers, and the index and vertex buffer of
  the quad `Clear()` draws are created once per device instead of once per device context:
  JUCE 8 creates about 3 400 device contexts while the preset list of Minimal Audio Current
  is scrolled for 4.6 s, and the two buffers of each cost two drains of the command stream
  (11 806 synchronisations in such a drag before, 4 399 now). `FillOpacityMask()` is
  implemented: the mask becomes the opacity brush of a rectangle fill, sampled from the
  source rectangle with linear filtering and clamped half a texel inside it, so a mask
  cut from an atlas does not bleed its neighbours — vatSys pre-renders every map label
  and symbol into 4096×4096 atlases and composites them this way, and the radar map had no
  text and no waypoint symbols at all while the method was a stub.
- **D2D1 curve edges**: a filled curve is drawn as a polygon plus one triangle per curve
  piece, and the antialiasing ramp of that triangle reaches half a pixel to either side of
  the curve. Next to a short piece the triangle is thinner than that, so both halves of the
  ramp were cut off. Cubic Béziers are split in geometry units, which means artwork defined
  in large units and scaled down — an SVG logo, an icon set — is all short pieces, and its
  round edges came out stepped. Curve triangles now get a skirt on their open side, and
  the polygon ramps towards a curve seam by the distance to the curve instead of staying
  opaque up to it (including triangles that touch the outline only at a corner). Measured
  against box-filter coverage on a ring of radius 15 px drawn the way an SVG is, RMS error
  at the outer edge 0.061 before, 0.012 after, Windows 10 0.031; the d2d1 tests fail at
  the same places as before. Visible on the Steinberg logo of HALion 7's Writing Room Synths
  macro page
- **D2D1 straight edges stored as curves**: SVG exports often store a straight edge as a
  cubic Bézier with both control points on the chord. Filled, such a piece became a curve
  triangle of next to no area, and the skirt that carries its antialiasing ramp past the curve
  extrapolated the triangle's texture coordinates 0.75 px further: the ramp came out wide and
  uneven, and the edge looked stepped. Where the control point lay exactly on the chord and
  counted as inside the filled area, it was inserted into the outline, the triangulation made
  a triangle of zero area there, and that side lost its antialiasing. A piece whose control
  point lies within 1e-4 of the chord's length of the chord is filled along the chord now, and
  one within 1/16 px of it on the render target is drawn as a straight boundary edge, the
  threshold the stroke shader uses. RMS error against box-filter coverage: the Minimal Audio
  logo in Current's update popup (24 cubics, 12 of them straight edges) 0.304 → 0.048,
  Windows 10 0.046; a square of cubics with control points on its edges 0.251 and 0.100 →
  0.017, Windows 0.034. Curve fills change by at most 4/255 on 5 pixels of 158 400, and the
  d2d1 tests fail at the same places as before
- **D2D1 stroked curves**: `DrawGeometry()` and `DrawEllipse()` draw a curve's stroke as a
  strip of triangles that ended exactly at the stroke edge, so the half of the antialiasing
  ramp outside the edge had no pixels to run on and a stroked circle lost 3 % of its area;
  the strip now carries the same 0.75 px skirt the straight outline already had. The arc
  shader's distance estimate is corrected for curvature (it was 6 % short on a 3.4 px stroke
  around a 13 px radius, bright outside and dark inside), and a Bézier piece is drawn as its
  chord only when its control point lies within 1/16 px of it instead of a whole pixel — a
  path in large units, split into 30° pieces, had every piece replaced by a chord half a pixel
  inside the curve and its stroke moved inwards by a fifth of a pixel. RMS error against
  box-filter coverage on the stroked ring of issue 444: 0.067 → 0.011 (Windows 10 0.032),
  scaled stroke 0.169 → 0.011; fills and the d2d1 tests unchanged. A cubic is split into at
  least one quadratic per 45° of turn, whatever its units: a single quadratic through a quarter
  circle lies up to 1.1 % of the radius inside it, and a ring drawn under an up-scaling
  transform (where the unit-based tolerance allowed one piece) had its stroke a twentieth of a
  pixel inside the ideal ring; the Bézier stroke distance is exact to second order now, which
  takes the width error off wide strokes (6 px: 0.019 → 0.014)
- **D2D1 path closing**: `ID2D1GeometrySink::Close()` splits Bézier pieces where they cross
  another piece of the path. The search subdivides both pieces down to 1/1024 of their range
  and took every final pair whose bounding boxes still overlapped as a crossing, so two curves
  running along each other were split all along the stretch they share. The round caps of
  strokes that meet at one point lie on the same circle: the download arrow in Minimal Audio
  Current's preset list (three strokes, 1 DIP wide) grew from 24 pieces to 2159, the pairwise
  passes after the search took 70 ms per close, and the icon is rebuilt on every repaint, so
  the list fell behind while its scrollbar was dragged. A crossing is reported only where the
  chords of the final pieces cross now: 1.7 ms for that path, and the busiest thread of Reaper
  spends 207–221 instead of 379–417 ticks on the same drag. Rendering is pixel-identical at
  1x and 2x, and the d2d1 tests fail at the same places
- **D2D1 stroke styles**: `DrawGeometry()` and `DrawLine()` took only the miter limit and the
  transform type from a stroke style; every join was mitered and every end flat, whatever the
  style asked for, and the mitered join fan was all but invisible under antialiasing, leaving
  only its spike. Serum 2 strokes its waveforms with round joins and round caps, and the spikes
  showed as thin dark hairs at every peak once the view was zoomed in. Joins now follow
  `lineJoin` — miter clipped at the limit, bevel, miter-or-bevel, round — and open figures
  and lines get their `startCap`/`endCap` (flat, square, round, triangle). The corner's four
  vertices are built by the outline vertex shader from the two directions and the join type,
  and the pixel shader ramps the exact outline (a disc for round, three bounding lines for the
  rest) instead of relying on the geometry. On the zig-zag of `projects/d2d1-curve-edge-aa`
  the round-joined stroke went from RMS 0.098 to 0.007 against the ideal coverage (Windows 10
  0.015), miter and bevel to 0.021 and 0.024 against Windows; 28 stroke figure checks in the
  d2d1 suite pass that did not before (56 → 28 failures). Dashes are still ignored
- **D2D1 geometry realizations**: `DrawGeometryRealization()` drew only into a command list;
  on a bitmap, DXGI surface or swap chain target it returned without drawing, so an application
  that uses `CreateFilledGeometryRealization()` / `CreateStrokedGeometryRealization()` lost the
  shape entirely. Upstream has the same stub. A realization keeps the geometry it was created
  from, and it is now drawn like `FillGeometry()` / `DrawGeometry()` on every target, with its
  stroke width and stroke style; the flattening tolerance given at creation is not applied.
  `test_geometry_realization()` checks the pixels now (6 failures before, 0 after). No affected
  application is known — JUCE 8.0.0 to 8.0.15 never call the API
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
- **D2D1 device creation**: every device compiled the shape and effect shaders from HLSL
  source, 27 ms each time, and every WIC, DC and HWND render target creates a device of its
  own. The compiled code is kept in the factory now and shared by its devices. Nuendo 15
  draws the icons of its "Add Track" submenu into one WIC render target after another
  (38 for 19 icons); opening the submenu took 1.42 s, during which the menu ignored the
  mouse, and takes 0.22 s now
- **D2D1 device context options**: `CreateDeviceContext()` printed a `FIXME` for
  `D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS`, the option JUCE's
  Direct2D backend passes for the device context it creates per paint. The option is a
  hint, and this implementation does not spread rendering across threads, so there is
  nothing for it to switch on: it is accepted silently now, and the `FIXME` is left for
  bits the enumeration does not define. Cymatics Hotline with its editor open, 25 s with
  `+fixme`: 661 of 815 log lines were that message, 0 of 154 now. Under `+d2d` the
  contexts remain visible as one `Created device context` line per call (561 in 10 s)
- **D2D1 multi-threaded factories**: a device context of a `D2D1_FACTORY_TYPE_MULTI_THREADED`
  factory holds the factory lock for the whole of every call, as Windows serialises the
  calls on the objects such a factory creates. The lock used to cover only the D3D11
  submission; the constant buffer maps, the scratch buffers and the line and fill batches
  ran unprotected, and two threads drawing onto one render target corrupted them. vatSys
  draws the runways of its map with `Parallel.ForEach` onto one bitmap render target and
  crashed in `ID3D11DeviceContext::Map()`, with DXVK and with wined3d. Eight threads
  calling `DrawLine()`/`DrawGeometry()` on one bitmap render target crashed every run in
  both architectures (wined3d assertion in 32-bit, heap corruption in 64-bit) and complete
  8 of 8 runs now; vanilla Wine 11.0 crashed 3 of 3 32-bit runs. JUCE 8 creates its
  factory multi-threaded as well. Single-threaded factories are unchanged
- **DComp**: IDCompositionDesktopDevice implementation with a D2D1 bitmap rendering path,
  dirty-rect clipping and DIB+BitBlt presentation; IDCompositionDevice3/4/5, composition
  and dynamic textures, D3D11 BeginDraw and surface handle export; rootless visual trees
  composited onto the target window at ~60 Hz, cross-process targets, backdrop capture;
  the backdrop under a transparent page is taken from the window only for a target's
  first composition buffer (where two captures in a row agree) and is final from then on,
  black after a size change — reading it back before every composite composited the
  window's own stale pixmap memory and our own earlier frames under the page (FL Studio's
  Sounds tab started offline: nested old frames after a resize, a ghost of the offline
  card after a tab switch); when a hosted target moves, the area it leaves is handed back to the host only after a
  round trip on our X connection, so the host's repaint cannot overtake the blit that is
  still on its way (a docked WebView2 pane dragged in FL Studio left 3 px strips of the
  plugin image behind, 30-70 px on a fast drag); the visual tree is serialized against the
  composite walk: `SetContent()`, `AddVisual()`, `RemoveVisual()`, `RemoveAllVisuals()`, the
  dynamic texture's `SetTexture()` and the last `Release()` of a visual take a tree lock under
  which the walk snapshots the leaves with a reference each, and the readback runs outside
  it — Chromium's compositor thread sets content and commits while the tree timer composites
  the same target in the window's thread (measured with Ableton Live 12), so the texture a
  leaf was being read from could be released under the walk, the access violation reported
  for Power BI Desktop in giang17/wine pull request 24; `SetRoot()` holds a reference on the
  root visual, which it never did (an application that releases its own reference after
  `SetRoot()` left the target with a dangling pointer); a `Commit()` from another thread than
  the window's no longer composites the tree in the caller but posts the composite to the
  window's thread (`WM_WINE_DCOMP_COMMIT_COMPOSITE`), where the tree timer runs it as well —
  measured with Ableton Live 12, Chromium's compositor thread entered the composite of a target
  while the timer pass was still inside it, and both wrote the composition buffer and blitted it;
  a target's delivery is clipped to what the targets of its process that lie above it in the
  window tree have on the screen, the order the DWM composes them in — the GDI visible region
  of the target's DC does not clip a `WS_EX_TRANSPARENT` sibling of an ancestor (for GDI
  painting that sibling is painted afterwards and covers the lower window again), so the
  covered targets kept delivering and the last one to deliver won. Power BI Desktop 2.158
  keeps its five views in five WinForms hosts over each other, all visible and all
  `WS_EX_TRANSPARENT`, each with a WebView2 inside: the main window showed the views in turns,
  200–330 ms each, five brightness levels in an 8 s capture; now one, and the four covered
  targets paint nothing. Nested targets are not ordered here, they are blended into one
  frame as before; targets of other processes are not seen (`WINE_DCOMP_STACK_CLIP=0`
  turns it off); a composite that finds a swapchain leaf with nothing published is held
  back for up to 250 ms and the window keeps the frame it shows, as the DWM keeps the last
  composed frame until the next `Commit()` — Chromium replaces its swapchain on every resize
  step, and the old one takes the properties wined3d published on its window with it before
  the new one has presented (5–81 ms per step, median 40), so the composite timer delivered
  the tree without that leaf: the backdrop alone. Power BI Desktop 2.158 went black over the
  whole window for one to four frames on resize steps and on every switch between maximized
  and normal; in colour-marked captures 13 of 15 and 22 of 25 such events covered more than
  half the window before, 0 of 12 and 0 of 25 after. The strip a growing window gains before
  Chromium follows still shows the backdrop (`WINE_DCOMP_HOLD_ABSENT_LEAF=0` turns the hold
  off)
- **DComp on a layered top-level without attributes**: a rootless visual tree whose target
  is a top-level window with `WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP` and no layered
  attributes is presented with `UpdateLayeredWindow()`, composited over a transparent
  backdrop and with its per-pixel alpha. Windows shows such a window only through the DWM;
  winex11 does not map a layered window before it has attributes, so the GDI blit of the
  rootless path left it unmapped for good (reported for NinjaTrader 8's embedded WebView2
  chat, WineHQ bug 58921; the chat itself turned out to be a child target of another process
  and needed the wineserver changes below). The dxgi swapchain bind already gave this window class an opaque
  `LWA_ALPHA`, but only for a swapchain bound as root content. Measured app-free over 16
  combinations of top-level/child target, four extended styles and root/rootless content:
  the two that failed (top-level, layered without attributes, rootless tree) now render,
  the other 14 are unchanged, and a tree covering half the window leaves exactly the
  uncovered half transparent
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
  index.
  For a family that is not installed, JUCE 8 asks GDI for its substitute
  (`CreateFontFaceFromHdc()`) and maps that face back with `GetFontFromFontFace()`. DWrite
  referenced the file by win32u's NT path (`\??\Z:\...`), while the system collection holds
  the DOS path from the `Fonts` key, so the lookup always failed with `DWRITE_E_NOFONT` —
  even for Wine's own Tahoma. Cymatics Hotline (JUCE 8.0.4, asks for Helvetica Neue, which it
  does not embed) drew no button labels and retried about 19 times per second, each retry
  walking the whole collection; with ~3750 host fonts in the prefix its editor froze and the
  host stopped answering `Ctrl+Q`. The NT prefix is now stripped: Hotline asks twice, finds
  GDI's choice (Arial) and shows its labels. A prefix whose fonts directory holds the same
  file under two spellings (`arial.ttf` from the font setup next to an older `Arial.ttf`)
  still failed: GDI had loaded one, the `Fonts` key names the other, and local file
  references compare by write time and path. `GetFontFromFontFace()` now falls back to
  matching local files by path alone, ignoring case and write time, as Windows treats the
  two names as one file.
- **ClearType-style subpixel text** (from Cade / @shibco, `shibco/ableton-linux`):
  a glyph run asking for `DWRITE_TEXTURE_CLEARTYPE_3x1` used to be rendered once in
  greyscale and copied into all three subpixels. DWrite now rasterises the outline scaled
  three times horizontally and filters each row with the five-tap FIR FreeType uses for
  `FT_RENDER_MODE_LCD`; D2D1 blends such runs once per colour channel; GDI takes its
  antialiasing from the prefix rather than the host's fontconfig. Needs the registry keys
  under *Font Setup* below — without them nothing changes. The one-pixel padding of a
  ClearType glyph's bounds reads the glyph cache under the fontface lock like every other
  cache user: WPF rasterises on its render thread with the fontfaces its UI threads lay
  text out with, and NinjaTrader 8 crashed in the heap while a chart was resized. See
  `Related` for the origin
- **DXGI**: composition swapchain, FLIP_SEQUENTIAL preservation, DComp popup handling,
  monitored fence support. The swapchain-to-window mapping is published on the desktop
  window, whose property list is shared across the window station, so it is keyed by
  owning process id — otherwise a host application and its WebView2 child can allocate a
  swapchain at the same address and resolve each other's window; the reblit timer of a
  DComp target window presents only on the thread that presents the swapchain — JUCE
  does so on the window thread and needs the timer's present, Qt Quick presents from a
  render thread, and a second presenting thread left one of the two asleep in wined3d's
  frame latency wait (Dorico 5 froze when an entry of its VST instrument menu was picked)
- **D3D11**: ID3D11Fence with CPU timeline semantics. `DiscardResource()` and
  `DiscardView()` are implemented, backported from Wine 11.18 (five upstream commits,
  including the fixes for buffers without a structure byte stride, the sub-resource index
  of array textures and the resource reference leaked by `DiscardView()`); before that
  they were stubs whose FIXME made up 95 % of the log of a 9.5-minute Reaper session,
  because a WebView2 plug-in issues `DiscardView()` dozens of times per second
- **D3D11 format support**: `CheckFormatSupport()` handed out what the backend can do with
  the texture standing in for a format, so a typeless format was sampleable and a render
  target, integer formats were filterable, blendable and resolvable, D32_FLOAT was readable
  by shaders while R32_FLOAT had no comparison sampling, and the bits that are properties
  of the format itself — `DISPLAY`, `CAST_WITHIN_BIT_LAYOUT`, `BACK_BUFFER_CAST`,
  `SO_BUFFER` — were never set. FNA3D gates its swapchain on `DISPLAY` (WineHQ bug 56383).
  Measured on Windows 10 for every DXGI format at feature levels 9_3 to 11_1: the answer is
  restricted by what the format is (typeless, integer, depth, depth view, compressed, sRGB,
  video) and completed with the format's own properties, `TYPED_UNORDERED_ACCESS_VIEW` needs
  11_0. `CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2)` was `E_NOTIMPL`; it now reports
  typed UAV stores, the guaranteed R32 loads and atomics, and fails like Windows when no bit
  applies (tiled resources, logic ops and shared resources are not reported, consistent with
  the `D3D11_OPTIONS` the device returns). Two `todo_wine` in the d3d11 and d3d10core tests
  are gone; the tests pass on Windows 10 in both architectures
- **FP16 swapchains**: DXGI presents an `R16G16B16A16_FLOAT` swapchain as scRGB — its
  values are linear and are encoded for display, on SDR outputs too. wined3d copied them to
  the window unencoded, so everything drawn into such a swapchain was too dark. GMPI, the
  framework behind SynthEdit and its plug-ins, renders into one as soon as
  `CheckFormatSupport()` reports `DISPLAY` for that format (see above): SynthEdit's tiled
  panel background came out at 2,8,86 instead of 25,50,157, its structure view at 105
  instead of 172. The present blit now encodes a floating-point back buffer to sRGB; a probe
  writing 0.5 reads back 188 in the window (128 before), 8-bit swapchains are unchanged. The
  Vulkan renderer presents it through a `B8G8R8A8_SRGB` swapchain image for the same reason
  (it fell back to `B8G8R8A8_UNORM` before, or took a float surface format in the non-linear
  sRGB colour space where one was offered).
  `CheckColorSpaceSupport()` and `SetColorSpace1()` answer by the back buffer format, the
  rule DXVK applies as well: scRGB only for `R16G16B16A16_FLOAT`, sRGB for every other
  format (`SetColorSpace1()` accepted sRGB only, also for FP16, and the check was `E_NOTIMPL`)
- **WineD3D**: composition buffer with dirty rect accumulation, GL buffer recycling pool
  (70 % RSS reduction), and a `vs_out` initialisation that no longer trips NVIDIA's shader
  compiler warnings. Client threads that wait for the command-stream thread (a map that
  goes through the CS, a full queue, a busy resource) block on an event instead of
  yielding in a loop — while the CS thread sits in a vsync'd swap, the loop cost a full
  core per waiting thread; Kontakt 8, which maps dynamic index buffers through the CS
  every frame, went from 66 % to 3 % on its render thread. The one wait that protocol
  cannot serve is the STOP packet at device destruction: the CS thread releases the
  queues on its way out and must not touch the command stream afterwards, so it sets
  the event unconditionally from a copied handle and the destroying thread waits for
  that — a device released while the CS thread was still busy used to hang the process
  (Cubase 15 probes and releases a D3D11 device before its splash). What remains on NVIDIA is the
  driver's own busy-wait for the vblank in the CS thread (`wined3d_cs` at ~95 % for a
  static window); `__GL_YIELD=USLEEP` in the environment makes the driver sleep instead
  and takes it to ~5 % at the same frame rate. The frame latency wait in
  `wined3d_cs_emit_present()` counts its waiters and a woken waiter passes the signal on:
  with one flag and one auto-reset event, the second of two threads presenting
  swapchains of the same device was never woken (two threads at 20000 presents each hung
  in 3 of 3 runs, 10 of 10 complete now). A client thread that waits for the command stream in a finish or a
  resource-idle wait releases the global wined3d mutex for the wait and takes it back
  afterwards, so threads of different devices no longer serialise on each other's CS
  thread — SynthEdit's WPF chrome (D3D9) and its GMPI canvas (D3D11) at every resize
  step, where the canvas thread spent ~106 of ~120 ms per step on the mutex, none now.
  The wait for queue space keeps the mutex (the producer has captured the queue head),
  and every waiter on the progress event counts itself and passes the signal on, since
  two threads of one device may now wait at once (Cubase 15's video engine and GUI hung
  on the single signal otherwise). The gamma ramp is saved when an application first
  sets one and restored only then: reading and restoring it for every swapchain was two
  X round trips under the mutex, 6–82 ms each while the X server is busy with a resize
  (WPF creates a swapchain per resize step), and the restore also undid a night-light
  ramp set outside Wine whenever a windowed D3D application exited. A viewport update
  that does not change the state emits no packet (d2d1 set it per primitive). With the
  Vulkan renderer a buffer the CPU does not access is no longer uploaded through a BO in
  device-local memory, which the CPU cannot map: the initial data of a
  `D3D11_USAGE_DEFAULT` buffer aborted the process on an assertion (SynthEdit 4 s after
  start, `VK_ERROR_MEMORY_MAP_FAILED`). Such uploads go through system memory, as upstream.
- **Backports from Wine 11.20** (six upstream commits, cherry-picked with their origin):
  the `cs_queue` fields are aligned to cache lines so the client and the CS thread no
  longer share one, the queue pointers are published with `WriteRelease()` instead of a
  full `InterlockedExchange()`, the GLSL program key compares the pixel shader ID first
  and `parse_channel_desc()` no longer calls `strlen()` (wined3d); `media_source_start()`
  no longer reads an uninitialised stream count (winegstreamer, Wine bug 60241); and a
  `RtlWaitOnAddress()` entry is unlinked with release semantics and re-checked with acquire
  semantics, so a waiter cannot see its address cleared before the unlink is visible
  (ntdll, the queue the loader-lock wake above runs through)
- **wintypes**: `RoResolveNamespace()` looks up the `.winmd` files in `system32\WinMetadata`
  (backport of 5542cd8541f and 03d18c456aa from Wine 11.19, with their tests). The stub
  returned `RO_E_METADATA_NAME_NOT_FOUND` for every namespace, and the .NET Framework resolves
  a Windows Runtime type through this call: Power BI Desktop 2.158 stopped at start-up with
  `Could not find Windows Runtime type 'Windows.Storage.StorageFile'` (WineHQ bug 57410, an
  empty main window and a 2x2 "Something went wrong" dialog). `StorageFile` resolves; the
  start then failed on `Windows.Storage.Streams.IInputStreamReference`, which no installed
  `.winmd` defined — the next entry
- **windows.*.winmd (widl, include)**: eight more Windows Runtime metadata files are generated
  and installed — Windows.Foundation, Windows.Foundation.Collections,
  Windows.Foundation.Metadata, Windows.Storage.Streams, Windows.Storage.FileProperties,
  Windows.Networking.Connectivity, Windows.UI.Core and Windows.UI.Xaml.Interop — and
  `windows.storage.fileproperties.idl` carries the content property classes (backport of
  13289668fd1 from Wine 11.11). widl writes only the types of one IDL into its `.winmd`, so of
  the Windows.* types the ten installed files refer to, 37 were defined in none of them; with
  eighteen files the set holds 381 type definitions instead of 202, and 31 references stay
  open (parameterized interfaces, which widl does not write, the Windows.Foundation.Metadata
  attribute classes, `EventRegistrationToken`, `AsyncStatus`, `Windows.UI.Input.PointerPoint`).
  widl itself needed three fixes to produce the files: the class column of the MethodImpl
  table was written with the index width of the table that the class index happened to name
  (backport of 8b963e98fe4 from Wine 11.17; `windows.networking.connectivity.winmd` had four
  rows 8 bytes wide and every table after them shifted), the contract attribute of a method
  is taken from the class (an interface without one crashed widl on `windows.foundation.idl`;
  from Tao Wen's change for Power BI Desktop, taowen/wine f2ca524b98f), and a property with a
  setter only is registered on the setter, as midlrt does (`ICoreWindow2.PointerPosition`
  crashed widl on `windows.ui.core.idl`). Power BI Desktop 2.158 resolves
  `Windows.Storage.StorageFile`, `Windows.Storage.FileProperties.StorageItemContentProperties`
  and `Windows.Foundation.Metadata.ApiInformation` from these files and starts without the
  `.winmd` of a Windows installation (measured on the devel branch, whose local NTLM sign-in
  it also needs); `Windows.System.Launcher` and `IStorageItemProperties` are still defined in
  no IDL
- **msxml3**: the SAX writer (`MXXMLWriter`) accepts `putDocumentLocator()`, and with indentation
  enabled it no longer writes a newline before the first element or indents the end tag of an
  element with text, which changed the text content (backport of 7981b37f728 from Wine 11.4 and
  of 138e23626e3 and 75b3d038b58 from Wine 11.11). The Analysis Services engine of Power BI
  Desktop (`msmdsrv.exe` 17.0) rewrites its `msmdsrv.ini` through `SAXXMLReader` into an
  indenting `MXXMLWriter`: the `E_NOTIMPL` from `putDocumentLocator()` aborted the parse and the
  engine exited before it opened its port, and with the old indentation `<Port>0</Port>` came
  back as `0` followed by a line break and a tab, so the next start rejected the file
  (`msmdsrv_Failed_ini.bak`). The engine now starts and listens; the next stop was
  `ImpersonateAnonymousToken()` (ntdll, below)
- **ntdll**: `NtImpersonateAnonymousToken()` lets the thread impersonate a new token for the
  anonymous logon SID (S-1-5-7) at `SecurityImpersonation` level, without privileges, as
  measured on Windows 10 19045; the stub returned `STATUS_NOT_IMPLEMENTED`, also upstream as of
  Wine 11.20. Windows lists only the untrusted mandatory label as group and uses logon session
  `0x3e6`; here the anonymous logon SID is the only group, because the server takes the primary
  group from the group list. The Analysis Services engine of Power BI Desktop calls
  `ImpersonateAnonymousToken()` right after `listen()` and stopped with "Invalid function"; it
  now keeps listening and writes its port file
- **mshtml**: `IHTMLTextAreaElement::disabled` is implemented (backport of f4880b7ee65 from
  Wine 11.12, with its test); the stub returned `E_NOTIMPL`. Power BI Desktop shows its error
  dialog as an HTML page in the WinForms web browser control, which stays 2x2 pixels until the
  page script reports its size; the jQuery UI focus search read `textarea.disabled`, the
  exception aborted the script, and the invisible modal dialog locked the main window. The
  dialog now opens at its size and can be closed
- **mshtml**: a document in IE10 or IE11 mode gets an agent style sheet that sets
  `min-width` and `min-height` to 0, the minimum size IE 10 and 11 give flex items. Gecko
  implements the automatic minimum size of the flexbox spec, the item's content size, and a
  page written for IE that sizes a flex item to its container and lets a child scroll had that
  item run out of the container instead. Power BI Desktop's options dialog: the options pane
  grew to 1052 px in a 668 px dialog body and ran over the footer, where the transparent box
  took every click meant for OK and Cancel (a hit-test probe found the click on the pane's
  `SECTION`, not on the button); now the pane scrolls and the buttons work. The sheet changes
  nothing for other elements, whose automatic minimum is zero already, and any rule of the
  page overrides it; the mshtml `style`, `dom` and `events` tests give the same results with
  and without it
- **winemenubuilder**: a shortcut without arguments no longer produces a desktop entry whose
  `Exec` line ends in an empty `""` argument (backport of 1977760e374 from Wine 11.19). vatSys,
  which registers the `vatsys://` URL protocol, exited silently during its .NET start-up when
  launched with that argument from the desktop icon Wine had generated: no window, no crash,
  nothing in its own error log
- **ntdll**: MADV_FREE for MEM_RESET (improved page reclaim behaviour)
- **ntdll**: the heap enabled the LFH frontend only for size classes up to 1K; blocks between
  1K and 16K stayed in the backend heap, whose free path writes the free-list links over the
  first 16 user bytes of a freed block (the vtable pointer of a C++ object). Windows 10 19045
  switches every class up to 16K once the frontend is active, after ~2040 allocations or 18
  live blocks, and keeps the content of freed LFH blocks, so use-after-free bugs on such
  objects are silent there and threw `std::__non_rtti_object` under Wine. Measured with
  a probe on both sides, test in `kernel32/tests/heap.c` (`test_lfh_activation`)
- **Unix pipes and sockets on the standard handles (ntdll)**: a FIFO or Unix socket
  inherited on fds 0-2 — the journald stream socket of a desktop launcher, `wine app |
  tee` — reads as `FILE_TYPE_PIPE`, but it is a plain file object and every named-pipe
  call on it failed: `SetNamedPipeHandleState()` with `ERROR_INVALID_HANDLE`,
  `GetNamedPipeHandleState()`/`GetNamedPipeInfo()` with `ERROR_INVALID_FUNCTION`,
  `PeekNamedPipe()` with `ERROR_NOT_SUPPORTED`, `FlushFileBuffers()` with
  `ERROR_INVALID_PARAMETER`. libuv's `uv_pipe_open()` turned the first into `EBADF`, and
  Electron applications started from a `.desktop` file aborted with "Uncaught Exception:
  Error: open EBADF" while creating `process.stderr` (Native Access). ntdll now answers
  these calls for such a descriptor as Windows 10 does for an anonymous pipe: byte mode
  and blocking waits are accepted, message mode is refused with `ERROR_INVALID_PARAMETER`,
  `FilePipeLocalInformation` reports the real buffer size and the bytes pending, the
  closing state once the writer is gone, `PeekNamedPipe()` returns the bytes available
  (copied without consuming them, `tee()` for a FIFO and `MSG_PEEK` for a socket),
  `ERROR_BROKEN_PIPE` on an empty pipe without writer and `ERROR_ACCESS_DENIED` on the
  write end, `FileNameInformation` is an empty name, and a flush succeeds. Only
  `PIPE_NOWAIT` is still refused (`ERROR_NOT_SUPPORTED`; Windows accepts it), because it
  would need `O_NONBLOCK` on a file description the other end shares. Real named pipes
  are untouched, the server still handles them
- **ntdll heap**: the first-fit walk in `find_free_block()` tries 16 blocks of the first
  free list and then continues with the next list, where every block is large enough.
  With many free blocks of nearly the same size the walk took thousands of steps for each
  allocation; blocks of 1 KiB and more are hit hardest, because their LFH bins need
  thousands of live blocks before they are enabled. Dorico 6, ten mode switches: 399 → 9.9
  steps per call, 15.5 % → 0.3 % of the process samples in the walk, 9 % less CPU time,
  2 % more anonymous memory. `kernel32:heap` passes unchanged on both architectures
- **ntdll**: `GetUserPreferredUILanguages()` and its process, thread and system variants
  returned a one-entry dummy list with the current locale and printed a `FIXME` on every
  call. JUCE 8.0.4 asks for the user's display language for every piece of text it shapes
  (twice, for the size and the buffer): with the Cymatics Hotline editor open the message
  came 552 times per second and made up 98 % of the log. Wine 11.17/11.18 implemented the
  family (Alexandre Julliard, `c1197354dfc` to `1350cc58cdc`, cherry-picked with their
  origin): the lists come from `HKCU\Control Panel\Desktop\PreferredUILanguages` and
  `MuiCached\MachinePreferredUILanguages`, fall back to the user's and the system's UI
  language, and resource lookup walks that list with en-US as the last resort. The message
  is gone and the answer is the real list; notepad and winecfg titles under `de_DE` and
  `en_US` unchanged, `kernel32:locale` passes
- **ntdll (debugger attach, wow64)**: attaching winedbg to a 32-bit process — `attach`,
  `bt all` — killed it with a page fault on execute access at an address like
  `0xffbb11b0`. A 64-bit debugger creates its break-in thread at its own
  `DbgUiRemoteBreakin`, and a thread of a wow64 process starts in 32-bit code, so it ran at
  the low half of the 64-bit address (upstream as well; only the old wow64 had a
  workaround). The target process now starts that thread at the `DbgUiRemoteBreakin` of its
  32-bit ntdll. A 32-bit program in `Sleep()` and vatSys 1.4.20 (78 threads) survive
  `bt all` and an attach/detach; 64-bit processes and the automatic crash handler, which
  does not attach this way, are unchanged
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
- **winex11**: an application that leaves the maximized state while keeping its window
  rect no longer ends up at the geometry the window manager remembers from before the
  maximize. With the rect unchanged there was nothing to request, so the window manager's
  own restore geometry was adopted; it is now answered once with the application's rect,
  which is where Windows leaves the window. Qt 6.5's `QWidget::restoreGeometry()` does
  exactly this with a snapshot taken while maximized: Dorico 5 dropped out of its maximized
  window on every switch to Engrave mode and every lower-zone toggle and shrank to its
  previous size; it now stays screen-sized, only the maximized flag is gone, which is what
  the application asks for (issue 18 on the public repository)
- **winex11**: fullscreen switches hold against the window manager's own reconfiguration.
  After a decoration or `_NET_WM_STATE` change the first `ConfigureNotify` that carries the
  serial of our configure request but another geometry is the window manager's reaction to
  that change, not its answer; it is no longer adopted, the desired geometry is requested
  once more (Ableton Live 12 came back from F11 smaller by the frame extents every time —
  upstream behaviour up to 11.18). And KWin acknowledges `_NET_WM_STATE_FULLSCREEN` before it
  reconfigures the window: the config from before is now waited out instead of applied, which
  took the window out of fullscreen again (Fender Studio Pro 8 reached fullscreen in about
  half of the attempts, a regression of this branch); DComp window support, backing store; ownerless TOOLWINDOW popups are no
  longer folded into the active window's group, given a transient_for owner, or mapped as
  UTILITY — this fixes sticky, wrongly decorated and always-on-top plugin menus on KDE;
  the opaque black X expose background is suppressed while a GL/D3D client window is taken
  off the screen for offscreen rendering, so the client area no longer flashes black for a
  frame (visible in Ableton Live 12 when toggling the Learn View panel); a window hidden
  while its map was still unacknowledged no longer stays mapped and unpainted for the
  session (upstream fix for Wine bug 59932, cherry-picked; FL Studio's Browser panel at
  startup); a per-pixel-alpha layered window on a 32-bit visual no longer gets a bounding
  shape on every change of its contour — the compositor composes the alpha itself, and
  the shape update made KWin repaint between the resize and the new image of an
  animated popup, which showed a strip of it or nothing for single frames (FL Studio's
  About fruit, 16 flickers in 36 test cycles against 0); the mask is kept as the input
  shape, so clicks on transparent pixels still pass through, set after the image is in
  place and at most once a second during an animation; since the mask no longer cuts the
  window, the pixels a `ULW_COLORKEY` colour key removes are cleared to transparent in the
  image itself — they stay opaque in the surface, and with the bounding shape gone an
  `UpdateLayeredWindow()` colour key had no visible effect at all (Microsoft Agent's
  characters on a solid box; a 128×128 probe now keeps exactly its unkeyed 64×64 square,
  like vanilla Wine); a colour key given as `PALETTEINDEX` keys nothing, as measured on
  Windows 10, where Wine used to turn it into black and cut away the black pixels
- **Window-surface repaints (win32u, winex11)**: a series of erase-and-repaint races in
  the window-surface path. Flushes are held back while an erase waits for its repaint,
  `XShmPutImage` is waited for before the surface is painted into again, a new surface
  inherits its predecessor's pixels, a plain move invalidates only child windows, the
  xrender PutImage path is flushed, and a client window that will render offscreen is
  created under the dummy parent. Seen as MDI captions going dark during a resize, a
  black toolbar after a menu and a stale MDI client after a drag in SynthEdit 1.5, and as
  a one-frame black flash on a fast resize in FL Studio. An empty surface clip region now
  disables the flush instead of lifting the clip: a top-level covered completely by a
  child with a pixel format has nothing left to flush, and the unclipped flush painted the
  surface over what the child had presented. A plug-in editor embedded by yabridge is
  exactly as large as its top-level, so Mercurial Tones Dagon, which places a Direct3D 12
  child over its whole editor, stayed black there while it rendered in a Wine-hosted DAW
- **winex11, wineserver**: a window painted through `UpdateLayeredWindow()` shows its
  window surface even when a Direct3D swap chain targets it. The client window the swap
  chain puts on the top-level is rendered offscreen, so it no longer covers the surface,
  and the surface region of a layered window without layered attributes keeps its client
  area despite the pixel format. WPF renders a popup through Direct3D 9, hands the frame
  to `UpdateLayeredWindow()` and never presents again; the client window then showed a
  stale frame or undefined pixmap content, and SynthEdit 1.5's menus came up as a copy of
  the screen below them or black once the empty-region flush above was disabled. WineHQ
  bug 60173 reports the same for every WPF menu, tooltip and `AllowsTransparency` window.
- **winex11**: the present blit that `x11drv_client_surface_reblit()` repeats on a
  `VisibilityNotify` is dropped for a window's own swap chain once the window shows
  `UpdateLayeredWindow()` content. WPF creates a Direct3D 9 swap chain for an
  `AllowsTransparency` window, presents it once at start-up and from then on only hands
  frames to `UpdateLayeredWindow()`; the pending blit was replayed on the `VisibilityNotify`
  that follows a resize, over the window surface flushed a moment before, and only the parts
  a later flush covered came back. NinjaTrader 8's welcome window grows from 378×550 to
  902×502 for its trading-mode page and kept an L-shaped hole at its top-left corner (376×127
  plus a 56 px column, 68 744 pixels with alpha 0 in the two measured starts, showing stripes
  or a cyan tint from the undefined pixmap of the resized offscreen client window).
  `scripts/repro-513-ulw-reblit.sh` shows 188 752 magenta pixels of a 902×502 window before
  and 0 after. A child's swap chain inside such a window is still repeated.
- **wineserver, winex11**: a child window of another process with
  `WS_EX_NOREDIRECTIONBITMAP` shows its DirectComposition content over a top-level painted
  through `UpdateLayeredWindow()`, as the DWM composes it on Windows. Such a top-level
  hands out a one-pixel surface rect that keeps GDI out of it and its children, and dcomp
  delivers a tree through GDI; the DC of that child now keeps the top-level's client area,
  also when the child's class has `CS_PARENTDC` and its DC is clipped like the parent's
  (only the top-left pixel showed the tree before, WineHQ bug 58921 comment 16). The
  top-level's surface flush leaves the child alone like any foreign child, also when the
  child carries `WS_EX_TRANSPARENT` (Chromium's "Intermediate D3D Window" does) or
  covers the whole window: an empty surface region no longer means an unclipped flush for
  a layered window. NinjaTrader 8's chat window, a WPF layered window around a
  WebView2 page, showed only its grey background (WineHQ bug 58921). A child of the
  layered window's own process still shows nothing, since it paints into the layered
  surface that the next `UpdateLayeredWindow()` overwrites
- **winex11**: a DC on a window whose top-level belongs to another process takes the
  visual that top-level was created with instead of the default one. The xrender picture
  format follows the DC's visual, and a format whose depth differs from the drawable's makes
  `XRenderCreatePicture` fail with BadMatch, a fatal X error. WebView2's GPU process
  presents a DirectComposition swap chain into its intermediate D3D child window under the
  browser process's tooltip top-level, which carries a depth-32 ARGB visual
  (`WS_EX_NOREDIRECTIONBITMAP`); the GDI present of the first tooltip killed the GPU process
  (Splice INSTRUMENT with WebView2 runtime 154). The visual id is published on the window as
  a property next to the X window id, so the lookup is a client-side `XGetVisualInfo`
  without a round trip.
- **d3d11**: the immediate context exposes `ID3D11VideoContext1` (the methods beyond
  `ID3D11VideoContext` are stubs). WebView2 runtime 154's GPU process queries it right after
  `ID3D11VideoDevice1` while initializing DirectComposition and aborts with a CHECK when the
  query fails; it crashed six times at startup until Chromium fell back to a software
  compositor, which is also the configuration the tooltip crash above surfaced in. With the
  interface the first GPU process survives and composites normally.
- **win32u**: a mouse capture taken without a menu or move/size loop ends with
  `WM_CANCELMODE` when the window manager moves, resizes or changes the state of a window
  of that thread, unless a mouse button is held. The press on the window manager frame
  never reaches Wine; on Windows it would go to the capture window as a client click. WPF
  menus (SynthEdit 1.5) take the mouse that way, so an open menu stayed open and stayed
  behind at its old screen position while the window was dragged away. Win32 drop-down
  lists lose their capture the same way but have no handler for it, so they still stay
  until the next click. WineHQ bug 12027.
- **win32u**: `TrackPopupMenu()` keeps the mouse capture on the owner window, as Windows
  does, instead of moving it to the menu window, and a popup menu that does not track the
  right button (no `TPM_RIGHTBUTTON`) no longer swallows that button. Its `WM_RBUTTONUP`
  goes to the owner in screen coordinates, where `DefWindowProc` turns it into
  `WM_CONTEXTMENU`, and a right click outside the menu ends it and stays in the queue like
  a left click; neither happens while the left button is held for the menu. During the
  menu `GetCapture()` and `GUITHREADINFO.hwndMenuOwner` name the owner, the owner gets
  `WM_CAPTURECHANGED` when the menu ends, and a second `TrackPopupMenu()` from one of its
  handlers fails with `ERROR_POPUP_ALREADY_ACTIVE` unless it passes `TPM_RECURSE`. An
  application that opens a context menu from `WM_RBUTTONDOWN` and finishes the press in
  `WM_RBUTTONUP`, `WM_CONTEXTMENU` or `WM_CAPTURECHANGED` never saw those messages; Groove
  Agent SE opens its menus with an invisible `SMTG MessageOnlyWindow` as the owner. The
  pointer grab of the X11 driver stays on the menu window, since the owner need not be
  mapped. Measured against Windows 10 19045 with a self-driving probe: the eleven gestures
  of the first reference run now give the same messages. The new `test_menu_owner_input`
  passes, and the two capture checks in the user32 message tests that were marked todo now
  pass as well. Still different: the middle button, which Windows passes to the owner
  during a menu, and `WM_MENURBUTTONUP`, which Wine does not send.
- **winex11**: the driver reports the pointer position when the pointer leaves one of its
  windows (`LeaveNotify`), the way it already does when the pointer enters one. Without it
  the position the server holds stays where the pointer was last seen inside the window
  while it sits on the frame the window manager draws, and the `WM_MOUSEMOVE` the server
  synthesizes after every window move to resync hover state lands at that stale point. It
  falls on a different part of the window after each step of a window manager drag; in
  SynthEdit 1.5 the menu bar items lit up one after another while the window was dragged
  by its title bar.
- **winex11**: on Xwayland, a `LeaveNotify` that takes the pointer to an ancestor while it
  still reports a position inside the window is treated as stale. Xwayland stops seeing the
  pointer once it is on a decoration the compositor draws (KWin on Wayland); the crossing
  event and `XQueryPointer()` then both repeat the point where the pointer was last seen
  inside the window, so the report above put the server's position back at that point and
  the `WM_MOUSEMOVE` the server synthesizes after every step of a window manager drag kept
  landing inside the window. The driver now puts the pointer just outside the edge it left
  through and, while the window manager moves the window, keeps it there relative to the
  window (`X11DRV_GetWindowStateUpdates`). vatSys under KWin 5.27.11 on Wayland (Xwayland
  23.2.6): the menu bar items lit up one after another while the window was dragged by its
  title bar; in one capture the position the server held stayed at a single point 21–43 px
  below the top of the window rect through 288 moves of the window, over the menu strip in
  274 of 290 samples. With the fix no item lights up. An X server that sees the pointer
  where it is (plain X11) reports the crossing with a position outside the window and is
  unchanged.
- **winex11**: while the window manager moves a window on `_NET_WM_MOVERESIZE` (a title bar
  the application reports as `HTCAPTION`), the driver holds the capture on that window and
  the loop waiting for the end of the move no longer passes it mouse messages — what the move
  loop of win32u does, and Windows. The window manager has the pointer grabbed for the whole
  drag, so the position the server holds stays where the drag started, and the
  `WM_MOUSEMOVE` the server synthesizes after every step of the move went to whatever window
  the moved window had under that point; the `LeaveNotify` report above never comes when the
  drag starts inside our own window. Power BI Desktop 2.158 draws its title bar in WebView2:
  its six title bar buttons lit up one after another while the window was dragged, the
  frozen point wandering across 331–1493 px of the window's width in one capture; with the
  capture on the moved window in 405 of 412 samples of a drag, none lit up. A plain capture:
  with `GUI_INMOVESIZE` the driver would grab the pointer the window manager holds.
- **winex11**: the window manager is offered no close function (`MWM_FUNC_CLOSE`) while the
  window's `SC_CLOSE` is grayed, disabled or missing in its system menu or the class carries
  `CS_NOCLOSE`. That is the test `WM_DELETE_WINDOW` is already discarded under, and the
  state the caption close button is drawn grayed in on Windows; the hints offered the
  function for every enabled window, so KWin drew an active close button whose click went
  nowhere. Qt 6 grays `SC_CLOSE` for a window created without `Qt::WindowCloseButtonHint`:
  the preferences and view options dialogs of Dorico 6 could not be closed from the title
  bar, while `Esc` and the dialog's own button worked. The button is now drawn disabled.
- **winex11**: a tool window (`WS_EX_TOOLWINDOW`) with a full caption gets its frame from
  the window manager like any other titled window. Since 2007 Wine has left every tool
  window undecorated and drawn the caption itself, so REAPER's floating FX chain windows,
  captioned `WS_EX_TOOLWINDOW` popups, showed Wine's own title bar inside the
  desktop theme. Tool windows without a caption (menus, tooltips, shadows) and windows
  that draw their own non-client area stay undecorated, and the window keeps its Windows
  geometry: the smaller tool window caption is what the window manager's frame replaces.
  Checked in REAPER, Cubase and Ableton Live 12; FL Studio and Fender Studio Pro 8 draw
  their own frame and are unchanged. Setting `DecorateToolWindows` to `N` under
  `HKCU\Software\Wine\X11 Driver`, or per application under
  `HKCU\Software\Wine\AppDefaults\<app.exe>\X11 Driver`, restores the previous behaviour.
- **winex11**: an ownerless layered popup that never takes the focus (`WS_EX_NOACTIVATE`) and is
  neither click-through (`WS_EX_TRANSPARENT`) nor topmost is managed by the window manager. Codejock
  Xtreme Toolkit Pro draws the drop shadow of its frames into such a window (`XTPFrameShadow`,
  style `0x94000000`, ex style `0x08080080`, WineHQ bug 60187); `is_window_managed()` mapped it
  override-redirect, so the shadow stayed above every other window and on every virtual desktop,
  where on Windows it is an ordinary top-level window. The bug's proposed test leaves out
  `WS_EX_NOACTIVATE` and `WS_EX_TOPMOST` and would also catch the four shadow windows JUCE 8
  creates around every popup menu and tooltip (ex style `0x00080088`, no `WS_EX_TRANSPARENT`).
  Measured on Xvfb with kwin_x11 with a reproducer carrying the Codejock styles
  (`shadow524.c`): override-redirect before, managed after with
  `_NET_WM_DESKTOP` and skip-taskbar states, below a foreign window that is activated; topmost,
  click-through, owned and non-`WS_EX_NOACTIVATE` variants and the JUCE 8 menu shadows stay
  override-redirect. SynthEdit's menus, REAPER with a JUCE 8 plugin editor and FL Studio reach the
  new branch zero times.
- **win32u**: a captioned window whose client area reaches into the caption or the borders
  — an application that draws its own title bar from `WM_NCCALCSIZE` — is shown with its
  full window rectangle and without a window manager frame (upstream `6cfa941df3f`,
  Wine 11.20, cherry-picked with its origin; Wine bugs 59877 and 42877). Before, the frame
  was derived from the standard caption and covered the top of the application's own title
  bar: a test window that keeps its borders and drops only the caption got decorations
  `0x7a` and an X window starting 23 px below its client origin; it now gets decorations
  `0x0` and the whole window. The main windows of FL Studio 2026 (no
  `WS_CAPTION`), Ableton Live 12 and Cubase 15 (client area inside the frame) keep their
  frame and geometry unchanged.
- **win32u**: a captioned window whose client top is not where `DefWindowProc`'s
  `WM_NCCALCSIZE` put it, or whose client `DefWindowProc` never computed, is shown with its
  full window rectangle and without a window manager frame as well (vatSys ASD window lost
  its title bar when maximised, giang17/wine issue 26, issue 534). `DefWindowProc` records
  the insets it returns from `WM_NCCALCSIZE` in the window, and the visible rect
  computation compares the client top with that record. Before, the ASD window that
  `Windows > Air` opens (`WS_OVERLAPPEDWINDOW`, its own 26 px title strip from
  `WM_NCCALCSIZE`) was undecorated while restored, because its client reaches into the
  frame; maximised by its own button, the application insets the client by 4/30/4/6, the
  client lay inside the standard frame, the visible rect shrank to it, the decorations
  flipped to `0x7a`, KWin put its title bar over the strip and moved the window down, and
  the application's next `WM_NCCALCSIZE` flipped the hints back with the strip outside
  the window. Now the decorations stay `0x0` through maximise and restore, the maximised
  window is the work area ((0,0)-(1920,1080), client (0,26)-(1920,1078)) and the strip
  stays on screen. A test window with a 30 px strip of its own
  (`projects/nc-titlebar-test/ncfull.c`) behaves the same at 96 and 120 DPI and in a
  prefix whose `CaptionHeight` makes the standard inset 30 px too, which a comparison
  against the standard metrics could not tell apart. Nine standard non-client layouts
  (plain, client edge, scroll bars, tool window, dialog frame, menu bar, a handler that
  takes `DefWindowProc`'s answer unchanged, one that adds a band at the bottom) keep
  `0x7a` and `0x1a` (`stdwin.c`). Unchanged, measured by the `_MOTIF_WM_HINTS` of every
  mapped window and a `+win` trace: Pianoteq 9, REAPER 7 and its FX window, Ableton Live
  12 Intro, Nuendo 15 (project window, MixConsole, VST MultiPanner), Power BI Desktop and
  the NinjaTrader 8 log-in window (both undecorated before), and vatSys's main window and
  tool windows. The window manager places the maximised undecorated window on the work
  area, so an application frame that `WM_NCCALCSIZE` keeps at the edge is on screen there
  (4 px for `ncfull.c`), where Windows keeps it off screen.
- **win32u / winex11**: a top-level window the window manager raised on its own (a click
  under X11) is brought to the top of the win32 Z-order as well, as Windows does on
  activation, and the win32 order that results is applied back to the X stacking. An
  application may keep another of its windows above the activated one from
  `WM_WINDOWPOSCHANGING` — Cubase 15 implements *Always on Top* for its tool windows that
  way (the Audio Performance monitor carries neither `WS_EX_TOPMOST` nor an owner) — and
  Wine then saw no Z-order change at all: `set_active_window` never touched the Z-order,
  so the project window already sat below the monitor in win32 terms while KWin had just
  raised it over the monitor. `winex11` had no way to express a sibling-relative order
  either: `XConfigureWindow` with `CWSibling` is refused with `BadMatch` under a
  reparenting window manager, and a pure raise is dropped for managed windows (upstream
  `69e3a51b3dc`). The driver now sends the EWMH `_NET_RESTACK_WINDOW` message for a
  window the win32 order puts below a visible sibling, on every Z-order change and, flagged
  by win32u, after a window-manager raise; a change of `_NET_CLIENT_LIST_STACKING` is
  checked for the active window too, since a click on the frame never reaches Wine.
  Verified with Cubase's monitor against clicks into the inactive and the active project
  window, the monitor and the KWin title bar (12/12 keep the monitor above); the user32
  window tests are unchanged.
- **win32u / winex11**: the Z-order resync after a window-manager raise no longer counts a
  window's own owned popups as predecessors. `sync_zorder_after_wm_raise()` looked for the
  first visible window above the clicked top-level and, if there was one, requested
  `HWND_TOP` with `SWP_WINE_ZORDER_SYNC`; an owned popup above its owner is the normal
  win32 order (`swp_owner_popups()` puts it back there on every Z-order change of the
  owner), so the request changed nothing in win32 and still made the driver send
  `_NET_RESTACK_WINDOW(Below, popup)` to the window manager — on every click, as long as one
  visible owned popup sat above the main window. vatSys (giang17/wine issue 21, 32-bit
  WinForms, the map drawn with Direct2D) flickered on nearly every map click once one of its
  owned forms (Jurisdiction, Error Window, the OzStrips form) had ended up above the main
  window; in the clean launches all of them sat below it. The window manager keeps owned
  windows above their owner anyway (`WM_TRANSIENT_FOR`, set by winex11 for owned windows).
  The predecessor walk in win32u and the two in winex11 (`sync_window_position()` for the
  sibling of a managed window, `net_client_list_stacking_notify()`) now skip windows owned by
  the window, directly or through the owner chain; an unmanaged window still counts them for
  "top", since it must not be raised over its own popups, and ownerless predecessors keep the
  behaviour of the entry above. Measured with `scripts/repro-517-owned-popup-resync.c` (main
  window with a child, 8 clicks into the child on `:99` with kwin_x11): one owned popup above
  the main window — resync and `_NET_RESTACK_WINDOW` on 8 of 8 clicks before, 0 of 8 after,
  the popup stays above in `_NET_CLIENT_LIST_STACKING`; the Cubase pattern (ownerless tool
  window held above from `WM_WINDOWPOSCHANGING`) — 8 resyncs, 15 restack requests and the
  tool window above after every click, before and after alike; with both windows the restack
  now names the tool window instead of the popup. The reporter's build with the equivalent
  win32u change: 0 resyncs in 26 map clicks against 9 of 13 before, no flicker. user32 `win`
  tests: the same five failures before and after.
  A click into a child of the active window (the map, not the main window itself) went
  through the `WM_MOUSEACTIVATE` branch of `process_mouse_message()` and resynced as an
  activation, i.e. with `SetWindowPos(HWND_TOP)`, although the window was active before the
  click and Windows leaves the Z-order alone. Once owned `WS_EX_APPWINDOW` windows counted as
  predecessors (group transients, entry below), every plain map click in vatSys moved the main
  window to the top, `swp_owner_popups()` lifted the owned windows back above it, and the map
  was erased and repainted in full — the map flicker the reporter still saw on Plasma 6.7.5
  under XWayland with no change of the X stacking (giang17/wine issue 29). Such a click now
  takes the path of a click into the window itself and only re-applies the win32 place.
  Measured with `projects/appwindow-taskbar-test/childclick.c` (main window with a child map,
  two owned `WS_EX_APPWINDOW` windows over it, five clicks into the map on `:99` with
  kwin_x11): `WM_WINDOWPOSCHANGING` to the main window, `WM_ERASEBKGND` and full-size
  `WM_PAINT` to the map 5/5/5 before, 0/0/0 after, also with the map focusing itself on
  `WM_MOUSEMOVE` as vatSys does — the same messages as Windows 10 19045 in the reference VM
  (`selfclick`: `WM_MOUSEACTIVATE` to the map and the main window, `WM_LBUTTONDOWN`, no
  Z-order or paint message); vatSys map clicks resync as "already active" (3 of 3); the
  Cubase pattern of `repro-517` (`tool`, `both`) keeps the tool window above after 8 of 8
  clicks. user32 `msg` tests: identical failure lines; `win`: no new failure.
  The re-application itself no longer touches the win32 Z-order either: it passed the nearest
  *visible* predecessor as insert-after, and with windows that are never shown between that
  predecessor and the main window (vatSys owns several, between its ATIS window and the main
  window) the call moved the main window above them, ran `swp_owner_popups()` and invalidated
  the whole window on every map click — the full map repaint the reporter still measured on
  Plasma 6.7.5 after the change above, found and analysed by the reporter. It now passes
  `SWP_NOZORDER`; the driver works the X sibling out of the win32 order anyway. `childclick.c`
  with three never-shown owned windows (`hidden`): `WM_WINDOWPOSCHANGED`, `WM_ERASEBKGND` and
  full `WM_PAINT` 5/5/5 before, 0/0/0 after, one restack request per click before and after;
  Windows 10 19045 sends none of them with the same Z-order. vatSys map clicks: no
  `swp_owner_popups()`, no post-move invalidation; `repro-517` keeps the owned and the tool
  window above after 8 of 8 clicks.
- **win32u**: the palette messages of an activation are sent only on a palettized display.
  `set_active_window()` sent `WM_QUERYNEWPALETTE` to every newly active window and, when the
  window answered, broadcast `WM_PALETTEISCHANGING`. Wine's own user32 message tests, which
  also run on Windows, never require the message: `dlls/user32/tests/msg.c` lists
  `WM_QUERYNEWPALETTE` 35 times, 33 times as `optional` and twice as `wine_only`, and two of the
  optional entries carry the note that the message should not be sent. Both messages serve the
  realization of a logical palette, which only a palettized display has. WinForms answers
  the message in `Control.WmQueryNewPalette` with `Invalidate(true)`, so under Wine every
  activation erased and repainted the whole window with its children. vatSys (giang17/wine
  issue 21) calls `SetFocus` on its map child on every `WM_MOUSEMOVE`; each time the pointer
  came back onto the map from a popup the main window was activated and the Direct2D map
  flickered. `is_palette_display()` now checks `RASTERCAPS` of the display DC once per process
  (winex11 reports `RC_PALETTE` only for a palettized visual) and both messages are skipped
  otherwise. Measured on a TrueColor display, 11 activations alternating between two windows:
  11 `WM_QUERYNEWPALETTE` before, 0 after. user32 `msg` tests: the same failure lines before
  and after; the `test_msg_setpos` steps that expect `WM_ACTIVATEAPP` where Wine sent
  `WM_QUERYNEWPALETTE` now pass inside their `todo_wine` blocks (9 steps).
- **win32u / winex11 / wineserver**: owned `WS_EX_APPWINDOW` windows get their own taskbar
  entry, and owned windows follow their owner as on Windows. vatSys (giang17/wine issue 29)
  owns its strip windows and VSCS and gives them `WS_EX_APPWINDOW`; on Windows each has a
  taskbar button, under KDE none had one, because winex11 set `WM_TRANSIENT_FOR` to the owner
  and task managers never list a transient window. winex11 now makes such windows transient
  for the root window — a group transient (NETWM 7.3) — with the owner as group leader: KWin
  keeps a group transient above every window of its group mapped before it, so the owner
  never comes above them, and task managers list it (Plasma 5 X11 model
  `xwindowtasksmodel.cpp:224` skips only transients for a known non-root window; the Plasma 6
  Wayland model takes KWin's parent window, which a group transient does not have). The first
  version of this change (issue 531) left the windows without any transient hint and had the
  driver restack the owner after every raise; under KWin on Wayland each of those corrections
  was a visible frame — the map click flicker of issue 517 came back — and an owner the
  application activated during a move of an owned window (vatSys focuses its map on
  `WM_MOUSEMOVE`) stayed above that window for good (giang17/wine issue 29, issue 538).
  Measured with `projects/appwindow-taskbar-test/run-538.sh` on kwin_x11: with the group
  transient the owned windows lie above the owner from the first map, and the driver's
  stacking corrections after clicks and activations of the owner drop from 13 to 0 for
  `tbtest` and from 11 to 0 for vatSys. Known cost of the group transient: KWin also keeps
  it above the owned windows mapped before it, so an older owned window cannot be raised
  above a younger one (vatSys VSCS, mapped before the strips, stays below them where they
  overlap; the order changes with the map order after a minimise and restore). Taking the
  owner's transient relation away in issue 531 also exposed where Wine's own owner Z-order
  differed from
  Windows 10 (measured with `projects/appwindow-taskbar-test/ownerz.c` in the reference VM):
  `SetWindowPos(owner, HWND_TOP)` and showing the owner now lift all its owned windows above
  it, also those below it because the owner was set with `GWLP_HWNDPARENT` after creation;
  `ShowWindow` minimising the owner sends `WM_SHOWWINDOW(SW_PARENTCLOSING)` to its owned
  windows and leaving the minimised state `SW_PARENTOPENING`, so they hide and come back with
  it; `SW_HIDE` keeps the Z-order place of a top-level window that is neither active nor an
  owner. On the X side the driver restacks such a window below its win32 predecessor after
  the map (KWin stacks a window mapped without activation below the active one) and the owner
  below it, and repeats the stacking check once the window manager confirms an activation;
  the server's expose heuristic no longer moves a window above a window it owns, nor above a
  same-owner `WS_EX_APPWINDOW` sibling. Measured: `ownerz` order, visibility and messages
  identical to Windows 10 under kwin_x11 (and under the null driver on the 11.20 experiment);
  vatSys with six taskbar entries, the strips and VSCS above the map after activation,
  minimise and restore. user32 tests on the 11.20 experiment with the same changes: `win` no
  new failure and five `todo_wine` checks succeeding, `msg` identical failure lines.
  Smoke-tested on the desktop with Ableton Live 12, the KM88 Editor, FL Studio 2026 and
  Cubase 15 (its tool windows are ownerless and unaffected).
  A group transient is related only to the group members the window manager managed before
  it (KWin `X11Window::checkGroup()`, the same in 5.27 and 6.7): an owned window shown before
  its owner — vatSys shows VSCS and ATIS behind its splash screen and the main window after
  it — stayed unrelated to the owner, and KWin raised the main window above those two on
  every click and activation while OzStrips, shown later, stayed above (giang17/wine issue
  29, issue 548). Such a window now takes the owner as `WM_HINTS` group leader only while
  the owner is visible, and showing or hiding the owner rebuilds the hints of the owned
  windows mapped before it, so KWin regroups them behind the owner and keeps them above it
  from then on. Measured with `latemain.c` (owner shown 2 s after two owned windows) on
  kwin_x11: the owner lies below them from the first snapshot and the driver's corrections
  over five click rounds drop from 8 to 0; vatSys's main window, shown after VSCS and the
  strip windows, is never above them after the start over map clicks and activations
  (`run-vatsys-clicks.sh`, `xprop -spy`), where before every click raised and lowered it.
- **winex11**: a window shown without activation no longer flashes in the taskbar. KWin marks
  every normal window it does not activate on map with `_NET_WM_STATE_DEMANDS_ATTENTION`;
  Windows flashes a taskbar button only on `FlashWindowEx()` or when another process keeps the
  foreground. The driver now removes the state once per map when the map did not ask for
  activation or the foreground belongs to the same process, unless `FlashWindowEx()` set it.
  vatSys's strip windows and VSCS came up flashing on every start.
- **win32u / winex11**: the window manager gets the application's minimum and maximum track
  size as `PMinSize`/`PMaxSize`. `set_size_hints()` sent them only for windows that are not
  resizable; a resizable window got position and gravity only, so the window manager shrank it
  below the `ptMinTrackSize` of `WM_GETMINMAXINFO` during an interactive resize, win32 clamped
  every step back to the minimum (`DefWindowProc`, `WM_WINDOWPOSCHANGING`) and asked for the
  clamped size, which the interactive resize overrode — after the release the X window stayed
  small while win32 kept the minimum, and the edges win32 placed outside the visible window could
  not be grabbed (vatSys VSCS, 610x318 minimum, X window 430 px wide, giang17/wine issue 21).
  `get_min_max_info()` now caches the track sizes it obtains in the window structure, and the
  driver reads them through `NtUserCallHwndParam_GetMinMaxTrack` in raw pixels without sending a
  message (the hints are updated under the driver's own lock, where application code must not
  run); `PMinSize` goes out for every resizable window, `PMaxSize` only when the application set a
  maximum. Measured with `scripts/repro-521-min-track-size.c` (minimum 400x300) on kwin_x11: an
  interactive corner drag left the X window at 92x41 against win32's 400x300 before (19 `mismatch
  config` warnings), and at 392x266 — the minimum less the frame — after, no mismatch.
- **wineserver**: hit tests and rectangle mapping work at negative screen coordinates when the
  window's DPI differs from the monitor's. `scale_dpi()` (`server/user.h`) multiplied the
  signed coordinate by an unsigned DPI, so a negative value wrapped around (−780 at 120 → 96
  DPI became 35790769) and `is_point_in_window()` missed every window whose point lay left of
  or above the primary monitor: `WindowFromPoint()` returned NULL, mouse messages went to the
  desktop thread, and the window got neither `WM_SETCURSOR` nor clicks there. Only a DPI-unaware
  (or system-aware) window on a winecfg DPI above 96 with a second monitor at negative
  coordinates reaches the mapping; at 96 DPI `map_dpi_point()` returns before scaling. vatSys
  maximised on a second monitor whose top is at −144 (giang17/wine issue 26, problem 1): the
  band from the client top (−95) to y = 0 — the MenuStrip and the top of the map — took no
  input, so the menus were dead and the pointer kept the map cursor until it left the window at
  the top. The multiplication is now done in signed 64 bits, the form of upstream merge request
  12210 (open, Rémi Bernon). Measured with `projects/menuhit-533/menuhit.exe` (DPI-unaware,
  menu bar, 28 px strip child, map child) at 120 DPI on a nested weston with two Xwayland
  outputs and the primary on the right, window at x = −1000: before, six pointer positions over
  menu bar, strip and map all missed (`WindowFromPoint()` NULL, no `WM_MOUSEMOVE`) and a click
  on the menu bar did nothing; after, `HTMENU`, strip and map hits at the expected logical
  points and the click opened the popup — the same on 11.0 and 11.20. Positive coordinates
  were unaffected before and after.
- **d2d1**: the bitmap methods that use the D3D11 immediate context take the lock of a
  multi-threaded factory as well. Since the device-context lock of issue 511,
  `ID2D1Bitmap::CopyFromMemory()`, `CopyFromBitmap()`/`CopyFromRenderTarget()`, `Map()` and
  `Unmap()` still ran unlocked on the immediate context of the device, which is not thread-safe.
  vatSys (giang17/wine issue 21) crashed in DXVK's `d3d11.dll` from its map render thread —
  `d3d11 + 0x154b1` behind the batch flush of `SetAntialiasMode()`, `+ 0x15529` behind `Map()`
  in the scratch buffer two days earlier, the same function — while another thread updated a
  bitmap; wined3d's `d3d11` serialises its entry points itself and hid the race in the smoke
  tests of issue 511. `D2D1_FACTORY_TYPE_MULTI_THREADED` promises synchronisation of "the
  factory and the objects it creates", so the four methods now take `factory->cs` (recursive, a
  bitmap call from inside a locked device-context call stays legal). Measured with
  `scripts/repro-520-bitmap-update-race.c` (8 drawing threads plus one `CopyFromMemory()`
  thread, i686, headless): before, DXVK 3 of 3 runs crashed inside DXVK and wined3d crashed in
  `get_scratch_buffer()` once and hung at the first frame once; after, DXVK 4 of 5 runs of 300
  frames and wined3d 3 of 3 runs of 100 frames completed (the remaining DXVK run was at frame 200
  when the 180 s limit hit, no error).
- **d2d1**: closing a path no longer hangs when resolving overlapping Bézier control triangles
  reaches a triangle one ULP across. Halving such a triangle rounds every midpoint back onto its
  corners, so the half that stays in place is bitwise identical to the segment, its area stays
  tiny but non-zero, and `d2d_geometry_resolve_beziers()` inserted the same point forever —
  `ID2D1GeometrySink::Close()` never returned and the figure grew without bound (134,268 to
  374,781 vertices in 24 s, every split at the same index). The loop now splits the other segment
  of the pair when the chosen one would not change, and leaves the pair alone when neither would;
  only the exact no-change case counts, so every input the old loop finished takes the same path
  as before. Found with the path fuzzer linked from WineHQ bug 60115 (random closed paths on a
  6x6 grid): 2 hangs in 1,000,000 seeds before, none after;
  the d2d1 tests report the same failures before and after on x86_64 and i386. Related upstream
  report: WineHQ bug 60265 (Affinity Designer 3); the depth limit proposed there for crossing
  curves is a separate change (issue 523).
- **d2d1**: the walks of the fill triangulation end on degenerate paths. With points a few ULPs
  apart and subnormal coordinates the orientation tests contradict each other: the walk that
  removes the edges a new segment crosses (`d2d_cdt_cut_edges()`) went around in a circle and
  grew its edge stack at about 3.5 GB/s (14 GB resident after 4 s), or destroyed an edge twice
  or the edge it connects to next and crashed later; a face walk in `d2d_cdt_fixup()` and the
  edge ring in `d2d_cdt_insert_segment()` never closed. The cut walk now refuses an edge it has
  already collected and the edges the caller still needs, the other walks are bounded by the
  vertex and edge counts, and each case skips the segment like the existing failure paths.
  Measured with a fuzzer of random line paths with such coordinates: 87 failures in 20,000 paths
  before (69 out of memory, 16 hangs, 2 crashes), none in 100,000 after; without subnormal
  coordinates none before or after, and none of the new checks fires on those paths. Vanilla
  Wine 11.0 crashed on 1,340 and hung on 12 of the same 20,000 paths (not analysed further);
  WineHQ bug 60115 reports stack overflows in the same triangulation code for FL Studio + Serum 2.
- **d2d1**: the subdivision of overlapping Bézier control triangles is bounded. Splitting a pair
  separates the triangles only where the curves keep apart; where they cross, or run along each
  other, the halves overlap again at every level, and `d2d_geometry_resolve_beziers()` went on
  until a triangle had rounded down to a point, at a depth of about 25 for coordinates in the
  hundreds. Every split shifts the figure arrays behind it and adds a segment that is tested
  against all earlier ones; WineHQ bug 60265 reports a figure of Affinity Designer 3 with a
  cluster of nodes at nearly the same coordinates growing past 900,000 vertices until the process
  ran out of memory. A pair now stops splitting once either segment has reached depth 8, where a
  piece covers 1/256 of its curve (the bound of the patch attached to that bug), and the segment
  count is capped at 16 times the initial count as a backstop. The overlap left at a crossing is
  at most that piece's control triangle. Measured with the bug's reproducer, a rosette of N
  curves whose control points all lie near the centre: with vanilla's single quadratic per cubic,
  the bound holds at depth 8, 162 segments grow to 2,403 instead of further, and `Close()` takes
  38 ms instead of 51 ms for N = 160 (vanilla Wine 11.0: 23 s); in a 1016x1016 render of the
  N = 80 rosette 74 pixels change (0.007 %, single pixels at crossings). The cubic reproducer as
  attached never reaches depth 8 here (maximum 5) and renders identically; the fork closed it in
  52 ms for N = 320 already before this change, vanilla Wine 11.0 needs 507 s. The fuzzers of
  issues 522 and 523 run 1,000,000 integer and 120,000 float paths without a new failure, and the
  d2d1 tests report the same failures as before on x86_64 and i386.
- **d2d1**: the self-intersection pass no longer reports a crossing where two Bézier pieces run
  along the same line. A cubic whose end point coincides with its last control point, or with
  its start point, runs out along a straight line and folds back over itself (with p0 = p2 = p3
  the curve is p0 + 3t(1 - t)² (p1 - p0)); its quadratic pieces lie on that line over each
  other, and so do those of a second such curve on the same line. In the leaves of
  `d2d_geometry_intersect_bezier_bezier()` the points of two such pieces are rounded onto either
  side of the shared line, and `d2d_point_segments_cross()` reported a crossing in about every
  other leaf: one such cubic became 432 segments, a path of four cubics on one line 2,127, the
  overlap loop split those into 9,333, and the triangulation needed 20 s to insert a constraint
  through the cluster (`d2d_cdt_fixup()` with the exact incircle predicate). A leaf is now skipped
  when either chord runs along the other's line within 1e-6 of the coordinate magnitude, as the
  pass already skips coincident segments and parallel lines; the path keeps its 7 segments and
  closes at once. Found with the float path fuzzer of issue 522 (points on the line through two
  earlier points): the fuzzers run 1,000,000 integer and 120,000 float paths with the same
  results as before (no hang or crash; 152, 1,176 and 5,102 failed `Close()` calls in the three runs, before and after; the 20,000 float paths with Béziers close in 26 s instead of 89 s), the d2d1 tests report the same failures on x86_64 and i386,
  and the rosette, crossing-curve and arrow paths of issue 523 render identically except for
  the join of two petals of the cubic rosette that meet with opposite tangents, which no longer
  counts as a crossing (296 of 1016x1016 pixels, by up to 78 of 255).
- **d2d1**: closing a large path takes time linear in its vertex count where three loops of
  `ID2D1GeometrySink::Close()` were quadratic, for every path. `d2d_cdt_generate_faces()`
  classified every face of the triangulation with `d2d_path_geometry_point_inside()`, a ray
  cast over all figure edges (about 2n casts over n edges); the edges are now bucketed by rows
  of the geometry's y-range once per path and a probe is cast over its row only, with the same
  tests, figure bounds included, and a path whose edges mostly share one row is cast over the
  figures directly as before. The grid loop of `d2d_geometry_intersect_self()` cleared its
  array of tested segments with a `memset` per outer segment (a gigabyte of writes for 32,000
  segments); the array now holds the index of the outer segment that last tested each one.
  `d2d_cdt_insert_segments()` scanned all edges for one leaving the start vertex of every
  figure; the cdt now keeps that edge per vertex, the one with the lowest index as the scan
  found it, maintained where edges get their vertices and are destroyed. The results are the
  same: the renders of 29 paths (the rosettes, crossing curves, fold-back cubics and fuzzer
  reproducers of issues 523 and 526, the issue 458 arrow, polylines, polygons, a grid of 200
  small figures) are hash-identical, the d2d1 tests report the same failures on x86_64 and
  i386, and the fuzzers run 1,000,000 integer and 120,000 float paths with the same results
  (no hang or crash; 152, 1,176 and 5,102 failed `Close()` calls, as before; the integer run takes 349 s instead of 414 s). `Close()` of a 32,000-point polyline spanning its height takes 181 ms instead of
  1,335 ms, a 32,000-gon 63 ms instead of 303 ms, a grid of 1,600 polygons of 20 points 55 ms
  instead of 243 ms; at 4,000 points 2.4, 2.6 and 1.9 ms instead of 26, 16 and 6.5 ms. A
  polyline of 32,000 points along one line stays at 2.8 s: every face's probe still meets every
  edge there (issue 528; a winding propagation over the triangulation would be linear for that
  case too, not done).
- **d2d1**: the degenerate paths that failed `ID2D1GeometrySink::Close()` with `E_FAIL` close as
  on Windows. Of a million random closed paths on a 6x6 integer grid 152 failed and lost their
  figures, of 120,000 paths with float coordinates a few ULPs apart or below `FLT_MIN` 6,278;
  Windows closes all of them (reference VM, same binaries). Four causes, traced with probes in
  the triangulation: a line of zero length (two consecutive identical points) meeting a Bézier
  made `d2d_geometry_add_bezier_line_intersections()` divide zero by zero, and the check
  `t < 0 || t > 1` let the NaN through as a vertex that sorted after every other one and left the
  triangulation contradicting itself (duplicate edges, a vertex connected to itself, a vertex
  without edges) — every one of the 152 integer failures; the range checks of the bezier-line
  and line-line intersections now reject NaN. Coordinates below `FLT_MIN` make the exact
  orientation and incircle predicates contradict each other; `Close()` flushes them to zero
  (within 1.2e-38 of it). On an inconsistent triangulation the ring walk of
  `d2d_cdt_insert_segment()` accepted a sector that was not the triangle the segment leaves
  through, and the cut walk went around the whole outline back to the start vertex, where the
  caller connected the vertex to itself; the sector is checked against the face and a walk that
  returns to its start skips the segment without destroying anything. A vertex left without
  edges by a skipped constraint skips the segments starting there instead of failing the path
  (the faces are classified by a point inside them, so only the fill along those segments is
  off). Result: 0 of 200,000 integer, 20,000 float-with-Bézier and 100,000 float paths fail (32,
  1,176 and 5,102 before; without the sector and walk checks 3 and 6 of the float paths would
  still); 29 regular paths render hash-identically, the d2d1 tests report the same failures on
  x86_64 and i386. Tests `test_close_zero_length_line` and `test_close_subnormal_coordinates`.
  The fuzzers count failed `Close()` calls (`DONE fail=N`, `scripts/d2d1-fuzz/run.sh` sums them)
  and `scripts/d2d1-fuzz/windows-job.bat` runs them in the reference VM (issue 525).
- **d2d1**: a fill or text drawn on a device context without a target reports `D2DERR_WRONG_STATE`
  instead of crashing. `FillRectangle()`, `DrawGlyphRun()`, `DrawText()`, `DrawTextLayout()` and
  `FillOpacityMask()` reached `d2d_device_context_draw()`, which bound `&target.bitmap->rtv` with
  `target.bitmap` NULL (address 0x20) and crashed in `d3d11_device_context_OMSetRenderTargets()`;
  `FillRectangle()` lost its check when it got its own rectangle geometry instead of going
  through `FillGeometry()`, the text and opacity mask entry points never had one. All of them
  now set the error with the tags of the first offending call, as `Clear()`, `DrawBitmap()`,
  `DrawGeometry()` and `FillGeometry()` do and `test_no_target()` expects, and
  `d2d_device_context_draw()` refuses a context without a bitmap target itself. The d2d1 test
  binary used to die in `test_no_target()` with exit status 5 and no summary line (the tests
  queued after it on that thread went unrun); it now ends with 20,701 tests on x86_64 and 20,676
  on i386 and the same failures as before. 29 paths render hash-identically (issue 527).
- **win32u**: a window that loses its window surface for direct drawing no longer gets the
  surface's pixels copied over its client area when a pixel format is set on it. The switch
  happens in the first window position change after the client surface was attached, the
  window DC draws over the attached client window, and the copy landed on top of a frame
  the application had already presented; Dorico 6's Qt toolbar popups (top levels drawn
  through a composition swapchain) came up black in one of four openings when the first
  GL swap won that race. Upstream (11.20) makes the same copy.
- **win32u**: GDI output of a child window that presents offscreen no longer paints over the
  presented frame. The DCs of a window with a pixel format draw straight into the top-level's
  X drawable, which is harmless while the GL or Vulkan client window sits attached on top; a
  child renders offscreen, though, and its frame reaches the top-level only through the copy
  of each present, so the erase that `RedrawWindow(RDW_ERASE)` brings after every resize step
  painted over it until the next present. vatSys presents its Direct3D 11 map about three
  times a second during a modal resize, and the map went blank for most of a drag resize
  (giang17/wine issue 29): 312 of 1801 frames with wined3d on an Xvfb display, 466 of 1172
  with DXVK on the X11 desktop (NVIDIA), against 0 of 2638 screen dumps on Windows 10, where
  the presented frame stays. A window that has presented by such a copy now has its DCs
  bound to the top-level surface, whose flush leaves the child's client area alone; a window
  whose content is GDI drawing keeps direct drawing. After the change: 0 blank frames with
  both renderers, the SynthEdit 1.5 module panel click delivers its pixels and the MDI
  canvases show no episode in the resize measurement, user32 `dce` and opengl32 `opengl`
  pass, user32 `msg` fails in the same places as before.
- **win32u / winex11**: an offscreen Vulkan surface (a child window presenting through DXVK or
  vkd3d) shows the frame it has just presented, not the one before. Such a surface reaches the
  top-level through a copy of its composite-redirected client window right after the host
  present, and on NVIDIA the frame lands in that window 4 to 9 ms later, so the copy took the
  previous frame; an application that presents only when something changes stayed one frame
  behind until it presented again. In vatSys's strip windows with DXVK the selection marker
  followed a selection made on the map only after the next click or scroll in the strip window
  (giang17/wine issue 27); wined3d presents child swap chains through GDI and was not affected.
  `vkQueueWaitIdle()` and `vkWaitForPresentKHR()` both return before the frame is there; the
  window's XDamage event marks it, and the render thread now waits for that event (at most
  100 ms) before the copy, as the GL path waits for its swap. Single presents of a D3D11 child:
  DXVK 64-bit and 32-bit 0 of 6 correct before, 6 of 6 after, wined3d 6 of 6 before and after.
  Continuous presents: 60.2 fps with vsync before and after; without vsync 0.15 ms more per
  frame. Without libXdamage nothing changes.
- **win32u / winex11**: a process that registered the keyboard with `RIDEV_INPUTSINK` gets keys while
  a non-Wine window is active, as on Windows — dinput registers it that way for `DISCL_BACKGROUND`
  keyboards. X delivers core key events only to the focused client, so vatSys's push-to-talk (a
  DirectInput 8 keyboard with `DISCL_BACKGROUND`) only worked while a vatSys window had the focus
  (giang17/wine issue 28). win32u now tells the driver when the process gets or loses such a
  keyboard (new `pSetRawKeyboardSink()`, `WINE_GDI_DRIVER_VERSION` 109), and winex11 then selects
  `XI_RawKeyPress/Release` on the root window; a raw key is only sent while no Wine window is the
  foreground, so the key reaches sinks as `RIM_INPUTSINK` and no application gets a `WM_KEYDOWN`.
  Processes without a keyboard sink never select raw keys. Measured on Xvfb with kwin_x11: keys typed
  into a native X window arrive (before: nothing), held keys once down and once up, keys into the
  probe and into a second Wine process are not doubled, a probe without `RIDEV_INPUTSINK` gets nothing.
  Under Xwayland the X server only sees keys typed into Wayland windows if the compositor forwards
  them (KDE: System Settings → Applications → Legacy X11 App Support, `XwaylandEavesdrops`).
- **win32u**: the menu bar of a captioned window is measured and drawn through the
  undocumented non-client theming messages, as on Windows: `WM_UAHMEASUREMENUITEM`
  (0x0094) per item with an item size of 0x16, `WM_UAHINITMENU` (0x0093) before measuring
  and painting, `WM_UAHDRAWMENU` (0x0091) for the bar background and `WM_UAHDRAWMENUITEM`
  (0x0092) per item with a `DRAWITEMSTRUCT` in window coordinates and the states
  `ODS_SELECTED`, `ODS_GRAYED|ODS_DISABLED` and `ODS_INACTIVE`. `DefWindowProc` measures
  and draws the default, so an application that does not handle the messages gets the
  previous result; one that does can override the size and paint the bar itself (the line
  below the bar stays, as on Windows). A single owner-drawn item takes the whole bar back
  to the classic path, again as measured on Windows 10 (32-bit processes included).
  The bar is laid out as on Windows: the items start at the top of the bar and are
  `SM_CYMENU - 1` tall, the extra row of `SM_CYMENU` is the line below the bar, and
  `GetMenuBarInfo` reports the bar as ending where the items end. Wine kept that row as
  a border above the items and drew the line on the first client row, where the
  non-client clip cut it off: the last row of a bar an application paints itself was
  never painted (a light line under Live's dark bar) and the line below the bar never
  visible. Same client area as before, the `todo_wine` on `rcBar` in the user32 menu
  test from 2012 passes.
  Ableton Live 12 makes its menu bar 4 px taller this way and adds the same 4 px when
  sizing its main window; without the measure message the window grew until it hit the
  maximum height and lost its frame (Wine bug 57955), and without the draw messages its
  bar stayed light instead of the dark bar Live paints on Windows. This replaces two
  earlier workarounds for the same bug (suppressed reentrant size-only
  `WM_WINDOWPOSCHANGED`, lifted `window == visible` decoration gate in winex11), both
  removed; transparent (0x00) surface init for ARGB popups; the system arrow is shown
  again when an application hides the cursor and sets none; cursors are process-local in
  Wine, so a `WM_WINE_SETCURSOR` for an out-of-process child window (WebView2, bridged
  plug-ins) arrived with a handle the receiving process rejected and the previous cursor
  stayed — the owner now publishes each cursor's first frame in a named section and the
  receiver builds a proxy from it; the owned DC of a window hosted below a window of
  another process refreshes its visible region on every `GetDC`, since the move of that
  foreign ancestor never marks it dirty in this process (a composition blit through the
  stale DC landed where the pane had been, over the area the host had just erased);
  `DisplayConfigGetDeviceInfo` answers `DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL`
  with the nominal 1000 (80 nits) instead of failing — SynthEdit 1.5 asks for it about
  twice a second while idle; `WM_ACTIVATEAPP` carries the thread of the previous
  foreground window when the activation comes from another process instead of 0 — with
  0, Dorico 5's audio engine process and Dorico blocked on each other as soon as a
  plug-in editor opened; the CBT hook and the activation messages go out once per
  activation, as on Windows — a window that activates itself again from its `WM_ACTIVATE`
  handler got a second round and recursed until the stack was gone (Wine bug 46274; the
  fix wine-staging carries as `user32-recursive-activation`, with the mark cleared on
  every exit and only by the call that set it). The drawing messages go out only while a visual style is active, as on Windows,
  where they come from the themed non-client painting: Cubase 15 paints its bar through
  them with the theme's MENU class (`OpenThemeData( NULL, L"MENU" )`, `DrawThemeTextEx()`),
  and without an active style that handle is NULL and the bar came up without text. The
  measure message is sent either way, because the size the owner answers with is what the
  window's geometry is built on (Live's main window grows to the maximal height without
  it). Ableton Live's dark bar therefore needs a visual style enabled in winecfg; its
  window geometry does not.
- **wineserver**: a top-level's surface flush no longer overwrites child windows that
  belong to a *different process*. Such a child draws straight into the top-level's
  drawable while the owner flushes its own surface over it with a delay — a black
  one-frame flash over embedded panels whenever the two overlap. Its client rect is now
  subtracted from the surface region, the treatment GL and Vulkan children already
  receive. Affects any application that embeds out-of-process content: FL Studio's hub,
  Ableton Live 12 (WebView2), CEF-based plugin GUIs. Trade-off: a foreign child that stops
  painting without a geometry change leaves its last frame on screen until something
  moves. `WINE_DISABLE_FOREIGN_CHILD_CLIP=1` restores the previous behaviour. The owner
  recomputes that region only itself, so a foreign child that is shown, moved or resized
  after the region was taken now prods the owner's top-level (win32u, the same
  `update_window_state` that window creation already uses) — FL Studio's Browser window,
  open at start with the Sounds tab, went black for ~0.5 s after every activation change
  until it was hidden and shown once, because Chromium's child grew into it after FL had
  taken the region. A foreign window below a `WS_EX_TRANSPARENT` window of the owner is
  subtracted as well: the transparent window itself does not obscure anything for GDI, but
  the region walk used to skip its whole subtree. WinForms gives a control with
  `BackColor=Transparent` that style, and Power BI Desktop 2.158 hosts each of its WebView2
  views in one; dragging its window by the title bar erases the whole form on every step
  (`RedrawWindow(RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN)` plus `UpdateWindow()`), and
  the flush put that `COLOR_BTNFACE` over the views — 32 grey frames in 195 drag steps of a
  15 fps capture, now none
- **wineserver**: a window that is given a parent of another thread through `SetParent()`
  shares that thread's input only when it is a `WS_CHILD` window, as on Windows. A
  `WS_POPUP` window reparented this way keeps its own thread input: its activation
  deactivates the other thread's window through the foreground switch (a notify,
  `WM_WINE_SETACTIVEWINDOW`) instead of making that window the "previous" of
  `set_active_window()` and sending it `WM_NCACTIVATE`/`WM_ACTIVATE` synchronously. FL
  Studio's bit bridge (`ilbridge.exe`, a 32-bit plug-in in the 64-bit host) reparents the
  plug-in's popup window into FL's frame while FL's main thread waits for the bridge in a
  plain `WaitForMultipleObjects()`; the synchronous send never returned and both processes
  sat there (WineHQ bug 44827, the soft lock on showing the GUI of a bit-bridged plug-in;
  the same hang on un-parenting the window again, with "External window" enabled).
  Measured against Windows 10 with `projects/setparent-bridge-probe` (host blocking in
  `WaitForSingleObject()`, client reparenting across processes): `SetParent()` returns in
  50 ms there with the host's thread input left separate, and the host gets the same
  four deactivation messages once its own thread pumps again; a `WS_CHILD` window created
  under the foreign parent blocks and attaches on both. The editor at 0,0 that
  giang17/wine issue 23 reports is the parent-relative origin of a window that was
  un-parented again, the position Windows gives it too
- **wineserver**: the list of the siblings from a given window (`NtUserBuildHwndList()`,
  behind `EnumChildWindows()`) and `FindWindowEx()` from a given child stop at a window that
  is not linked into its parent's z-order. Destroying a window from another thread first
  unlinks it (`set_parent` with no parent); the walk from such a window to the end of the
  children list never got there — it went round the parent's unlinked list, and the server
  with it, at 100 % of a core with every client waiting. Closing Power BI Desktop 2.158 hung
  this way in two of four tries: its WebView2 host thread unlinks a `Chrome_WidgetWin_0`
  while the main thread enumerates that window's siblings
- **wined3d**: a GDI present of a window below a top level of another process blits
  through a fresh cache DC instead of the one held since the swap chain was created.
  That DC keeps the device origin and visible region of the window's position at that
  time: `invalidate_dce()` runs in the process that moves a window, so a move of the
  foreign ancestor never marks it dirty in the presenting process. FL Studio's bit bridge
  (`ilbridge.exe`) parents the plug-in's popup into FL's frame; after the frame was
  dragged, every present of the plug-in's Direct2D swap chain (the GDI readback path, the
  window has no X window of its own) still landed at the old position — the plug-in
  image stayed there while the frame at the new position showed only the borders FL
  copies along at each drag step (dense vertical lines with a mouse drag, the "stripes"
  of issue 479). Measured on `:99` with a 400×150 px drag in 40 px steps: all presents
  after the drag went through the held DC at the old origin; with the fresh DC the image
  follows the frame and nothing is left behind. The held DC stays as it is, a GL context
  that is not forced to GDI is bound to it
- **wined3d (Vulkan renderer)**: the SPIR-V vertex and fragment pipes carry `state_nop`
  entries for the 13 fixed-function render states that the stateblock turns into
  replacement shaders (`LIGHTING`, `FOGENABLE`, `SPECULARENABLE`, `COLORKEYENABLE`,
  `RANGEFOGENABLE`, `COLORVERTEX`, `LOCALVIEWER`, `NORMALIZENORMALS`, the four material
  sources, `VERTEXBLEND`), as upstream `d231cb2832a` added them to the GLSL pipes. Without
  them `validate_state_table()` printed 13 `err:d3d:validate_state_table … should have a
  representative` lines at every device creation with `renderer=vulkan` (65 at SynthEdit's
  start). Nothing changes in behaviour: the Vulkan context checks none of these states,
  and `d3d9_test stateblock` gives the same results before and after, 64- and 32-bit.
  Upstream 11.20 still prints the lines
- **opengl32 (WoW64)**: the Vulkan mapping behind a persistently mappable GL buffer is kept
  for the lifetime of the buffer instead of being created and destroyed around every
  `glMapBufferRange()`/`glUnmapBuffer()` pair. 32-bit wined3d does not keep its buffer
  objects mapped (`wined3d_map_persistent()` is false on 32-bit), so every upload maps and
  unmaps the 64 MiB allocator chunk; on WoW64 each of these went through `vkMapMemory2KHR`
  (placed) and `vkUnmapMemory2KHR`, which the NVIDIA driver implements as two 64 MiB `mmap()`
  calls per cycle (`PROT_NONE` over the range to release it, `MAP_SHARED` on `/dev/nvidiactl` to
  map it again) plus control ioctls. vatSys (PE32, SharpDX Direct2D) made
  ~1000 such cycles per second, the `wined3d_cs` thread spent 85 % of its time in them, the
  radar map only appeared after a window move and zooming, resizing and the whole UI lagged
  by seconds (WineHQ bug 60354 reports the same for 32-bit games). With the mapping retained
  the chunk is mapped once: `mmap` 5760 → 206 and `ioctl` 17 861 → 2440 in a 3 s sample of
  the CS thread, and none of the remaining `mmap()` calls maps 64 MiB any more (the driver still
  opens `/dev/nvidiactl` for about 40 small mappings per second). The 32-bit reservation of the address
  range was already kept across unmaps, so this costs no additional address space; the
  retained mappings show up in the process RSS (13 chunks, ~830 MiB for vatSys). The
  `VkApplicationInfo` of the Vulkan instance that this path creates now carries the right
  `sType` and `apiVersion = VK_API_VERSION_1_0`; the loader no longer prints
  `VkInstanceCreateInfo::pApplicationInfo::apiVersion has value of 0` at every GL context
  creation. Mesa drivers were never affected (upstream MR 11579 adds a driver-side mapping
  extension for them); upstream 11.20 still re-creates the mapping on every cycle
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
- **iphlpapi**: `GetAdaptersInfo()` reported one `IP_ADDR_STRING` more than needed for
  every IPv4 address of the host, loopback's included. Windows 10 reports the adapters
  times `sizeof(IP_ADAPTER_INFO)` plus one `IP_ADDR_STRING` only for each address of an
  adapter beyond its first, measured with one and two adapters and with a second address
  on one. Nuendo 15 sizes its second call as a whole number of `IP_ADAPTER_INFO`
  structures, got `ERROR_BUFFER_OVERFLOW`, and crashed on Studio > ADR API network setup.
  The lease times in `IP_ADAPTER_INFO` stay 32-bit in 32-bit code, so the structure keeps
  Windows' 640 bytes there after `time_t` became 64-bit on i386 in Wine 11.19
- **sapi**: the placeholder voice token `Wine Default Voice` carried no attributes, so a
  .NET Framework application enumerating voices got an `InstalledVoice` with `Name == null`
  and `Culture == null`, and every `SpeechSynthesizer.Speak()` failed in the voice matching
  of System.Speech 4.8: `NullReferenceException` in `Helpers.CompareInvariantCulture()`
  (from `InstalledVoice.FirstEnabled()`, when no default voice is resolvable, as with
  Wine's sapi whose category reported no default) or `ArgumentNullException` from
  `String.IndexOf(null)` in `VoiceSynthesis.MatchVoice()` (when one is). The token now
  carries a name, `Language=409`, gender, age and vendor, but deliberately no CLSID: Wine's
  engine is a stub, and a token pointing at it captured the output silently (0 bytes even
  for an SSML prompt that names another voice), whereas without it System.Speech disables
  the token on first use and a real voice wins, or reports "No voice installed on the
  system" when there is none. `ISpObjectTokenCategory::GetDefaultTokenId()` reads
  `DefaultTokenId` under HKCU and HKLM before `DefaultDefaultTokenId`, and
  `ISpDataKey::EnumValues()` is implemented (upstream MR 9733). A native voice remains
  necessary for audio, and the MS Speech SDK 5.1 voices did not speak with this alone:
  their engine (`spttseng.dll`) takes its pronunciations from lexicon classes of
  Microsoft's `sapi.dll` (`SpLexicon` with its user lexicon, `SpCompressedLexicon`) and a
  `SpPhoneConverter`, none of which Wine's sapi has. Creating the engine failed, through
  `ISpVoice` with `REGDB_E_CLASSNOTREG`, while System.Speech went on with the half
  initialised engine and rendered every word as the same 0.52 s of samples. Those classes
  read the SDK's undocumented `.lxa` files and stay native, registered to a renamed copy
  of the SDK's `sapi.dll` (Wine substitutes its builtin for a file named `sapi.dll`); they
  need three object token methods that were stubs or missing, now implemented after
  Microsoft's 5.1 `sapi.dll` (69 probe results identical): `ISpObjectTokenInit::
  InitFromDataKey()`, `ISpObjectToken::MatchesAttributes()` (attribute values compared
  case-insensitively, in token enumeration too) and `GetStorageFileName()` (path kept
  under `<token>\<caller CLSID>\Files`, a new file created empty and reported with
  `S_FALSE`). `ISpVoice` output is then byte-identical to that of Microsoft's complete
  5.1 `sapi.dll`, and vatSys 1.4.20's ATIS pattern (System.Speech into a 48 kHz stream)
  speaks, as do `SelectVoice()` and `<phoneme>` prompts. System.Speech itself needs SAPI
  5.3 interfaces and does not start with the 5.1 `sapi.dll` as a whole. Windows 10 ships no
  default voice in the registry either, but its SAPI picks one when a category without
  `DefaultTokenId` is enumerated or asked for its default (the first token whose `Language`
  contains the user's UI language, else the first token) and stores it under HKCU;
  System.Speech, which enumerates the voices when a `SpeechSynthesizer` is constructed, reads
  only that value and otherwise compares the voices with the thread's UI culture by walking
  `CultureInfo.Parent` up to a neutral culture, which never ends for the invariant culture.
  vatSys sets the invariant UI culture, so saving an ATIS hung forever in
  `SpeechSynthesizer.Speak()`. `GetDefaultTokenId()` and `EnumTokens()` now store the
  default as Windows does, and `SetDefaultTokenId()` is implemented.
  [scripts/speechsdk51-setup.sh](scripts/speechsdk51-setup.sh) prepares a 64-bit prefix
  for the SDK voices: it installs the SDK from `SpeechSDK51.exe` in Windows 2000 mode (its
  MSI installs the runtime only there), returns to the previous Windows version, and points
  the four classes at the renamed copy of the SDK's `sapi.dll`; `--check` reports the state.
  In a fresh prefix with .NET 4.8, the vatSys ATIS pattern then speaks without any further
  registration step
- **msvcp**: the stream classes' move constructors and move assignment were exported as
  stubs, so a C++ application calling one aborted with "Call to unimplemented function" —
  Ableton Live's indexer did, on startup in a fresh prefix, so Live restarted it in a loop
  and its browser stayed empty. Implemented for msvcp110, msvcp120, msvcp140 and msvcp_win;
  Live no longer needs a native VC runtime in the prefix
- **ucrtbase (complex math)**: `cabs`, `cabsf`, `conj`, `conjf`, `cexpf`, `csqrtf`, `cpow`
  and the MSVC multiplication helpers `_Cmulcc`, `_Cmulcr`, `_FCmulcc` and `_FCmulcr` were
  stubs, upstream as well. Nuendo 15's plug-in set imports all ten, and opening its
  Ambisonics Mixing template aborted with "unimplemented function ucrtbase.dll.cabsf" as
  soon as the VST MultiPanner loaded. The C99 functions are ported from musl like the
  existing `cexp` and `carg`; their edge cases (errno, `_matherr`, Inf and NaN) were
  measured on Windows 10 in both architectures, and `cpow` takes `pow()` for a real base
  and exponent as Windows does (`cpow(2, 3)` is exactly 8)
- **msvcrt/ucrtbase (`_hypot`, `_hypotf`)**: both returned the musl result without a math
  error, so an overflow reached neither `errno` nor the application's `_matherr` handler,
  and `cabs`/`cabsf` inherited that. Measured on Windows 10 through ucrtbase and
  msvcrt.dll in both architectures: `_hypot` reports `_OVERFLOW` with `ERANGE` everywhere,
  handing `_matherr` the magnitudes scaled by 2^-600 on x86_64 and by the larger one on
  i386, where every subnormal result — an exact one such as `_hypot(1e-320, 0)` included —
  is reported as `_UNDERFLOW` without touching `errno`; `_hypotf` reports `_OVERFLOW` on
  x86_64 only, with the arguments as passed. Inf and NaN arguments are not reported
- **advapi32 (credential attributes)**: `CredWriteW` dropped `CREDENTIAL.Attributes`, and
  `CredReadW`/`CredEnumerateW` returned none. Steinberg's License Engine marks its stored
  refresh token with an attribute (`isUtf-8`) and discards a token without it ("1.1 Format
  refresh token detecting -- resetting"), so closing the Activation Manager signed the user
  out and every Steinberg product opened its login page at start. The attributes are now
  stored with the credential and returned by the read, enumerate and ANSI functions; a
  conformance test in `dlls/advapi32/tests/cred.c` covers the round trip
- **avrt (MMCSS)**: `AvSetMmThreadCharacteristics`, `AvSetMmThreadPriority` and
  `AvRevertMmThreadCharacteristics` were stubs. A thread that registers for the `Pro Audio`,
  `Audio`, `Capture` or `Playback` task is now scheduled `SCHED_FIFO` (priority 3 at
  `AVRT_PRIORITY_NORMAL`, 5 at `AVRT_PRIORITY_CRITICAL`, the level of a JACK client thread),
  and the revert puts it back to `SCHED_OTHER`. Ableton Live 12 registers its audio workers
  this way (26 on a 20-core machine) and waits in every audio cycle for the slowest of them;
  as ordinary threads they lost the race against the GUI often enough that Live's CPU meter
  showed 140–4900 % on an empty set and jackdbus logged hundreds of xruns per minute, with
  the fix 6–8 % and none after the engine start. Needs a non-zero `RLIMIT_RTPRIO` in the
  Wine process (the `audio` group's `rtprio` limit); otherwise, and with
  `WINE_AVRT_RTPRIO=0`, the threads keep their previous priority. `WINE_AVRT_RTPRIO=<n>`
  sets the CRITICAL level. Other MMCSS tasks (`Games`, `Window Manager`, …) are left alone:
  Windows caps MMCSS threads through `SystemResponsiveness`, Linux has no such limit.
  `mmdevapi` registers its own pump thread (`audio_client_timer`, the last stage before the
  device) the same way, so a WASAPI client's mix is not handed to the hardware by a thread
  that ranks below the client's audio threads; Cubase 15 through its built-in ASIO driver had
  six `Audio Realtime` threads at SCHED_FIFO 4 and the pump at nice -19 before.
- **winealsa (mmdevapi)**: `IAudioClient::Start()` on a render stream whose ALSA device ran
  dry after `Stop()` recovers the device before writing to it. In that state
  `snd_pcm_avail_update()` returns `-EPIPE`, and `alsa_start()` passed the value on as an
  unsigned frame count; after alsa-lib's `snd_pcm_recover: underrun occurred` the retried write
  read past the end of the stream buffer, and `mmdevapi` stopped at its assertion
  (`unix call 6 returned status c0000005`). The window lies between the device running dry and
  the next period of the stream's timer, which recovers it. Studio Pro 8 hit it when closing
  its main window and when loading a project. A client that stops and restarts a 30 ms
  shared-mode stream at random phases aborted within the first 100 of 1000 rounds in three of
  three runs before, and ran all rounds after. Upstream has the same code (wine-11.20 and
  master of 2026-10-19)
- **Virtual-desktop compositor (winex11)**: windows inside a Wine virtual desktop get
  real per-pixel alpha, which the plain desktop drawable cannot provide. A small
  XDamage-driven compositor assembles frames off screen and composites them through an
  ARGB overlay onto an opaque child window, so translucent plugin popups and drop shadows
  blend correctly instead of showing black or leftover pixels
- **msvcrt (ucrtbase)**: the tmpnam() family returns names in the temporary directory as
  the UCRT does (`<temp>\u<pid>.<n>`, one prefix letter per function, base 36), instead
  of the `\s<pid>.<n>` form in the drive root that only the classic msvcrt.dll uses.
  Measured on Windows 10. libzmq builds its signaller socket pair from `_wtmpnam_s()` and
  `_wmkdir()`; with the root form a process whose current drive is `Z:` cannot create
  the directory and Native Instruments Kontakt 7 aborted at start-up when launched from
  a Unix working directory. Upstream since wine-11.19 (`d8e85ca62a5`, MR 12102)
- **AF_UNIX sockets (ws2_32, wineserver, ntdll)**: Unix-domain socket support, based on
  the long-standing wine-staging patch set plus hardening and five conformance fixes of
  our own — a socket is given its family before bind, a bound socket is reported as a
  reparse point by GetFileAttributes and can be opened and queried through CreateFile with
  FSCTL_GET_REPARSE_POINT, and an address carrying only the family (namelen 2) is accepted
  and leaves the socket unnamed, as measured on Windows. With these the ws2_32 AF_UNIX
  conformance tests run at all (upstream they skip) and pass. Needed by applications that talk to a local helper over a Unix socket;
  FL Studio's Cloud plugins install normally instead of spinning on a socket that never
  appears
- **WebSocket Protocol Component (websocket)**: `websocket.dll` implements the client and
  server side of the WebSocket Protocol Component API, whose exports upstream are stubs:
  handle creation and property validation, the client and server handshakes (random
  `Sec-WebSocket-Key`, SHA-1/base64 `Sec-WebSocket-Accept`, subprotocol and extension
  selection), the `WebSocketGetAction()`/`WebSocketCompleteAction()` send and receive action
  queues, masking (with the zero-key form used when masking is disabled), fragmentation by
  send and receive buffer size, streaming UTF-8 validation, ping/pong with an automatic pong
  on a received ping, and the close handshake. Buffers follow the layout the .NET Framework
  `WebSocketProtocolComponent` expects — a receive area at the start of the allocated buffer,
  a send area after it, received payloads unmasked in place — so
  `System.Net.WebSockets.ClientWebSocket` works. The conformance test in
  `dlls/websocket/tests` was first run with the native `websocket.dll`; the builtin
  reproduces that trace line by line (200 checks, no structural difference once addresses,
  keys and masks are normalised). Not mirrored: a fragmented message whose first frame
  carries a reserved bit without a negotiated extension, which native delivers as a custom
  message and the builtin rejects as a protocol error. NinjaTrader 8 logged on and then
  stopped with "The type initializer for 'System.Net.WebSockets.WebSocketProtocolComponent'
  threw an exception" at `fixme:websocket:WebSocketCreateClientHandle … stub`; its
  simulation data connection now comes up
- **Keyboard input into out-of-process content (wineserver, winex11)**: a process that
  translates keys for a window owned by *another* process now has the whole thread input
  family attached, derives the AltGr modifier from the passed key state rather than from a
  process-global cache of the last X11 event, and posts a finished IME result string as
  WM_CHAR as well. Together these make typing work in embedded WebView2 fields, including
  AltGr characters such as `@`, `\` and `€` on non-US layouts
- **Lock keys (winex11)**: when the CapsLock, NumLock or ScrollLock state of X differs from
  Wine's, the driver sets the toggle bit directly instead of sending a lock key press and
  release before the next key. Wine starts with every toggle off, so with NumLock on at
  login (Plasma's default) every application saw a NumLock press nobody made, in its window
  messages, low-level hooks and raw input; DirectInput 8 reads raw input, and vatSys's key
  binding dialog captured NumLock instead of the key pressed. With any `WH_KEYBOARD_LL` hook
  on the desktop the mismatch never settled (WineHQ bug 60059): the pair came before every
  key press and release, and with CapsLock typed text changed case at random. Measured with
  a 32-bit DirectInput 8 test, ten keys, NumLock on: before 1 fake `DIK_NUMLOCK` without a
  hook and 20 with one, the toggle left off; now 0 and the toggle on, as on Windows 11. A
  lock key changed while no Wine window had the focus is no longer replayed to hooks
- **Application compatibility layers (win32u)**: an entry under
  `Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers` (HKCU or HKLM) named
  after the full path of the executable applies, as on Windows. Wine looked up only the
  file name, which Windows 10 22H2 ignores (measured with a test program); the file name
  and the default value stay as Wine extensions. The vatSys plugins OzStrips and Maestro
  write `DPIUNAWARE` under the full path of `vatSys.exe`. Without it vatSys became system
  aware later in its run, and from wine-11.16 on its strip bays stayed empty: their
  panels, created under the WinForms parking window while the process was still unaware,
  could not be moved into the system-aware bay forms, because `SetParent()` refuses
  windows of different DPI awareness. With only the full-path entry, vatSys on 11.20 now
  creates the four strip bay devices again (six `DXGID3D10CreateDevice` instead of two)
  and leaves no panel in the parking window. Windows created before the process awareness
  is set follow it once it is set since the next entry, so the reparenting succeeds
  without a layer as well
- **DPI awareness of windows created before the process sets it (win32u, server)**: a
  window created while the process DPI awareness is still unset takes the awareness once
  the process sets it, as on Windows. Measured on Windows 10 22H2
  (`projects/dpi-process-503-probe`): every window of the process created before
  `SetProcessDPIAware()` or `SetProcessDpiAwarenessContext()` reports the process
  awareness afterwards, whatever thread context it was created under and on whichever
  thread, children included; a window whose awareness is already higher keeps it (a
  system-aware window stays system aware when the process declares itself unaware). Wine
  fixed the awareness at creation. WinForms applications that load WPF late (vatSys)
  create their first forms unaware, WPF then calls `SetProcessDPIAware()`, and from
  wine-11.16 on `SetParent()` refuses to move the earlier, unaware panels into the later,
  system-aware forms (`ERROR_INVALID_STATE`), which left the vatSys strip bays empty
  without a `DPIUNAWARE` layer. Now the `create_window` request carries
  `NTUSER_DPI_CONTEXT_FLAG_PROCESS` while the process awareness is unset, the server
  marks the window (children and reparented windows take the mark of their parent), and
  the new request `set_process_dpi_awareness` raises the marked windows of the process
  when it sets its awareness. Window rectangles are not rescaled (not measured on
  Windows; at 96 dpi there is nothing to rescale). Covered by `user32/tests/sysparams.c`
  (`test_dpi_context`, `test_SetProcessDpiAwarenessContext`); the test program's
  results for Windows 10, Wine before and after lie next to it in the project
- **Direct2D window targets with DXVK (winex11)**: the window of an `ID2D1HwndRenderTarget`
  renders offscreen when its swap chain is a Vulkan one. d2d1 marks such a window so that a
  child is attached to its top-level instead, because wined3d presents it through GDI and
  the blit of an offscreen window never comes. DXVK presents through the Vulkan swap chain,
  and attached, the window stayed black: vatSys's strip bays and its VSCS panel on X11 (and
  under Xwayland, as reported), while its map, a Vulkan child without the mark, rendered.
  Now they render offscreen like the map. OpenGL surfaces, wined3d's default renderer,
  keep the attached path
- **GDI present for a Direct2D window target with child windows (d2d1)**: an
  `ID2D1HwndRenderTarget` whose window has child windows when it is created gets no DXGI
  swap chain on a device that is not wined3d. It renders into a GDI-compatible texture, and
  `EndDraw()` blits the texture's DC to a window DC taken with `DCX_USESTYLE`, which clips
  the children of a `WS_CLIPCHILDREN` window — the way the DC render target presents.
  vatSys's VSCS window is such a child, with WinForms children (the Add Freq panel, the
  Text page) on top of it. wined3d presents a child window's swap chain through GDI and
  winex11 blits an offscreen Vulkan surface, so the children stay visible there; winewayland
  attaches DXVK's Vulkan client surface as a `wl_subsurface` above the top-level surface
  that holds the GDI children, and they were covered (reported with 11.20 + DXVK +
  winewayland; measured in a Plasma Wayland login and reproduced with a test program in a
  nested weston: 0 of the 12 800 pixels of a red GDI grandchild visible, all of them with
  this change, the wined3d and winex11 runs unchanged). The path costs one GPU-to-CPU copy
  per present (VSCS repaints every 500 ms); only windows with children at creation time and
  only `B8G8R8A8` targets take it. `WINE_D2D1_HWND_GDI_PRESENT=0` disables it, `=1` takes it
  on wined3d too, `=2` for every window target
- **Minimised windows under the Wayland driver (winewayland)**: backport of 7310a48e3d1,
  3972b7b5337 and 8d64701d80b from Wine 11.8 (WineHQ bug 59577). Minimising sets
  `WS_MINIMIZE` and moves the window to (-32000,-32000); the driver did not tell the
  compositor and kept a 160x24 surface that stayed so on every configure, so a window
  minimised from its title bar remained a bar that could be neither clicked nor restored
  (Notepad on KWin 5.27, Plasma Wayland). Now the driver minimises through
  `xdg_toplevel_set_minimized`, sends no `unset_maximized`/`unset_fullscreen` for a minimised
  window, and restores it with `SC_RESTORE` when the compositor sends a configure;
  3972b7b5337 fixes a use-after-release of the window rects in the configure handler. On
  KWin 5.27 the window comes back through Alt+Tab, a click on its task bar entry does not
  restore it (the same with Wine 11.20, which has the three commits). Only the Wayland driver
  is affected; the fork default is winex11 through Xwayland
- **Scroll bar size (win32u)**: the default `ScrollHeight` and `ScrollWidth` are 17 px
  (-255 twips) as on Windows — measured on Windows 10 22H2, where
  `HKCU\Control Panel\Desktop\WindowMetrics` carries -255 and
  `GetSystemMetrics(SM_CYHSCROLL)` returns 17 — instead of 16 px (-240), unchanged since the
  metrics became scalable (a657e5f6efa, 2013). WinForms sizes an auto-sized horizontal
  `TrackBar` to `SM_CYHSCROLL * 8 / 3`: 42 px instead of 45, and the sliders of vatSys's
  Display Settings, which paint their value from row `Height / 2 + 10`, cut the digits. The
  default goes into a new prefix; an existing prefix keeps the value in its registry
  (`reg add "HKCU\Control Panel\Desktop\WindowMetrics" /v ScrollHeight /d -255 /f`, the
  same for `ScrollWidth`, brings it in line)
- **GDI+ font metrics and text baseline (gdiplus)**: three things, measured against Windows
  10 with Terminus (TTF) Bold 4.39, the font vatSys ships. `GdipCreateFontFamilyFromName()`
  finds a family whose only face is bold (backport of e6e9d7627c0 from Wine 11.17): the
  lookup compared the face's full name "Terminus (TTF) Bold" with the family name and
  failed, and .NET fell back to GenericSansSerif, Tahoma here. A font that sets
  `USE_TYPO_METRICS` (OS/2 `fsSelection` bit 7) reports its typographic metrics as cell
  ascent, descent and line spacing — 833/167/1090 for Terminus, as Windows reports them,
  where hhea says 849/183/1122. `GdipDrawString()` places the baseline where Windows GDI+
  does: at the cell ascent scaled to the em size and rounded, or at GDI's hinted ascent
  (`tmAscent`) when the font has a VDMX table and that is larger — not at `tmAscent`
  unconditionally (94 of 94 family and size pairs on Windows 10, 69 of 70 under Wine
  afterwards; before, the digits of 16 px Terminus sat one row lower than on Windows). With
  the three and the 45 px slider, the digits under vatSys's brightness sliders are complete,
  as on Windows
- **Glyphs FreeType refuses (win32u)**: a glyph `FT_Load_Glyph()` rejects is drawn empty
  with the advance from `hmtx` instead of failing, and the ABC widths of a glyph that still
  fails are zero: `GetTextExtentPoint32()`, `GetCharABCWidths()` and the `DrawText()`
  measurement left them uninitialised, and whatever the stack held became the width.
  TerminusTTF-Bold 4.39 has a `.notdef` whose instruction count exceeds
  `maxp.maxSizeOfInstructions` (`FT_Err_Too_Many_Hints`), every control character maps to
  it, and WinForms measures a tab for the shortcut column of every drop-down menu: vatSys's
  menus opened across the screen, 30 849 px wide with Wine 11.20 and 32 475 217 px wide and
  blank with 11.0 (WineHQ bug 60548 reports the same menus with the same font). Windows
  draws nothing for the glyph and advances 8 px at 16 px (measured on Windows 10); so does
  Wine now
- **Embedded bitmap strikes in GDI metrics (win32u)**: `GetGlyphOutline(GGO_METRICS)`,
  `GetCharWidth32()`, `GetCharABCWidths()`, `GetTextExtentPoint32()` and the `DrawText()`
  measurement take the advance of a font's embedded bitmap strike when the face has one at
  the current ppem, and the ClearType rasterisation draws the strike's pixels as the mono
  one already did; the grey formats keep the outline. Before, every glyph was loaded with
  `FT_LOAD_NO_BITMAP` for everything but `GGO_BITMAP`, so a face whose strike is wider than
  its hinted outline was measured narrower than it was drawn. Windows 10 reports the
  strike's advance for `GGO_METRICS`, the ABC widths and `DrawText(DT_CALCRECT)` under every
  quality and font smoothing setting and draws the strike for NONANTIALIASED, DEFAULT and
  CLEARTYPE quality (reference VM; `GetCharWidth32()` and `GetTextExtentPoint32()` on that
  build still report the outline, on the reporter's Windows the strike — Wine now gives the
  strike throughout). TerminusTTF-Bold 4.39, shipped with vatSys, has strikes at 12 to
  28 ppem, and those at 14 and 18 ppem are one pixel wider than the hinted outline: vatSys
  sizes its strip fields from `TextRenderer.MeasureText("_")` on the 18 px font (reporter's
  reading of the IL) and draws them with DirectWrite, whose GDI-compatible layout already
  took the strike (10 px per glyph), so every field was one pixel per character too narrow
  and the callsign ran past its box. Wine's own Tahoma and Tahoma Bold carry strikes at 8 to
  16 ppem that Microsoft's Tahoma does not have, with advances that differ from Windows
  (11 ppem: `i` 2 px against 3, `g` 6 against 7); faces from the Wine data directory keep
  their strike for the mono rasterisation only, as before. Remaining difference: Windows
  trims the strike to its ink for the ABC split and the black box (`_` at 18 px: 1/8/1 and
  8x1 against 0/10/0 and 10x18 here)
- **GDI+ text metrics from the outline (gdiplus, win32u)**: gdiplus selects its fonts with
  `ANTIALIASED_QUALITY`, and a font selected with that quality keeps the outline's advances
  for `GGO_METRICS` and the measurements built on it where the strike metrics above would
  take the embedded bitmap strike. GDI+ on Windows measures the outline's advances
  regardless of a strike (Windows 10: 9.0 px per character for Terminus (TTF) Bold at 18 px
  and 7.0 at 14 px, where the strikes are 10 and 8 px wide; the reporter's Windows: 180.00
  for 20 characters at 18 px) and rasterises the outline, as Wine's gdiplus does through
  `GGO_GRAY8_BITMAP`; with the strike's advance in the measurement and the outline in the
  rasterisation, vatSys's connection window wrapped its "Disconnect" button — a 100 × 30 px
  `Graphics.DrawString()` with a default `StringFormat` in the application's own button
  class — after nine characters, "Disconnec" / "t", once connected. For GDI itself the
  reporter's Windows reports the outline for ANTIALIASED quality (9 px and ABC 0/8/1 at
  18 px against 10 px for DEFAULT and CLEARTYPE), which Wine gives now; DEFAULT quality,
  the `TextRenderer` measurement of the strip fields, keeps the strike
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
  Studio Pro 8. A presentation clock whose time source is replaced while it runs starts
  the new source at its current time, as Windows does (measured on Windows 10): Studio
  Pro installs its own time source right after each session start, and the video stayed
  black from the clip start on and after a loop wrap because that source was never started
- **Source reader row order (mfreadwrite)**: with advanced video processing enabled, the
  source reader converts through the video processor MFT, which emits bottom-up RGB
  whenever its output type leaves `MF_MT_DEFAULT_STRIDE` open — correct for the standalone
  MFT, measured on Windows — while the reader's caller gets top-down samples from native
  and a media type without the stride, so it cannot learn otherwise. Fender Studio Pro 8's
  timeline thumbnails came out upside down. The reader now hands the processor a copy of
  the output type with the stride pinned, which the processor keeps for every later
  re-creation of its conversion pipeline (a stream change upstream, a new input type set
  through the exposed transform), and strips the stride again from the type it reports.
  The earlier form of this fix pinned the stride only for the duration of the
  `SetOutputType()` call, so the first re-negotiation fell back to bottom-up; a test now
  drives both re-negotiations through a decoder-plus-processor chain and checks the row
  order of the delivered samples (4 failures before, 0 after; the suite is otherwise
  unchanged at 3062 tests, 219 todo)
- **Video players that drive the H.264 decoder themselves (wined3d, d3d11, d2d1,
  winegstreamer)**: Cubase 15 does not render video through the EVR. It drives the H.264
  decoder MFT directly: it enumerates it, hands it a DXGI device manager, receives NV12
  Direct3D 11 textures, reads each frame back and draws it through a Direct2D effect of
  its own with a YCbCr-to-RGB pixel shader. Four things were missing for that: wined3d
  could allocate planar NV12 textures only in its Vulkan renderer (the OpenGL renderer now
  keeps one texture per plane and creates plane views on them); `CheckFormatSupport`
  never reported `D3D11_FORMAT_SUPPORT_CPU_LOCKABLE`, and the application polls that bit
  and does not upload a frame without it; `ID2D1DeviceContext::DrawImage` dropped every
  custom effect with "Unhandled effect" (effects with a single draw transform and bitmap
  inputs are now drawn through the application's pixel shader); and the decoder's pool of
  ten D3D samples ran empty, because the player decodes about twelve frames ahead and
  holds the sample of every frame in that window until it has been shown — thirteen at a
  time, fifteen around a restart (the pool now grows to 32 on demand). The decoder also
  returned every frame four inputs late: one held by `h264parse` until the next access
  unit starts, two kept by `nvh264dec` to pipeline decoding against display because the
  transform answered the latency query as not live, one by the DPB bookkeeping of
  GStreamer's H.264 decoder base class. The transform now reports itself as live, which
  drops the two pipelining frames and with them the decoder restart the player performed
  at every keyframe, where the picture went black for 0.7 s. Where neither NVDEC nor
  VA-API serves the H.264 MFT and gst-libav's `avdec_h264` decodes instead, the live
  answer also makes libav choose slice threading over frame threading, which returned the
  first frame seventeen inputs late with the sixteen threads Wine allows it; a single-slice
  stream then decodes on one thread (11 ms per 4K frame on a 16-thread desktop, still real
  time at 30 fps). The video window shows the picture in stop and play
- **ProRes video in Cubase and Nuendo (d2d1 bitmap formats)**: Steinberg decodes ProRes with
  its own `VideoCodec-Apple-ProRes.dll` on the CPU, uploads the v210 output into an
  `R10G10B10A2_UNORM` texture and converts it with a one-input Direct2D custom effect. At
  start-up the video engine asks `ID2D1DeviceContext::IsDxgiFormatSupported()` for the whole
  DXGI range and builds that path only when the 10-bit format gets a yes. Wine answered from
  a table of seven bitmap formats; Windows 10 accepts fifteen (measured for every format and
  alpha mode, with and without `D2D1_BITMAP_OPTIONS_TARGET`, in both architectures, test
  `test_dxgi_format_support`): `R10G10B10A2_UNORM`, `R8_UNORM`, `R8G8_UNORM` and
  `BC1`/`BC2`/`BC3_UNORM` are bitmap formats but not target formats, and `B8G8R8X8_UNORM` is
  not a target format either. With the no, the engine decoded the first frame of the Nuendo 15
  demo clip four times, dropped it and never asked for another: the player stayed black (the
  clip itself opens with eight seconds of black, so the first frame is not the evidence —
  the file position of the `.mov` handle during playback is). With the table from this
  branch the clip plays in Nuendo 15 and in Cubase 15, which ships the same codec
- **winegstreamer**: GStreamer plugins are loaded through its plugin scanner helper again
  instead of inside the application. Initialization has passed `--gst-disable-registry-fork`
  since 2010. The helper is a separate `gst-plugin-scanner` process started with fork and
  exec; with it disabled, every plugin whose file changed since the registry cache was
  written is loaded in-process by the first GStreamer user after a package update. Loading a plugin runs its class
  registration, and that is not free: the `openni2` plugin from gst-plugins-bad calls
  `openni::OpenNI::initialize()`, whose PrimeSense driver starts a thread that waits with
  `select()`. A process holding more than 1024 descriptors — Cubase 15 does once its video
  engine runs on `nvcodec` — then dies in glibc's `FD_SET` check with
  `bit out of range 0 - FD_SETSIZE`. With the helper, the plugin loads in the scanner, a
  crash there blacklists the plugin, and the application never sees it (measured with a
  forced full rescan: media source created, two streams, where the same run aborted before)
- **windows.security.authentication.web.core**: WebAuthenticationCoreManager
  implementation, for applications that probe the WinRT web-account API on startup
- **windows.globalization**: `Windows.Globalization.Calendar` for the Gregorian calendar —
  every `ICalendar` field, `Add*` and `*AsString` method with 12- and 24-hour clocks, both
  factory interfaces and `ITimeZoneOnCalendar` with IANA time zone ids, mapped through the
  CLDR table onto the Windows time zone rules. The conformance test holds the behaviour
  measured on Windows 10: range 0001–9999 in local time, the day clamped when a month or
  year changes, daylight-saving gaps rejected by `put_Hour()` but skipped by `AddDays()`,
  and the current offset kept in a repeated hour. Other calendar systems return
  `E_NOTIMPL`, and time zone names come from the Windows time zone data instead of ICU
  ("W. Europe Daylight Time", "GMT+2"). Cubase 13 and 15 construct one in `headtracking.dll`
  at load time
- **No visual style by default (wine.inf)**: upstream activates the Light theme in every
  new prefix. This branch creates new prefixes with `ThemeActive=0`: the classic look with
  the win32u default colours (button face, menu and scroll bar 212 208 200), which is what
  Reaper's own dialogs look like on Linux. The theme stays installed and can be switched on
  in winecfg — see the note on the visual style below
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

**Visual style.** Upstream `wine.inf` activates the Light theme in a new prefix. This
branch sets `ThemeActive` to `0` instead; the `DllName`, `ColorName` and `SizeName` entries
stay, so winecfg still offers the theme under Desktop Integration. Existing prefixes are not
changed, the entries carry the no-clobber flag. The reason, seen in Reaper 7.80: with the Light
theme active, Win32 dialogs lose their 3D edges, and the first activation replaces the system
colours with the theme's (white windows, menus and tree controls, button face 245 245 245);
setting the classic colours again while the theme is active did not bring the grey dialog
background back. The *WinRT theme* selector next to it only writes `Themes\Personalize\AppsUseLightTheme`,
which `windows.ui` UISettings and uxtheme's `ShouldAppsUseDarkMode()` report to applications
that ask; the Win32 drawing reads neither, so light and dark look the same.

When switching the theme on in winecfg, two things follow. The first activation also writes
the theme's `[SysMetrics]` fonts (Tahoma 8 and 10pt) into `WindowMetrics`, so run
`wine-font-setup.sh` again afterwards to get Segoe UI 9pt back. And Ableton Live 12's dark
menu bar is only drawn with an active theme, as on Windows. Switch the theme off in winecfg,
not by deleting the registry value: only uxtheme restores the system colours and fonts it
saved when the theme was first activated.

**ntsync** (recommended, upstream Wine feature): with a kernel that provides the
`ntsync` driver (`/dev/ntsync`), make sure `/usr/include/linux/ntsync.h` exists before
running `./configure`; check with `grep HAVE_LINUX_NTSYNC_H include/config.h` afterwards.
NT synchronisation then runs in the kernel instead of through the wineserver, and the
audio thread keeps up at 64 samples / 48 kHz without xruns (measured here with Serum 2 and
FL Studio).

**DXVK**: not a hard conflict, but the two do not combine inside one application — in a
prefix DXVK replaces `dxgi.dll` and `d3d11.dll`, while this branch's composition-swapchain
and DComp popup handling live in `dxgi` (the GL present in `wined3d`). The simplest setup
is to leave DXVK out of the prefix; the package alone changes nothing, it takes effect
once `setup_dxvk.sh` has run there. If it is needed for something else on the same
machine, select per application rather than per prefix — and for a 32-bit D3D11
application that per-application route can be the cheaper one by a wide margin: vatSys
(PE32, SharpDX Direct2D on D3D11) took 24 % of a core with DXVK 3.1.1 against 113 % with
wined3d on an RTX 2000 Ada, where wined3d's command stream thread stays near one core even
after the wow64 mapping fix above (`opengl32 (WoW64)`), while the map rendered identically
and nothing in that application needs this branch's `dxgi` or `wined3d` presentation
path. The switch is
`WINEDLLOVERRIDES="d3d11,dxgi,d3d10core=n"` (the prefix copies, DXVK) against `=b` (the
builtins of this branch); a setup that survives desktop-icon launches and keeps `system32`
untouched puts DXVK's three DLLs next to the host's executable (Wine looks there first
for native DLLs), sets the same three overrides under
`HKCU\Software\Wine\AppDefaults\<host>.exe\DllOverrides`, and points
`DXVK_CONFIG_FILE` in `HKCU\Environment` at a `dxvk.conf` — DXVK reads that file from the
working directory otherwise, and hosts like REAPER change theirs before the first D3D
call. Moving the trio together is the predictable choice, since a partial override runs
one implementation's D3D11 against the other's DXGI; single overrides have worked here
(EZ Keys 2 with `d3d10core=n`, Korg Modwave and Opsix with `d3d11=n`), but that
combination is not something this branch tests. Two things not to read as "DXVK is not
active": DXVK's HUD appears only in an image DXVK presents itself, and none of the
plug-ins measured here let it — EZ Keys 2 draws through a Direct2D DC render target and a
GDI blit, Serum 2 through a DComp surface and this branch's blit, JUCE 8 plug-ins through
a composition swapchain that `dcomp` reads back — so a missing HUD says nothing, while
`WINEDEBUG=+loaddll` (the DLL next to the host as `native`) and `DXVK_LOG_LEVEL=info` do.

**DXVK and DComp plug-ins**: `DxgiFactory::CreateSwapChainForComposition` returns
`E_NOTIMPL` unless `dxgi.enableDummyCompositionSwapchain` is set, and DXVK's own
`dxvk.conf` still calls that option *not a valid implementation of DirectComposition
swapchains*. The code has moved past its own description — since the fix for DXVK issue
5053 the present path also runs for a swap chain with no window at all, which is the
composition case. What is missing on that route is the other half: something has to
composite the result, and upstream Wine's `dcomp` does not. This branch's `dcomp` does: a
swapchain that publishes no composition window of this `dxgi` is read back through public
D3D11 (buffer 0 into a staging texture, fetched per readback and never held) and composited
like a composition texture, with `WM_PAINT` passed on to the application, which draws into
its swapchain from there. Measured with the option set: a reproducer shows the swapchain
colour over the whole client area where it showed the window's own before, and a JUCE 8
test window comes out pixel-identical to the builtin run. Plug-ins that draw into a DComp
surface instead of a swapchain (Serum 2) do not need the option: their Direct2D runs on
DXVK's D3D11 and the surface reaches the window through this branch's blit as before —
measured complete and stable. What decides whether a swapchain plug-in is usable is DXVK
itself: two things in DXVK 3.1.1 get in the way, both found with an app-free reproducer
and both fixed in DXVK master the same day (issue 5919). The first `Present1` with dirty
rects after a single full `Present` composed over black — a deferred-clear regression for
shared images, `8438318`. And the back buffers stayed incomplete across `Present1` with
dirty rects, which is how JUCE 8 paints, so a readback alternated between two partial
frames (KORG Trinity flickered at 60 Hz) — `ba62c42` writes the composed frame back into
the last back buffer, as native D3D does, measured against Windows 10. With a DXVK build
from master at or past `ba62c42`, Trinity through this branch's readback is stable; with
3.1.1 the per-application switch above remains the answer for JUCE plug-ins. The readback
path costs nothing while no foreign swapchain is set as content.

**GL present for top-level windows** (default ON): D3D11 swapchains on top-level windows
present through the driver's SwapBuffers (EGL by default in Wine 11) directly from the GPU
instead of the GDI readback path — this removes a large per-frame GPU→CPU copy (order of
650 MB/s display-server traffic during continuous UI activity in Ableton Live) and fixes
main-window flicker when an app hosts WebView2 content (Ableton Live's Learn View).
`WS_CHILD` and `WS_POPUP` windows keep the GDI path, unless their flip-model or sequential
swapchain has a back buffer the GDI path cannot hand to a DC (any format other than the BGR
ones, e.g. `R8G8B8A8_UNORM`): those present through GL as well, because the GDI path drew
nothing for them — VirtualDJ 2026 showed only black windows. Adopted from shibco/ableton-linux
patch 0055 (diagnosis: ClickSentinel). If you see misplaced frames, set
`WINE_DISABLE_GL_PRESENT=1` to restore the GDI path for every window.

**One wined3d per process in dxgi**: every `CreateDXGIFactory*()` call created a new wined3d
object, and with it a full adapter initialisation — a window, a GL context, the extension
strings, the card guess. Measured with VirtualDJ 2026, which creates a factory before every
frame: 36 ms per call on llvmpipe, 60–480 ms on an NVIDIA card, and a render loop throttled
to 20 frames per second by it. The factories now share the first wined3d of the process
(0.03 ms per call); on Windows and under DXVK the call is trivial.

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
| VirtualDJ 2026 (build 9583) | Custom skin engine (D3D11, FLIP_SEQUENTIAL swapchains with an R8G8B8A8 back buffer) | Runs — decks, browser, and the skin re-lays out while the window is being resized (4–5 relayouts per second during a corner drag). Needs two patches from this branch: the wined3d GL present for flip-model swapchains without a BGR back buffer, without which both windows stay black, and the wined3d shared between all DXGI factories of the process — the application calls `CreateDXGIFactory1()` before every frame and several times per relayout, and at 60–480 ms per call on an NVIDIA card the skin followed a resize only seconds after the drag |
| Native Access 3.25.2 | Electron/Chromium (D3D11 + DComp) | Installs, signs in and installs products (verified with a Kontakt 7 update, Supercharger and the bundled NTKDaemon). Needs the `powershell` patch from this branch: without it the installer loops forever on "Native Access is running" and the bundled NTKDaemon is never installed, leaving the app stuck at "grant permission to install dependencies". Kontakt 8's own installer aborts under Wine for unrelated reasons (InstallAware) |
| Kontakt 8 Player 8.12.1 | Custom; installer: InstallAware/MSI | Installs and runs. The official installer needs the `msi` string-pool patch from this branch — without it it aborts after 77 s with "Setup has failed: FALSE", writes no installer log and leaves the product missing (WineHQ bug 59056). Both routes verified with the patch: running the setup executable directly, and a reinstall through Native Access 3; each produces the desktop shortcut and registry entries. Both the standalone and the VST3 in Reaper run: the Player browser lists its 1940 presets and instruments load and play, alongside Kontakt 6 and 7 |
| SynthEdit 1.5 | Custom engine (D2D1 + winex11 client surfaces) | Fully functional — the MDI canvas draws completely and stays stable: no permanently black regions after a menu closes, no caption flicker while resizing or moving the top-level window, and canvas children no longer vanish mid-resize. Needs the winex11 client-surface and d2d1 bitmap-brush fixes from this branch |
| EPROM - Memory Rites (VST3 in Reaper) | JUCE 8.0.13; splash/login via WebView2 | Fully functional — the splash and login window render through WebView2, and once signed in the GUI is entirely JUCE. Requires the `HideWineVersion` entry (`wine.inf` sets it for the plug-in hosts); without it JUCE 8.0.13 detects Wine, falls back from Direct2D to GDI and the plug-in comes up with a black background and missing GUI elements |
| Minimal Audio Current / Evoke / Lucid (VST3 in Reaper) | JUCE 8.0.13 | Fully functional, same `HideWineVersion` requirement as above — without it the GDI fallback leaves the background black and parts of the interface missing |
| Minimal Hub | Tauri v2 + SvelteKit + WebView2 | Starts, signs in and installs products (an 11.8 MB update completed). Needs the `secur32`/schannel `DecryptMessage` fix from this branch: without it the `oauth/token` request stops after a partial response and the app waits indefinitely, because reqwest has no response timeout. Its installer step also shells out to `powershell Start-Process`, but in a form the `powershell` patch does not recognise — it ran both with and without that patch |
| Fender Studio Pro 8 | CCL (DXGI + DWrite + DComp) | Fully functional — the song view draws completely and stays stable, no stale tool bar or transport and no flicker; the transport playhead and the selection rectangle no longer flicker while the transport runs, and video on the timeline plays, seeks, loops and jumps without stalling or going black. Starting at all needs the `UIAnimationManager2` and `UIAnimationTransitionLibrary2` implementation from this branch; without it the CCL framework aborts with "requires Windows 10 or later" |
| Steinberg Cubase Pro 15.0.30 | Custom (DComp + D2D1 + DirectWrite) + WebView2 | Installs through Steinberg's own bootstrapper and runs: project window, MixConsole with live meters, Hub. Installing needs the `msi` feature-cost fix (the setup crashed before its first dialog) and the Script SIP for signed PowerShell — without `pwrshsip`/`wintrust` the installer stops at "preinstall.ps1 … not trusted". Starting needs the `Windows.Globalization.Calendar` runtime class, without which `headtracking.dll` aborts behind the licence splash, and the `comdlg32` folder-dialog fix, without which the Hub reports the project folder as read-only. The window itself needs the dcomp virtual-surface resize and child-surface readback work — and the d2d1 WIC target fix, without which the MixConsole level meters stay empty. The video player needs the planar NV12 textures in the OpenGL renderer, the `CPU_LOCKABLE` format bit, custom Direct2D effects and the larger decoder sample pool (see the video-player entry above) |
| Steinberg Nuendo 15.0.30 | Custom (as Cubase) | The Ambisonics Mixing and Audio Post Production templates and the demo project "The Crone" open and play; the Ambisonics template brings up the MixConsole with the 3OA bus and the VST MultiPanner. The ProRes 422 demo clip (10-bit, decoded by Steinberg's own `VideoCodec-Apple-ProRes.dll`) plays in the video player, which needs the d2d1 bitmap-format table from this branch — without `R10G10B10A2_UNORM` in `IsDxgiFormatSupported` the engine never builds its 10-bit upload path and the player stays black. Needs the `ucrtbase` complex-math functions from this branch; without them the template aborts while the MultiPanner loads |
| vatSys 1.4.20 (VATSIM ATC radar client) | .NET Framework 4.8 WinForms + SharpDX Direct2D/DirectWrite; GDI+ for the application's own controls | Runs with the Australian profile: map with labels and symbols, menus and strips with the bundled Terminus (TTF) font, strip bays and VSCS, spoken ATIS with the Speech SDK 5.1 voices (`scripts/speechsdk51-setup.sh`), AFV voice. Needs from this branch: `FillOpacityMask()`, the serialised draw calls of a multi-threaded factory, the embedded-strike GDI metrics together with the GDI+ outline metrics, the compatibility layers under the full path, the lock-key toggle states, the winemenubuilder backport, the sapi default voice token, and — with DXVK per application, which takes the process from 113 % to 24 % of a core on NVIDIA — the offscreen Vulkan surfaces of Direct2D window targets and the GDI present of a target with child windows. On a Wayland desktop keep Wine on Xwayland |
| NinjaTrader 8.1.8.4 | .NET Framework 4.8 WPF (Direct3D 9); chat in WebView2 inside an `UpdateLayeredWindow()` window | Runs with native .NET Framework 4.8 (`winetricks dotnet48`; under Wine-Mono it stops at start-up, WineHQ bug 59948): logs on to the simulation data servers, charts with live data, the chat, and the built-in self-update (8.1.8.3 → 8.1.8.4). Needs from this branch: the `websocket.dll` WebSocket Protocol Component, without which the logon fails in the .NET `WebSocketProtocolComponent` type initializer; the wineserver changes that keep the client area of an `UpdateLayeredWindow()` window in its surface region and let a DirectComposition child of another process draw over it, without which the chat shows only its grey background; and the present blit no longer replayed over a layered window's content, without which the welcome window keeps an L-shaped hole at its top-left corner |
| Power BI Desktop 2.158 | .NET Framework 4.8 WinForms + WebView2 (report views, title bar) + IE web browser control (dialogs); Analysis Services engine `msmdsrv.exe` | Runs on the devel branch only: the local NTLM sign-in to the Analysis Services engine needs Wine 11.17 or later, on `d2d1-dcomp-11.0` the start ends in "Authentication failed". Also needs native .NET Framework 4.8 (`winetricks dotnet48`), Gecko for the IE control and `ntlm_auth` from Samba's winbind package (the daemon need not run). Then: the untitled report with its semantic model, the options and error dialogs, move, resize and maximize without flicker or black frames, clean exit; importing data and publishing were not tried. Needs from this branch: `NtImpersonateAnonymousToken()`, the Windows Runtime metadata files (without them the start fails on `Windows.Storage.Streams.IInputStreamReference`), the dcomp clipping of covered `WS_EX_TRANSPARENT` hosts and the hold of an absent swap-chain leaf, the wineserver fixes for the surface flush over foreign children and for the sibling list during an unlink, the winex11 capture during a window-manager move, and the IE flex minimum in mshtml. `Windows.System.Launcher` and `IStorageItemProperties` are defined in no IDL yet |

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

  The values themselves are `text_enhanced_contrast`, `text_linear_blend`,
  `text_grayscale_blend` and `text_cleartype_blend` under `HKCU\Software\Wine\Direct2D`, and
  `outline_in_natural_modes` under `HKCU\Software\Wine\DirectWrite`. Enhanced
  contrast is also in winecfg's
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

Since 2026-09-17 the branch also keeps such embedded fonts. A font that an
application registers with `AddFontMemResourceEx` is visible to GDI in that process
only; DirectWrite builds its system collection from the registry font list and
never sees it, so an application that embeds its GUI font and then asks DirectWrite
for it *by family name* gets whatever DirectWrite falls back to — JUCE 5 to 7 take
family 0 of the collection, which under Wine is the first installed font in
alphabetical order (the Kurzweil KM88 Editor rendered its whole interface in
Dorico's serif *Academico* that way). `win32u` therefore writes each memory font
to `C:\windows\fonts\wine-embedded\<hash>.<otf|ttf>` and registers it in the
`Fonts` key the way an installer would, and `dwrite` rebuilds a cached system
collection when that key changes. The first start of such an application still
shows the fallback (it asks for the font before it registers it); every later
start finds it. Font collections, bitmap fonts, PDF-style subset fonts and fonts
whose OS/2 `fsType` forbids embedding altogether are left alone;
`HKCU\Software\Wine\Fonts` → `PersistMemoryFonts` = `"0"` switches the feature
off, `"installable"` limits it to fonts whose `fsType` allows installable
embedding. `HKCU\Software\Wine\Fonts\Embedded Fonts` lists what has been kept.

The other half of that fallback are families that every Windows ships and no Wine
prefix has: KORG Legacy Cell asks DirectWrite for *Meiryo UI* for its menus and
dialogs and ended up in the same serif. DirectWrite on Windows knows no
substitution, but since 2026-09-17 this branch's `dwrite` resolves a family that
`FindFamilyName` cannot find through the GDI `FontSubstitutes` key (up to three
steps; if the chain ends at a missing family too, the system message font stands
in), and `wine.inf` and the setup script point *Meiryo UI*, *Meiryo*, *Yu Gothic
UI*, *Microsoft YaHei UI*, *Microsoft JhengHei UI* and *Malgun Gothic* at Segoe UI.
Families without an entry still report "not found", so applications that probe
for a font before bundling their own keep working. `--check` reports the entries.

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
the shader instead. `text_cleartype_blend` (see *Enhanced contrast*) reproduces what
Windows does instead and takes precedence for text in a solid colour; the linear blend
then only reaches gradient and bitmap brushes.

### Enhanced contrast — worth setting on a dark interface

DirectWrite reports an *enhanced contrast* value alongside gamma and ClearType
level; Wine hardcodes it to zero, so nothing is applied. This branch honours it and
lets the prefix override it — in winecfg (*Graphics* tab, *Direct2D text*, three named
settings) or by hand:

```bash
wine reg add 'HKCU\Software\Wine\Direct2D' /v text_enhanced_contrast /t REG_DWORD /d 70 /f
wineserver -k    # the value is read when d2d1 loads, so restart the application
```

The value is in hundredths; deleting it restores the default. On its own it raises
partial coverage while leaving fully covered and empty samples alone, so at 0 — the
default — rendering is bit-identical to not having the feature at all. `AppDefaults`
works as for the other keys, so it can be set for a single application. With
`text_cleartype_blend` (below) the value feeds DirectWrite's own curve instead, where
it only reaches dark text.

| value | when |
|---|---|
| `0` | default. Clean as it stands; leave it unless you have a reason |
| `50` | what Windows runs. A safe middle if you want a little more weight |
| `70` | dark interfaces with dense body text — light text on dark looks thinner than it geometrically is, and this puts the weight back |

On a **light** background the effect works against you — there the same setting makes
text look heavy. Watch small sizes if you go high: at 11px a strong setting can start
closing the counters in `e`, `a` and `g`.

All of the above reaches ClearType text only. Direct2D draws ClearType on opaque
targets alone, so text on a target with an alpha channel is greyscale, and that covers
whole interfaces: Cubase 15's Hub draws everything into premultiplied DirectComposition
surfaces, and its light-on-dark sidebar looked thin however the ClearType settings
were tuned. Wine handed greyscale coverage through unchanged; DirectWrite does not. It
applies its greyscale enhanced contrast only to dark text (scaled down to nothing as the
text colour gets light) and then corrects the coverage for the gamma of a blend in
encoded space, which gives light text weight and takes most of the contrast back from
dark text. The branch does the same when asked to; `scripts/wine-font-setup.sh` sets it
with the other text switches:

```bash
wine reg add 'HKCU\Software\Wine\Direct2D' /v text_grayscale_blend /t REG_DWORD /d 1 /f
# or for one application only:
wine reg add 'HKCU\Software\Wine\AppDefaults\Cubase15.exe\Direct2D' /v text_grayscale_blend /t REG_DWORD /d 1 /f
```

Gamma and greyscale contrast come from the DirectWrite rendering parameters, as on
Windows; text in a gradient or bitmap brush keeps the plain coverage. Measured against
Windows 10 with Segoe UI at 12 px on a premultiplied target, as extra ink over the bare
coverage: light-on-dark text +12.8 % on Windows and +13.5 % here, dark-on-light +2.7 %
and +3.9 %, black on white -2.7 % and -2.3 % — with Windows' parameters (gamma 1.8). Wine's
dwrite reports gamma 2.0, which makes light text about three points heavier than that.
Bold and larger text stay a few points above Windows, from FreeType's coverage rather
than the blend. With the value unset, greyscale text is bit-identical. An earlier
`text_grayscale_enhanced_contrast` applied the ClearType curve above to every greyscale
pixel regardless of colour; it made dark text on light backgrounds blotchy and was
replaced by this.

ClearType text gets the same treatment on request. DirectWrite scales the enhanced
contrast for the text colour as above, applies it per subpixel channel, corrects each
channel for the gamma of an encoded-space blend with that channel of the text colour as
its intensity, and blends in encoded space. The branch's own curve ignored the colour,
and `text_linear_blend` on top of it corrected the gamma a second time: with Windows'
parameters black on white lost 22 % of its ink against 10 % on Windows, and without
the contrast 35 %.

```bash
wine reg add 'HKCU\Software\Wine\Direct2D' /v text_cleartype_blend /t REG_DWORD /d 1 /f
wine reg add 'HKCU\Software\Wine\Direct2D' /v text_enhanced_contrast /t REG_DWORD /d 50 /f
```

The contrast belongs with it: Wine's dwrite reports 0 where Windows reports 0.5, and
at 0 the blend takes 23 % of the ink from black text on white (Windows: 10 %). With
both set, Segoe UI at 12 px on an opaque target measures against Windows 10 as
light-on-dark +17.0 % here and +11.5 % there, black on white -11.5 % and -9.7 %; the
light-on-dark rest comes from FreeType spreading subpixel coverage over more partial
pixels than Windows' rasteriser, not from the blend. For a run in a solid colour this
takes precedence over `text_linear_blend`; gradient and bitmap brushes keep the
previous path. Unset, ClearType text is bit-identical.

Unrelated to the above, FL Studio's Piano Roll needs one more font fix to show
flat/sharp symbols (♭ ♯) instead of tofu boxes — FL bypasses Wine's font
fallback through `GetGlyphIndices`. That one has its own project:
[giang17/flstudio-wine-font-fix](https://github.com/giang17/flstudio-wine-font-fix).

## Steinberg installers: PowerShell hosting in Wine-Mono

The complete setup in a new prefix, this step included, is in
[documentation/steinberg-setup-guide.md](documentation/steinberg-setup-guide.md).

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
No bundled successor series is planned, and no fixes are submitted upstream while Wine's
Clean-Room Guidelines exclude LLM-generated code; the fork itself stays a rolling
devel-tracking distribution.

## Related

- Discussion: [yabridge#413](https://github.com/robbert-vdh/yabridge/issues/413)
- Upstream: [wine-mirror/wine](https://github.com/wine-mirror/wine)
- Subpixel text: [shibco/ableton-linux](https://github.com/shibco/ableton-linux),
  pull request 155 by Cade (@shibco), who wrote the nine patches and wants to take
  them to WineHQ. Reviewed there by @ClickSentinel, whose measurements are worth
  reading before touching this code

## License

Same as Wine -- GNU LGPL. See [LICENSE](LICENSE) for details.
