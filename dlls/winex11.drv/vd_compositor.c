/*
 * Virtual-desktop per-pixel-alpha mini-compositor (issue 64).
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
 * per-pixel alpha.  The overlay tracks the VD root's geometry and stacking
 * and lets input pass through (empty XInputShape) so the VD stays usable.
 *
 * Process boundary: the compositor only ever runs in the process that owns the
 * VD root -- normally explorer, which created it.  The applications drawing
 * into that desktop are OTHER processes; they never call X11DRV_CreateDesktop
 * and never composite anything.  SubstructureNotify on the root only reports
 * their map/configure/destroy, never their drawing, so XDamage is what makes
 * the whole thing work: one damage object per child, on the same connection
 * whose queue X11DRV_ProcessEvents drains, is the only trigger that tells the
 * root owner "a foreign window changed its pixels, recomposite".
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
#include <stdlib.h>
#include <X11/Xlib.h>

#include "x11drv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

#if defined(SONAME_LIBXCOMPOSITE) && defined(SONAME_LIBXRENDER)

#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>
#ifdef SONAME_LIBXDAMAGE
#include <X11/extensions/Xdamage.h>
#endif
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
#ifdef SONAME_LIBXDAMAGE
MAKE_FUNCPTR(XDamageQueryExtension)
MAKE_FUNCPTR(XDamageCreate)
MAKE_FUNCPTR(XDamageDestroy)
MAKE_FUNCPTR(XDamageSubtract)
#endif
#undef MAKE_FUNCPTR
#ifdef SONAME_LIBXEXT
static typeof(XShapeCombineRectangles) * pvd_XShapeCombineRectangles;
#endif

/* one tracked child of a VD root: the window plus its damage object, whose
 * notifications are the compositor's only trigger for foreign-process drawing */
struct vd_child
{
    Window window;
    XID damage;  /* Damage; 0 when damage tracking is unavailable for it */
};

struct vd_root_state
{
    BOOL active;          /* this slot is compositing a live VD root */
    Display *display;     /* connection owning the root's events and damage objects */
    Window vd_root;       /* the RGB24 virtual-desktop root */
    Window overlay;       /* ARGB32 overlay window covering the VD root */
    Colormap overlay_colormap; /* ARGB32 colormap the overlay was created with */
    Picture overlay_pict; /* ARGB32 picture over the overlay window */
    BOOL overlay_mapped;  /* overlay is mapped (only while the VD root is viewable) */
    BOOL dirty;           /* a recomposit of this root's child set is pending */
    struct vd_child *children;
    unsigned int children_count;
    unsigned int children_size;
};

static struct vd_root_state vd_roots[MAX_VD_ROOTS];
static BOOL vd_ready;     /* one-time: extensions loaded + queried + VD mode active */
static BOOL vd_use_damage;
static int vd_damage_event_base;
static int vd_damage_error_base;

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

/* XDamage is optional: without it the compositor still works, but it can only
 * recomposit on the coarse events it sees itself, which never covers another
 * process drawing into its window. */
static void load_damage(void)
{
#ifdef SONAME_LIBXDAMAGE
    void *xd = dlopen( SONAME_LIBXDAMAGE, RTLD_NOW );

    if (!xd) { WARN( "unable to load %s, VD compositing stays event-driven only\n", SONAME_LIBXDAMAGE ); return; }
#define LOAD(f) do { if (!(pvd_##f = dlsym( xd, #f ))) { WARN( "missing %s\n", #f ); return; } } while (0)
    LOAD( XDamageQueryExtension );
    LOAD( XDamageCreate );
    LOAD( XDamageDestroy );
    LOAD( XDamageSubtract );
#undef LOAD

    /* the event/error bases are assigned by the server when the extension is
     * registered, so they are the same on every connection to it. */
    if (!pvd_XDamageQueryExtension( gdi_display, &vd_damage_event_base, &vd_damage_error_base ))
    {
        WARN( "no DAMAGE extension on this server\n" );
        return;
    }
    /* the event dispatcher indexes its handler table with the raw event type
     * and does not range-check it, so refuse to put events it cannot hold on
     * the queue at all. */
    if (vd_damage_event_base + XDamageNotify >= MAX_EVENT_HANDLERS)
    {
        WARN( "damage event type %d out of dispatcher range\n", vd_damage_event_base + XDamageNotify );
        return;
    }
    vd_use_damage = TRUE;
    TRACE( "XDamage available, event base %d\n", vd_damage_event_base );
#endif
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
    load_damage();
    vd_ready = TRUE;
    return TRUE;
}

