// SuperCRT (Linux port) -- samples a region of the desktop and draws it back out through
// the CRT simulation from Super Win the Game.
//
// Pipeline (identical to the CC0 CRTSim reference's Render(), see assets/CC0-COPYING.txt):
//   1. grab the desktop region into the "clean" texture
//   2. composite pass: persistence/bleed, NTSC artifacts, unsharp mask -> even/odd RTTs
//   3. screen mesh + frame mesh, lit and masked, into the full-size buffer
//   4. downsample blur of that buffer, 5. upsample blur  6. bloom composite onto the window
//
// D3D9 -> OpenGL mapping notes are inline where the two APIs disagree (texture
// orientation, cull winding, half-pixel offsets, per-sampler filter states).

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <GL/glx.h>

#include "assets.h"
#include "capture.h"
#include "gl_api.h"
#include "marker.h"
#include "params.h"
#include "shader.h"
#include "shaders.h"
#include "ui.h"

#define DEG2RAD 0.017453292519943295f
#define DEFAULT_FOV_DEGREES 15.0f

// ---------------------------------------------------------------------------
// Small math helpers (column-major, matching GLSL's mat4 layout)
// ---------------------------------------------------------------------------

typedef struct { float x, y, z; } Vec3;

static void M4_Identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void M4_Multiply(float out[16], const float a[16], const float b[16])
{
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + r] * b[c * 4 + k];
            }
            out[c * 4 + r] = sum;
        }
    }
}

// Right-handed look-at, same handedness as D3DXMatrixLookAtRH.
static void M4_LookAtRH(float m[16], Vec3 eye, Vec3 center, Vec3 up)
{
    Vec3 f = { center.x - eye.x, center.y - eye.y, center.z - eye.z };
    float len = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
    f.x /= len; f.y /= len; f.z /= len;

    Vec3 s = { f.y * up.z - f.z * up.y, f.z * up.x - f.x * up.z, f.x * up.y - f.y * up.x };
    len = sqrtf(s.x * s.x + s.y * s.y + s.z * s.z);
    s.x /= len; s.y /= len; s.z /= len;

    Vec3 u = { s.y * f.z - s.z * f.y, s.z * f.x - s.x * f.z, s.x * f.y - s.y * f.x };

    M4_Identity(m);
    m[0] = s.x;  m[4] = s.y;  m[8]  = s.z;
    m[1] = u.x;  m[5] = u.y;  m[9]  = u.z;
    m[2] = -f.x; m[6] = -f.y; m[10] = -f.z;
    m[12] = -(s.x * eye.x + s.y * eye.y + s.z * eye.z);
    m[13] = -(u.x * eye.x + u.y * eye.y + u.z * eye.z);
    m[14] = (f.x * eye.x + f.y * eye.y + f.z * eye.z);
}

// GL-convention perspective (clip z in [-1,1]).  Only the z mapping differs from
// D3DXMatrixPerspectiveFovRH; x/y -- and therefore everything on screen -- is identical,
// and depth ordering is preserved.
static void M4_PerspectiveRH(float m[16], float fov_y, float aspect, float znear, float zfar)
{
    const float f = 1.0f / tanf(fov_y * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1.0f;
    m[14] = (2.0f * zfar * znear) / (znear - zfar);
}

// ---------------------------------------------------------------------------
// GL resources
// ---------------------------------------------------------------------------

typedef struct {
    unsigned fbo, tex, depth;
    int width, height;
} Target;

typedef struct {
    unsigned vao;
    unsigned buffers[5];
    unsigned index_buffer;
    int num_indices;
} MeshGL;

typedef struct {
    unsigned program;
    int RcpScrWidth, RcpScrHeight, Tuning_Sharp, Tuning_Persistence, Tuning_Bleed,
        Tuning_Artifacts, curFrameMap, prevFrameMap, NTSCArtifactTex, NTSCLerp;
} CompositePass;

typedef struct {
    unsigned program;
    int wvpMat, camPos, Tuning_LightPos, UVScalar, UVOffset, CRTMask_Scale, Tuning_Overscan,
        Tuning_Dimming, Tuning_Satur, Tuning_ReflScalar, Tuning_Barrel, Tuning_Mask_Brightness,
        Tuning_Mask_Opacity, Tuning_Diff_Brightness, Tuning_Spec_Brightness, Tuning_Spec_Power,
        Tuning_Fres_Brightness, Tuning_FrameColor, compFrameMap, shadowMaskMap;
} MeshPass;

typedef struct {
    unsigned program;
    int uSource, BloomScale, Upsample;
} PostPass;

typedef struct {
    unsigned program;
    int PreBloomBuffer, UpsampledBuffer, BloomScalar, BloomPower;
} PresentPass;

typedef struct { float x, y, w, h; } Rectf;

// Set SUPERCRT_TRACE=1 to log pointer input and mode transitions; useful when a remote or
// grabbed pointer behaves differently from a local one.
static int g_trace = -1;

static void Trace(const char *fmt, ...)
{
    if (g_trace < 0) {
        const char *env = getenv("SUPERCRT_TRACE");
        g_trace = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
    }
    if (!g_trace) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static int RectContains(Rectf r, float x, float y)
{
    return r.w > 0.0f && r.h > 0.0f && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// One clickable row of the settings panel.
typedef struct {
    Rectf row;
    Rectf slider;   // value area; empty for action rows
    int index;      // tunable index, or tunable_count + action index
    int is_action;
} OverlayHit;

// Which part of the target frame a point grabs.  The margin is generous so edges and
// corners are easy to hit with a remote/touch pointer.
typedef enum {
    ZONE_OUTSIDE = 0,
    ZONE_MOVE,
    ZONE_LEFT,
    ZONE_RIGHT,
    ZONE_TOP,
    ZONE_BOTTOM,
    ZONE_TOP_LEFT,
    ZONE_TOP_RIGHT,
    ZONE_BOTTOM_LEFT,
    ZONE_BOTTOM_RIGHT,
} RegionZone;

typedef struct {
    // X11 / GLX
    Display *dpy;
    int screen;
    Window root;
    Window win;
    Colormap colormap;
    XVisualInfo *visual_info;
    GLXFBConfig fbconfig;
    GLXContext ctx;
    Atom wm_delete_window;
    Atom net_wm_state;
    Atom net_wm_state_fullscreen;

    int width, height;      // current drawable size (== Dst dims)
    int windowed_width, windowed_height;
    int mode;               // 0 windowed, 1 fullscreen (EWMH), 2 borderless
    int window_is_override_redirect;
    int screen_width, screen_height;
    int using_shm;
    int quit;

    // GL objects
    Target comp[2];
    Target full, down, up;
    unsigned clean_tex, artifacts_tex, mask_tex;
    unsigned clear_fbo;
    unsigned samp_point_clamp, samp_point_repeat, samp_linear_clamp, samp_linear_repeat,
        samp_linear_border;
    CompositePass composite;
    MeshPass screen_pass, frame_pass;
    PostPass post;
    PresentPass present;
    MeshGL screen_mesh, frame_mesh;
    unsigned empty_vao;
    unsigned full_quad_vbo;

    // Per-frame scratch
    Capture *capture;
    Marker *marker;
    UI ui;
    unsigned char *pattern;
    int frame_index;
    int even_frame;
    int overlay_open;
    int overlay_selected;
    int capture_outline_enabled;
    double last_fps_time;
    int fps_frames;
    double fps;
    char status[160];
    int status_frames;
    char config_path[4096];
    int dirty_settings;

    // --- mouse state ---
    int pointer_inside;
    Rectf hud_settings, hud_target, hud_close;
    OverlayHit overlay_hits[64];
    int overlay_hit_count;
    int slider_row;         // tunable index being dragged, -1 when idle
    int slider_start_x;
    float slider_start_value;
    int edit_mode;          // target-area drag session
    int region_drag_zone;
    int region_drag_start_x, region_drag_start_y;
    int region_start_rect[4];
    int src_alloc_w, src_alloc_h;
    Cursor cursor_fleur, cursor_h, cursor_v, cursor_nw, cursor_ne, cursor_sw, cursor_se;
} App;

static const float kQuadPositions[3][2] = {
    { -1.0f, 1.0f }, { -1.0f, -3.0f }, { 3.0f, 1.0f },
};

// ---------------------------------------------------------------------------
// Resource helpers
// ---------------------------------------------------------------------------

static void Target_Free(Target *t)
{
    if (t->depth) {
        glDeleteRenderbuffers(1, &t->depth);
    }
    if (t->fbo) {
        glDeleteFramebuffers(1, &t->fbo);
    }
    if (t->tex) {
        glDeleteTextures(1, &t->tex);
    }
    memset(t, 0, sizeof(*t));
}

static int Target_Create(Target *t, int width, int height, const char *name, int with_depth)
{
    memset(t, 0, sizeof(*t));
    t->width = width;
    t->height = height;

    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);

    if (with_depth) {
        glGenRenderbuffers(1, &t->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "supercrt: framebuffer '%s' incomplete (%#x)\n", name, status);
        return 0;
    }
    return 1;
}

static void Target_Clear(Target *t, float r, float g, float b, float a, int clear_depth)
{
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->width, t->height);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (clear_depth && t->depth) {
        glDepthMask(GL_TRUE);
    }
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | (clear_depth && t->depth ? GL_DEPTH_BUFFER_BIT : 0));
}

// Uploads a bitmap with row 0 = top of the image, which is the orientation every shader
// in this port assumes (the reference's D3D textures have v = 0 at the image top).
static unsigned CreateTextureFromBitmap(const Bitmap *bitmap, int repeat, int linear)
{
    unsigned tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bitmap->width, bitmap->height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, bitmap->rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static unsigned CreateSampler(int wrap, int linear, int border)
{
    unsigned sampler = 0;
    glGenSamplers(1, &sampler);
    const int filter = linear ? GL_LINEAR : GL_NEAREST;
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, filter);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, filter);
    const int wrap_mode = border ? GL_CLAMP_TO_BORDER : (wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, wrap_mode);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, wrap_mode);
    if (border) {
        // crtbase.fx: BorderColor = 0xff000000.
        const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, black);
    }
    return sampler;
}

static int MeshGL_Upload(MeshGL *g, const Mesh *mesh)
{
    memset(g, 0, sizeof(*g));
    g->num_indices = mesh->num_indices;

    glGenVertexArrays(1, &g->vao);
    glBindVertexArray(g->vao);

    const int count = mesh->num_vertices;
    const void *sources[5] = { mesh->positions, mesh->normals, mesh->colors, mesh->uv0, mesh->uv1 };
    const int components[5] = { 3, 3, 4, 2, 1 };
    const int is_color[5] = { 0, 0, 1, 0, 0 };

    for (int i = 0; i < 5; ++i) {
        if (!sources[i] || components[i] == 0) {
            continue;
        }
        const size_t stride = (size_t)components[i] * (is_color[i] ? sizeof(unsigned char) : sizeof(float));
        glGenBuffers(1, &g->buffers[i]);
        glBindBuffer(GL_ARRAY_BUFFER, g->buffers[i]);
        glBufferData(GL_ARRAY_BUFFER, stride * (size_t)count, sources[i], GL_STATIC_DRAW);
        glEnableVertexAttribArray((unsigned)i);
        glVertexAttribPointer((unsigned)i, components[i],
                              is_color[i] ? GL_UNSIGNED_BYTE : GL_FLOAT,
                              is_color[i] ? GL_TRUE : GL_FALSE, (GLsizei)stride, (void *)0);
    }

    // Bound while the VAO is active, so the VAO records it for DrawElements.
    glGenBuffers(1, &g->index_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)mesh->num_indices * sizeof(uint16_t),
                 mesh->indices, GL_STATIC_DRAW);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    return Gl_Check("MeshGL_Upload") == GL_NO_ERROR;
}

