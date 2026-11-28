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
 * alpha (XRenderComposite, PictOpOver) onto an overlay window that covers the
 * VD root.  Input passes through it (empty XInputShape) so the VD stays usable.
 *
 * The overlay is a CHILD of the VD root, and the blending happens entirely
 * here: the children are composited onto the root's own painted background,
 * captured while it is still uncovered (capture_background), so the result is
 * opaque and the host compositor has nothing left to blend.
 *
 * The first design made the overlay an ARGB32 top-level of the real root and
 * left the blending to the host compositor.  That works visually but costs the
 * stacking bond: an override-redirect top-level is not part of the window
 * manager's stack, so it stayed on top of whatever the user switched to, and
 * the WM reparenting the VD root in and out of its frame turned every restack
 * attempt into a race (issue 139).  As a child, the overlay inherits the root's
 * coordinate space and its place in the host's stack for free.
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
MAKE_FUNCPTR(XCompositeUnredirectWindow)
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
    /* geometry cache: asking the server per child per frame is a blocking
     * round trip each time, and the repaint runs in the desktop owner's event
     * loop -- with a dozen children that is enough to stall it.  The values
     * come from the ConfigureNotify/MapNotify we already receive. */
    int x, y;
    int width, height;
    Visual *visual;
    BOOL viewable;
    BOOL geometry_valid;
    /* Last content the client actually painted, kept to put back after a remap:
     * the fresh backing pixmap a remap produces is pre-filled by the server from
     * the parent, and the client only paints the part its shadow covers. */
    Pixmap save_pixmap;
    Picture save_pict;
    int save_width, save_height;
    BOOL save_valid;      /* the copy holds real content, not undefined memory */
    BOOL save_dirty;      /* the client painted since the copy was taken */
    BOOL needs_restore;   /* a map happened; the next composite restores */
};

struct vd_root_state
{
    BOOL active;          /* this slot is compositing a live VD root */
    BOOL pending;         /* registered, but the takeover is deferred until a child appears */
    Display *display;     /* connection owning the root's events and damage objects */
    Window vd_root;       /* the RGB24 virtual-desktop root */
    Window overlay;       /* opaque overlay window, a child of the VD root */
    Pixmap back_pixmap;   /* off-screen buffer a frame is assembled in */
    Picture back_pict;    /* picture over that buffer -- the composit target */
    int back_width, back_height;
    BOOL overlay_mapped;  /* overlay is mapped (only while the VD root is viewable) */
    Pixmap bg_pixmap;     /* the VD root's own painted background, captured while visible */
    GC bg_gc;             /* for blitting that background, kept to avoid a GC per frame */
    int bg_width, bg_height;
    BOOL bg_valid;        /* the capture matches the root's current size and content */
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
    LOAD( xc, XCompositeUnredirectWindow );
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

static void vd_activate_root( struct vd_root_state *r );

/* a root that is registered but whose takeover has not happened yet */
static struct vd_root_state *vd_find_pending_root( Window vd_root )
{
    int i;
    for (i = 0; i < MAX_VD_ROOTS; i++)
        if (vd_roots[i].pending && vd_roots[i].vd_root == vd_root) return &vd_roots[i];
    return NULL;
}

static struct vd_root_state *vd_alloc_root( void )
{
    int i;
    for (i = 0; i < MAX_VD_ROOTS; i++)
        if (!vd_roots[i].active && !vd_roots[i].pending) return &vd_roots[i];
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
    c->geometry_valid = FALSE;
    c->viewable = FALSE;
    c->save_pixmap = 0;
    c->save_pict = 0;
    c->save_width = c->save_height = 0;
    c->save_valid = FALSE;
    c->save_dirty = TRUE;    /* nothing saved yet: the first composite takes one */
    c->needs_restore = FALSE;

