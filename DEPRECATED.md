# Branch Status: DEPRECATED (2026-05-03)

This branch (`d2d1-v6`) is **deprecated** and no longer maintained.

## Last update

`4c3c8a3ca7c` — *d2d1: Premultiply alpha in gradient stop colors for correct blending*  
2026-02-14

## What replaces this branch

For active D2D1 development, use **[`d2d1-dcomp-11.0`](https://github.com/giang17/wine/tree/d2d1-dcomp-11.0)** (full stack: D2D1 + DComp + DWrite + WineD3D + winex11).

For Wine 11.x devel-tracking distros (`wine-tkg-dev` etc.), use the frozen **[`d2d1-dcomp-11.8`](https://github.com/giang17/wine/tree/d2d1-dcomp-11.8)** snapshot until Wine 12.0 stable lands.

## Why this is deprecated

Since 2026-02-14, **41+ further D2D1 commits** have landed in `d2d1-dcomp-11.0`:

- PushLayer stencil clipping (D3D11 stencil-based clip rendering)
- ImageBrush UV fix (atlas-bleed in 9-slice tooltips)
- Scratch buffer reuse (98.6% fewer VB/IB allocations)
- intersect_self spatial grid (98.7% segment-pair reduction)
- FillRectangle scratch geometry cache
- Sivov's Stream-Path architecture (cherry-picked from Wine 11.x devel)
- Sivov's Bezier-Arc approximation (replaces our SVG-F.6 implementation)
- 15 misc upstream bug fixes pre-baked from Wine 11.0..11.8

`d2d1-v6` no longer reflects the current state of the D2D1 work, and notably
predates the Stream-Path / Bezier-Arc architectural switch.

## Replacement plan for upstream submission

After the Wine 12.0 stable rebase (~mid-May 2026), a new clean D2D1-only
series (working name `d2d1-v7`) will be extracted from `d2d1-dcomp-12.0`.
That series will be the upstream-submission candidate, sitting on top of
Sivov's Stream-Path architecture and incorporating all current fixes.

The frozen patch ZIPs in `patches/v6-full/` and `patches/v6-full-11.0/`
on the main fork branches are kept for historical reference.

## License

Same as Wine — GNU LGPL. See [LICENSE](LICENSE) for details.
