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

    XImage *image;
    int image_w, image_h;
    int image_owned_by_xlib;

    int use_shm;
    void *xext;
    Bool (*pQueryExtension)(Display *);
    XImage *(*pCreateImage)(Display *, Visual *, unsigned int, int, char *,
                            XShmSegmentInfoLocal *, unsigned int, unsigned int);
    Bool (*pAttach)(Display *, XShmSegmentInfoLocal *);
    Bool (*pDetach)(Display *, XShmSegmentInfoLocal *);
    Bool (*pGetImage)(Display *, Drawable, XImage *, int, int, unsigned long);
    XShmSegmentInfoLocal shminfo;
    int shm_attached;
};

static void Capture_FreeImage(Capture *c)
{
    if (!c->image) {
        return;
    }
    if (c->image_owned_by_xlib) {
        XDestroyImage(c->image);
    } else {
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
    c->image = NULL;
    c->image_owned_by_xlib = 0;
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
    if (c->xext) {
        dlclose(c->xext);
    }
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
        // Allocated per grab in Capture_Grab via XGetImage.
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

    const int grab_w = x1 - x0;
    const int grab_h = y1 - y0;

    if (!c->image || c->image_w != grab_w || c->image_h != grab_h) {
        if (c->use_shm) {
            if (!Capture_Resize(c, grab_w, grab_h)) {
                return 0;
            }
        } else {
            c->image_w = grab_w;
            c->image_h = grab_h;
        }
    }

    if (c->use_shm) {
        if (!c->pGetImage(c->dpy, c->root, c->image, x0, y0, AllPlanes)) {
            fprintf(stderr, "supercrt: XShmGetImage failed, falling back to XGetImage\n");
            c->use_shm = 0;
            Capture_FreeImage(c);
            c->image = NULL;
        }
    }
    if (!c->use_shm) {
        if (c->image) {
            XDestroyImage(c->image);
            c->image = NULL;
        }
        c->image = XGetImage(c->dpy, c->root, x0, y0, (unsigned int)grab_w, (unsigned int)grab_h,
                             AllPlanes, ZPixmap);
        if (!c->image) {
            fprintf(stderr, "supercrt: XGetImage failed\n");
            return 0;
        }
        c->image_owned_by_xlib = 1;
    }

    out->x = x0;
    out->y = y0;
    out->width = grab_w;
    out->height = grab_h;
    out->offset_x = x0 - x;
    out->offset_y = y0 - y;
    return 1;
}

unsigned char *Capture_Data(const Capture *c)
{
    return c->image ? (unsigned char *)c->image->data : NULL;
}

int Capture_PixelsPerLine(const Capture *c)
{
    if (!c->image) {
        return 0;
    }
    return c->image->bytes_per_line / (c->image->bits_per_pixel / 8);
}

int Capture_BitsPerPixel(const Capture *c)
{
    return c->image ? c->image->bits_per_pixel : 0;
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
