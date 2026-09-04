/*
 * The frame-layer contract between dcomp and wined3d.
 *
 * Copyright 2026 Giang Nguyen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_DCOMP_LAYER_H
#define __WINE_DCOMP_LAYER_H

/* A DirectComposition tree that covers only a sliver of its window does not
 * take the window over: it delivers just the rectangles it covers and leaves
 * the message loop with the application.  Those pixels used to go to the
 * window directly, after the present that produced the frame -- two writers
 * for one window with no ordering between them, so every swap landing between
 * two deliveries showed a window without them.
 *
 * Instead the tree publishes its leaves as a layer with alpha and wined3d
 * draws them into the frame it is about to present.  One writer, no race.
 * This is the same route DirectComposition content above the coverage
 * threshold already takes, and on Windows it is what the DWM does for all of
 * it.
 *
 * dcomp owns the layer, wined3d only ever reads it.  Two properties carry it:
 *
 *   WINE_DCOMP_LAYER_PROP       on the target window, set by dcomp.  Points at
 *                               a struct wine_dcomp_layer.  Never removed and
 *                               never freed while the window lives -- see the
 *                               lifetime note below.
 *   WINE_DCOMP_LAYER_SINK_PROP  on the same window, maintained by wined3d as a
 *                               count of swapchains able to draw the layer.
 *                               dcomp publishes a layer only while it is set,
 *                               and keeps blitting to the window otherwise --
 *                               a window nobody composites for must not lose
 *                               its content.
 *
 * Locking.  dcomp composites into a private back buffer, then takes the lock
 * exclusively just long enough to swap in the pointer and the geometry; the
 * exclusive acquire also waits out any reader still holding the buffer that is
 * about to become the next back buffer, which is what makes recycling it safe.
 * wined3d holds the lock shared while it reads bits and uploads them.  Neither
 * side ever blocks the other for longer than one upload, and in particular the
 * present path never waits for a composite.
 *
 * Liveness.  A sink that never actually draws would be worse than no fix at
 * all: the leaves would not be missing from half the frames but from all of
 * them.  And a sink can be wrong -- measured on Fender Studio Pro 8, three of
 * its seven swapchains never present so much as one frame in a whole session.
 * So the compositor counts every layer it draws and dcomp checks that the
 * count moves; if it does not, dcomp goes back to blitting on its own.  That
 * makes the sink an arming signal rather than a promise, and no bookkeeping
 * error on either side can cost a window its content.
 *
 * Lifetime.  A reader looks the property up and then takes the lock, so the
 * structure must not go away in between.  It therefore outlives the target:
 * dcomp frees the pixel buffers under the exclusive lock and leaves bits NULL,
 * but the structure itself stays for as long as the window does (a later
 * target on the same window picks it up again through the property).  That
 * leaks one structure per window that ever hosted a target -- a few hundred
 * bytes, bounded by the number of such windows, and the price of a reader that
 * never has to take a heavier lock than a property lookup.
 *
 * Layout drift.  The structure crosses a DLL boundary with no link between the
 * halves: dcomp and wined3d each carry their own build of this header, and a
 * half-deployed pair reads pointers and dimensions through the wrong layout --
 * not a load error, not a log line, but an access to memory the layer does not
 * own.  Two defenses, both checked by every reader:
 *
 *   - magic and size lead the structure.  A reader that finds anything else
 *     reports the mismatch and ignores the layer; dcomp, which owns the
 *     property, replaces the rejected structure with a fresh one.
 *   - The property name carries the layout generation (the "2" below).  A
 *     reader built against an older layout looks up the old name, finds
 *     nothing, and simply never composites the layer -- the drawn-liveness in
 *     dcomp then falls back to blitting on its own.  Bump the name together
 *     with any change to this structure.
 */

#define WINE_DCOMP_LAYER_PROP       L"__wine_dcomp_layer2"
#define WINE_DCOMP_LAYER_SINK_PROP  L"__wine_dcomp_layer_sink"

#define WINE_DCOMP_LAYER_MAGIC      0x4c434457 /* "WDCL" */

/* The commit generation of the __wine_dcomp_child_* leaf properties, on the
 * target window.  dcomp holds it odd while it rewrites the set and advances it
 * to the next even value after __wine_dcomp_child_count; wined3d reads it
 * before and after consuming the set.  An odd or a moved value marks a torn
 * read, a missing one under a non-zero child count marks a dcomp half that
 * predates this protocol -- both worth a log line, neither a reason to change
 * behavior. */
#define WINE_DCOMP_CHILD_GEN_PROP   L"__wine_dcomp_child_gen"

/* Rectangles carried per frame before the region is given up on and its
 * bounding box published instead.  A tree of a transport playhead and a
 * dragged selection is two rectangles a thousand pixels apart, and their
 * bounding box is most of the window -- which the compositor would then upload
 * and draw in full, every present.  Eight is far more than the trees this path
 * sees (measured: one to three) and keeps the fallback out of reach. */
#define WINE_DCOMP_LAYER_MAX_RECTS 8

struct wine_dcomp_layer
{
    DWORD magic;                /* WINE_DCOMP_LAYER_MAGIC */
    DWORD size;                 /* sizeof(struct wine_dcomp_layer): the layout fingerprint */
    SRWLOCK lock;
    /* All of the following are valid only while the lock is held. */
    DWORD *bits;                /* premultiplied BGRA, 0 = fully transparent, NULL = nothing published */
    unsigned int width, height; /* extent of bits, in client pixels */
    RECT box;                   /* bounding box of rects; empty means nothing is published */
    unsigned int rect_count;
    RECT rects[WINE_DCOMP_LAYER_MAX_RECTS];   /* the parts of bits that carry this frame's leaves */
    /* Outside the lock, atomic: incremented by the compositor for every layer
     * it draws, read by dcomp to tell a sink that draws from one that does not. */
    LONG drawn;
};

#endif /* __WINE_DCOMP_LAYER_H */