/* Children are created and destroyed by foreign processes at any time, so every
 * request aimed at one can legitimately lose the race against its destruction.
 * Those errors are expected and must not reach the default handler. */
static int vd_child_gone_error( Display *display, XErrorEvent *event, void *arg )
{
    if (event->error_code == BadWindow) return TRUE;
    if (event->error_code == BadDrawable) return TRUE;
    if (event->error_code == BadPixmap) return TRUE;
    if (event->error_code == BadMatch) return TRUE;
#ifdef SONAME_LIBXDAMAGE
    if (vd_use_damage && event->error_code == vd_damage_error_base + BadDamage) return TRUE;
#endif
    return FALSE;
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

static struct vd_child *vd_find_child( struct vd_root_state *r, Window child )
{
    unsigned int i;
    for (i = 0; i < r->children_count; i++)
        if (r->children[i].window == child) return &r->children[i];
    return NULL;
}

/* Start watching a child: without a damage object on it we never learn that its
 * owning process redrew it.  Idempotent -- the same child is offered from
 * CreateNotify, MapNotify and the paint-time tree sweep. */
static void vd_track_child( struct vd_root_state *r, Window child )
{
    struct vd_child *c;

    /* The VD root selects StructureNotify on itself as well as
     * SubstructureNotify for its children, and both report through the same
     * event fields -- so its own Map/Reparent arrives here looking exactly like
     * a child's.  Watching the root would put a damage object on a window we
     * never composite (it is not in its own XQueryTree), and every repaint
     * would then prune it again. */
    if (child == r->vd_root || child == r->overlay) return;
    if (vd_find_child( r, child )) return;

    if (r->children_count == r->children_size)
    {
        unsigned int size = r->children_size ? r->children_size * 2 : 16;
        struct vd_child *tmp = realloc( r->children, size * sizeof(*tmp) );
        if (!tmp) return;
        r->children = tmp;
        r->children_size = size;
    }
    c = &r->children[r->children_count++];
    c->window = child;
    c->damage = 0;

#ifdef SONAME_LIBXDAMAGE
    if (!vd_use_damage) return;
    X11DRV_expect_error( r->display, vd_child_gone_error, NULL );
    c->damage = pvd_XDamageCreate( r->display, child, XDamageReportNonEmpty );
    XSync( r->display, False );
    if (X11DRV_check_error())
    {
        WARN( "child %lx gone before its damage was created\n", child );
        c->damage = 0;
    }
#endif
    TRACE( "tracking child %lx of VD root %lx, damage %lx\n", child, r->vd_root, c->damage );
}

/* Stop watching a child.  'alive' is FALSE once the window is known to be gone:
 * the server destroys the damage together with its drawable, so asking for it
 * to be destroyed again is a BadDamage. */
static void vd_untrack_child( struct vd_root_state *r, Window child, BOOL alive )
{
    struct vd_child *c = vd_find_child( r, child );

    if (!c) return;
#ifdef SONAME_LIBXDAMAGE
    if (c->damage && alive)
    {
        X11DRV_expect_error( r->display, vd_child_gone_error, NULL );
        pvd_XDamageDestroy( r->display, c->damage );
        XSync( r->display, False );
        X11DRV_check_error();
    }
#endif
    *c = r->children[--r->children_count];
}

/* Free a slot's overlay resources and mark it reusable.  Used both on fatal
 * init failure and on self-heal when the VD root disappears. */
static void vd_teardown_root( struct vd_root_state *r )
{
    /* the VD root is gone at this point, so its children are too: no damage to
     * destroy, just drop the bookkeeping. */
    while (r->children_count) vd_untrack_child( r, r->children[0].window, FALSE );
    free( r->children );
    r->children = NULL;
    r->children_size = 0;

    if (r->overlay_pict) pvd_XRenderFreePicture( gdi_display, r->overlay_pict );
    if (r->overlay) XDestroyWindow( gdi_display, r->overlay );
    /* after the window is gone, so the colormap is not freed while still installed */
    if (r->overlay_colormap) XFreeColormap( gdi_display, r->overlay_colormap );
    r->active = FALSE;
    r->display = NULL;
    r->vd_root = 0;
    r->overlay = 0;
    r->overlay_colormap = 0;
    r->overlay_pict = 0;
    r->overlay_mapped = FALSE;
    r->dirty = FALSE;
}

/* Glue the overlay to the VD root: same screen rectangle, stacked directly on
 * top of it.  The VD root is a managed top-level -- the window manager places
 * it, resizes it and reparents it into a decoration frame -- so neither its
 * position nor its stacking is fixed.  Without this the overlay would sit at
 * the position the VD root had at creation time, before the WM ever placed it,
 * and would drop behind the VD root as soon as the desktop window is raised. */
static void vd_overlay_follow_root( struct vd_root_state *r )
{
    Window real_root = DefaultRootWindow( gdi_display );
    Window ancestor, root, parent, *children;
    XWindowAttributes attr;
    XWindowChanges changes;
    unsigned int count;
    int x, y;

    if (!XGetWindowAttributes( gdi_display, r->vd_root, &attr )) return;
    if (attr.map_state != IsViewable)
    {
        if (r->overlay_mapped) XUnmapWindow( gdi_display, r->overlay );
        r->overlay_mapped = FALSE;
        return;
    }
    if (!XTranslateCoordinates( gdi_display, r->vd_root, real_root, 0, 0, &x, &y, &ancestor )) return;

    /* a window can only be stacked relative to a sibling, so climb to the VD
     * root's own top-level ancestor -- its WM frame, when it has one. */
    for (ancestor = r->vd_root;;)
    {
        if (!XQueryTree( gdi_display, ancestor, &root, &parent, &children, &count )) return;
        if (children) XFree( children );
        if (!parent || parent == real_root) break;
        ancestor = parent;
    }

    changes.x = x;
    changes.y = y;
    changes.width = attr.width;
    changes.height = attr.height;
    changes.sibling = ancestor;
    changes.stack_mode = Above;
    XConfigureWindow( gdi_display, r->overlay,
                      CWX | CWY | CWWidth | CWHeight | CWSibling | CWStackMode, &changes );
    if (!r->overlay_mapped)
    {
        XMapWindow( gdi_display, r->overlay );
        r->overlay_mapped = TRUE;
    }
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

    /* the redirected backing pixmap is replaced whenever the child is resized,
     * so it has to be named again for every frame rather than cached. */
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

/* Bring the tracked child set in line with the real tree.  This is what makes
 * damage tracking robust across the process boundary: a foreign process can
 * have created and mapped its window before this root was even redirected, and
 * CreateNotify only ever arrives for windows created afterwards. */
static void vd_sync_children( struct vd_root_state *r, Window *children, unsigned int count )
{
    unsigned int i, j;

    for (i = 0; i < count; i++) vd_track_child( r, children[i] );

    for (i = 0; i < r->children_count; )
    {
        for (j = 0; j < count; j++) if (children[j] == r->children[i].window) break;
        if (j < count) { i++; continue; }
        /* gone from the tree: destroyed, or reparented away with its damage
         * still alive -- vd_child_gone_error covers the first case. */
        vd_untrack_child( r, r->children[i].window, TRUE );
    }
}

void vd_compositor_paint( Display *display )
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

        /* a root is only ever painted by the connection that owns it: that is
         * the one whose queue delivered the events marking it dirty. */
        if (!r->active || r->display != display || !r->dirty) continue;
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

        vd_overlay_follow_root( r );
        if (!r->overlay_mapped) continue;  /* VD root not viewable, nothing to show */

        if (!XGetWindowAttributes( gdi_display, r->overlay, &oa ))
        {
            WARN( "overlay %lx gone, tearing down compositor slot\n", r->overlay );
            vd_teardown_root( r );
            continue;
        }

        if (!XQueryTree( gdi_display, r->vd_root, &root, &parent, &children, &count )) continue;

        /* damage bookkeeping first: it takes error traps of its own, and those
         * must not nest inside the one guarding the composit pass below --
         * X11DRV_expect_error serialises on a plain mutex. */
        vd_sync_children( r, children, count );

        /* clear the overlay to fully transparent so each frame is rendered
         * from scratch -- no ghosting from moved or removed children. */
        pvd_XRenderFillRectangle( gdi_display, PictOpSrc, r->overlay_pict, &clear,
                                  0, 0, oa.width, oa.height );

        X11DRV_expect_error( gdi_display, vd_child_gone_error, NULL );
        /* XQueryTree returns children in bottom-to-top stacking order;
         * compositing them in that order with PictOpOver preserves Z. */
        for (j = 0; j < count; j++) composite_child( r, children[j] );
        XSync( gdi_display, False );
        X11DRV_check_error();

        if (children) XFree( children );
    }
    XFlush( gdi_display );
}

