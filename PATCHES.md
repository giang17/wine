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

**Base**: Wine 11.0 stable (`wine-11.0` tag) — this branch.

## Branches

| Branch | Description |
|--------|-------------|
| `d2d1-dcomp-11.0` | **Recommended for stable users.** Full stack on Wine 11.0 stable: D2D1 + DComp + DWrite + WineD3D performance + winex11. Actively maintained. This branch. |
| `d2d1-dcomp-11.11` | **Recommended for devel / rolling-release users** (e.g. `wine-tkg-dev`). Newest full stack rebased onto Wine 11.11 devel — the current rolling base. Same D2D1 + DComp + DWrite + WineD3D + winex11 stack as `d2d1-dcomp-11.0`, including the Tier 1-3 review hardening, the Trinity nested-popup z-order fix, the embedded-Trinity in-plugin (IFX) popup flicker fix, the DComp thread-safety hardening, and the WineD3D process-heap fix for the Trinity heap-mismatch crash. Plugin-tested by maintainer (Serum 2, Korg Trinity, Pianoteq 9, WineSynth). |
| `d2d1-dcomp-11.10` | Same full stack rebased onto Wine 11.10 devel. Plugin-tested by maintainer (Serum 2, Korg Trinity, Pianoteq 9, WineASIO). **Superseded by `d2d1-dcomp-11.11`** as the rolling devel base — kept as the previous rolling snapshot. |
| `d2d1-dcomp-11.8` | **Frozen** snapshot port onto Wine 11.8 devel. **Superseded by `d2d1-dcomp-11.11`** — kept for historical reference. Compiles clean against `wine-11.8`, not full plugin-tested by maintainer. |
| `d2d1-v6` | **Deprecated** (last update 2026-02-14). Was the upstream-targeted D2D1-only patch series; superseded by 41+ later commits in `d2d1-dcomp-11.0`. Kept for historical reference. |

> **Note on Wine versioning**: WineHQ devel releases run `11.8 → 11.9 → 11.10 → …`
> every two weeks; `.10` is the tenth devel iteration, not `1.0`. Stable `12.0`
> arrives on the yearly cadence (≈ January 2027), not mid-2026. This fork tracks
> devel releases on a rolling basis (currently 11.11) rather than waiting for 12.0.

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

## License

Same as Wine -- GNU LGPL. See [LICENSE](LICENSE) for details.
