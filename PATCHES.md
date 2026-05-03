# Wine D2D1 / DComp Patches

This fork contains patches for Wine's Direct2D (d2d1), DirectComposition (dcomp),
DirectWrite (dwrite), and related subsystems. The patches fix rendering and
performance issues that prevented modern JUCE 8 and VSTGUI-based Windows
applications from running correctly under Wine — particularly audio plugins
(Serum2, Korg Trinity, Korg Prophecy, Pianoteq 9) in DAWs like Reaper.

These newer plugin frameworks rely on DirectComposition for window management
and D2D1 for GPU-accelerated 2D rendering — APIs that were largely unimplemented
in Wine. With these patches, plugins using DComp + D2D1 now render correctly
and run stable in production use.

**Base**: Wine 11.8 devel (`wine-11.8` tag) — this branch.

## Branches

| Branch | Description |
|--------|-------------|
| `d2d1-dcomp-11.0` | **Recommended for stable users.** Full stack on Wine 11.0 stable. Actively maintained. |
| `d2d1-dcomp-11.8` | **Frozen.** Snapshot port of the full stack onto Wine 11.8 devel, for `wine-tkg-dev` users. **No backports** of bug fixes. Use until `d2d1-dcomp-12.0` lands (~mid-May 2026). Compiles clean against `wine-11.8`, not full plugin-tested by maintainer. |
| `d2d1-v6` | 15 D2D1 rendering patches only (upstream reference, see below) |

## Full Stack (Branch: `d2d1-dcomp-11.0`)

This is the recommended branch. It includes all 15 D2D1 patches plus:

- **DComp**: IDCompositionDesktopDevice implementation with D2D1 bitmap rendering path,
  dirty-rect clipping, DIB+BitBlt presentation (9 phases)
- **DWrite**: Rendering mode 5 fix, IDWriteFontSet::GetMatchingFonts implementation,
  font fallback mapping for Miscellaneous Symbols and Arrows (U+2B00-2BFF, fixes star
  rating display in Serum2)
- **DXGI**: Composition swapchain, FLIP_SEQUENTIAL preservation, DComp popup handling,
  micro-resize for stale UI
- **WineD3D**: Composition buffer with dirty rect accumulation, GL buffer recycling pool
  (70% RSS reduction), scratch buffer reuse (98.6% fewer allocs), constant buffer
  dirty-check (23% fewer DISCARDs), private heap to isolate alloc churn
- **ntdll**: MADV_FREE for MEM_RESET (improved page reclaim behavior)
- **D2D1 private heap**: Isolates high-frequency geometry alloc/free from process heap
- **winex11**: DComp window support, backing store, micro-resize suppression
- **shell32**: VirtualDesktopManager COM stub

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

**DXVK compatibility**: Do **not** install DXVK alongside this patch set. The DXGI
patches (DComp popup handling, GL SwapBuffers) modify Wine's builtin `dxgi.dll`.
DXVK replaces `dxgi.dll` with its own implementation, which discards these patches.
Furthermore, DXVK's `d3d11.dll` and Wine's `dxgi.dll` are **not interchangeable** —
they use different internal COM interfaces (`DxgiSwap*` / `D3D11DXGI*` vs
`IWineDXGI*`) and mixing them causes crashes. With the DComp rendering path active,
DXVK offers no benefit anyway — D2D1 draws go through a bitmap+BitBlt path, not the
DXGI swapchain, so WineD3D performs equally well.

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

## Tested Applications

| Application | Framework | Status |
|-------------|-----------|--------|
| Serum2 (VST3 in Reaper) | VSTGUI + DComp | Fully functional, all waveform views + envelopes + presets |
| Korg Trinity (VST3 in Reaper) | JUCE 8 + DComp | Fully functional, DComp rendering path |
| Korg Prophecy (VST3 in Reaper) | JUCE 8 + DComp | Fully functional |
| Pianoteq 9 (standalone + VST3) | JUCE 8 + DComp | Fully functional |
| FL Studio (Wine + ntsync) | Custom | Runs without xruns at 64 samples / 48 kHz |
| WineSynth (custom VSTGUI plugin) | VSTGUI + DComp | 18k+ partial redraws without crash |

## Font Setup

The DWrite patches fix font rendering in VSTGUI-based plugins, but some applications
also require system fonts to be configured correctly (Unicode symbols, GDI menus).

See **[documentation/wine-font-setup-guide.md](documentation/wine-font-setup-guide.md)**
for a step-by-step guide covering:

- Installing `NotoSansSymbols2` and `DejaVuSans` into the Wine prefix
- Setting GDI `FontLink` registry entries for correct menu symbol rendering
- Working around the missing `BitPDisp-10` tooltip font in Serum2

## D2D1 Patches Only (Branch: `d2d1-v6`)

This branch contains only the 15 D2D1 rendering patches without DComp, DWrite, or
performance fixes. It is maintained as a clean upstream reference for potential
submission to Wine upstream. **For normal use, prefer `d2d1-dcomp-11.0` above.**

15 patches fixing core D2D1 rendering issues:

1. **AddArc implementation** -- path geometry arc segments
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

## License

Same as Wine -- GNU LGPL. See [LICENSE](LICENSE) for details.
