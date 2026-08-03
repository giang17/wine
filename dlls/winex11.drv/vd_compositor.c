/*
 * Virtual-desktop per-pixel-alpha mini-compositor (issue 64, phase 1 POC).
 *
 * In Wine's "emulate a virtual desktop" mode every Wine top-level window
 * becomes a direct override-redirect child of the VD root (an RGB24 window
 * that is itself a child of the real root).  The real host compositor only
 * composites true top-level windows of the real root, so the VD root's
 * children are never per-pixel-alpha blended: the X server blits them
 * non-composited onto the RGB24 VD root, and any ARGB32 (0,0,0,0) pixel
 * collapses to opaque RGB (0,0,0) -- a hard black box where a translucent
 * shadow/border should be.
 *
 * Making the VD root itself ARGB32 does NOT work: it breaks Wine's XRender
 * pipeline (RenderCreatePicture BadMatch, window mapping stalls).  Instead
 * this compositor keeps the VD root RGB24, redirects its subwindows manually
 * (XCompositeRedirectSubwindows Manual) and composites them with per-pixel
 * alpha onto a SEPARATE ARGB32 overlay window that is a true top-level child
 * of the real root -- so the host compositor blends the overlay with
 * per-pixel alpha.  The overlay covers the VD root geometrically and lets
 * input pass through (empty XInputShape) so the VD stays usable.
 *
 * Phase 1 is brute-force: the whole child set is recomposited on every
 * relevant event, with no XDamage tracking (phase 2).
 *
 * Known limitation: Wine creates several transient VD roots during startup
 * (explorer/wineboot phases); this module tracks only the most recently
 * initialised one via a single global state.  Per-root state and damage-driven
 * incremental updates are phase 2/3.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <X11/Xlib.h>

#include "x11drv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

#if defined(SONAME_LIBXCOMPOSITE) && defined(SONAME_LIBXRENDER)

#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>
#ifdef SONAME_LIBXEXT
#include <X11/extensions/shape.h>
#endif

#define MAKE_FUNCPTR(f) static typeof(f) * pvd_##f;
MAKE_FUNCPTR(XCompositeQueryExtension)
MAKE_FUNCPTR(XCompositeRedirectSubwindows)
MAKE_FUNCPTR(XCompositeNameWindowPixmap)
MAKE_FUNCPTR(XRenderQueryExtension)
MAKE_FUNCPTR(XRenderFindVisualFormat)
MAKE_FUNCPTR(XRenderCreatePicture)
MAKE_FUNCPTR(XRenderFreePicture)
MAKE_FUNCPTR(XRenderFillRectangle)
MAKE_FUNCPTR(XRenderComposite)
#undef MAKE_FUNCPTR
#ifdef SONAME_LIBXEXT
static typeof(XShapeCombineRectangles) * pvd_XShapeCombineRectangles;
#endif

static struct
{
    BOOL active;
    Window vd_root;       /* the RGB24 virtual-desktop root */
    Window overlay;       /* ARGB32 overlay window covering the VD root */
    Picture overlay_pict; /* ARGB32 picture over the overlay window */
    BOOL dirty;           /* a recomposit of the whole child set is pending */
} vd_comp;

static Bool load_functions(void)
{
    void *xc = dlopen( SONAME_LIBXCOMPOSITE, RTLD_NOW );
    void *xr = dlopen( SONAME_LIBXRENDER,    RTLD_NOW );
    if (!xc || !xr) { WARN( "unable to load Xcomposite/Xrender\n" ); return FALSE; }

#define LOAD(h, f) do { if (!(pvd_##f = dlsym( h, #f ))) { WARN( "missing %s\n", #f ); return FALSE; } } while (0)
    LOAD( xc, XCompositeQueryExtension );
    LOAD( xc, XCompositeRedirectSubwindows );
    LOAD( xc, XCompositeNameWindowPixmap );
    LOAD( xr, XRenderQueryExtension );
    LOAD( xr, XRenderFindVisualFormat );
    LOAD( xr, XRenderCreatePicture );
    LOAD( xr, XRenderFreePicture );
    LOAD( xr, XRenderFillRectangle );
    LOAD( xr, XRenderComposite );
#undef LOAD
#ifdef SONAME_LIBXEXT
    /* XShape is optional -- only used for input pass-through. */
    {
        void *xext = dlopen( SONAME_LIBXEXT, RTLD_NOW );
        if (xext) pvd_XShapeCombineRectangles = dlsym( xext, "XShapeCombineRectangles" );
    }
#endif
    return TRUE;
}

/* Composite one child onto the overlay at its current geometry.  ARGB32
 * children blend with their alpha (PictOpOver), opaque children just paint. */
static void composite_child( Window child )
{
    XWindowAttributes attr;
    XRenderPictFormat *fmt;
    Pixmap pixmap;
    Picture pict;

    if (!XGetWindowAttributes( gdi_display, child, &attr )) return;
    if (attr.class == InputOnly) return;
    if (attr.map_state != IsViewable) return;

    fmt = pvd_XRenderFindVisualFormat( gdi_display, attr.visual );
    if (!fmt) return;

    pixmap = pvd_XCompositeNameWindowPixmap( gdi_display, child );
    if (!pixmap) return;

    pict = pvd_XRenderCreatePicture( gdi_display, pixmap, fmt, 0, NULL );
    if (pict)
    {
        /* child geometry is relative to the VD root; the overlay shares the
         * VD root's geometry, so the same coordinates place it correctly. */
        pvd_XRenderComposite( gdi_display, PictOpOver, pict, None, vd_comp.overlay_pict,
                              0, 0, 0, 0, attr.x, attr.y, attr.width, attr.height );
        pvd_XRenderFreePicture( gdi_display, pict );
    }
    XFreePixmap( gdi_display, pixmap );
}

