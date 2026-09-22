// Outline for the target (capture) area.
//
// The reference implementation places a color-keyed "source window" over the pixels it
// samples; the X11 port draws the same affordance as an override-redirect window whose
// bounding shape is a frame plus corner brackets and whose input shape is empty, so it
// never takes focus or clicks and never appears inside the capture it frames.
//
// Every painted pixel lives *outside* the sampled rectangle: the frame hugs the rectangle
// from the outside and the brackets sit outside the corners.  That keeps the outline out
// of the captured image no matter how thick it is drawn.
#pragma once

#include <X11/Xlib.h>

typedef struct Marker Marker;

Marker *Marker_Create(Display *dpy, int screen);
void Marker_Destroy(Marker *m);

void Marker_SetEnabled(Marker *m, int enabled);
// Frame the rectangle (x, y, width, height) on the root window.
void Marker_SetRect(Marker *m, int x, int y, int width, int height);
// Adjustment mode: thicker, brighter frame with corner brackets and a grab cursor, so the
// area is obvious while it is being moved and resized.
void Marker_SetInteractive(Marker *m, int interactive);
void Marker_Raise(Marker *m);
// Redraws the frame; call when handling an Expose event.
void Marker_Redraw(Marker *m);
int Marker_IsSupported(const Marker *m);

// Window geometry the marker wants for a region, exposed for tests/tools.
void Marker_WindowRect(int x, int y, int width, int height, int *out_x, int *out_y, int *out_w,
                       int *out_h);