    /* the only geometry query per child: from here on the cache is kept up to
     * date from ConfigureNotify/MapNotify instead of asking the server again. */
    {
        XWindowAttributes attr;

        X11DRV_expect_error( r->display, vd_child_gone_error, NULL );
        if (XGetWindowAttributes( gdi_display, child, &attr ) && attr.class != InputOnly)
        {
            c->x = attr.x;
            c->y = attr.y;
            c->width = attr.width;
            c->height = attr.height;
            c->visual = attr.visual;
            c->viewable = (attr.map_state == IsViewable);
            c->geometry_valid = TRUE;
        }
        XSync( gdi_display, False );
        X11DRV_check_error();
    }

#ifdef SONAME_LIBXDAMAGE
    if (!vd_use_damage) return;
    X11DRV_expect_error( r->display, vd_child_gone_error, NULL );
    c->damage = pvd_XDamageCreate( r->display, child, XDamageReportNonEmpty );
    /* NB: keep this in sync with vd_child_gone_error, which also has to cover
     * the BadMatch that NameWindowPixmap raises for an unredirected child. */
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
    if (c->save_pict) pvd_XRenderFreePicture( gdi_display, c->save_pict );
    if (c->save_pixmap) XFreePixmap( gdi_display, c->save_pixmap );
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

