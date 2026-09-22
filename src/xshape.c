// See xshape.h.
#include <dlfcn.h>
#include <stdlib.h>

#include "xshape.h"

// X11/extensions/shape.h constants, restated.  Values matter: ShapeBounding is 0 and
// ShapeInput is 2, so swapping them silently blanks the window.
#define SHAPE_SET 0
#define SHAPE_UNSORTED 0
#define SHAPE_BOUNDING 0
#define SHAPE_INPUT 2

typedef void (*CombineRectanglesFn)(Display *, Window, int, int, int, XRectangle *, int, int,
                                    int);

struct XShapeApi {
    void *lib;
    CombineRectanglesFn combine_rectangles;
};

XShapeApi *XShape_Open(void)
{
    XShapeApi *api = calloc(1, sizeof(*api));
    if (!api) {
        return NULL;
    }

    api->lib = dlopen("libXext.so.6", RTLD_NOW | RTLD_LOCAL);
    if (api->lib) {
        api->combine_rectangles = (CombineRectanglesFn)dlsym(api->lib, "XShapeCombineRectangles");
    }
    return api;
}

void XShape_Close(XShapeApi *api)
{
    if (!api) {
        return;
    }
    // Deliberately not dlclose'd: Xlib holds per-extension callbacks that point into libXext
    // and walks them during XCloseDisplay, so unloading first risks jumping into freed code.
    // A standalone reproducer of that crash is easy to write, so the handle is released and
    // the library left mapped for the process lifetime instead.
    free(api);
}

int XShape_Supported(const XShapeApi *api)
{
    return api && api->combine_rectangles != NULL;
}

void XShape_SetBounding(XShapeApi *api, Display *dpy, Window win, const XRectangle *rects,
                        int count)
{
    if (!XShape_Supported(api)) {
        return;
    }
    api->combine_rectangles(dpy, win, SHAPE_BOUNDING, 0, 0, (XRectangle *)rects, count, SHAPE_SET,
                            SHAPE_UNSORTED);
}

void XShape_SetEmptyInput(XShapeApi *api, Display *dpy, Window win)
{
    if (!XShape_Supported(api)) {
        return;
    }
    api->combine_rectangles(dpy, win, SHAPE_INPUT, 0, 0, NULL, 0, SHAPE_SET, SHAPE_UNSORTED);
}

void XShape_SetFullInput(XShapeApi *api, Display *dpy, Window win, int width, int height)
{
    if (!XShape_Supported(api)) {
        return;
    }
    XRectangle full = { 0, 0, (unsigned short)(width > 0 ? width : 1),
                        (unsigned short)(height > 0 ? height : 1) };
    api->combine_rectangles(dpy, win, SHAPE_INPUT, 0, 0, &full, 1, SHAPE_SET, SHAPE_UNSORTED);
}
