// See marker.h.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xutil.h>

#include "marker.h"

// X11/extensions/shape.h constants and entry points, restated (libxext-dev is not a
// build requirement; libXext is dlopen'd).  Values matter: ShapeBounding is 0 and
// ShapeInput is 2, so swapping them silently blanks the window.
#define SHAPE_SET 0
#define SHAPE_UNSORTED 0
#define SHAPE_BOUNDING 0
#define SHAPE_CLIP 1
#define SHAPE_INPUT 2

// Room outside the sampled rectangle for the frame and the brackets.
#define MARKER_PAD 9
#define MARKER_BRACKET 8
#define MARKER_BRACKET_THICK 2

#define COLOR_IDLE 0x009a8f7cu
#define COLOR_ACTIVE 0x00ffc82bu
#define COLOR_EDGE 0x00000000u

struct Marker {
    Display *dpy;
    Window win;
    GC gc;
    int screen;
    int x, y, width, height; // window geometry (region + padding)
    int enabled;
    int interactive;
    int supported;
    void *xext;
    void (*pCombineRectangles)(Display *, Window, int, int, int, XRectangle *, int, int, int);
};

Marker *Marker_Create(Display *dpy, int screen)
{
    Marker *m = calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->dpy = dpy;
    m->screen = screen;

    m->xext = dlopen("libXext.so.6", RTLD_NOW | RTLD_LOCAL);
    if (m->xext) {
        m->pCombineRectangles =
            (void (*)(Display *, Window, int, int, int, XRectangle *, int, int, int))
                dlsym(m->xext, "XShapeCombineRectangles");
    }

    Window root = RootWindow(dpy, screen);
    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.background_pixmap = None;
    attrs.event_mask = ExposureMask;

    m->win = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixmap | CWEventMask, &attrs);

    XGCValues gcv;
    memset(&gcv, 0, sizeof(gcv));
    gcv.foreground = COLOR_IDLE;
    m->gc = XCreateGC(dpy, m->win, GCForeground, &gcv);

    m->supported = m->pCombineRectangles != NULL;
    if (!m->supported) {
        fprintf(stderr, "supercrt: libXext/XShape unavailable; target outline disabled\n");
        return m;
    }
    m->width = 1;
    m->height = 1;
    Marker_SetRect(m, 0, 0, 1, 1);
    return m;
}

void Marker_Destroy(Marker *m)
{
    if (!m) {
        return;
    }
    if (m->win) {
        XDestroyWindow(m->dpy, m->win);
    }
    if (m->gc) {
        XFreeGC(m->dpy, m->gc);
    }
    if (m->xext) {
        dlclose(m->xext);
    }
    free(m);
}

int Marker_IsSupported(const Marker *m) { return m->supported; }

// A ring of four strips hugging the sampled rectangle from the outside, `t` pixels thick.
// Never overlaps the rectangle itself, so nothing the marker paints can be captured.
static void Marker_Ring(const Marker *m, short t, XRectangle out[4])
{
    const short px = MARKER_PAD;
    const short w = (short)m->width;
    const short h = (short)m->height;
    const short span = (short)(w - 2 * px + 2 * t);

    out[0] = (XRectangle){ (short)(px - t), (short)(px - t), (unsigned short)span, (unsigned short)t };
    out[1] = (XRectangle){ (short)(px - t), (short)(h - px), (unsigned short)span, (unsigned short)t };
    out[2] = (XRectangle){ (short)(px - t), (short)px, (unsigned short)t, (unsigned short)(h - 2 * px) };
    out[3] = (XRectangle){ (short)(w - px), (short)px, (unsigned short)t, (unsigned short)(h - 2 * px) };
}

static void Marker_Brackets(const Marker *m, XRectangle out[8])
{
    const short px = MARKER_PAD;
    const short b = MARKER_BRACKET;
    const short k = MARKER_BRACKET_THICK;
    const short x0 = px;                 // sampled rectangle, in window coordinates
    const short y0 = px;
    const short x1 = (short)(m->width - px);
    const short y1 = (short)(m->height - px);

    // Each corner is an L lying outside the rectangle: one arm along the top/bottom edge,
    // one along the left/right edge.
    out[0] = (XRectangle){ (short)(x0 - b), (short)(y0 - k), b, k };
    out[1] = (XRectangle){ (short)(x0 - k), (short)(y0 - b), k, b };
    out[2] = (XRectangle){ x1, (short)(y0 - k), b, k };
    out[3] = (XRectangle){ x1, (short)(y0 - b), k, b };
    out[4] = (XRectangle){ (short)(x0 - b), y1, b, k };
    out[5] = (XRectangle){ (short)(x0 - k), y1, k, b };
    out[6] = (XRectangle){ x1, y1, b, k };
    out[7] = (XRectangle){ x1, y1, k, b };
}