    if (r->back_pict) pvd_XRenderFreePicture( gdi_display, r->back_pict );
    if (r->back_pixmap) XFreePixmap( gdi_display, r->back_pixmap );
    if (r->bg_gc) XFreeGC( gdi_display, r->bg_gc );
    if (r->bg_pixmap) XFreePixmap( gdi_display, r->bg_pixmap );
    if (r->overlay) XDestroyWindow( gdi_display, r->overlay );
    r->active = FALSE;
    r->pending = FALSE;
    r->display = NULL;
    r->vd_root = 0;
    r->overlay = 0;
    r->back_pixmap = 0;
    r->back_pict = 0;
    r->back_width = r->back_height = 0;
    r->overlay_mapped = FALSE;
    r->bg_pixmap = 0;
    r->bg_gc = 0;
    r->bg_width = r->bg_height = 0;
    r->bg_valid = FALSE;
    r->dirty = FALSE;
}

/* Errors a composit pass can legitimately produce: the children belong to other
 * processes and can stop being redirected, viewable or exist at all between the
 * tree query and the requests naming their pixmaps. */
static int vd_composite_error( Display *display, XErrorEvent *event, void *arg )
{
    return (event->error_code == BadMatch || event->error_code == BadWindow ||
            event->error_code == BadDrawable || event->error_code == BadPixmap);
}

/* Glue the overlay to the VD root: same screen rectangle, stacked directly on
 * top of it.  The VD root is a managed top-level -- the window manager places
 * it, resizes it and reparents it into a decoration frame -- so neither its
 * position nor its stacking is fixed.  Without this the overlay would sit at
 * the position the VD root had at creation time, before the WM ever placed it,
 * and would drop behind the VD root as soon as the desktop window is raised. */
static Bool capture_background( struct vd_root_state *r );

static void vd_overlay_follow_root( struct vd_root_state *r )
{
    XWindowAttributes attr;
    XWindowChanges changes;

    if (!XGetWindowAttributes( gdi_display, r->vd_root, &attr )) return;
    if (attr.map_state != IsViewable)
    {
        if (r->overlay_mapped) XUnmapWindow( gdi_display, r->overlay );
        r->overlay_mapped = FALSE;
        return;
    }

    /* As a child of the VD root the overlay shares its coordinate space and its
     * place in the host's stack: no translation, no sibling stacking, and
     * nothing that can race the window manager reparenting the root into its
     * frame.  Only the size has to be followed. */
    if (attr.width != r->bg_width || attr.height != r->bg_height)
    {
        changes.width = attr.width;
        changes.height = attr.height;
        XConfigureWindow( gdi_display, r->overlay, CWWidth | CWHeight, &changes );
        /* the captured base no longer covers the root; it is refreshed on the
         * next capture, which needs the root uncovered again */
        r->bg_valid = FALSE;
        if (r->overlay_mapped)
        {
            XUnmapWindow( gdi_display, r->overlay );
            r->overlay_mapped = FALSE;
        }
    }

    if (!r->overlay_mapped)
    {
        if (!capture_background( r )) return;  /* the opaque base comes first */
        XMapWindow( gdi_display, r->overlay );
        /* above its siblings: they are redirected and therefore not drawn, but
         * a later-mapped child would otherwise sit above the overlay */
        XRaiseWindow( gdi_display, r->overlay );
        r->overlay_mapped = TRUE;
    }
}

/* The overlay is an ordinary window now, so anything drawn into it is on screen
 * immediately -- and a frame is not one drawing operation but many: the
 * background first, then every child on top.  Painting that directly into the
 * overlay lets a screen refresh fall between the steps, which shows up as the
 * window content flickering while a window is being moved (the configure events
 * arrive in quick succession, so it repaints often).  Assemble the frame in an
 * off-screen pixmap instead and blit it in one operation; the screen then only
 * ever sees finished frames. */
static Bool ensure_back_buffer( struct vd_root_state *r, int width, int height )
{
    XRenderPictFormat *fmt;

    if (r->back_pixmap && (r->back_width != width || r->back_height != height))
    {
        if (r->back_pict) pvd_XRenderFreePicture( gdi_display, r->back_pict );
        XFreePixmap( gdi_display, r->back_pixmap );
        r->back_pixmap = 0;
        r->back_pict = 0;
    }
    if (r->back_pixmap) return TRUE;

    r->back_pixmap = XCreatePixmap( gdi_display, r->overlay, width, height, default_visual.depth );
    if (!r->back_pixmap) return FALSE;
    fmt = pvd_XRenderFindVisualFormat( gdi_display, default_visual.visual );
    if (!fmt) return FALSE;
    r->back_pict = pvd_XRenderCreatePicture( gdi_display, r->back_pixmap, fmt, 0, NULL );
    if (!r->back_pict) return FALSE;
    r->back_width = width;
    r->back_height = height;
    return TRUE;
}

/* Blank the part of a child's backing pixmap that lies outside the VD root.
 *
 * A window's backing store only holds defined content where the window lies
 * inside its parent.  Map one straddling the desktop edge and the part beyond it
 * comes up as whatever was in that memory -- and comes up OPAQUE, so a window
 * that should fade out into nothing grows a hard border instead.  On a JUCE drop
 * shadow, whose colour is premultiplied black throughout, that reads as a black
 * seam along the outer edge, and it survives: what repaints the window only
 * writes where there is something to draw, and a fully transparent edge has
 * nothing.  The garbage is baked into the backing store and travels with the
 * window, so it stays visible long after the window is back inside the desktop.
 *
 * The server will not fix this either -- every drawing request aimed at a window
 * is clipped to its parent, so even clearing the whole window leaves the part
 * outside untouched.  A pixmap has no parent and no such clip, which is why
 * writing to it directly is what reaches the area.
 *
 * Nothing can be lost by it: those pixels are not displayed while they are off
 * the desktop, and no one paints them.  Undefined memory is all that is being
 * replaced. */
static void clear_offscreen_edges( struct vd_child *c, Picture pict, int root_width, int root_height )
{
    static const XRenderColor transparent = { 0, 0, 0, 0 };
    int over;

    if ((over = min( -c->x, c->width )) > 0)
        pvd_XRenderFillRectangle( gdi_display, PictOpSrc, pict, &transparent,
                                  0, 0, over, c->height );
    if ((over = min( -c->y, c->height )) > 0)
        pvd_XRenderFillRectangle( gdi_display, PictOpSrc, pict, &transparent,
                                  0, 0, c->width, over );
    if ((over = min( c->x + c->width - root_width, c->width )) > 0)
        pvd_XRenderFillRectangle( gdi_display, PictOpSrc, pict, &transparent,
                                  c->width - over, 0, over, c->height );
    if ((over = min( c->y + c->height - root_height, c->height )) > 0)
        pvd_XRenderFillRectangle( gdi_display, PictOpSrc, pict, &transparent,
                                  0, c->height - over, c->width, over );
}

/* Keep a copy of the last content the client actually painted, and put it back
 * after a remap.
 *
 * A remap hands the window a backing pixmap the server has pre-filled from the
 * parent -- with IncludeInferiors, so including this compositor's own overlay,
 * which carries the finished frame.  The client then paints its shadow into it,
 * but only where that shadow has coverage: the outer edge fades to alpha 0, there
 * is nothing to draw out there, and so the copied screen content stays -- and it
 * arrived opaque, so it shows.  What appears is whatever happened to be underneath:
 * desktop icons, the desktop colour, or parts of the application's own GUI.
 *
 * Measured on the Kurzweil KM88 editor, whose shadow windows are unmapped and
 * remapped while the window is dragged: of the two 4-row bands sampled at the top
 * and bottom of the 1280x12 shadow, exactly one comes back foreign and complete
 * while the other is clean.  The client had painted -- just not out there.
 *
 * Blanking the fresh pixmap instead cannot work, and that was measured rather than
 * argued: at the first composite after a remap the client has already painted (30
 * remaps out of 30), and a blank raises no damage of its own, so anything it
 * deleted would never be redrawn.  Restoring a copy has no such problem -- the
 * window keeps its size across the cycle, so the copy holds exactly the content
 * that belongs there, and writing it back cannot lose what the client put in.
 *
 * The copy is refreshed only when the client has painted since it was taken, so a
 * shadow that just sits there costs one copy, not one per frame.  Nothing is lost
 * if a copy is unavailable: without one, this does nothing and the old behaviour
 * stands. */
#define VD_SAVE_MAX_PIXELS (512 * 512)  /* 1 MB per child at 32bpp; a shadow is far below */

static Bool ensure_save_buffer( struct vd_root_state *r, struct vd_child *c, XRenderPictFormat *fmt )
{
    if (c->save_pixmap && (c->save_width != c->width || c->save_height != c->height))
    {
        if (c->save_pict) pvd_XRenderFreePicture( gdi_display, c->save_pict );
        XFreePixmap( gdi_display, c->save_pixmap );
        c->save_pixmap = 0;
        c->save_pict = 0;
        c->save_valid = FALSE;   /* a copy of the old size restores nothing */
    }
    if (c->save_pixmap) return TRUE;
    /* the artefact lives on shadow and popup borders; a full-screen ARGB32 child is
     * not worth a megabyte of shadow copy per frame it paints. */
    if (c->width * c->height > VD_SAVE_MAX_PIXELS) return FALSE;

    if (!(c->save_pixmap = XCreatePixmap( gdi_display, r->vd_root, c->width, c->height, 32 )))
        return FALSE;
    if (!(c->save_pict = pvd_XRenderCreatePicture( gdi_display, c->save_pixmap, fmt, 0, NULL )))
    {
        XFreePixmap( gdi_display, c->save_pixmap );
        c->save_pixmap = 0;
        return FALSE;
    }
    c->save_width = c->width;
    c->save_height = c->height;
    return TRUE;
}

static void save_backing_store( struct vd_root_state *r, struct vd_child *c, Picture pict,
                                XRenderPictFormat *fmt )
{
    if (!c->save_dirty) return;
    if (!ensure_save_buffer( r, c, fmt )) return;
    pvd_XRenderComposite( gdi_display, PictOpSrc, pict, None, c->save_pict,
                          0, 0, 0, 0, 0, 0, c->width, c->height );
    c->save_valid = TRUE;
    c->save_dirty = FALSE;
}

static void restore_backing_store( struct vd_child *c, Picture pict )
{
    if (!c->save_valid) return;
    if (c->save_width != c->width || c->save_height != c->height) return;
    pvd_XRenderComposite( gdi_display, PictOpSrc, c->save_pict, None, pict,
                          0, 0, 0, 0, 0, 0, c->width, c->height );
}

/* Composite one child onto the frame under construction at its current geometry.  ARGB32
 * children blend with their alpha (PictOpOver), opaque children just paint. */
static void composite_child( struct vd_root_state *r, struct vd_child *c, int root_width, int root_height )
{
    XRenderPictFormat *fmt;
    Pixmap pixmap;
    Picture pict;

    if (!c->geometry_valid || !c->viewable) return;

    fmt = pvd_XRenderFindVisualFormat( gdi_display, c->visual );
    if (!fmt) return;

    /* the redirected backing pixmap is replaced whenever the child is resized,
     * so it has to be named again for every frame rather than cached. */
    pixmap = pvd_XCompositeNameWindowPixmap( gdi_display, c->window );
    if (!pixmap) return;

    pict = pvd_XRenderCreatePicture( gdi_display, pixmap, fmt, 0, NULL );
    if (pict)
    {
        /* only a per-pixel-alpha child is affected by any of these: an opaque one
         * has no transparent edge for foreign content to survive in. */
        if (fmt->depth == 32)
        {
            if (c->needs_restore)
            {
                restore_backing_store( c, pict );
                c->needs_restore = FALSE;
                /* the good content is back in place; saving now would only copy
                 * what we just wrote. */
                c->save_dirty = FALSE;
            }
            clear_offscreen_edges( c, pict, root_width, root_height );
            /* after the edge clear, so the copy carries the already corrected
             * content and a later restore cannot reintroduce an off-desktop seam. */
            save_backing_store( r, c, pict, fmt );
        }

        /* child geometry is relative to the VD root; the overlay shares the
         * VD root's geometry, so the same coordinates place it correctly. */
        pvd_XRenderComposite( gdi_display, PictOpOver, pict, None, r->back_pict,
                              0, 0, 0, 0, c->x, c->y, c->width, c->height );
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

        if (!ensure_back_buffer( r, oa.width, oa.height ))
        {
            WARN( "no back buffer for overlay %lx\n", r->overlay );
            if (children) XFree( children );
            continue;
        }

        /* Start each frame from the captured root background, so the composit is
         * opaque -- the host does not have to blend anything -- and nothing from
         * the previous frame ghosts through where a child moved or vanished. */
        if (r->bg_valid)
            XCopyArea( gdi_display, r->bg_pixmap, r->back_pixmap, r->bg_gc, 0, 0,
                       min( r->bg_width, oa.width ), min( r->bg_height, oa.height ), 0, 0 );
        else
            pvd_XRenderFillRectangle( gdi_display, PictOpSrc, r->back_pict, &clear,
                                      0, 0, oa.width, oa.height );

        /* XQueryTree returns children in bottom-to-top stacking order;
         * compositing them in that order with PictOpOver preserves Z.
         *
         * The trap is not optional: NameWindowPixmap raises BadMatch for a
         * child that stopped being redirected or viewable since the tree was
         * read -- which happens constantly, because the children belong to
         * other processes -- and BadMatch is not in the set ignore_error lets
         * through on gdi_display, so it would terminate the desktop process.
         * One sync for the whole pass, not one per child: the geometry is
         * cached, so this is the only round trip left in a repaint. */
        X11DRV_expect_error( gdi_display, vd_composite_error, NULL );
        for (j = 0; j < count; j++)
        {
            struct vd_child *c = vd_find_child( r, children[j] );
            if (c) composite_child( r, c, oa.width, oa.height );
        }
        XSync( gdi_display, False );
        X11DRV_check_error();

        /* one blit, so the screen never shows a half-assembled frame */
        XCopyArea( gdi_display, r->back_pixmap, r->overlay, r->bg_gc,
                   0, 0, oa.width, oa.height, 0, 0 );

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
        /* The client painted, so the saved copy is behind and has to be retaken --
         * gating it on this is what keeps a static shadow at one copy instead of
         * one per frame. */
        c->save_dirty = TRUE;
        r->dirty = TRUE;
        return TRUE;
    }
    /* damage of a child we already stopped tracking -- do not subtract, the
     * object may be gone and that would be a BadDamage. */
    return FALSE;
}
#endif

/* Substructure events name the window that selected the mask in xany.window and
 * the affected child in their own field; for structure events the two are the
 * same.  The distinction matters because SubstructureNotifyMask on the VD root
 * is ours: without it Wine would never see these events, and its dispatcher
 * attributes them to the desktop window -- reacting to foreign children as if
 * they were its own, which breaks the desktop handover outright. */
static BOOL is_substructure_event( XEvent *event )
{
    switch (event->type)
    {
    case CreateNotify:    return event->xcreatewindow.window != event->xany.window;
    case DestroyNotify:   return event->xdestroywindow.window != event->xany.window;
    case UnmapNotify:     return event->xunmap.window != event->xany.window;
    case MapNotify:       return event->xmap.window != event->xany.window;
    case ReparentNotify:  return event->xreparent.window != event->xany.window;
    case ConfigureNotify: return event->xconfigure.window != event->xany.window;
    case GravityNotify:   return event->xgravity.window != event->xany.window;
    case CirculateNotify: return event->xcirculate.window != event->xany.window;
    default:              return FALSE;
    }
}

BOOL vd_compositor_notify( XEvent *event )
{
    struct vd_root_state *r = NULL;
    struct vd_child *c;
    int i;
    BOOL consumed = FALSE;

    if (!vd_ready) return consumed;

    /* Decided up front so that every exit path below reports it: an event that
     * only reached this process because of the SubstructureNotifyMask this
     * compositor puts on the VD root must not travel on to Wine's dispatcher,
     * which would attribute a foreign child's map/configure/destroy to the
     * desktop window itself. */
    consumed = is_substructure_event( event ) &&
               (vd_find_root( event->xany.window ) || vd_find_pending_root( event->xany.window ));

#ifdef SONAME_LIBXDAMAGE
    /* Damage is deliberately handled here rather than through
     * X11DRV_register_event_handler: the dispatcher can drop an event before
     * the handler table is consulted (host_window_filter_event swallows
     * everything addressed to a host window), and a swallowed damage event is
     * never subtracted, which permanently silences that child. */
    if (vd_use_damage && event->type == vd_damage_event_base + XDamageNotify)
    {
        vd_handle_damage( (XDamageNotifyEvent *)event );
        return consumed;
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
        if (event->type == EnterNotify && event->xcrossing.detail == NotifyInferior) return consumed;
        break;
    case GraphicsExpose:
    case NoExpose:
        /* crude stand-ins for "someone drew something"; only needed while
         * there is no damage tracking to report it precisely. */
        if (vd_use_damage) return consumed;
        break;
    default:
        return consumed;
    }

    /* A registered root takes over once its first child is mapped: by then the
     * desktop window exists and Wine is done setting it up, so redirecting no
     * longer races the creation (see vd_activate_root). */
    if (event->type == MapNotify)
    {
        for (i = 0; i < MAX_VD_ROOTS; i++)
        {
            if (!vd_roots[i].pending) continue;
            if (vd_roots[i].display != event->xany.display) continue;
            if (vd_roots[i].vd_root != event->xany.window) continue;
            if (event->xmap.window == vd_roots[i].vd_root) break;  /* the root's own map */
            vd_roots[i].pending = FALSE;
            vd_activate_root( &vd_roots[i] );
            break;
        }
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
        if (vd_use_damage) return consumed;
        for (i = 0; i < MAX_VD_ROOTS; i++)
            if (vd_roots[i].active && vd_roots[i].display == event->xany.display)
                vd_roots[i].dirty = TRUE;
        return consumed;
    }

    switch (event->type)
    {
    case CreateNotify:
        vd_track_child( r, event->xcreatewindow.window );
        break;
    case MapNotify:
        vd_track_child( r, event->xmap.window );
        if ((c = vd_find_child( r, event->xmap.window )))
        {
            c->viewable = TRUE;
            /* A remap means a fresh backing pixmap, pre-filled by the server from
             * the parent.  The next composite puts the saved content back over it. */
            c->needs_restore = TRUE;
        }
        break;
    case ConfigureNotify:
        /* keeps the geometry cache current, which is what lets the repaint run
         * without a per-child round trip to the server. */
        if ((c = vd_find_child( r, event->xconfigure.window )))
        {
            c->x = event->xconfigure.x;
            c->y = event->xconfigure.y;
            c->width = event->xconfigure.width;
            c->height = event->xconfigure.height;
        }
        break;
    case ReparentNotify:
        if (event->xreparent.parent == r->vd_root) vd_track_child( r, event->xreparent.window );
        else vd_untrack_child( r, event->xreparent.window, TRUE );
        break;
    case UnmapNotify:
        /* Keep the child tracked instead of dropping it.  Its saved backing-store
         * copy has to survive the unmap/remap cycle a client puts its shadow
         * windows through -- surviving that cycle is the entire point of the copy,
         * and a dropped entry would come back with nothing to restore from.  The
         * damage object stays valid across it as well: it hangs on the window, not
         * on its map state.  Children that leave the tree for good are still
         * dropped, by vd_sync_children and by the DestroyNotify below. */
        if ((c = vd_find_child( r, event->xunmap.window ))) c->viewable = FALSE;
        break;
    case DestroyNotify:
        vd_untrack_child( r, event->xdestroywindow.window, FALSE );
        break;
    }
    r->dirty = TRUE;

    return consumed;
}

/* The overlay is a child of the VD root, not a top-level of the real root.
 *
 * A top-level would have to be blended by the host compositor to show per-pixel
 * alpha, and buying that costs the stacking bond: an override-redirect window is
 * not part of the window manager's stack, so it stayed on top of whatever the
 * user switched to, and the WM reparenting the VD root in and out of its frame
 * turned every restack attempt into a race (issue 139).
 *
 * Compositing already happens here -- XRenderComposite with PictOpOver -- so the
 * host does not need to blend anything, as long as the children are blended onto
 * an opaque base rather than onto transparency.  That base is the VD root's own
 * background, captured in capture_background() while it is still visible.  The
 * result is opaque, the overlay is an ordinary child, and X stacks it with its
 * siblings for free. */
static Bool create_overlay( struct vd_root_state *r )
{
    XSetWindowAttributes attr;
    XWindowAttributes ra;
    unsigned long mask;

    if (!XGetWindowAttributes( gdi_display, r->vd_root, &ra )) return FALSE;

    attr.background_pixmap = None;  /* every pixel is painted from the composit */
    attr.border_pixel = 0;
    attr.colormap = default_colormap;
    attr.event_mask = ExposureMask;
    mask = CWBackPixmap | CWBorderPixel | CWColormap | CWEventMask;

    r->overlay = XCreateWindow( gdi_display, r->vd_root, 0, 0, ra.width, ra.height, 0,
                                default_visual.depth, InputOutput, default_visual.visual,
                                mask, &attr );
    if (!r->overlay) return FALSE;

    /* nothing is ever drawn into the overlay directly: a frame is assembled in
     * the back buffer and blitted in one go (see vd_compositor_paint). */

    /* left unmapped: it is mapped once the background has been captured, so it
     * never shows a frame of uninitialised content. */

#ifdef SONAME_LIBXEXT
    /* empty input shape: the overlay never absorbs pointer/keyboard input,
     * everything falls through to the VD root beneath it. */
    if (pvd_XShapeCombineRectangles)
        pvd_XShapeCombineRectangles( gdi_display, r->overlay, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted );
#endif
    return TRUE;
}

/* Second half of the setup: create the overlay and take over the drawing of
 * the root's children.  Deliberately not done from vd_compositor_init: that
 * runs in the middle of X11DRV_CreateDesktop, before the desktop window even
 * exists on the Win32 side, and redirecting (plus the XSync it needs) at that
 * point makes the desktop creation that follows fail with BadWindow on a root
 * that is torn down and recreated underneath us.  By the time the first child
 * appears the desktop is settled and the takeover is safe. */
/* Capture the VD root's own painted background as the composit's opaque base.
 *
 * After the manual subwindow redirect the root shows nothing but its background:
 * the server no longer draws the children, so what is on screen at that moment
 * is exactly the base the composit needs.  It has to be taken before the overlay
 * is mapped -- once the root is covered, the content of the covered area is
 * undefined.  Wine's desktop root carries no background pixmap of its own
 * (explorer paints it on Expose), so ParentRelative would yield nothing and a
 * copy is the only reliable base. */
static Bool capture_background( struct vd_root_state *r )
{
    XWindowAttributes ra;

    if (r->overlay_mapped) return r->bg_valid;  /* root is covered, keep what we have */
    if (!XGetWindowAttributes( gdi_display, r->vd_root, &ra )) return FALSE;
    if (ra.map_state != IsViewable) return FALSE;

    if (r->bg_pixmap && (r->bg_width != ra.width || r->bg_height != ra.height))
    {
        XFreePixmap( gdi_display, r->bg_pixmap );
        r->bg_pixmap = 0;
    }
    if (!r->bg_pixmap)
    {
        r->bg_pixmap = XCreatePixmap( gdi_display, r->vd_root, ra.width, ra.height, default_visual.depth );
        if (!r->bg_pixmap) return FALSE;
        r->bg_width = ra.width;
        r->bg_height = ra.height;
    }
    if (!r->bg_gc && !(r->bg_gc = XCreateGC( gdi_display, r->bg_pixmap, 0, NULL ))) return FALSE;

    XCopyArea( gdi_display, r->vd_root, r->bg_pixmap, r->bg_gc,
               0, 0, r->bg_width, r->bg_height, 0, 0 );
    r->bg_valid = TRUE;
    return TRUE;
}

static void vd_activate_root( struct vd_root_state *r )
{
    /* create the overlay BEFORE redirecting: if overlay creation fails we
     * leave the root's children drawn normally by the X server (visible,
     * opaque -- the status quo), never stuck behind a manual redirect with
     * nothing compositing them. */
    if (!create_overlay( r ))
    {
        WARN( "VD compositor: overlay creation failed for root %lx\n", r->vd_root );
        vd_teardown_root( r );
        return;
    }

    /* manual subwindow redirect: the X server stops drawing the VD root's
     * children itself and gives us redirected backing pixmaps. */
    pvd_XCompositeRedirectSubwindows( gdi_display, r->vd_root, CompositeRedirectManual );
    /* ...  including the overlay, which is a child as well: take it back out,
     * or the only window that is supposed to be drawn would be the one the
     * server stops drawing. */
    pvd_XCompositeUnredirectWindow( gdi_display, r->overlay, CompositeRedirectManual );
    XSync( gdi_display, False );

    r->active = TRUE;
    r->dirty = TRUE;
    TRACE( "VD mini-compositor active, overlay %lx over VD root %lx, damage %s\n",
           r->overlay, r->vd_root, vd_use_damage ? "on" : "off" );
}

void vd_compositor_init( Display *display, Window vd_root )
{
    struct vd_root_state *r;

    if (!vd_ensure_ready()) return;

    /* idempotent: Wine re-initialises desktop roots across setup phases; never
     * register the same root twice (that was the phase-1 leak: a second overlay
     * per root, the first orphaned with an active manual redirect). */
    if (vd_find_root( vd_root ) || vd_find_pending_root( vd_root )) return;

    r = vd_alloc_root();
    if (!r) { WARN( "VD compositor: no free slot (max %d roots)\n", MAX_VD_ROOTS ); return; }

    r->display = display;
    r->vd_root = vd_root;
    r->overlay = 0;
    r->back_pixmap = 0;
    r->back_pict = 0;
    r->overlay_mapped = FALSE;
    r->dirty = FALSE;
    r->children = NULL;
    r->children_count = 0;
    r->children_size = 0;
    r->active = FALSE;   /* raised by vd_activate_root once a child shows up */
    r->pending = TRUE;   /* registered, waiting for the desktop to settle */

    TRACE( "VD mini-compositor registered for VD root %lx, waiting for first child\n", vd_root );
}

#else  /* !(SONAME_LIBXCOMPOSITE && SONAME_LIBXRENDER) */

void vd_compositor_init( Display *display, Window vd_root ) { /* no composite/render support built in */ }
BOOL vd_compositor_notify( XEvent *event ) { return FALSE; }
void vd_compositor_paint( Display *display ) { }

#endif