void vd_compositor_paint( void )
{
    Window root, parent, *children;
    unsigned int count, i;
    XWindowAttributes attr;
    XRenderColor clear = { 0, 0, 0, 0 };

    if (!vd_comp.active || !vd_comp.dirty) return;
    vd_comp.dirty = FALSE;

    if (!XGetWindowAttributes( gdi_display, vd_comp.overlay, &attr )) return;

    /* clear the overlay to fully transparent so each frame is rendered from
     * scratch -- no ghosting from moved or removed children. */
    pvd_XRenderFillRectangle( gdi_display, PictOpSrc, vd_comp.overlay_pict, &clear,
                              0, 0, attr.width, attr.height );

    if (!XQueryTree( gdi_display, vd_comp.vd_root, &root, &parent, &children, &count ))
        return;
    /* XQueryTree returns children in bottom-to-top stacking order; compositing
     * them in that order with PictOpOver preserves the Z order. */
    for (i = 0; i < count; i++) composite_child( children[i] );
    if (children) XFree( children );

    XFlush( gdi_display );
}

void vd_compositor_notify( XEvent *event )
{
    if (!vd_comp.active) return;

    switch (event->type)
    {
    /* anything that can change the pixels or stacking of a redirected child,
     * plus NoExpose/GraphicsExpose for the XPutImage/XCopyArea that draw child
     * content.  paint() is deduped (runs at most once per ProcessEvents call). */
    case Expose:
    case GraphicsExpose:
    case NoExpose:
    case CreateNotify:
    case MapNotify:
    case UnmapNotify:
    case ConfigureNotify:
    case ReparentNotify:
    case DestroyNotify:
    case VisibilityNotify:
        vd_comp.dirty = TRUE;
        break;
    default:
        break;
    }
}

static Bool create_overlay( void )
{
    XSetWindowAttributes attr;
    XWindowAttributes ra;
    XRenderPictFormat *fmt;
    Colormap cmap;
    unsigned long mask;

    if (!argb_visual.visual) return FALSE;
    if (!XGetWindowAttributes( gdi_display, vd_comp.vd_root, &ra )) return FALSE;

    cmap = XCreateColormap( gdi_display, DefaultRootWindow(gdi_display), argb_visual.visual, AllocNone );
    attr.override_redirect = True;
    attr.background_pixel = 0;
    attr.border_pixel = 0;
    attr.colormap = cmap;
    attr.event_mask = ExposureMask;
    attr.save_under = True;
    mask = CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask | CWSaveUnder;

    vd_comp.overlay = XCreateWindow( gdi_display, DefaultRootWindow(gdi_display),
                                     ra.x, ra.y, ra.width, ra.height, 0, 32, InputOutput,
                                     argb_visual.visual, mask, &attr );
    if (!vd_comp.overlay) return FALSE;

    fmt = pvd_XRenderFindVisualFormat( gdi_display, argb_visual.visual );
    if (!fmt) return FALSE;
    vd_comp.overlay_pict = pvd_XRenderCreatePicture( gdi_display, vd_comp.overlay, fmt, 0, NULL );
    if (!vd_comp.overlay_pict) return FALSE;

    XMapWindow( gdi_display, vd_comp.overlay );
    XRaiseWindow( gdi_display, vd_comp.overlay );

#ifdef SONAME_LIBXEXT
    /* empty input shape: the overlay never absorbs pointer/keyboard input,
     * everything falls through to the VD root beneath it. */
    if (pvd_XShapeCombineRectangles)
        pvd_XShapeCombineRectangles( gdi_display, vd_comp.overlay, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted );
#endif
    return TRUE;
}

void vd_compositor_init( Window vd_root )
{
    int ev, er;

    vd_comp.vd_root = vd_root;
    vd_comp.active = FALSE;
    vd_comp.dirty = FALSE;

    if (!is_virtual_desktop()) return;
    if (!usexcomposite) return;
    if (!load_functions()) return;
    if (!pvd_XCompositeQueryExtension( gdi_display, &ev, &er )) return;
    if (!pvd_XRenderQueryExtension( gdi_display, &ev, &er )) return;

    /* manual subwindow redirect: the X server stops drawing the VD root's
     * children itself and gives us redirected backing pixmaps. */
    pvd_XCompositeRedirectSubwindows( gdi_display, vd_root, CompositeRedirectManual );

    if (!create_overlay()) { WARN( "VD compositor: overlay creation failed\n"); return; }

    vd_comp.active = TRUE;
    vd_comp.dirty = TRUE;
    TRACE( "VD mini-compositor active, overlay %lx over VD root %lx\n", vd_comp.overlay, vd_root );
}

#else  /* !(SONAME_LIBXCOMPOSITE && SONAME_LIBXRENDER) */

void vd_compositor_init( Window vd_root ) { /* no composite/render support built in */ }
void vd_compositor_notify( XEvent *event ) { }
void vd_compositor_paint( void ) { }

#endif