#ifdef SONAME_LIBXDAMAGE
/* A child's pixels changed.  This is the only notification the root owner gets
 * about drawing done by another process, so it is what actually drives the
 * compositor once the desktop is up. */
static BOOL vd_handle_damage( XDamageNotifyEvent *event )
{
    int i;

    for (i = 0; i < MAX_VD_ROOTS; i++)
    {
        struct vd_root_state *r = &vd_roots[i];
        struct vd_child *c;
        unsigned int j;

        if (!r->active || r->display != event->display) continue;
        for (c = NULL, j = 0; j < r->children_count; j++)
            if (r->children[j].damage == event->damage) { c = &r->children[j]; break; }
        if (!c) continue;

        /* clear the region BEFORE compositing, never after: anything drawn in
         * between raises a fresh notification instead of being swallowed.
         * Without this the child reports damage exactly once and then goes
         * quiet forever. */
        pvd_XDamageSubtract( r->display, c->damage, None, None );
        r->dirty = TRUE;
        return TRUE;
    }
    /* damage of a child we already stopped tracking -- do not subtract, the
     * object may be gone and that would be a BadDamage. */
    return FALSE;
}
#endif

void vd_compositor_notify( XEvent *event )
{
    struct vd_root_state *r = NULL;
    int i;

    if (!vd_ready) return;

#ifdef SONAME_LIBXDAMAGE
    /* Damage is deliberately handled here rather than through
     * X11DRV_register_event_handler: the dispatcher can drop an event before
     * the handler table is consulted (host_window_filter_event swallows
     * everything addressed to a host window), and a swallowed damage event is
     * never subtracted, which permanently silences that child. */
    if (vd_use_damage && event->type == vd_damage_event_base + XDamageNotify)
    {
        vd_handle_damage( (XDamageNotifyEvent *)event );
        return;
    }
#endif

    switch (event->type)
    {
    /* structural changes: they alter which children exist, where they are and
     * how they stack, none of which shows up as damage. */
    case Expose:
    case CreateNotify:
    case MapNotify:
    case UnmapNotify:
    case ConfigureNotify:
    case ReparentNotify:
    case DestroyNotify:
    case VisibilityNotify:
        break;
    case FocusIn:
    case EnterNotify:
        /* The overlay is a separate top-level, so the window manager raising
         * the VD root leaves it behind -- and nothing reports that: the root
         * keeps its stacking position among its own siblings, and a compositing
         * WM never sends it an Expose.  A desktop with no activity in it would
         * stay stuck behind its overlay, showing nothing at all.  Turning
         * towards the desktop is the moment that matters, and it is rare enough
         * to repaint on unconditionally. */
        if (event->type == EnterNotify && event->xcrossing.detail == NotifyInferior) return;
        break;
    case GraphicsExpose:
    case NoExpose:
        /* crude stand-ins for "someone drew something"; only needed while
         * there is no damage tracking to report it precisely. */
        if (vd_use_damage) return;
        break;
    default:
        return;
    }

    /* SubstructureNotify events name the root that selected them in the same
     * field as xany.window, so a child event is attributed to its root here. */
    for (i = 0; i < MAX_VD_ROOTS; i++)
    {
        if (!vd_roots[i].active) continue;
        if (vd_roots[i].display != event->xany.display) continue;
        if (vd_roots[i].vd_root == event->xany.window) { r = &vd_roots[i]; break; }
    }

    if (!r)
    {
        /* not one of our roots: without damage tracking we cannot tell whether
         * it changed a composited child, so recomposit this connection's roots.
         * With damage this is dead weight and the events above are enough. */
        if (vd_use_damage) return;
        for (i = 0; i < MAX_VD_ROOTS; i++)
            if (vd_roots[i].active && vd_roots[i].display == event->xany.display)
                vd_roots[i].dirty = TRUE;
        return;
    }

    switch (event->type)
    {
    case CreateNotify:
        vd_track_child( r, event->xcreatewindow.window );
        break;
    case MapNotify:
        vd_track_child( r, event->xmap.window );
        break;
    case ReparentNotify:
        if (event->xreparent.parent == r->vd_root) vd_track_child( r, event->xreparent.window );
        else vd_untrack_child( r, event->xreparent.window, TRUE );
        break;
    case UnmapNotify:
        vd_untrack_child( r, event->xunmap.window, TRUE );
        break;
    case DestroyNotify:
        vd_untrack_child( r, event->xdestroywindow.window, FALSE );
        break;
    }
    r->dirty = TRUE;
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
    r->overlay_colormap = cmap;  /* owned by the slot, freed in vd_teardown_root */
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

    /* left unmapped: vd_overlay_follow_root maps it once the WM has placed the
     * VD root, so it never shows up at the wrong position first. */

#ifdef SONAME_LIBXEXT
    /* empty input shape: the overlay never absorbs pointer/keyboard input,
     * everything falls through to the VD root beneath it. */
    if (pvd_XShapeCombineRectangles)
        pvd_XShapeCombineRectangles( gdi_display, r->overlay, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted );
#endif
    return TRUE;
}

