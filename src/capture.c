// See capture.h.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xutil.h>

#include "capture.h"

// Xext/shm.h's XShmSegmentInfo, restated: libxext-dev is not a build requirement.
typedef unsigned long ShmSeg;
typedef struct {
    ShmSeg shmseg;
    int shmid;
    char *shmaddr;
    Bool readOnly;
} XShmSegmentInfoLocal;

struct Capture {
    Display *dpy;
    Window root;
    Visual *visual;
    int depth;
    int screen_w, screen_h;

    // Two slots, never mixed: `image` is the MIT-SHM image (its pixels live in the shm segment) and
    // `xlib_image` the fallback XGetImage result.  Sharing one slot let a fallback grab replace the
    // SHM image, after which the next SHM grab ran on a non-SHM image, failed, and switched SHM off
    // for good -- on every run that read a window beneath the viewer.  `last` is the slot the most
    // recent grab filled, which is what Capture_Data and friends describe.
    XImage *image;
    int image_w, image_h;
    XImage *xlib_image;
    XImage *last;

    int use_shm;
    int shm_drawables;   // SHM for window pixmaps too; cleared on the first refusal for one
    void *xext;
    Bool (*pQueryExtension)(Display *);
    XImage *(*pCreateImage)(Display *, Visual *, unsigned int, int, char *,
                            XShmSegmentInfoLocal *, unsigned int, unsigned int);
    Bool (*pAttach)(Display *, XShmSegmentInfoLocal *);
    Bool (*pDetach)(Display *, XShmSegmentInfoLocal *);
    Bool (*pGetImage)(Display *, Drawable, XImage *, int, int, unsigned long);
    XShmSegmentInfoLocal shminfo;
    int shm_attached;

    // XComposite, for reading a window's own pixels through its offscreen pixmap.
    void *xcomposite;
    Pixmap (*pNameWindowPixmap)(Display *, Window);
    void (*pRedirectWindow)(Display *, Window, int);
};

#define COMPOSITE_REDIRECT_AUTOMATIC 0

static volatile int g_composite_error;

static int Capture_QuietXError(Display *dpy, XErrorEvent *ev)
{
    (void)dpy;
    (void)ev;
    g_composite_error = 1;
    return 0;
}

static int g_shm_error, g_shm_req_major, g_shm_req_minor;

static int Capture_ShmXError(Display *dpy, XErrorEvent *ev)
{
    (void)dpy;
    g_shm_error = ev->error_code;
    g_shm_req_major = ev->request_code;
    g_shm_req_minor = ev->minor_code;
    return 0;
}

static void Capture_FreeImage(Capture *c)
{
    if (!c->image) {
        return;
    }
    {
        if (c->shm_attached) {
            c->pDetach(c->dpy, &c->shminfo);
            c->shm_attached = 0;
        }
        if (c->shminfo.shmaddr) {
            shmdt(c->shminfo.shmaddr);
            c->shminfo.shmaddr = NULL;
        }
        // The image struct itself came from XShmCreateImage; its data lives in the shm
        // segment, so only the struct may be freed.
        XFree(c->image);
    }
    if (c->last == c->image) {
        c->last = NULL;
    }
    c->image = NULL;
}

Capture *Capture_Create(Display *dpy, int screen)
{
    Capture *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->dpy = dpy;
    c->root = RootWindow(dpy, screen);
    c->visual = DefaultVisual(dpy, screen);
    c->depth = DefaultDepth(dpy, screen);
    c->screen_w = DisplayWidth(dpy, screen);
    c->screen_h = DisplayHeight(dpy, screen);

    // XComposite ships separately from Xext; without it a window's own pixels are unreadable and
    // the caller is told so rather than handed black.
    c->xcomposite = dlopen("libXcomposite.so.1", RTLD_NOW | RTLD_LOCAL);
    if (c->xcomposite) {
        c->pNameWindowPixmap =
            (Pixmap (*)(Display *, Window))dlsym(c->xcomposite, "XCompositeNameWindowPixmap");
        c->pRedirectWindow = (void (*)(Display *, Window, int))
            dlsym(c->xcomposite, "XCompositeRedirectWindow");
    }

    c->xext = dlopen("libXext.so.6", RTLD_NOW | RTLD_LOCAL);
    if (c->xext) {
        c->pQueryExtension = (Bool (*)(Display *))dlsym(c->xext, "XShmQueryExtension");
        c->pCreateImage = (XImage *(*)(Display *, Visual *, unsigned int, int, char *,
                                       XShmSegmentInfoLocal *, unsigned int, unsigned int))
            dlsym(c->xext, "XShmCreateImage");
        c->pAttach = (Bool (*)(Display *, XShmSegmentInfoLocal *))dlsym(c->xext, "XShmAttach");
        c->pDetach = (Bool (*)(Display *, XShmSegmentInfoLocal *))dlsym(c->xext, "XShmDetach");
        c->pGetImage = (Bool (*)(Display *, Drawable, XImage *, int, int, unsigned long))
            dlsym(c->xext, "XShmGetImage");
    }

    if (c->pQueryExtension && c->pCreateImage && c->pAttach && c->pDetach && c->pGetImage &&
        c->pQueryExtension(dpy)) {
        c->use_shm = 1;
        c->shm_drawables = 1;
    } else {
        fprintf(stderr, "supercrt: MIT-SHM unavailable, falling back to XGetImage "
                        "(slower for large capture regions)\n");
        c->use_shm = 0;
    }
    return c;
}

