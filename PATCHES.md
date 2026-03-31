# Wine D2D1 / DComp Patches

This fork contains patches for Wine's Direct2D (d2d1), DirectComposition (dcomp),
DirectWrite (dwrite), and related subsystems. The patches fix rendering issues in
Windows applications that rely heavily on D2D1, such as audio plugins (Serum2,
Korg Trinity, Korg Prophecy) running in DAWs (Reaper) via Wine.

**Base**: Wine 11.0 stable (`wine-11.0` tag)

## Branches

| Branch | Description |
|--------|-------------|
| `d2d1-v6` | 15 D2D1 rendering patches (standalone, upstream-suitable) |
| `d2d1-dcomp-11.0` | Full stack: D2D1 + DComp + DWrite + WineD3D performance + winex11 |

## D2D1 Patches (Branch: `d2d1-v6`)

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

### Applying

```bash
git clone https://github.com/giang17/wine.git
cd wine
git checkout d2d1-v6
./configure --prefix=/usr/local --enable-win64
make -j$(nproc)
sudo make install
```

## Full Stack (Branch: `d2d1-dcomp-11.0`)

Everything in `d2d1-v6` plus:

- **DComp**: IDCompositionDesktopDevice implementation with D2D1 bitmap rendering path,
  dirty-rect clipping, DIB+BitBlt presentation (9 phases)
- **DWrite**: Rendering mode 5 fix, IDWriteFontSet::GetMatchingFonts implementation
- **DXGI**: Composition swapchain, FLIP_SEQUENTIAL preservation, DComp popup handling,
  micro-resize for stale UI
- **WineD3D**: Composition buffer with dirty rect accumulation, GL buffer recycling pool
  (70% RSS reduction), scratch buffer reuse (98.6% fewer allocs), constant buffer
  dirty-check (23% fewer DISCARDs)
- **winex11**: DComp window support, backing store, micro-resize suppression
- **shell32**: VirtualDesktopManager COM stub

## Tested Applications

| Application | Status |
|-------------|--------|
| Serum2 (VST3 in Reaper) | Fully functional, all waveform views + envelopes + presets |
| Korg Trinity (VST3 in Reaper) | Fully functional, DComp rendering path |
| Korg Prophecy (VST3 in Reaper) | Fully functional |
| WineSynth (custom VSTGUI plugin) | 18k+ partial redraws without crash |

## Related

- Discussion: [yabridge#413](https://github.com/robbert-vdh/yabridge/issues/413)
- Upstream: [wine-mirror/wine](https://github.com/wine-mirror/wine)

## License

Same as Wine -- GNU LGPL. See [LICENSE](LICENSE) for details.