void vd_compositor_init( Display *display, Window vd_root )
{
    struct vd_root_state *r;

    if (!vd_ensure_ready()) return;

    /* idempotent: Wine re-initialises desktop roots across setup phases; never
     * register the same root twice (that was the phase-1 leak: a second overlay
     * per root, the first orphaned with an active manual redirect). */
    if (vd_find_root( vd_root )) return;

    r = vd_alloc_root();
    if (!r) { WARN( "VD compositor: no free slot (max %d roots)\n", MAX_VD_ROOTS ); return; }

    r->display = display;
    r->vd_root = vd_root;
    r->overlay = 0;
    r->overlay_colormap = 0;
    r->overlay_pict = 0;
    r->overlay_mapped = FALSE;
    r->dirty = FALSE;
    r->children = NULL;
    r->children_count = 0;
    r->children_size = 0;
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
    XSync( gdi_display, False );

    r->active = TRUE;
    r->dirty = TRUE;
    TRACE( "VD mini-compositor active, overlay %lx over VD root %lx, damage %s\n",
           r->overlay, vd_root, vd_use_damage ? "on" : "off" );
}

#else  /* !(SONAME_LIBXCOMPOSITE && SONAME_LIBXRENDER) */

void vd_compositor_init( Display *display, Window vd_root ) { /* no composite/render support built in */ }
void vd_compositor_notify( XEvent *event ) { }
void vd_compositor_paint( Display *display ) { }

#endif