static void MeshGL_Draw(const MeshGL *g)
{
    glBindVertexArray(g->vao);
    glDrawElements(GL_TRIANGLES, g->num_indices, GL_UNSIGNED_SHORT, (void *)0);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Asset paths
// ---------------------------------------------------------------------------

static int ResolveAsset(char *out, size_t out_size, const char *filename)
{
    // Search order: the tree layout (assets/ beside the binary, as in the Windows
    // release), then ../share/supercrt/assets for `make install`.
    static const char *suffixes[] = { "assets/%s", "../assets/%s", "../../assets/%s",
                                      "../share/supercrt/assets/%s" };
    const char *exe_dir = Params_ExeDir();

    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        if (exe_dir) {
            snprintf(out, out_size, "%s/%s", exe_dir, "");
            size_t len = strlen(out);
            snprintf(out + len, out_size - len, suffixes[i], filename);
        } else {
            snprintf(out, out_size, suffixes[i], filename);
        }
        if (access(out, R_OK) == 0) {
            return 1;
        }
    }
    // Last resort: relative to the current directory.
    snprintf(out, out_size, "%s", filename);
    return access(out, R_OK) == 0;
}

// ---------------------------------------------------------------------------
// Test pattern (--pattern): deterministic stand-in for the captured desktop
// ---------------------------------------------------------------------------

static void FillPattern(unsigned char *px, int width, int height, int frame)
{
    static const unsigned char bars[8][3] = {
        { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
        { 255, 0, 255 }, { 255, 0, 0 }, { 0, 0, 255 }, { 16, 16, 16 },
    };

    for (int y = 0; y < height; ++y) {
        unsigned char *row = px + (size_t)y * (size_t)width * 4;
        for (int x = 0; x < width; ++x) {
            unsigned char *p = row + (size_t)x * 4;
            const int bar = (x * 8) / width;
            if (y < height / 8) {
                // Horizontal ramp: makes scanlines and the shadow mask easy to inspect.
                p[0] = p[1] = p[2] = (unsigned char)(x * 255 / (width > 1 ? width - 1 : 1));
            } else if (y < height / 4) {
                // 1px checkerboard: shows chroma bleed and unsharp halo.
                const unsigned char v = ((x + y) & 1) ? 235 : 20;
                p[0] = p[1] = p[2] = v;
            } else {
                p[0] = bars[bar][0];
                p[1] = bars[bar][1];
                p[2] = bars[bar][2];
                if (y < height / 2) {
                    // Dim the bars so the upper half reads as a different field.
                    p[0] = (unsigned char)(p[0] / 3);
                    p[1] = (unsigned char)(p[1] / 3);
                    p[2] = (unsigned char)(p[2] / 3);
                }
            }
            p[3] = 255;
        }
    }

    // A block sweeping left to right: persistence trails and NTSC shimmer show here.
    const int size = height / 8;
    const int span = width + size * 2;
    const int bx = (frame * (span / 120)) % span - size;
    const int by = height / 2 - size / 2;
    for (int y = by; y < by + size; ++y) {
        if (y < 0 || y >= height) {
            continue;
        }
        for (int x = bx; x < bx + size; ++x) {
            if (x < 0 || x >= width) {
                continue;
            }
            unsigned char *p = px + ((size_t)y * (size_t)width + (size_t)x) * 4;
            p[0] = 250; p[1] = 250; p[2] = 250; p[3] = 255;
        }
    }
}

// ---------------------------------------------------------------------------
// PPM dump (verification aid)
// ---------------------------------------------------------------------------

static int WritePPM(const char *path, int width, int height, const unsigned char *rgba)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "supercrt: cannot write %s\n", path);
        return 0;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int y = height - 1; y >= 0; --y) { // GL rows run bottom-up
        const unsigned char *row = rgba + (size_t)y * (size_t)width * 4;
        for (int x = 0; x < width; ++x) {
            fwrite(row + (size_t)x * 4, 1, 3, f);
        }
    }
    fclose(f);
    return 1;
}

// ---------------------------------------------------------------------------
// Window / GLX
// ---------------------------------------------------------------------------

typedef void (*PFNSwapIntervalEXT)(Display *, GLXDrawable, int);
typedef int (*PFNSwapIntervalMESA)(unsigned);
typedef int (*PFNSwapIntervalSGI)(int);

static void App_ApplySwapInterval(App *a, int interval)
{
    PFNSwapIntervalEXT ext = (PFNSwapIntervalEXT)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    PFNSwapIntervalMESA mesa = (PFNSwapIntervalMESA)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalMESA");
    PFNSwapIntervalSGI sgi = (PFNSwapIntervalSGI)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalSGI");
    if (ext) {
        ext(a->dpy, a->win, interval);
    } else if (mesa) {
        mesa((unsigned)interval);
    } else if (sgi) {
        sgi(interval);
    } else {
        fprintf(stderr, "supercrt: no swap-control extension; frames are not vsynced\n");
    }
}

static void App_CreateWindow(App *a, int override_redirect, int width, int height)
{
    // Windowed placement mirrors the reference, which puts its destination window to the
    // right of the source region: overlapping the sampled rectangle would feed the sim
    // back into itself.
    int x = 0, y = 0;
    if (!override_redirect) {
        if (g_params.SrcX + g_params.SrcWidth + 64 + width <= a->screen_width) {
            x = g_params.SrcX + g_params.SrcWidth + 64;
            y = g_params.SrcY;
        } else if (g_params.SrcY + g_params.SrcHeight + 64 + height <= a->screen_height) {
            x = g_params.SrcX;
            y = g_params.SrcY + g_params.SrcHeight + 64;
        } else {
            // No room beside the region on this screen; tuck the window into the far
            // corner rather than on top of the pixels it is sampling.
            x = a->screen_width - width - 32;
            y = a->screen_height - height - 32;
            if (x < 0) {
                x = 0;
            }
            if (y < 0) {
                y = 0;
            }
        }
    }
    if (a->win) {
        glXMakeCurrent(a->dpy, None, NULL);
        XDestroyWindow(a->dpy, a->win);
        a->win = 0;
    }

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.colormap = a->colormap;
    attrs.background_pixel = BlackPixel(a->dpy, a->screen);
    attrs.border_pixel = 0;
    attrs.override_redirect = override_redirect ? True : False;
    attrs.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask | FocusChangeMask |
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                       EnterWindowMask | LeaveWindowMask;

    a->win = XCreateWindow(a->dpy, a->root, x, y, (unsigned)width, (unsigned)height, 0,
                           a->visual_info->depth, InputOutput, a->visual_info->visual,
                           CWColormap | CWBackPixel | CWBorderPixel | CWOverrideRedirect | CWEventMask,
                           &attrs);
    a->window_is_override_redirect = override_redirect;

    XStoreName(a->dpy, a->win, "SuperCRT");
    XSetWMProtocols(a->dpy, a->win, &a->wm_delete_window, 1);
    XMapWindow(a->dpy, a->win);
    XFlush(a->dpy);

    if (!glXMakeCurrent(a->dpy, a->win, a->ctx)) {
        fprintf(stderr, "supercrt: glXMakeCurrent failed on the new window\n");
        exit(1);
    }
    App_ApplySwapInterval(a, g_params.VSync ? 1 : 0);
}

