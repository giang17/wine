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
 * Multi-root: Wine creates several VD roots during startup (transient
 * explorer/wineboot phases plus the final desktop root).  Each gets its own
 * redirect + overlay tracked in a per-root slot; init is idempotent (the same
 * root re-initialised across phases does not leak a second overlay), and
 * paint() self-heals slots whose VD root has been destroyed.
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

#define MAX_VD_ROOTS 8  /* Wine creates a handful of transient VD roots during setup */

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

struct vd_root_state
{
    BOOL active;          /* this slot is compositing a live VD root */
    Window vd_root;       /* the RGB24 virtual-desktop root */
    Window overlay;       /* ARGB32 overlay window covering the VD root */
    Picture overlay_pict; /* ARGB32 picture over the overlay window */
    BOOL dirty;           /* a recomposit of this root's child set is pending */
};

static struct vd_root_state vd_roots[MAX_VD_ROOTS];
static BOOL vd_ready;     /* one-time: extensions loaded + queried + VD mode active */

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

/* One-time readiness: VD mode + extensions loaded + queried.  Cheap after the
 * first call (cached in vd_ready). */
static Bool vd_ensure_ready(void)
{
    int ev, er;
    if (vd_ready) return TRUE;
    if (!is_virtual_desktop()) return FALSE;
    if (!usexcomposite) return FALSE;
    if (!load_functions()) return FALSE;
    if (!pvd_XCompositeQueryExtension( gdi_display, &ev, &er )) return FALSE;
    if (!pvd_XRenderQueryExtension( gdi_display, &ev, &er )) return FALSE;
    vd_ready = TRUE;
    return TRUE;
}

static struct vd_root_state *vd_find_root( Window vd_root )
{
    int i;
    for (i = 0; i < MAX_VD_ROOTS; i++)
        if (vd_roots[i].active && vd_roots[i].vd_root == vd_root) return &vd_roots[i];
    return NULL;
}

static struct vd_root_state *vd_alloc_root( void )
{
    int i;
    for (i = 0; i < MAX_VD_ROOTS; i++)
        if (!vd_roots[i].active) return &vd_roots[i];
    return NULL;
}

/* Free a slot's overlay resources and mark it reusable.  Used both on fatal
 * init failure and on self-heal when the VD root disappears. */
static void vd_teardown_root( struct vd_root_state *r )
{
    if (r->overlay_pict) pvd_XRenderFreePicture( gdi_display, r->overlay_pict );
    if (r->overlay) XDestroyWindow( gdi_display, r->overlay );
    r->active = FALSE;
    r->vd_root = 0;
    r->overlay = 0;
    r->overlay_pict = 0;
    r->dirty = FALSE;
}

/* Composite one child onto the overlay at its current geometry.  ARGB32
 * children blend with their alpha (PictOpOver), opaque children just paint. */
static void composite_child( struct vd_root_state *r, Window child )
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
        pvd_XRenderComposite( gdi_display, PictOpOver, pict, None, r->overlay_pict,
                              0, 0, 0, 0, attr.x, attr.y, attr.width, attr.height );
        pvd_XRenderFreePicture( gdi_display, pict );
    }
    XFreePixmap( gdi_display, pixmap );
}

void vd_compositor_paint( void )
{
    int i;
    if (!vd_ready) return;

    for (i = 0; i < MAX_VD_ROOTS; i++)
    {
        struct vd_root_state *r = &vd_roots[i];
        XWindowAttributes ra, oa;
        Window root, parent, *children;
        unsigned int count, j;
        XRenderColor clear = { 0, 0, 0, 0 };

        if (!r->active || !r->dirty) continue;
        r->dirty = FALSE;

        /* self-heal: transient VD roots are destroyed during wineboot/explorer
         * setup; if ours is gone, tear the slot down (the redirect dies with
         * the root).  An orphaned overlay on a dead root would otherwise leak
         * and composite against a stale window. */
        if (!XGetWindowAttributes( gdi_display, r->vd_root, &ra ))
        {
            WARN( "VD root %lx gone, tearing down compositor slot\n", r->vd_root );
            vd_teardown_root( r );
            continue;
        }
        if (!XGetWindowAttributes( gdi_display, r->overlay, &oa ))
        {
            WARN( "overlay %lx gone, tearing down compositor slot\n", r->overlay );
            vd_teardown_root( r );
            continue;
        }

        /* clear the overlay to fully transparent so each frame is rendered
         * from scratch -- no ghosting from moved or removed children. */
        pvd_XRenderFillRectangle( gdi_display, PictOpSrc, r->overlay_pict, &clear,
                                  0, 0, oa.width, oa.height );

        if (XQueryTree( gdi_display, r->vd_root, &root, &parent, &children, &count ))
        {
            /* XQueryTree returns children in bottom-to-top stacking order;
             * compositing them in that order with PictOpOver preserves Z. */
            for (j = 0; j < count; j++) composite_child( r, children[j] );
            if (children) XFree( children );
        }
    }
    XFlush( gdi_display );
}