void Capture_Destroy(Capture *c)
{
    if (!c) {
        return;
    }
    Capture_FreeImage(c);
    if (c->xlib_image) {
        XDestroyImage(c->xlib_image);
    }
    // Both libraries are left loaded: Xlib keeps per-extension callbacks that point into them and
    // walks them during XCloseDisplay, so unloading here risks jumping into freed code.
    (void)c->xext;
    (void)c->xcomposite;
    free(c);
}

static int Capture_Resize(Capture *c, int width, int height)
{
    Capture_FreeImage(c);

    if (c->use_shm) {
        size_t bytes = (size_t)width * (size_t)height * 4;
        int shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (shmid < 0) {
            fprintf(stderr, "supercrt: shmget failed, falling back to XGetImage\n");
            c->use_shm = 0;
        } else {
            char *addr = shmat(shmid, NULL, 0);
            if (addr == (char *)-1) {
                shmctl(shmid, IPC_RMID, NULL);
                c->use_shm = 0;
            } else {
                memset(&c->shminfo, 0, sizeof(c->shminfo));
                c->shminfo.shmid = shmid;
                c->shminfo.shmaddr = addr;
                c->shminfo.readOnly = False;
                c->image = c->pCreateImage(c->dpy, c->visual, (unsigned int)c->depth, ZPixmap,
                                           addr, &c->shminfo, (unsigned int)width,
                                           (unsigned int)height);
                if (!c->image || !c->pAttach(c->dpy, &c->shminfo)) {
                    fprintf(stderr, "supercrt: XShm setup failed, falling back to XGetImage\n");
                    if (c->image) {
                        XFree(c->image);
                        c->image = NULL;
                    }
                    shmdt(addr);
                    c->shminfo.shmaddr = NULL;
                    shmctl(shmid, IPC_RMID, NULL);
                    c->use_shm = 0;
                } else {
                    c->shm_attached = 1;
                    // Release our reference; the segment lives until XShmDetach.
                    shmctl(shmid, IPC_RMID, NULL);
                }
            }
        }
    }

    if (!c->use_shm) {
        // SHM setup failed: grabs go to the xlib_image slot instead.
        c->image = NULL;
    }

    c->image_w = width;
    c->image_h = height;
    return 1;
}

int Capture_Grab(Capture *c, int x, int y, int width, int height, CaptureRegion *out)
{
    memset(out, 0, sizeof(*out));

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width;
    int y1 = y + height;
    if (x1 > c->screen_w) {
        x1 = c->screen_w;
    }
    if (y1 > c->screen_h) {
        y1 = c->screen_h;
    }
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }

    CaptureRegion grabbed;
    if (!Capture_GrabDrawable(c, c->root, x0, y0, x1 - x0, y1 - y0, &grabbed)) {
        return 0;
    }
    // The drawable variant reports the region in its own coordinates; the root's are the
    // screen's, so shift back to where the caller asked for.
    grabbed.offset_x += x0 - x;
    grabbed.offset_y += y0 - y;
    *out = grabbed;
    return 1;
}

