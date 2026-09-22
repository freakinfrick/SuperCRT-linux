// Tiny immediate-mode 2D layer for the settings overlay: solid rectangles and bitmap
// text drawn straight into the swap chain, batched into one dynamic vertex buffer.
#pragma once

typedef struct {
    float x, y, u, v;
    float r, g, b, a;
    float textured;
} UIVertex;

typedef struct {
    unsigned program;
    unsigned vao, vbo;
    unsigned atlas;
    int vbo_bytes;
    UIVertex *verts;
    int count, capacity;
    float width, height;
    int clip_active;
} UI;

// Builds the program and uploads the baked font atlas.  Returns 1 on success.
int UI_Init(UI *ui);
void UI_Shutdown(UI *ui);

// Starts a frame in window pixel coordinates (origin top-left).
void UI_Begin(UI *ui, int width, int height);

void UI_Rect(UI *ui, float x, float y, float w, float h, float r, float g, float b, float a);
// Draws one line of ASCII text with its top-left corner at (x, y).
void UI_Text(UI *ui, float x, float y, float r, float g, float b, float a, const char *text);
void UI_TextF(UI *ui, float x, float y, float r, float g, float b, float a, const char *fmt, ...);

// Width / line height of text in pixels, for layout.
float UI_TextWidth(const char *text);
float UI_LineHeight(void);

// Clips everything queued afterwards to a window-pixel rectangle (origin top-left), so a
// scrolling list can run past its frame without drawing over what is outside it.  UI_ClearClip
// restores the whole window.  Each change flushes the batch it has already queued.
void UI_SetClip(UI *ui, float x, float y, float w, float h);
void UI_ClearClip(UI *ui);

// Uploads and draws everything queued since UI_Begin.
void UI_End(UI *ui);