void vd_compositor_notify( XEvent *event )
{
    int i;
    if (!vd_ready) return;

    switch (event->type)
    {
    /* anything that can change the pixels or stacking of a redirected child,
     * plus NoExpose/GraphicsExpose for the XPutImage/XCopyArea that draw child
     * content.  paint() is deduped (runs at most once per root per ProcessEvents). */
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
        break;
    default:
        return;
    }

    /* Attribute the event to a specific root first: the VD root receives the
     * SubstructureNotify of its override-redirect children plus its own Expose. */
    for (i = 0; i < MAX_VD_ROOTS; i++)
    {
        if (!vd_roots[i].active) continue;
        if (event->xany.window == vd_roots[i].vd_root)
        {
            vd_roots[i].dirty = TRUE;
            return;
        }
    }
    /* Event is for a child or an unknown window: mark all active roots dirty
     * (brute-force safety; phase 2 Damage makes this precise per child). */
    for (i = 0; i < MAX_VD_ROOTS; i++)
        if (vd_roots[i].active) vd_roots[i].dirty = TRUE;
}

static Bool create_overlay( struct vd_root_state *r )
{
    XSetWindowAttributes attr;
    XWindowAttributes ra;
    XRenderPictFormat *fmt;
    Colormap cmap;
    unsigned long mask;

    if (!argb_visual.visual) return FALSE;
    if (!XGetWindowAttributes( gdi_display, r->vd_root, &ra )) return FALSE;

    cmap = XCreateColormap( gdi_display, DefaultRootWindow(gdi_display), argb_visual.visual, AllocNone );
    attr.override_redirect = True;
    attr.background_pixel = 0;
    attr.border_pixel = 0;
    attr.colormap = cmap;
    attr.event_mask = ExposureMask;
    attr.save_under = True;
    mask = CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask | CWSaveUnder;

    r->overlay = XCreateWindow( gdi_display, DefaultRootWindow(gdi_display),
                                ra.x, ra.y, ra.width, ra.height, 0, 32, InputOutput,
                                argb_visual.visual, mask, &attr );
    if (!r->overlay) return FALSE;

    fmt = pvd_XRenderFindVisualFormat( gdi_display, argb_visual.visual );
    if (!fmt) return FALSE;
    r->overlay_pict = pvd_XRenderCreatePicture( gdi_display, r->overlay, fmt, 0, NULL );
    if (!r->overlay_pict) return FALSE;

    XMapWindow( gdi_display, r->overlay );
    XRaiseWindow( gdi_display, r->overlay );

#ifdef SONAME_LIBXEXT
    /* empty input shape: the overlay never absorbs pointer/keyboard input,
     * everything falls through to the VD root beneath it. */
    if (pvd_XShapeCombineRectangles)
        pvd_XShapeCombineRectangles( gdi_display, r->overlay, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted );
#endif
    return TRUE;
}

void vd_compositor_init( Window vd_root )
{
    struct vd_root_state *r;

    if (!vd_ensure_ready()) return;

    /* idempotent: Wine re-initialises desktop roots across setup phases; never
     * register the same root twice (that was the phase-1 leak: a second overlay
     * per root, the first orphaned with an active manual redirect). */
    if (vd_find_root( vd_root )) return;

    r = vd_alloc_root();
    if (!r) { WARN( "VD compositor: no free slot (max %d roots)\n", MAX_VD_ROOTS ); return; }

    r->vd_root = vd_root;
    r->overlay = 0;
    r->overlay_pict = 0;
    r->dirty = FALSE;
    r->active = FALSE;  /* raised only after the overlay + redirect succeed */

    /* create the overlay BEFORE redirecting: if overlay creation fails we
     * leave the root's children drawn normally by the X server (visible,
     * opaque -- the status quo), never stuck behind a manual redirect with
     * nothing compositing them. */
    if (!create_overlay( r ))
    {
        WARN( "VD compositor: overlay creation failed for root %lx\n", vd_root );
        vd_teardown_root( r );
        return;
    }

    /* manual subwindow redirect: the X server stops drawing the VD root's
     * children itself and gives us redirected backing pixmaps. */
    pvd_XCompositeRedirectSubwindows( gdi_display, vd_root, CompositeRedirectManual );

    r->active = TRUE;
    r->dirty = TRUE;
    TRACE( "VD mini-compositor active, overlay %lx over VD root %lx\n", r->overlay, vd_root );
}

#else  /* !(SONAME_LIBXCOMPOSITE && SONAME_LIBXRENDER) */

void vd_compositor_init( Window vd_root ) { /* no composite/render support built in */ }
void vd_compositor_notify( XEvent *event ) { }
void vd_compositor_paint( void ) { }

#endif
