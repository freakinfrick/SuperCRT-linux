// Desktop region grabber for the "clean" source image.
//
// The reference implementation BitBlts from the desktop DC under a color-keyed source
// window.  The X11 equivalent is a screen grab of the same rectangle on the root window,
// using MIT-SHM when it is available (shared memory, no X protocol copy) and plain
// XGetImage otherwise.
//
// libXext is dlopen'd rather than linked: XShm/XShape headers ship in libxext-dev, which
// this port does not require, and the ABI of the two functions used is stable.
#pragma once

#include <X11/Xlib.h>

typedef struct Capture Capture;

Capture *Capture_Create(Display *dpy, int screen);
// Forces the XGetImage path; for hosts where MIT-SHM is broken (remote X, some
// sandboxes) or for testing the fallback.
void Capture_DisableShm(Capture *c);
void Capture_Destroy(Capture *c);

typedef struct {
    int x, y, width, height; // region actually grabbed (clipped to the screen)
    int offset_x, offset_y;  // where that region sits inside the requested rect
} CaptureRegion;

// Grabs (x, y, width, height) from the root window.  Returns 1 on success and fills
// *out; returns 0 when the rect falls entirely off screen (the caller should then treat
// the source image as empty).
int Capture_Grab(Capture *c, int x, int y, int width, int height, CaptureRegion *out);

// Grabs from an arbitrary drawable instead of the root -- used to re-read the pixels under the
// viewer from the window beneath it.  The region must already lie inside `d`: there is no screen
// clipping here, because the caller has intersected it.  Only root grabs use MIT-SHM; a window
// grab always goes through XGetImage, so a refusal cannot disable sharing for the common path.
int Capture_GrabDrawable(Capture *c, Drawable d, int x, int y, int width, int height,
                         CaptureRegion *out);

// Grabs from a *window*, whose contents are read through XComposite rather than off the screen.
// A window covered by another has no readable pixels of its own -- on a plain server without a
// compositor, reading one returns black even when it is fully visible -- so the window is
// redirected to an offscreen pixmap first, which is what makes "what is underneath the viewer"
// answerable at all.  Returns 0 when the composite extension is missing or the window cannot be
// redirected, in which case the caller should leave those pixels alone rather than invent them.
int Capture_GrabWindow(Capture *c, Window win, int x, int y, int width, int height,
                       CaptureRegion *out);

// Buffer describing the most recent grab: BGRX (or BGR) rows, top row first, which is
// exactly what GL_BGRA/GL_BGR uploads expect.
unsigned char *Capture_Data(const Capture *c);
int Capture_PixelsPerLine(const Capture *c);
int Capture_BitsPerPixel(const Capture *c);
int Capture_UsesShm(const Capture *c);
int Capture_ScreenWidth(const Capture *c);
int Capture_ScreenHeight(const Capture *c);