static void Marker_ApplyShapes(Marker *m)
{
    XRectangle rects[8 + 8];
    int count = 0;
    if (!m->supported) {
        return;
    }

    Marker_Ring(m, (short)(m->interactive ? 3 : 2), rects + count);
    count += 4;
    if (m->interactive) {
        Marker_Brackets(m, rects + count);
        count += 8;
    }

    m->pCombineRectangles(m->dpy, m->win, SHAPE_BOUNDING, 0, 0, rects, count, SHAPE_SET,
                          SHAPE_UNSORTED);

    // Empty input shape: clicks and focus pass straight through to whatever is below.
    m->pCombineRectangles(m->dpy, m->win, SHAPE_INPUT, 0, 0, NULL, 0, SHAPE_SET, SHAPE_UNSORTED);
}

void Marker_WindowRect(int x, int y, int width, int height, int *out_x, int *out_y, int *out_w,
                       int *out_h)
{
    *out_x = x - MARKER_PAD;
    *out_y = y - MARKER_PAD;
    *out_w = width + 2 * MARKER_PAD;
    *out_h = height + 2 * MARKER_PAD;
}

void Marker_SetRect(Marker *m, int x, int y, int width, int height)
{
    if (!m || !m->supported || width <= 0 || height <= 0) {
        return;
    }
    int win_x, win_y, win_w, win_h;
    Marker_WindowRect(x, y, width, height, &win_x, &win_y, &win_w, &win_h);
    if (win_x == m->x && win_y == m->y && win_w == m->width && win_h == m->height) {
        return;
    }
    m->x = win_x;
    m->y = win_y;
    m->width = win_w;
    m->height = win_h;

    XMoveResizeWindow(m->dpy, m->win, win_x, win_y, (unsigned int)win_w, (unsigned int)win_h);
    Marker_ApplyShapes(m);
    Marker_Redraw(m);
}

void Marker_SetInteractive(Marker *m, int interactive)
{
    if (!m || !m->supported || m->interactive == interactive) {
        return;
    }
    m->interactive = interactive;
    Marker_ApplyShapes(m);
    if (m->enabled) {
        Marker_Raise(m);
    }
    Marker_Redraw(m);
}

void Marker_SetEnabled(Marker *m, int enabled)
{
    if (!m || !m->supported || enabled == m->enabled) {
        return;
    }
    m->enabled = enabled;
    if (enabled) {
        XMapRaised(m->dpy, m->win);
        Marker_Redraw(m);
    } else {
        XUnmapWindow(m->dpy, m->win);
    }
}

void Marker_Raise(Marker *m)
{
    if (!m || !m->supported || !m->enabled) {
        return;
    }
    XRaiseWindow(m->dpy, m->win);
}

void Marker_Redraw(Marker *m)
{
    if (!m || !m->supported || !m->enabled) {
        return;
    }

    const unsigned int band = m->interactive ? COLOR_ACTIVE : COLOR_IDLE;
    XRectangle ring[4];
    const short thickness = (short)(m->interactive ? 3 : 2);

    XSetForeground(m->dpy, m->gc, band);
    Marker_Ring(m, thickness, ring);
    for (int i = 0; i < 4; ++i) {
        XFillRectangle(m->dpy, m->win, m->gc, ring[i].x, ring[i].y, ring[i].width, ring[i].height);
    }

    // Hairline immediately against the sampled pixels: keeps the frame readable over both
    // bright and dark desktops.
    XSetForeground(m->dpy, m->gc, COLOR_EDGE);
    Marker_Ring(m, 1, ring);
    for (int i = 0; i < 4; ++i) {
        XFillRectangle(m->dpy, m->win, m->gc, ring[i].x, ring[i].y, ring[i].width, ring[i].height);
    }

    if (m->interactive) {
        XRectangle brackets[8];
        Marker_Brackets(m, brackets);
        XSetForeground(m->dpy, m->gc, band);
        for (int i = 0; i < 8; ++i) {
            XFillRectangle(m->dpy, m->win, m->gc, brackets[i].x, brackets[i].y, brackets[i].width,
                           brackets[i].height);
        }
    }
    XFlush(m->dpy);
}
