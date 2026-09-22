// XShape access, shared by the sim window (input shape) and the target-area outline
// (bounding + input shape).
//
// libXext is dlopen'd rather than linked, following gl_api: libxext-dev is not a build
// requirement, and a missing extension should disable shaping rather than stop the binary
// from starting.  Open one handle per process and hand it to both users.
#pragma once

#include <X11/Xlib.h>

typedef struct XShapeApi XShapeApi;

// Returns NULL only when allocation fails; check XShape_Supported for the extension itself.
XShapeApi *XShape_Open(void);
void XShape_Close(XShapeApi *api);
// False when libXext or XShape is unavailable, in which case every call below is a no-op.
int XShape_Supported(const XShapeApi *api);

// Bounding shape: the region the window actually paints.  Anything outside it is not drawn
// and not part of the window for stacking purposes.
void XShape_SetBounding(XShapeApi *api, Display *dpy, Window win, const XRectangle *rects,
                        int count);

// Input shape: an empty region makes the window transparent to the pointer, so clicks,
// drags and focus pass through to whatever is underneath.
void XShape_SetEmptyInput(XShapeApi *api, Display *dpy, Window win);
// Restores a full-window input region of width x height, with the origin at the window's
// top-left corner.  Re-apply after a resize: the region does not follow the geometry.
void XShape_SetFullInput(XShapeApi *api, Display *dpy, Window win, int width, int height);