int Capture_GrabDrawable(Capture *c, Drawable d, int x, int y, int width, int height,
                         CaptureRegion *out)
{
    memset(out, 0, sizeof(*out));
    if (width <= 0 || height <= 0 || x < 0 || y < 0) {
        return 0;
    }

    const int grab_w = width;
    const int grab_h = height;
    // MIT-SHM for the root and for window pixmaps of the screen's depth (the only kind the caller
    // reads).  A refusal for a pixmap stops sharing for pixmaps only; one for the root stops it all.
    int use_shm = c->use_shm && (d == c->root || c->shm_drawables);
    c->last = NULL;

    if (use_shm && (!c->image || c->image_w != grab_w || c->image_h != grab_h)) {
        if (!Capture_Resize(c, grab_w, grab_h)) {
            return 0;
        }
        use_shm = c->use_shm && c->image != NULL;
    }

    if (use_shm) {
        g_shm_error = 0;
        int (*previous)(Display *, XErrorEvent *) = XSetErrorHandler(Capture_ShmXError);
        const Bool ok = c->pGetImage(c->dpy, d, c->image, x, y, AllPlanes);
        XSync(c->dpy, False);
        XSetErrorHandler(previous);
        if (ok && !g_shm_error) {
            c->last = c->image;
        } else {
            fprintf(stderr, "supercrt: XShmGetImage failed on the %s (X error %d, request %d.%d, "
                            "%dx%d at %d,%d), falling back to XGetImage for %s\n",
                    d == c->root ? "root" : "window pixmap", g_shm_error, g_shm_req_major,
                    g_shm_req_minor, grab_w, grab_h, x, y,
                    d == c->root ? "everything" : "window pixmaps");
            if (d == c->root) {
                c->use_shm = 0;
                Capture_FreeImage(c);
            } else {
                c->shm_drawables = 0;
            }
        }
    }
    if (!c->last) {
        if (c->xlib_image) {
            XDestroyImage(c->xlib_image);
        }
        c->xlib_image = XGetImage(c->dpy, d, x, y, (unsigned int)grab_w, (unsigned int)grab_h,
                                  AllPlanes, ZPixmap);
        if (!c->xlib_image) {
            fprintf(stderr, "supercrt: XGetImage failed\n");
            return 0;
        }
        c->last = c->xlib_image;
    }

    out->x = x;
    out->y = y;
    out->x = x;
    out->y = y;
    out->width = grab_w;
    out->height = grab_h;
    out->offset_x = 0; // nothing was clipped away: this grabbed exactly what was asked for
    out->offset_y = 0;
    return 1;
}

// The pixmap holding this window's own pixels, redirecting it first if nobody has.  NameWindowPixmap
// fails (BadMatch) on a window that is not redirected, and an error handler that does not exit is
// required for that probe: Xlib's default one ends the process.
static Pixmap Capture_WindowPixmap(Capture *c, Window win)
{
    if (!c->pNameWindowPixmap || !c->pRedirectWindow) {
        return None;
    }

    int (*previous)(Display *, XErrorEvent *) = XSetErrorHandler(Capture_QuietXError);
    g_composite_error = 0;
    Pixmap px = c->pNameWindowPixmap(c->dpy, win);
    XSync(c->dpy, False);

    if (!px || g_composite_error) {
        // Not redirected yet: do it ourselves.  Automatic redirection keeps the window on screen
        // exactly as before while also keeping its contents in an offscreen pixmap.
        g_composite_error = 0;
        c->pRedirectWindow(c->dpy, win, COMPOSITE_REDIRECT_AUTOMATIC);
        XSync(c->dpy, False);
        px = c->pNameWindowPixmap(c->dpy, win);
        XSync(c->dpy, False);
        if (g_composite_error) {
            px = None;
        }
    }
    XSetErrorHandler(previous);
    return px;
}

int Capture_GrabWindow(Capture *c, Window win, int x, int y, int width, int height,
                       CaptureRegion *out)
{
    memset(out, 0, sizeof(*out));
    const Pixmap px = Capture_WindowPixmap(c, win);
    if (px == None) {
        return 0;
    }
    // NameWindowPixmap hands out a new pixmap on every call; not freeing it leaked one per window
    // per frame, each pinning the window's old storage after a resize.
    const int ok = Capture_GrabDrawable(c, px, x, y, width, height, out);
    XFreePixmap(c->dpy, px);
    return ok;
}

unsigned char *Capture_Data(const Capture *c)
{
    return c->last ? (unsigned char *)c->last->data : NULL;
}

int Capture_PixelsPerLine(const Capture *c)
{
    if (!c->last) {
        return 0;
    }
    return c->last->bytes_per_line / (c->last->bits_per_pixel / 8);
}

int Capture_BitsPerPixel(const Capture *c)
{
    return c->last ? c->last->bits_per_pixel : 0;
}

void Capture_DisableShm(Capture *c)
{
    if (!c || !c->use_shm) {
        return;
    }
    Capture_FreeImage(c);
    c->use_shm = 0;
}

int Capture_UsesShm(const Capture *c) { return c->use_shm; }
int Capture_ScreenWidth(const Capture *c) { return c->screen_w; }
int Capture_ScreenHeight(const Capture *c) { return c->screen_h; }