static void App_SetMode(App *a, int mode)
{
    if (mode == a->mode && a->win) {
        return;
    }

    if (mode == 1) {
        // Fullscreen: the window manager owns geometry.  Without one, fall back to the
        // borderless path, which is what the reference's Fullscreen mode effectively does.
        if (XGetSelectionOwner(a->dpy, XInternAtom(a->dpy, "_NET_SUPPORTING_WM_CHECK", False)) == None) {
            fprintf(stderr, "supercrt: no window manager, using borderless fullscreen\n");
            mode = 2;
        }
    }

    a->mode = mode;
    if (mode == 0) {
        if (a->window_is_override_redirect) {
            App_CreateWindow(a, 0, a->windowed_width, a->windowed_height);
        } else {
            XResizeWindow(a->dpy, a->win, (unsigned)a->windowed_width, (unsigned)a->windowed_height);
        }
        if (a->net_wm_state) {
            XEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.xclient.type = ClientMessage;
            ev.xclient.window = a->win;
            ev.xclient.message_type = a->net_wm_state;
            ev.xclient.format = 32;
            ev.xclient.data.l[0] = 0; // _NET_WM_STATE_REMOVE
            ev.xclient.data.l[1] = (long)a->net_wm_state_fullscreen;
            XSendEvent(a->dpy, a->root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        }
    } else if (mode == 1) {
        if (a->window_is_override_redirect) {
            App_CreateWindow(a, 0, a->windowed_width, a->windowed_height);
        }
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = a->win;
        ev.xclient.message_type = a->net_wm_state;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = 1; // _NET_WM_STATE_ADD
        ev.xclient.data.l[1] = (long)a->net_wm_state_fullscreen;
        XSendEvent(a->dpy, a->root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    } else {
        // Borderless: unmanaged window covering the whole screen.
        App_CreateWindow(a, 1, a->screen_width, a->screen_height);
        XMoveWindow(a->dpy, a->win, 0, 0);
        XRaiseWindow(a->dpy, a->win);
    }
    XFlush(a->dpy);
}

// ---------------------------------------------------------------------------
// Pass setup
// ---------------------------------------------------------------------------

static int App_CreateTargets(App *a)
{
    const int src_w = g_params.SrcWidth;
    const int src_h = g_params.SrcHeight;
    const int dst_w = a->width;
    const int dst_h = a->height;

    Target_Free(&a->comp[0]);
    Target_Free(&a->comp[1]);
    Target_Free(&a->full);
    Target_Free(&a->down);
    Target_Free(&a->up);

    if (!Target_Create(&a->comp[0], src_w, src_h, "composite even", 0) ||
        !Target_Create(&a->comp[1], src_w, src_h, "composite odd", 0) ||
        !Target_Create(&a->full, dst_w, dst_h, "full", 1) ||
        !Target_Create(&a->down, dst_w / 16 > 0 ? dst_w / 16 : 1, dst_h / 16 > 0 ? dst_h / 16 : 1,
                       "downsample", 0) ||
        !Target_Create(&a->up, dst_w, dst_h, "upsample", 0)) {
        return 0;
    }

    // Both composite buffers start black so the first frame's "previous frame" tap is not
    // uninitialised memory.
    Target_Clear(&a->comp[0], 0.0f, 0.0f, 0.0f, 1.0f, 0);
    Target_Clear(&a->comp[1], 0.0f, 0.0f, 0.0f, 1.0f, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

static int App_CreateCleanTexture(App *a)
{
    if (a->clean_tex) {
        glDeleteTextures(1, &a->clean_tex);
        a->clean_tex = 0;
    }
    glGenTextures(1, &a->clean_tex);
    glBindTexture(GL_TEXTURE_2D, a->clean_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_params.SrcWidth, g_params.SrcHeight, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, a->clear_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a->clean_tex, 0);
    glViewport(0, 0, g_params.SrcWidth, g_params.SrcHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

static void App_UploadCleanRect(App *a, int x, int y, int width, int height, const void *pixels,
                                int row_pixels, int bgr)
{
    glBindTexture(GL_TEXTURE_2D, a->clean_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_pixels);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, bgr ? GL_BGRA : GL_RGBA,
                    GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static int App_CreatePasses(App *a)
{
    a->composite.program = Shader_Build(kVS_Fullscreen, kFS_Composite, "composite");
    a->screen_pass.program = Shader_Build(kVS_Mesh, kFS_Screen, "screen");
    a->frame_pass.program = Shader_Build(kVS_Mesh, kFS_Frame, "frame");
    a->post.program = Shader_Build(kVS_Fullscreen, kFS_Post, "post");
    a->present.program = Shader_Build(kVS_Fullscreen, kFS_Present, "present");
    if (!a->composite.program || !a->screen_pass.program || !a->frame_pass.program ||
        !a->post.program || !a->present.program) {
        return 0;
    }

    unsigned p = a->composite.program;
    a->composite.RcpScrWidth = glGetUniformLocation(p, "RcpScrWidth");
    a->composite.RcpScrHeight = glGetUniformLocation(p, "RcpScrHeight");
    a->composite.Tuning_Sharp = glGetUniformLocation(p, "Tuning_Sharp");
    a->composite.Tuning_Persistence = glGetUniformLocation(p, "Tuning_Persistence");
    a->composite.Tuning_Bleed = glGetUniformLocation(p, "Tuning_Bleed");
    a->composite.Tuning_Artifacts = glGetUniformLocation(p, "Tuning_Artifacts");
    a->composite.curFrameMap = glGetUniformLocation(p, "curFrameMap");
    a->composite.prevFrameMap = glGetUniformLocation(p, "prevFrameMap");
    a->composite.NTSCArtifactTex = glGetUniformLocation(p, "NTSCArtifactTex");
    a->composite.NTSCLerp = glGetUniformLocation(p, "NTSCLerp");

    MeshPass *passes[2] = { &a->screen_pass, &a->frame_pass };
    for (int i = 0; i < 2; ++i) {
        MeshPass *m = passes[i];
        p = m->program;
        m->wvpMat = glGetUniformLocation(p, "wvpMat");
        m->camPos = glGetUniformLocation(p, "camPos");
        m->Tuning_LightPos = glGetUniformLocation(p, "Tuning_LightPos");
        m->UVScalar = glGetUniformLocation(p, "UVScalar");
        m->UVOffset = glGetUniformLocation(p, "UVOffset");
        m->CRTMask_Scale = glGetUniformLocation(p, "CRTMask_Scale");
        m->Tuning_Overscan = glGetUniformLocation(p, "Tuning_Overscan");
        m->Tuning_Dimming = glGetUniformLocation(p, "Tuning_Dimming");
        m->Tuning_Satur = glGetUniformLocation(p, "Tuning_Satur");
        m->Tuning_ReflScalar = glGetUniformLocation(p, "Tuning_ReflScalar");
        m->Tuning_Barrel = glGetUniformLocation(p, "Tuning_Barrel");
        m->Tuning_Mask_Brightness = glGetUniformLocation(p, "Tuning_Mask_Brightness");
        m->Tuning_Mask_Opacity = glGetUniformLocation(p, "Tuning_Mask_Opacity");
        m->Tuning_Diff_Brightness = glGetUniformLocation(p, "Tuning_Diff_Brightness");
        m->Tuning_Spec_Brightness = glGetUniformLocation(p, "Tuning_Spec_Brightness");
        m->Tuning_Spec_Power = glGetUniformLocation(p, "Tuning_Spec_Power");
        m->Tuning_Fres_Brightness = glGetUniformLocation(p, "Tuning_Fres_Brightness");
        m->Tuning_FrameColor = glGetUniformLocation(p, "Tuning_FrameColor");
        m->compFrameMap = glGetUniformLocation(p, "compFrameMap");
        m->shadowMaskMap = glGetUniformLocation(p, "shadowMaskMap");
    }

    p = a->post.program;
    a->post.uSource = glGetUniformLocation(p, "uSource");
    a->post.BloomScale = glGetUniformLocation(p, "BloomScale");
    a->post.Upsample = glGetUniformLocation(p, "Upsample");

    p = a->present.program;
    a->present.PreBloomBuffer = glGetUniformLocation(p, "PreBloomBuffer");
    a->present.UpsampledBuffer = glGetUniformLocation(p, "UpsampledBuffer");
    a->present.BloomScalar = glGetUniformLocation(p, "BloomScalar");
    a->present.BloomPower = glGetUniformLocation(p, "BloomPower");

    glGenVertexArrays(1, &a->empty_vao);
    glGenBuffers(1, &a->full_quad_vbo);
    glBindVertexArray(a->empty_vao);
    glBindBuffer(GL_ARRAY_BUFFER, a->full_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadPositions), kQuadPositions, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return Gl_Check("App_CreatePasses") == GL_NO_ERROR;
}

static void DrawFullscreenQuad(const App *a)
{
    glBindVertexArray(a->empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

static void RenderComposite(App *a, int current, int source)
{
    Target_Clear(&a->comp[current], 0.0f, 128.0f / 255.0f, 1.0f, 1.0f, 0);

    const CompositePass *c = &a->composite;
    glUseProgram(c->program);

    const float rcp_w[2] = { 1.0f / (float)g_params.SrcWidth, 0.0f };
    const float rcp_h[2] = { 0.0f, 1.0f / (float)g_params.SrcHeight };
    glUniform2fv(c->RcpScrWidth, 1, rcp_w);
    glUniform2fv(c->RcpScrHeight, 1, rcp_h);
    glUniform1f(c->Tuning_Sharp, g_params.Sharp);
    glUniform4fv(c->Tuning_Persistence, 1, g_params.Persistence);
    glUniform1f(c->Tuning_Bleed, g_params.Bleed);
    glUniform1f(c->Tuning_Artifacts, g_params.Artifacts);
    glUniform1f(c->NTSCLerp, a->even_frame ? 0.0f : 1.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a->clean_tex);
    glBindSampler(0, a->samp_point_clamp);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, a->comp[source].tex);
    glBindSampler(1, a->samp_point_clamp);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, a->artifacts_tex);
    glBindSampler(2, a->samp_point_repeat);

    glUniform1i(c->curFrameMap, 0);
    glUniform1i(c->prevFrameMap, 1);
    glUniform1i(c->NTSCArtifactTex, 2);

    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    DrawFullscreenQuad(a);

    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glBindSampler(2, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
}

static void SetMeshUniforms(App *a, MeshPass *pass, int is_frame, int current, float ratio_buffer)
{
    glUseProgram(pass->program);

    const float fov = DEFAULT_FOV_DEGREES * DEG2RAD;
    const float xp = 1.0f / sinf(fov * 0.5f);
    const float distance = xp * cosf(fov * 0.5f);
    const Vec3 eye = { -distance, 0.0f, 0.0f };
    const Vec3 center = { 0.0f, 0.0f, 0.0f };
    const Vec3 up = { 0.0f, 0.0f, 1.0f };

    float view[16], proj[16], wvp[16];
    M4_LookAtRH(view, eye, center, up);
    M4_PerspectiveRH(proj, fov, (float)a->width / (float)(a->height > 0 ? a->height : 1), 1.0f, 100.0f);
    M4_Multiply(wvp, proj, view);

    glUniformMatrix4fv(pass->wvpMat, 1, GL_FALSE, wvp);
    glUniform3f(pass->camPos, eye.x, eye.y, eye.z);
    glUniform3fv(pass->Tuning_LightPos, 1, g_params.LightPos);

    const float ratio_viewport = 4.0f / 3.0f;
    const float ratio_desired_pixel = g_params.PixelRatio;
    const float scale_y = (ratio_buffer / ratio_viewport) * ratio_desired_pixel;
    const float uv_scalar[2] = { 1.0f, scale_y };
    const float uv_offset[2] = { 0.0f, (1.0f - scale_y) * 0.5f };
    glUniform2fv(pass->UVScalar, 1, uv_scalar);
    glUniform2fv(pass->UVOffset, 1, uv_offset);

    // The reference expands the shadow mask differently for the two meshes, and passes
    // the reciprocal of the overscan tuning to the screen effect only.
    const float mask_scale[2] = { (float)g_params.SrcWidth * 0.5f,
                                  (float)g_params.SrcHeight * (is_frame ? 0.5f : 1.0f) };
    glUniform2fv(pass->CRTMask_Scale, 1, mask_scale);
    glUniform1f(pass->Tuning_Overscan, is_frame ? g_params.Overscan : 1.0f / g_params.Overscan);

    glUniform1f(pass->Tuning_Dimming, g_params.Dimming);
    glUniform1f(pass->Tuning_Satur, g_params.Saturation);
    glUniform1f(pass->Tuning_ReflScalar, g_params.ReflectionScalar);
    glUniform1f(pass->Tuning_Barrel, g_params.Barrel);
    glUniform1f(pass->Tuning_Mask_Brightness, g_params.MaskBrightness);
    glUniform1f(pass->Tuning_Mask_Opacity, g_params.MaskOpacity);
    glUniform1f(pass->Tuning_Diff_Brightness, g_params.DiffuseBrightness);
    glUniform1f(pass->Tuning_Spec_Brightness, g_params.SpecularBrightness);
    glUniform1f(pass->Tuning_Spec_Power, g_params.SpecularPower);
    glUniform1f(pass->Tuning_Fres_Brightness, g_params.FresnelBrightness);
    glUniform3fv(pass->Tuning_FrameColor, 1, g_params.FrameColor);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a->comp[current].tex);
    glBindSampler(0, a->samp_linear_border);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, a->mask_tex);
    glBindSampler(1, a->samp_linear_repeat);
    glUniform1i(pass->compFrameMap, 0);
    glUniform1i(pass->shadowMaskMap, 1);

    glDisable(GL_BLEND);
    // The reference uses CullMode = CW.  D3D judges winding in screen space with y
    // running down, GL with y running up, so a D3D-clockwise triangle is GL
    // counter-clockwise: culling GL back faces with the default CCW front face is the
    // same rasterizer rule.
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
}

static void RenderMeshes(App *a, int current)
{
    Target_Clear(&a->full, 0.0f, 0.0f, 0.0f, 1.0f, 1);

    const float ratio_buffer = (float)g_params.SrcWidth / (float)g_params.SrcHeight;

    SetMeshUniforms(a, &a->screen_pass, 0, current, ratio_buffer);
    MeshGL_Draw(&a->screen_mesh);

    SetMeshUniforms(a, &a->frame_pass, 1, current, ratio_buffer);
    MeshGL_Draw(&a->frame_mesh);

    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
}

static void RenderBloom(App *a)
{
    const float inv_aspect = (float)a->height / (float)(a->width > 0 ? a->width : 1);

    glUseProgram(a->post.program);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    // Downsample.
    Target_Clear(&a->down, 0.0f, 0.0f, 0.0f, 1.0f, 0);
    const float down_scale[2] = { inv_aspect * g_params.BloomDownsampleSpread,
                                  g_params.BloomDownsampleSpread };
    glUniform2fv(a->post.BloomScale, 1, down_scale);
    glUniform1i(a->post.Upsample, 0);
    glUniform1i(a->post.uSource, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a->full.tex);
    glBindSampler(0, a->samp_linear_clamp);
    DrawFullscreenQuad(a);

    // Upsample.
    Target_Clear(&a->up, 0.0f, 0.0f, 0.0f, 1.0f, 0);
    const float up_scale[2] = { inv_aspect * g_params.BloomUpsampleSpread,
                                g_params.BloomUpsampleSpread };
    glUniform2fv(a->post.BloomScale, 1, up_scale);
    glUniform1i(a->post.Upsample, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a->down.tex);
    DrawFullscreenQuad(a);

    glBindSampler(0, 0);
    glUseProgram(0);
}

static void RenderPresent(App *a)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, a->width, a->height);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glUseProgram(a->present.program);
    glUniform1i(a->present.PreBloomBuffer, 0);
    glUniform1i(a->present.UpsampledBuffer, 1);
    glUniform1f(a->present.BloomScalar, g_params.BloomIntensity);
    glUniform1f(a->present.BloomPower, g_params.BloomPower);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a->full.tex);
    glBindSampler(0, a->samp_linear_clamp);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, a->up.tex);
    glBindSampler(1, a->samp_linear_clamp);

    DrawFullscreenQuad(a);

    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
}

// The capture outline only needs to move when the region actually changes.
static int RegionChanged(const int previous[4], const Params *p)
{
    if (previous[0] == p->SrcX && previous[1] == p->SrcY && previous[2] == p->SrcWidth &&
        previous[3] == p->SrcHeight) {
        return 0;
    }
    return 1;
}

static void RegionRemember(int previous[4], const Params *p)
{
    previous[0] = p->SrcX;
    previous[1] = p->SrcY;
    previous[2] = p->SrcWidth;
    previous[3] = p->SrcHeight;
}

// Defined with the other mouse handling below; the settings panel's action list can start
// a target-area drag.
static void EditMode_Enter(App *a);

// ---------------------------------------------------------------------------
// Settings overlay
// ---------------------------------------------------------------------------

typedef enum {
    ACTION_NONE = 0,
    ACTION_CENTER,
    ACTION_EDIT_TARGET,
    ACTION_TOP_LEFT,
    ACTION_BOTTOM_RIGHT,
    ACTION_OUTLINE,
    ACTION_FULLSCREEN,
    ACTION_ONTOP,
    ACTION_VSYNC,
    ACTION_SAVE,
    ACTION_RELOAD,
    ACTION_DEFAULTS,
    ACTION_QUIT,
} ActionKind;

typedef struct {
    const char *label;
    ActionKind action;
    const char *hint;
} OverlayAction;

static const OverlayAction kActions[] = {
    { "Drag target area with mouse",  ACTION_EDIT_TARGET,   "e" },
    { "Center capture on cursor",     ACTION_CENTER,        "c" },
    { "Set capture top-left",         ACTION_TOP_LEFT,      "t" },
    { "Set capture bottom-right",     ACTION_BOTTOM_RIGHT,  "b" },
    { "Toggle capture outline",       ACTION_OUTLINE,       "m" },
    { "Cycle window mode",            ACTION_FULLSCREEN,    "f" },
    { "Toggle always on top",         ACTION_ONTOP,         "a" },
    { "Toggle vsync",                 ACTION_VSYNC,         "v" },
    { "Save settings",                ACTION_SAVE,          "s" },
    { "Reload settings from disk",    ACTION_RELOAD,        "l" },
    { "Restore defaults",             ACTION_DEFAULTS,      "r" },
    { "Quit",                         ACTION_QUIT,          "q" },
};

static void SetStatus(App *a, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(a->status, sizeof(a->status), fmt, args);
    va_end(args);
    a->status_frames = 240;
}

static void Overlay_RowCount(int *tunable_count, int *action_count)
{
    Params_Tunables(tunable_count);
    *action_count = (int)(sizeof(kActions) / sizeof(kActions[0]));
}

static void RunAction(App *a, ActionKind action)
{
    int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
    Window root_ret, child_ret;
    unsigned mask = 0;
    XQueryPointer(a->dpy, a->root, &root_ret, &child_ret, &root_x, &root_y, &win_x, &win_y, &mask);

    switch (action) {
    case ACTION_EDIT_TARGET:
        EditMode_Enter(a);
        break;
    case ACTION_CENTER:
        g_params.SrcX = root_x - g_params.SrcWidth / 2;
        g_params.SrcY = root_y - g_params.SrcHeight / 2;
        SetStatus(a, "capture centered on %d,%d", root_x, root_y);
        a->dirty_settings = 1;
        break;
    case ACTION_TOP_LEFT:
        g_params.SrcX = root_x;
        g_params.SrcY = root_y;
        SetStatus(a, "capture top-left = %d,%d", root_x, root_y);
        a->dirty_settings = 1;
        break;
    case ACTION_BOTTOM_RIGHT: {
        const int w = root_x - g_params.SrcX;
        const int h = root_y - g_params.SrcY;
        if (w < 16 || h < 16) {
            SetStatus(a, "bottom-right must be at least 16px past the top-left");
            break;
        }
        g_params.SrcWidth = w;
        g_params.SrcHeight = h;
        SetStatus(a, "capture size = %dx%d", w, h);
        a->dirty_settings = 1;
        break;
    }
    case ACTION_OUTLINE:
        a->capture_outline_enabled = !a->capture_outline_enabled;
        Marker_SetEnabled(a->marker, a->capture_outline_enabled);
        SetStatus(a, "capture outline %s", a->capture_outline_enabled ? "on" : "off");
        break;
    case ACTION_FULLSCREEN:
        App_SetMode(a, (a->mode + 1) % 3);
        SetStatus(a, "window mode: %s", a->mode == 0 ? "windowed" : (a->mode == 1 ? "fullscreen" : "borderless"));
        break;
    case ACTION_ONTOP:
        g_params.AlwaysOnTop = !g_params.AlwaysOnTop;
        SetStatus(a, "always on top %s", g_params.AlwaysOnTop ? "on" : "off");
        a->dirty_settings = 1;
        break;
    case ACTION_VSYNC:
        g_params.VSync = !g_params.VSync;
        App_ApplySwapInterval(a, g_params.VSync ? 1 : 0);
        SetStatus(a, "vsync %s", g_params.VSync ? "on" : "off");
        a->dirty_settings = 1;
        break;
    case ACTION_SAVE:
        if (Params_Save(&g_params, a->config_path) == 0) {
            SetStatus(a, "saved %s", a->config_path);
            a->dirty_settings = 0;
        } else {
            SetStatus(a, "could not write %s", a->config_path);
        }
        break;
    case ACTION_RELOAD:
        Params_Load(&g_params, a->config_path);
        App_ApplySwapInterval(a, g_params.VSync ? 1 : 0);
        SetStatus(a, "reloaded %s", a->config_path);
        break;
    case ACTION_DEFAULTS:
        Params_Defaults(&g_params);
        SetStatus(a, "defaults restored (not saved)");
        a->dirty_settings = 1;
        break;
    case ACTION_QUIT:
        a->quit = 1;
        break;
    default:
        break;
    }
}

static void Overlay_Draw(App *a)
{
    UI *ui = &a->ui;
    const float line = UI_LineHeight();
    const float margin = 18.0f;
    a->overlay_hit_count = 0;

    // Translucent panel so the settings stay readable over the sim.
    UI_Rect(ui, 0.0f, 0.0f, (float)a->width, (float)a->height, 0.02f, 0.02f, 0.05f, 0.82f);

    float y = margin;
    UI_TextF(ui, margin, y, 0.55f, 0.95f, 0.75f, 1.0f, "SuperCRT (Linux port)  %dx%d  %.0f fps", a->width, a->height, a->fps);
    UI_Text(ui, (float)a->width - margin - 40.0f, y, 0.95f, 0.6f, 0.6f, 1.0f, "close [X]");
    a->hud_close = (Rectf){ (float)a->width - margin - 46.0f, y - 3.0f, 52.0f, line, };
    y += line;
    UI_Text(ui, margin, y, 0.75f, 0.75f, 0.8f, 1.0f,
            "Mouse: click a row to select, drag the value to change it, click an action to run it");
    y += line;
    UI_Text(ui, margin, y, 0.75f, 0.75f, 0.8f, 1.0f,
            "Keys: Esc close  Up/Down select  Left/Right adjust (Shift coarse)  Enter run action  E drag target area");
    y += line;
    UI_TextF(ui, margin, y, 0.75f, 0.75f, 0.8f, 1.0f,
             "Capture: %d,%d %dx%d  grab: %s  screen: %dx%d",
             g_params.SrcX, g_params.SrcY, g_params.SrcWidth, g_params.SrcHeight,
             a->using_shm ? "MIT-SHM" : "XGetImage", a->screen_width, a->screen_height);
    y += line * 1.4f;

    int tunable_count = 0, action_count = 0;
    Overlay_RowCount(&tunable_count, &action_count);
    const int total = tunable_count + action_count;
    if (a->overlay_selected >= total) {
        a->overlay_selected = total - 1;
    }

    const Tunable *tunables = Params_Tunables(&tunable_count);

    // Two footer lines plus the panel margin stay clear of the rows, and the columns
    // share whatever width is left, so the panel fits windows as small as a few hundred
    // pixels without spilling rows off the edge.
    const float footer_height = line * 2.0f;
    float rows_area = (float)a->height - y - footer_height - margin * 2.0f;
    int rows_per_column = (int)(rows_area / line);
    if (rows_per_column < 4) {
        rows_per_column = 4;
    }
    int columns = (total + rows_per_column - 1) / rows_per_column;
    if (columns < 1) {
        columns = 1;
    }
    const float column_width = (((float)a->width - margin * 2.0f) / (float)columns);

    for (int i = 0; i < total; ++i) {
        const int column = i / rows_per_column;
        const int row = i % rows_per_column;
        const float x = margin + (float)column * column_width;
        const float row_y = y + (float)row * line;
        const int selected = (i == a->overlay_selected);

        if (selected) {
            UI_Rect(ui, x - 4.0f, row_y - 1.0f, column_width - 8.0f, line, 0.25f, 0.5f, 0.85f, 0.55f);
        }

        char text[192];
        OverlayHit *hit = NULL;
        if (a->overlay_hit_count < (int)(sizeof(a->overlay_hits) / sizeof(a->overlay_hits[0]))) {
            hit = &a->overlay_hits[a->overlay_hit_count++];
            memset(hit, 0, sizeof(*hit));
            hit->row = (Rectf){ x - 6.0f, row_y - 2.0f, column_width - 4.0f, line };
            hit->index = i;
        }

        if (i < tunable_count) {
            const Tunable *t = &tunables[i];
            char value[64];
            if (t->is_int) {
                snprintf(value, sizeof(value), t->fmt, *(const int *)t->ptr);
            } else {
                snprintf(value, sizeof(value), t->fmt, (double)*(const float *)t->ptr);
            }
            snprintf(text, sizeof(text), "%-24s %s", t->label, value);
            UI_Text(ui, x, row_y, 0.92f, 0.92f, 0.95f, 1.0f, text);

            if (hit) {
                hit->is_action = 0;
                hit->slider = (Rectf){ x + column_width * 0.52f, row_y - 2.0f,
                                       column_width * 0.46f, line };
            }
            // Value track: communicates that the row is draggable.
            const float track_y = row_y + line * 0.5f;
            const float track_x = x + column_width * 0.52f;
            const float track_w = column_width * 0.40f;
            UI_Rect(ui, track_x, track_y - 2.0f, track_w, 4.0f, 0.25f, 0.27f, 0.32f, 0.9f);
            float fraction = 0.0f;
            if (!t->is_int && t->max > t->min) {
                fraction = (*(const float *)t->ptr - t->min) / (t->max - t->min);
            } else if (t->is_int && t->max > t->min) {
                fraction = ((float)*(const int *)t->ptr - t->min) / (t->max - t->min);
            }
            if (fraction < 0.0f) fraction = 0.0f;
            if (fraction > 1.0f) fraction = 1.0f;
            UI_Rect(ui, track_x, track_y - 2.0f, track_w * fraction, 4.0f, 0.45f, 0.75f, 0.95f, 1.0f);
        } else {
            const OverlayAction *action = &kActions[i - tunable_count];
            snprintf(text, sizeof(text), "[%s] %s", action->hint, action->label);
            UI_Text(ui, x, row_y, 0.6f, 0.9f, 0.7f, 1.0f, text);
            if (hit) {
                hit->is_action = 1;
            }
        }
    }
    const float footer = (float)a->height - margin - footer_height + line * 0.2f;
    UI_TextF(ui, margin, footer, 0.7f, 0.7f, 0.75f, 1.0f, "config: %s  %s",
             a->config_path, a->dirty_settings ? "(unsaved changes)" : "");
    if (a->status_frames > 0) {
        UI_Text(ui, margin, footer + line, 1.0f, 0.9f, 0.4f, 1.0f, a->status);
    }
}

// Bottom strip, shown while the pointer is over the sim window: makes the two things a
// mouse-only session needs (the settings panel and the target-area drag) discoverable.
static void Hud_Draw(App *a)
{
    UI *ui = &a->ui;
    a->hud_settings = (Rectf){ 0, 0, 0, 0 };
    a->hud_target = (Rectf){ 0, 0, 0, 0 };
    if (a->overlay_open || (!a->pointer_inside && !a->edit_mode)) {
        return;
    }

    const float bar_h = 30.0f;
    const float y = (float)a->height - bar_h;
    const float text_y = y + 8.0f;
    float x = 12.0f;

    UI_Rect(ui, 0.0f, y, (float)a->width, bar_h, 0.02f, 0.02f, 0.04f, 0.72f);

    if (a->edit_mode) {
        UI_TextF(ui, x, text_y, 1.0f, 0.85f, 0.35f, 1.0f,
                 "Target area %d,%d %dx%d  -  drag inside to move, edges/corners to resize, "
                 "click outside or Esc/Enter to finish",
                 g_params.SrcX, g_params.SrcY, g_params.SrcWidth, g_params.SrcHeight);
        return;
    }

    a->hud_settings = (Rectf){ x - 6.0f, y + 3.0f, 150.0f, bar_h - 6.0f };
    UI_Rect(ui, a->hud_settings.x, a->hud_settings.y, a->hud_settings.w, a->hud_settings.h,
            0.16f, 0.34f, 0.60f, 0.80f);
    UI_Text(ui, x, text_y, 0.95f, 0.95f, 1.0f, 1.0f, "Settings (Esc)");
    x += 162.0f;

    a->hud_target = (Rectf){ x - 6.0f, y + 3.0f, 216.0f, bar_h - 6.0f };
    UI_Rect(ui, a->hud_target.x, a->hud_target.y, a->hud_target.w, a->hud_target.h,
            0.16f, 0.34f, 0.60f, 0.80f);
    UI_Text(ui, x, text_y, 0.95f, 0.95f, 1.0f, 1.0f, "Adjust target area (E)");
    x += 228.0f;

    UI_TextF(ui, x, text_y, 0.70f, 0.76f, 0.82f, 1.0f, "target %d,%d %dx%d   %.0f fps%s",
             g_params.SrcX, g_params.SrcY, g_params.SrcWidth, g_params.SrcHeight, a->fps,
             a->dirty_settings ? "   (unsaved)" : "");
}

static void Overlay_Adjust(App *a, int direction, int coarse)
{
    int tunable_count = 0, action_count = 0;
    Overlay_RowCount(&tunable_count, &action_count);
    if (a->overlay_selected >= tunable_count) {
        return;
    }

    const Tunable *tunables = Params_Tunables(&tunable_count);
    const Tunable *t = &tunables[a->overlay_selected];
    const float delta = (coarse ? t->coarse : t->step) * (float)direction;

    if (t->is_int) {
        int *value = (int *)t->ptr;
        *value += (int)delta;
        if ((float)*value < t->min) *value = (int)t->min;
        if ((float)*value > t->max) *value = (int)t->max;
    } else {
        float *value = (float *)t->ptr;
        *value += delta;
        if (*value < t->min) *value = t->min;
        if (*value > t->max) *value = t->max;
    }
    a->dirty_settings = 1;
}

static void Overlay_RunSelected(App *a)
{
    int tunable_count = 0, action_count = 0;
    Overlay_RowCount(&tunable_count, &action_count);
    if (a->overlay_selected >= tunable_count) {
        RunAction(a, kActions[a->overlay_selected - tunable_count].action);
    }
}

// ---------------------------------------------------------------------------
// Mouse interaction
// ---------------------------------------------------------------------------

// A click inside the panel background is absorbed rather than falling through to the sim.
static RegionZone ZoneAt(int x, int y)
{
    const int margin = 10;
    const int x0 = g_params.SrcX, y0 = g_params.SrcY;
    const int x1 = x0 + g_params.SrcWidth, y1 = y0 + g_params.SrcHeight;
    if (x < x0 - margin || x > x1 + margin || y < y0 - margin || y > y1 + margin) {
        return ZONE_OUTSIDE;
    }
    const int left = x < x0 + margin;
    const int right = x > x1 - margin;
    const int top = y < y0 + margin;
    const int bottom = y > y1 - margin;
    if (left && top) return ZONE_TOP_LEFT;
    if (right && top) return ZONE_TOP_RIGHT;
    if (left && bottom) return ZONE_BOTTOM_LEFT;
    if (right && bottom) return ZONE_BOTTOM_RIGHT;
    if (left) return ZONE_LEFT;
    if (right) return ZONE_RIGHT;
    if (top) return ZONE_TOP;
    if (bottom) return ZONE_BOTTOM;
    return ZONE_MOVE;
}

static Cursor ZoneCursor(const App *a, RegionZone zone)
{
    switch (zone) {
    case ZONE_MOVE:         return a->cursor_fleur;
    case ZONE_LEFT:
    case ZONE_RIGHT:        return a->cursor_h;
    case ZONE_TOP:
    case ZONE_BOTTOM:       return a->cursor_v;
    case ZONE_TOP_LEFT:     return a->cursor_nw;
    case ZONE_TOP_RIGHT:    return a->cursor_ne;
    case ZONE_BOTTOM_LEFT:  return a->cursor_sw;
    case ZONE_BOTTOM_RIGHT: return a->cursor_se;
    default:                return None;
    }
}

static void EditMode_Leave(App *a)
{
    if (!a->edit_mode) {
        return;
    }
    Trace("edit: leave (%d,%d %dx%d)", g_params.SrcX, g_params.SrcY, g_params.SrcWidth,
          g_params.SrcHeight);
    a->edit_mode = 0;
    a->region_drag_zone = ZONE_OUTSIDE;
    XUngrabPointer(a->dpy, CurrentTime);
    XSelectInput(a->dpy, a->root, 0);
    Marker_SetInteractive(a->marker, 0);
    Marker_SetEnabled(a->marker, a->capture_outline_enabled);
    SetStatus(a, "target area %d,%d %dx%d", g_params.SrcX, g_params.SrcY, g_params.SrcWidth,
              g_params.SrcHeight);
}

// Target-area editing grabs the pointer for as long as it lasts: that is what lets a drag
// start anywhere inside the frame even though the frame itself is click-through, and a
// click outside the frame ends the session.
static void EditMode_Enter(App *a)
{
    if (a->edit_mode) {
        return;
    }
    a->edit_mode = 1;
    a->overlay_open = 0;
    a->slider_row = -1;
    Marker_SetEnabled(a->marker, 1);
    Marker_SetInteractive(a->marker, 1);
    Marker_Raise(a->marker);
    XSelectInput(a->dpy, a->root, ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    if (XGrabPointer(a->dpy, a->root, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                     GrabModeAsync, None, a->cursor_fleur, CurrentTime) != GrabSuccess) {
        fprintf(stderr, "supercrt: pointer grab refused; the target area can still be typed in\n");
    }
    Trace("edit: enter zone=%d rect %d,%d %dx%d", (int)a->region_drag_zone, g_params.SrcX,
          g_params.SrcY, g_params.SrcWidth, g_params.SrcHeight);
    SetStatus(a, "target area: drag inside to move, edges/corners to resize, click outside to finish");
    XFlush(a->dpy);
}

static void RegionDragBegin(App *a, RegionZone zone, int root_x, int root_y)
{
    Trace("drag: begin zone=%d at root (%d,%d), rect %d,%d %dx%d", (int)zone, root_x, root_y,
          g_params.SrcX, g_params.SrcY, g_params.SrcWidth, g_params.SrcHeight);
    a->region_drag_zone = (int)zone;
    a->region_drag_start_x = root_x;
    a->region_drag_start_y = root_y;
    a->region_start_rect[0] = g_params.SrcX;
    a->region_start_rect[1] = g_params.SrcY;
    a->region_start_rect[2] = g_params.SrcWidth;
    a->region_start_rect[3] = g_params.SrcHeight;
}

static void RegionDragUpdate(App *a, int root_x, int root_y)
{
    const RegionZone zone = (RegionZone)a->region_drag_zone;
    if (zone == ZONE_OUTSIDE) {
        return;
    }
    const int dx = root_x - a->region_drag_start_x;
    const int dy = root_y - a->region_drag_start_y;

    // Track edges, not origin+size, so dragging one edge past the other pins that edge
    // instead of inverting the rectangle.
    int left = a->region_start_rect[0];
    int top = a->region_start_rect[1];
    int right = left + a->region_start_rect[2];
    int bottom = top + a->region_start_rect[3];

    const int moves_x = (zone == ZONE_MOVE || zone == ZONE_LEFT || zone == ZONE_RIGHT ||
                         zone == ZONE_TOP_LEFT || zone == ZONE_TOP_RIGHT ||
                         zone == ZONE_BOTTOM_LEFT || zone == ZONE_BOTTOM_RIGHT);
    const int moves_y = (zone == ZONE_MOVE || zone == ZONE_TOP || zone == ZONE_BOTTOM ||
                         zone == ZONE_TOP_LEFT || zone == ZONE_TOP_RIGHT ||
                         zone == ZONE_BOTTOM_LEFT || zone == ZONE_BOTTOM_RIGHT);
    if (zone == ZONE_MOVE) {
        left += dx;
        right += dx;
        top += dy;
        bottom += dy;
    } else {
        if (moves_x && (zone == ZONE_LEFT || zone == ZONE_TOP_LEFT || zone == ZONE_BOTTOM_LEFT)) {
            left += dx;
        }
        if (moves_x && (zone == ZONE_RIGHT || zone == ZONE_TOP_RIGHT || zone == ZONE_BOTTOM_RIGHT)) {
            right += dx;
        }
        if (moves_y && (zone == ZONE_TOP || zone == ZONE_TOP_LEFT || zone == ZONE_TOP_RIGHT)) {
            top += dy;
        }
        if (moves_y && (zone == ZONE_BOTTOM || zone == ZONE_BOTTOM_LEFT || zone == ZONE_BOTTOM_RIGHT)) {
            bottom += dy;
        }
    }

    const int min_size = 16;
    if (right - left < min_size) {
        if (zone == ZONE_LEFT || zone == ZONE_TOP_LEFT || zone == ZONE_BOTTOM_LEFT) {
            left = right - min_size;
        } else {
            right = left + min_size;
        }
    }
    if (bottom - top < min_size) {
        if (zone == ZONE_TOP || zone == ZONE_TOP_LEFT || zone == ZONE_TOP_RIGHT) {
            top = bottom - min_size;
        } else {
            bottom = top + min_size;
        }
    }

    g_params.SrcX = left;
    g_params.SrcY = top;
    g_params.SrcWidth = right - left;
    g_params.SrcHeight = bottom - top;
    Trace("drag: zone=%d d=(%d,%d) -> rect %d,%d %dx%d", (int)zone, dx, dy, g_params.SrcX,
          g_params.SrcY, g_params.SrcWidth, g_params.SrcHeight);
    a->dirty_settings = 1;
}

// Integer rows are screen coordinates, so a pixel of drag is a pixel of movement; float
// rows sweep their whole range across roughly the width of a row.
static float SliderUnitsPerPixel(const Tunable *t)
{
    if (t->is_int) {
        return 1.0f;
    }
    return (t->max - t->min) / 300.0f;
}

static void SliderDragUpdate(App *a, int x)
{
    int count = 0;
    const Tunable *tunables = Params_Tunables(&count);
    if (a->slider_row < 0 || a->slider_row >= count) {
        return;
    }
    const Tunable *t = &tunables[a->slider_row];
    float value = a->slider_start_value + (float)(x - a->slider_start_x) * SliderUnitsPerPixel(t);
    if (value < t->min) {
        value = t->min;
    }
    if (value > t->max) {
        value = t->max;
    }
    if (t->is_int) {
        *(int *)t->ptr = (int)lroundf(value);
    } else {
        value = t->min + roundf((value - t->min) / t->step) * t->step;
        if (value < t->min) {
            value = t->min;
        }
        if (value > t->max) {
            value = t->max;
        }
        *(float *)t->ptr = value;
    }
    a->dirty_settings = 1;
}

// Rebuilds the buffers that depend on the source dimensions.  Driven from the frame loop
// so every editing path (drag, keys, config reload, restore-defaults) is covered.
static void App_SyncSourceSize(App *a)
{
    if (a->src_alloc_w == g_params.SrcWidth && a->src_alloc_h == g_params.SrcHeight) {
        return;
    }
    App_CreateCleanTexture(a);
    App_CreateTargets(a);
    a->src_alloc_w = g_params.SrcWidth;
    a->src_alloc_h = g_params.SrcHeight;
}

static void OnButtonPress(App *a, XButtonEvent *ev)
{
    Trace("press btn=%u win=(%d,%d) root=(%d,%d) edit=%d overlay=%d", ev->button, ev->x, ev->y,
          ev->x_root, ev->y_root, a->edit_mode, a->overlay_open);
    if (ev->button != Button1) {
        return;
    }

    if (a->edit_mode) {
        const RegionZone zone = ZoneAt(ev->x_root, ev->y_root);
        if (zone == ZONE_OUTSIDE) {
            EditMode_Leave(a);
            return;
        }
        RegionDragBegin(a, zone, ev->x_root, ev->y_root);
        return;
    }

    if (a->overlay_open) {
        if (RectContains(a->hud_close, (float)ev->x, (float)ev->y)) {
            a->overlay_open = 0;
            return;
        }
        OverlayHit *hits = a->overlay_hits;
        for (int i = 0; i < a->overlay_hit_count; ++i) {
            if (!RectContains(hits[i].row, (float)ev->x, (float)ev->y)) {
                continue;
            }
            Trace("overlay: hit row %d (%s) rect=%.0f,%.0f %.0fx%.0f slider=%.0f,%.0f %.0fx%.0f",
                  hits[i].index, hits[i].is_action ? "action" : "tunable", hits[i].row.x,
                  hits[i].row.y, hits[i].row.w, hits[i].row.h, hits[i].slider.x, hits[i].slider.y,
                  hits[i].slider.w, hits[i].slider.h);
            a->overlay_selected = hits[i].index;
            if (hits[i].is_action) {
                int tunable_count = 0;
                Params_Tunables(&tunable_count);
                RunAction(a, kActions[hits[i].index - tunable_count].action);
            } else if (RectContains(hits[i].slider, (float)ev->x, (float)ev->y)) {
                int count = 0;
                const Tunable *tunables = Params_Tunables(&count);
                const Tunable *t = &tunables[hits[i].index];
                a->slider_row = hits[i].index;
                a->slider_start_x = ev->x;
                a->slider_start_value =
                    t->is_int ? (float)*(const int *)t->ptr : *(const float *)t->ptr;
            }
            return;
        }
        return;
    }

    Trace("hud rects: settings=%.0f,%.0f %.0fx%.0f  target=%.0f,%.0f %.0fx%.0f",
          a->hud_settings.x, a->hud_settings.y, a->hud_settings.w, a->hud_settings.h,
          a->hud_target.x, a->hud_target.y, a->hud_target.w, a->hud_target.h);
    if (RectContains(a->hud_settings, (float)ev->x, (float)ev->y)) {
        a->overlay_open = 1;
        a->overlay_selected = 0;
        return;
    }
    if (RectContains(a->hud_target, (float)ev->x, (float)ev->y)) {
        EditMode_Enter(a);
        return;
    }
}

static void OnButtonRelease(App *a, XButtonEvent *ev)
{
    Trace("release btn=%u root=(%d,%d) edit=%d slider=%d", ev->button, ev->x_root, ev->y_root,
          a->edit_mode, a->slider_row);
    if (ev->button != Button1) {
        return;
    }
    if (a->edit_mode) {
        RegionDragUpdate(a, ev->x_root, ev->y_root);
        a->region_drag_zone = ZONE_OUTSIDE;
        return;
    }
    a->slider_row = -1;
}

static void OnMotion(App *a, XMotionEvent *ev)
{
    if (a->edit_mode && a->region_drag_zone != ZONE_OUTSIDE) {
        Trace("motion root=(%d,%d) drag", ev->x_root, ev->y_root);
    }
    if (a->edit_mode) {
        if (a->region_drag_zone != ZONE_OUTSIDE) {
            RegionDragUpdate(a, ev->x_root, ev->y_root);
        } else {
            const RegionZone zone = ZoneAt(ev->x_root, ev->y_root);
            XChangeActivePointerGrab(a->dpy, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                     ZoneCursor(a, zone), CurrentTime);
        }
        return;
    }
    if (a->slider_row >= 0) {
        SliderDragUpdate(a, ev->x);
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static int HandleKey(App *a, XKeyEvent *key)
{
    KeySym sym = XLookupKeysym(key, 0);
    const int shift = (key->state & ShiftMask) != 0;
    const int ctrl = (key->state & ControlMask) != 0;

    // While the target area is grabbed, the keyboard only gets to confirm or cancel.
    if (a->edit_mode) {
        if (sym == XK_Escape || sym == XK_Return || sym == XK_KP_Enter || sym == XK_e ||
            sym == XK_E || (ctrl && (sym == XK_q || sym == XK_Q))) {
            EditMode_Leave(a);
        }
        return 1;
    }

    if (ctrl && (sym == XK_q || sym == XK_Q)) {
        a->quit = 1;
        return 1;
    }
    if (sym == XK_q && !a->overlay_open) {
        a->quit = 1;
        return 1;
    }

    if (sym == XK_Escape) {
        a->overlay_open = !a->overlay_open;
        return 1;
    }

    // Global window-management shortcuts (the shipping app binds Esc for settings and
    // leaves window state to its config file; here it is always reachable).
    if (sym == XK_F11 || (sym == XK_f && !a->overlay_open)) {
        RunAction(a, ACTION_FULLSCREEN);
        return 1;
    }
    if ((sym == XK_e || sym == XK_E) && !a->overlay_open) {
        RunAction(a, ACTION_EDIT_TARGET);
        return 1;
    }

    if (!a->overlay_open) {
        return 1;
    }

    switch (sym) {
    case XK_Up:
        a->overlay_selected--;
        if (a->overlay_selected < 0) {
            a->overlay_selected = 0;
        }
        return 1;
    case XK_Down:
        a->overlay_selected++;
        return 1;
    case XK_Page_Up:
        a->overlay_selected -= 10;
        if (a->overlay_selected < 0) {
            a->overlay_selected = 0;
        }
        return 1;
    case XK_Page_Down:
        a->overlay_selected += 10;
        return 1;
    case XK_Left:
        Overlay_Adjust(a, -1, shift);
        return 1;
    case XK_Right:
        Overlay_Adjust(a, +1, shift);
        return 1;
    case XK_Home:
        Overlay_Adjust(a, -100000, 0);
        return 1;
    case XK_End:
        Overlay_Adjust(a, +100000, 0);
        return 1;
    case XK_Return:
    case XK_KP_Enter:
        Overlay_RunSelected(a);
        return 1;
    default:
        break;
    }

    switch (sym) {
    case XK_e:
    case XK_E: RunAction(a, ACTION_EDIT_TARGET); return 1;
    case XK_c: RunAction(a, ACTION_CENTER); return 1;
    case XK_t: RunAction(a, ACTION_TOP_LEFT); return 1;
    case XK_b: RunAction(a, ACTION_BOTTOM_RIGHT); return 1;
    case XK_m: RunAction(a, ACTION_OUTLINE); return 1;
    case XK_a: RunAction(a, ACTION_ONTOP); return 1;
    case XK_v: RunAction(a, ACTION_VSYNC); return 1;
    case XK_s: RunAction(a, ACTION_SAVE); return 1;
    case XK_l: RunAction(a, ACTION_RELOAD); return 1;
    case XK_r: RunAction(a, ACTION_DEFAULTS); return 1;
    default: return 0;
    }
}

static void HandleEvents(App *a)
{
    while (XPending(a->dpy)) {
        XEvent ev;
        XNextEvent(a->dpy, &ev);
        switch (ev.type) {
        case ConfigureNotify:
            if (ev.xconfigure.width != a->width || ev.xconfigure.height != a->height) {
                a->width = ev.xconfigure.width > 0 ? ev.xconfigure.width : 1;
                a->height = ev.xconfigure.height > 0 ? ev.xconfigure.height : 1;
                if (a->mode == 0 && !a->window_is_override_redirect) {
                    a->windowed_width = a->width;
                    a->windowed_height = a->height;
                }
                App_CreateTargets(a);
            }
            break;
        case Expose:
            Marker_Redraw(a->marker);
            break;
        case KeyPress:
            HandleKey(a, &ev.xkey);
            break;
        case ButtonPress:
            OnButtonPress(a, &ev.xbutton);
            break;
        case ButtonRelease:
            OnButtonRelease(a, &ev.xbutton);
            break;
        case MotionNotify:
            OnMotion(a, &ev.xmotion);
            break;
        case EnterNotify:
            Trace("enter window");
            a->pointer_inside = 1;
            break;
        case LeaveNotify:
            Trace("leave window");
            a->pointer_inside = 0;
            break;
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == a->wm_delete_window) {
                a->quit = 1;
            }
            break;
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

static void Usage(const char *argv0)
{
    printf(
        "SuperCRT (Linux port) -- view a region of your desktop through a CRT simulation.\n"
        "\n"
        "usage: %s [options]\n"
        "\n"
        "  --config PATH        settings file (default: ./supercrt.ini, else\n"
        "                       $XDG_CONFIG_HOME/supercrt/supercrt.ini)\n"
        "  --src X,Y,W,H        capture region on the root window\n"
        "  --dst W,H            output window size\n"
        "  --fullscreen         start fullscreen via the window manager\n"
        "  --borderless         start as a borderless window covering the screen\n"
        "  --no-outline         hide the capture-region outline\n"
        "  --no-vsync           disable vsync\n"
        "  --no-shm             force the XGetImage capture path (no MIT-SHM)\n"
        "  --pattern            use a built-in test pattern instead of the desktop\n"
        "  --frame-out FILE     write one frame to FILE as binary PPM and exit\n"
        "  --dump-target NAME   with --frame-out, dump 'window', 'clean', 'composite',\n"
        "                       'full', 'down' or 'up' instead of the presented frame\n"
        "  --frames N           frames to render before --frame-out (default 120)\n"
        "  --help               this text\n"
        "\n"
        "Mouse: move the pointer over the sim window for the bottom bar; click Settings for\n"
        "the config menu (click a row to select it, drag its value track to change it, click\n"
        "an action row to run it) and click Adjust target area to drag/resize the sampled\n"
        "rectangle directly on the desktop.\n"
        "Keys: Esc settings overlay, E target-area drag, F11 fullscreen, Ctrl+Q quit.\n"
        "In the overlay: Up/Down select, Left/Right adjust (Shift coarse), Enter run\n"
        "action, c/t/b set the capture region from the mouse, e drag target area, s save,\n"
        "l reload, r defaults, m outline, f window mode, a always-on-top, v vsync, q quit.\n",
        argv0);
}

static int ParseRect(const char *text, int *values, int count)
{
    const char *cursor = text;
    for (int i = 0; i < count; ++i) {
        char *end = NULL;
        long v = strtol(cursor, &end, 10);
        if (end == cursor) {
            return 0;
        }
        values[i] = (int)v;
        cursor = end;
        while (*cursor == ',' || *cursor == 'x' || *cursor == ' ') {
            ++cursor;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    App app;
    memset(&app, 0, sizeof(app));

    Params_Defaults(&g_params);

    const char *config_override = NULL;
    const char *frame_out = NULL;
    const char *dump_target = NULL;
    int frames_before_dump = 120;
    int use_pattern = 0;
    int force_mode = -1;
    int force_no_shm = 0;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            Usage(argv[0]);
            return 0;
        } else if (!strcmp(arg, "--config") && i + 1 < argc) {
            config_override = argv[++i];
        } else if (!strcmp(arg, "--src") && i + 1 < argc) {
            int v[4];
            if (!ParseRect(argv[++i], v, 4)) {
                fprintf(stderr, "supercrt: --src wants X,Y,W,H\n");
                return 2;
            }
            g_params.SrcX = v[0]; g_params.SrcY = v[1];
            g_params.SrcWidth = v[2]; g_params.SrcHeight = v[3];
        } else if (!strcmp(arg, "--dst") && i + 1 < argc) {
            int v[2];
            if (!ParseRect(argv[++i], v, 2)) {
                fprintf(stderr, "supercrt: --dst wants W,H\n");
                return 2;
            }
            g_params.DstWidth = v[0]; g_params.DstHeight = v[1];
        } else if (!strcmp(arg, "--fullscreen")) {
            force_mode = 1;
        } else if (!strcmp(arg, "--borderless")) {
            force_mode = 2;
        } else if (!strcmp(arg, "--no-outline")) {
            g_params.CaptureOutline = 0;
        } else if (!strcmp(arg, "--no-vsync")) {
            g_params.VSync = 0;
        } else if (!strcmp(arg, "--pattern")) {
            use_pattern = 1;
        } else if (!strcmp(arg, "--no-shm")) {
            force_no_shm = 1;
        } else if (!strcmp(arg, "--frame-out") && i + 1 < argc) {
            frame_out = argv[++i];
        } else if (!strcmp(arg, "--dump-target") && i + 1 < argc) {
            dump_target = argv[++i];
        } else if (!strcmp(arg, "--frames") && i + 1 < argc) {
            frames_before_dump = atoi(argv[++i]);
            if (frames_before_dump < 1) {
                frames_before_dump = 1;
            }
        } else {
            fprintf(stderr, "supercrt: unknown option '%s' (try --help)\n", arg);
            return 2;
        }
    }

    // Config precedence: explicit --config, then an INI shipped next to the binary (as in
    // the Windows release), then the XDG location, which is also where saves land.
    const char *sidecar = Params_SidecarConfigPath();
    const char *xdg = Params_XdgConfigPath();
    const char *config_path = config_override;
    if (!config_path) {
        if (sidecar && access(sidecar, R_OK) == 0) {
            config_path = sidecar;
        } else if (xdg) {
            config_path = xdg;
        } else {
            config_path = "supercrt.ini";
        }
    }
    snprintf(app.config_path, sizeof(app.config_path), "%s", config_path);
    const int loaded = Params_Load(&g_params, config_path);
    if (loaded) {
        printf("supercrt: loaded %s\n", config_path);
    }
    if (force_mode >= 0) {
        g_params.Fullscreen = force_mode;
    }

    if (g_params.SrcWidth < 16) g_params.SrcWidth = 16;
    if (g_params.SrcHeight < 16) g_params.SrcHeight = 16;
    if (g_params.DstWidth < 160) g_params.DstWidth = 160;
    if (g_params.DstHeight < 120) g_params.DstHeight = 120;

    // --- X11 ---
    app.dpy = XOpenDisplay(NULL);
    if (!app.dpy) {
        fprintf(stderr, "supercrt: cannot open display %s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    app.screen = DefaultScreen(app.dpy);
    app.root = RootWindow(app.dpy, app.screen);
    app.screen_width = DisplayWidth(app.dpy, app.screen);
    app.screen_height = DisplayHeight(app.dpy, app.screen);

    // Fit the initial window to this desktop: a window larger than the screen cannot be
    // placed sensibly, and one covering the capture region would feed the sim into
    // itself.  The file keeps whatever the user saves from here on.
    if (g_params.DstWidth > app.screen_width) {
        fprintf(stderr, "supercrt: clamping window width %d to screen width %d\n",
                g_params.DstWidth, app.screen_width);
        g_params.DstWidth = app.screen_width;
    }
    if (g_params.DstHeight > app.screen_height) {
        fprintf(stderr, "supercrt: clamping window height %d to screen height %d\n",
                g_params.DstHeight, app.screen_height);
        g_params.DstHeight = app.screen_height;
    }

    {
        int attrs[] = { GLX_RENDER_TYPE, GLX_RGBA_BIT,
                        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
                        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                        GLX_DEPTH_SIZE, 24,
                        GLX_DOUBLEBUFFER, True,
                        None };
        int count = 0;
        GLXFBConfig *configs = glXChooseFBConfig(app.dpy, app.screen, attrs, &count);
        if (!configs || count == 0) {
            fprintf(stderr, "supercrt: no suitable GLX framebuffer configuration\n");
            return 1;
        }
        app.fbconfig = configs[0];
        XFree(configs);
        app.visual_info = glXGetVisualFromFBConfig(app.dpy, app.fbconfig);
        if (!app.visual_info) {
            fprintf(stderr, "supercrt: glXGetVisualFromFBConfig failed\n");
            return 1;
        }
        app.colormap = XCreateColormap(app.dpy, app.root, app.visual_info->visual, AllocNone);
    }

    typedef GLXContext (*PFNCreateContextAttribs)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
    PFNCreateContextAttribs create_attribs =
        (PFNCreateContextAttribs)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    if (create_attribs) {
        int attrs[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
                        GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                        None };
        app.ctx = create_attribs(app.dpy, app.fbconfig, NULL, True, attrs);
    }
    if (!app.ctx) {
        app.ctx = glXCreateNewContext(app.dpy, app.fbconfig, GLX_RGBA_TYPE, NULL, True);
    }
    if (!app.ctx) {
        fprintf(stderr, "supercrt: could not create an OpenGL context\n");
        return 1;
    }

    app.wm_delete_window = XInternAtom(app.dpy, "WM_DELETE_WINDOW", False);
    app.net_wm_state = XInternAtom(app.dpy, "_NET_WM_STATE", False);
    app.net_wm_state_fullscreen = XInternAtom(app.dpy, "_NET_WM_STATE_FULLSCREEN", False);

    app.width = g_params.DstWidth;
    app.height = g_params.DstHeight;
    app.windowed_width = g_params.DstWidth;
    app.windowed_height = g_params.DstHeight;
    app.mode = g_params.Fullscreen ? g_params.Fullscreen : 0;

    // Create the context-bound window (borderless modes get an override-redirect window).
    App_CreateWindow(&app, app.mode == 2, app.width, app.height);

    if (!Gl_Load((void *(*)(const char *))glXGetProcAddressARB)) {
        return 1;
    }
    printf("supercrt: %s | %s | GLSL %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER),
           glGetString(GL_SHADING_LANGUAGE_VERSION));

    if (app.mode != 0) {
        const int mode = app.mode;
        app.mode = 0;
        App_SetMode(&app, mode);
    } else {
        App_ApplySwapInterval(&app, g_params.VSync ? 1 : 0);
    }

    // --- assets ---
    char path[4096];
    Bitmap mask, artifacts;
    Mesh screen_mesh, frame_mesh;
    if (!ResolveAsset(path, sizeof(path), "mask.bmp") || !Bitmap_LoadBMP(path, &mask)) {
        fprintf(stderr, "supercrt: cannot find assets/mask.bmp next to the binary\n");
        return 1;
    }
    if (!ResolveAsset(path, sizeof(path), "artifacts.bmp") || !Bitmap_LoadBMP(path, &artifacts)) {
        fprintf(stderr, "supercrt: cannot find assets/artifacts.bmp next to the binary\n");
        return 1;
    }
    if (!ResolveAsset(path, sizeof(path), "screen.m3d") || !Mesh_LoadM3D(path, &screen_mesh)) {
        fprintf(stderr, "supercrt: cannot find assets/screen.m3d next to the binary\n");
        return 1;
    }
    if (!ResolveAsset(path, sizeof(path), "frame.m3d") || !Mesh_LoadM3D(path, &frame_mesh)) {
        fprintf(stderr, "supercrt: cannot find assets/frame.m3d next to the binary\n");
        return 1;
    }

    // --- GL state ---
    app.mask_tex = CreateTextureFromBitmap(&mask, 1, 1);
    app.artifacts_tex = CreateTextureFromBitmap(&artifacts, 1, 0);
    Bitmap_Free(&mask);
    Bitmap_Free(&artifacts);

    app.samp_point_clamp = CreateSampler(0, 0, 0);
    app.samp_point_repeat = CreateSampler(1, 0, 0);
    app.samp_linear_clamp = CreateSampler(0, 1, 0);
    app.samp_linear_repeat = CreateSampler(1, 1, 0);
    app.samp_linear_border = CreateSampler(0, 1, 1);

    glGenFramebuffers(1, &app.clear_fbo);
    app.even_frame = 1;
    app.slider_row = -1;   // no value is being dragged until a press says otherwise
    app.capture_outline_enabled = g_params.CaptureOutline;
    app.overlay_selected = 0;

    App_CreateCleanTexture(&app);
    App_CreateTargets(&app);
    if (!App_CreatePasses(&app)) {
        return 1;
    }
    if (!MeshGL_Upload(&app.screen_mesh, &screen_mesh) || !MeshGL_Upload(&app.frame_mesh, &frame_mesh)) {
        return 1;
    }
    Mesh_Free(&screen_mesh);
    Mesh_Free(&frame_mesh);

    if (!UI_Init(&app.ui)) {
        return 1;
    }

    app.cursor_fleur = XCreateFontCursor(app.dpy, XC_fleur);
    app.cursor_h = XCreateFontCursor(app.dpy, XC_sb_h_double_arrow);
    app.cursor_v = XCreateFontCursor(app.dpy, XC_sb_v_double_arrow);
    app.cursor_nw = XCreateFontCursor(app.dpy, XC_top_left_corner);
    app.cursor_ne = XCreateFontCursor(app.dpy, XC_top_right_corner);
    app.cursor_sw = XCreateFontCursor(app.dpy, XC_bottom_left_corner);
    app.cursor_se = XCreateFontCursor(app.dpy, XC_bottom_right_corner);

    app.capture = Capture_Create(app.dpy, app.screen);
    if (force_no_shm) {
        Capture_DisableShm(app.capture);
    }
    app.using_shm = Capture_UsesShm(app.capture);
    app.marker = Marker_Create(app.dpy, app.screen);
    Marker_SetEnabled(app.marker, app.capture_outline_enabled);
    Marker_SetRect(app.marker, g_params.SrcX, g_params.SrcY, g_params.SrcWidth, g_params.SrcHeight);

    if (use_pattern) {
        app.pattern = malloc((size_t)g_params.SrcWidth * (size_t)g_params.SrcHeight * 4);
        if (!app.pattern) {
            return 1;
        }
    }

    printf("supercrt: capturing %d,%d %dx%d -> %dx%d window (%s)\n",
           g_params.SrcX, g_params.SrcY, g_params.SrcWidth, g_params.SrcHeight,
           app.width, app.height, app.using_shm ? "MIT-SHM" : "XGetImage");

    // --- main loop ---
    double fps_start = (double)clock() / CLOCKS_PER_SEC;
    double last_frame_time = fps_start;
    int frame_out_written = 0;

    while (!app.quit) {
        HandleEvents(&app);

        App_SyncSourceSize(&app);

        if (app.width != g_params.DstWidth || app.height != g_params.DstHeight) {
            g_params.DstWidth = app.width;
            g_params.DstHeight = app.height;
        }

        // 1. clean source image
        if (app.pattern) {
            FillPattern(app.pattern, g_params.SrcWidth, g_params.SrcHeight, app.frame_index);
            App_UploadCleanRect(&app, 0, 0, g_params.SrcWidth, g_params.SrcHeight, app.pattern,
                                g_params.SrcWidth, 0);
        } else {
            CaptureRegion region;
            if (Capture_Grab(app.capture, g_params.SrcX, g_params.SrcY, g_params.SrcWidth,
                             g_params.SrcHeight, &region)) {
                const int bpp = Capture_BitsPerPixel(app.capture);
                if ((region.offset_x || region.offset_y || region.width != g_params.SrcWidth ||
                     region.height != g_params.SrcHeight)) {
                    // Partially off screen: start from black, then blit what exists.
                    glBindFramebuffer(GL_FRAMEBUFFER, app.clear_fbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                           app.clean_tex, 0);
                    glViewport(0, 0, g_params.SrcWidth, g_params.SrcHeight);
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                }
                if (bpp == 32) {
                    App_UploadCleanRect(&app, region.offset_x, region.offset_y, region.width,
                                        region.height, Capture_Data(app.capture),
                                        Capture_PixelsPerLine(app.capture), 1);
                } else if (bpp == 24) {
                    App_UploadCleanRect(&app, region.offset_x, region.offset_y, region.width,
                                        region.height, Capture_Data(app.capture),
                                        Capture_PixelsPerLine(app.capture), 1);
                } else {
                    fprintf(stderr, "supercrt: unsupported root window depth (%d bpp)\n", bpp);
                    app.quit = 1;
                }
            }
        }

        // 2-6. CRT pipeline
        const int current = app.even_frame ? 0 : 1;
        const int source = app.even_frame ? 1 : 0;
        RenderComposite(&app, current, source);
        RenderMeshes(&app, current);
        RenderBloom(&app);
        RenderPresent(&app);

        UI_Begin(&app.ui, app.width, app.height);
        if (app.overlay_open) {
            Overlay_Draw(&app);
        } else {
            Hud_Draw(&app);
        }
        UI_End(&app.ui);

        if (frame_out && !frame_out_written && app.frame_index >= frames_before_dump) {
            // --dump-target reads an intermediate buffer instead of the swap chain, which
            // is how the port's verification separates capture, composite, mesh and bloom
            // stages from one another.
            int dump_w = app.width;
            int dump_h = app.height;
            unsigned fbo = 0;
            if (dump_target && !strcmp(dump_target, "composite")) {
                fbo = app.comp[app.even_frame ? 1 : 0].fbo;
                dump_w = app.comp[0].width;
                dump_h = app.comp[0].height;
            } else if (dump_target && !strcmp(dump_target, "full")) {
                fbo = app.full.fbo;
            } else if (dump_target && !strcmp(dump_target, "down")) {
                fbo = app.down.fbo;
                dump_w = app.down.width;
                dump_h = app.down.height;
            } else if (dump_target && !strcmp(dump_target, "up")) {
                fbo = app.up.fbo;
            } else if (dump_target && !strcmp(dump_target, "clean")) {
                fbo = app.clear_fbo;
                dump_w = g_params.SrcWidth;
                dump_h = g_params.SrcHeight;
            }

            unsigned char *pixels = malloc((size_t)dump_w * (size_t)dump_h * 4);
            if (pixels) {
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glReadBuffer(fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, dump_w, dump_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                if (WritePPM(frame_out, dump_w, dump_h, pixels)) {
                    printf("supercrt: wrote %s (%dx%d, target %s)\n", frame_out, dump_w, dump_h,
                           dump_target ? dump_target : "window");
                }
                free(pixels);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            frame_out_written = 1;
            app.quit = 1;
        }

        glXSwapBuffers(app.dpy, app.win);

        // Region outline follows the settings, and stays on top in borderless mode.
        static int last_region[4] = { -1, -1, -1, -1 };
        if (RegionChanged(last_region, &g_params)) {
            Trace("marker: rect -> %d,%d %dx%d", g_params.SrcX, g_params.SrcY, g_params.SrcWidth,
                  g_params.SrcHeight);
            Marker_SetRect(app.marker, g_params.SrcX, g_params.SrcY, g_params.SrcWidth, g_params.SrcHeight);
            RegionRemember(last_region, &g_params);
        }
        if (app.mode == 2 && g_params.AlwaysOnTop && (app.frame_index % 120) == 0) {
            XRaiseWindow(app.dpy, app.win);
        }

        app.even_frame = !app.even_frame;
        ++app.frame_index;
        if (app.status_frames > 0) {
            --app.status_frames;
        }

        // Frame pacing when vsync is unavailable (or forced off).
        const double now = (double)clock() / CLOCKS_PER_SEC;
        if (!g_params.VSync) {
            const double target = 1.0 / 60.0;
            double elapsed = now - last_frame_time;
            if (elapsed < target) {
                struct timespec ts;
                double sleep_s = target - elapsed;
                ts.tv_sec = (time_t)sleep_s;
                ts.tv_nsec = (long)((sleep_s - (double)ts.tv_sec) * 1e9);
                nanosleep(&ts, NULL);
            }
        }
        last_frame_time = (double)clock() / CLOCKS_PER_SEC;

        app.fps_frames++;
        if (now - fps_start >= 1.0) {
            app.fps = (double)app.fps_frames / (now - fps_start);
            app.fps_frames = 0;
            fps_start = now;
        }
    }

    // Teardown.  GL objects and the X connection are released by the process exit (which
    // is also all the reference implementation does); the capture segment and the outline
    // window are handed back explicitly because they outlive the connection otherwise.
    Capture_Destroy(app.capture);
    Marker_Destroy(app.marker);
    UI_Shutdown(&app.ui);
    XDestroyWindow(app.dpy, app.win);
    XCloseDisplay(app.dpy);

    (void)loaded;
    printf("supercrt: exiting\n");
    return 0;
}
