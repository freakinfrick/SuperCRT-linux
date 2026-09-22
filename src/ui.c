// See ui.h.
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_atlas.h"
#include "gl_api.h"
#include "shader.h"
#include "shaders.h"
#include "ui.h"

static const Glyph *GlyphFor(unsigned char c)
{
    if (c < FONT_FIRST || c > FONT_LAST) {
        c = '?';
    }
    return &kGlyphs[c - FONT_FIRST];
}

float UI_LineHeight(void) { return (float)FONT_LINE_HEIGHT; }

float UI_TextWidth(const char *text)
{
    float w = 0.0f;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        w += (float)GlyphFor(*p)->adv;
    }
    return w;
}

int UI_Init(UI *ui)
{
    memset(ui, 0, sizeof(*ui));

    ui->program = Shader_Build(kVS_UI, kFS_UI, "overlay");
    if (!ui->program) {
        return 0;
    }

    glGenVertexArrays(1, &ui->vao);
    glBindVertexArray(ui->vao);
    glGenBuffers(1, &ui->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, ui->vbo);

    const GLsizei stride = (GLsizei)sizeof(UIVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(UIVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(UIVertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(UIVertex, r));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(UIVertex, textured));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenTextures(1, &ui->atlas);
    glBindTexture(GL_TEXTURE_2D, ui->atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FONT_ATLAS_W, FONT_ATLAS_H, 0, GL_RED,
                 GL_UNSIGNED_BYTE, kFontAtlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return Gl_Check("UI_Init") == GL_NO_ERROR;
}

void UI_Shutdown(UI *ui)
{
    free(ui->verts);
    ui->verts = NULL;
    if (ui->atlas) {
        glDeleteTextures(1, &ui->atlas);
    }
    if (ui->vbo) {
        glDeleteBuffers(1, &ui->vbo);
    }
    if (ui->vao) {
        glDeleteVertexArrays(1, &ui->vao);
    }
    if (ui->program) {
        glDeleteProgram(ui->program);
    }
}

void UI_Begin(UI *ui, int width, int height)
{
    ui->count = 0;
    ui->width = (float)width;
    ui->height = (float)height;
    ui->clip_active = 0;
}

static UIVertex *PushVerts(UI *ui, int n)
{
    if (ui->count + n > ui->capacity) {
        int cap = ui->capacity ? ui->capacity : 4096;
        while (cap < ui->count + n) {
            cap *= 2;
        }
        UIVertex *grown = realloc(ui->verts, (size_t)cap * sizeof(UIVertex));
        if (!grown) {
            return NULL;
        }
        ui->verts = grown;
        ui->capacity = cap;
    }
    UIVertex *out = ui->verts + ui->count;
    ui->count += n;
    return out;
}

static void PushQuad(UI *ui, float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1,
                     float r, float g, float b, float a, float textured)
{
    UIVertex *v = PushVerts(ui, 6);
    if (!v) {
        return;
    }
    const float xs[6] = { x0, x0, x1, x0, x1, x1 };
    const float ys[6] = { y0, y1, y1, y0, y1, y0 };
    const float us[6] = { u0, u0, u1, u0, u1, u1 };
    const float vs[6] = { v0, v1, v1, v0, v1, v0 };
    for (int i = 0; i < 6; ++i) {
        v[i].x = xs[i];
        v[i].y = ys[i];
        v[i].u = us[i];
        v[i].v = vs[i];
        v[i].r = r;
        v[i].g = g;
        v[i].b = b;
        v[i].a = a;
        v[i].textured = textured;
    }
}

void UI_Rect(UI *ui, float x, float y, float w, float h, float r, float g, float b, float a)
{
    PushQuad(ui, x, y, x + w, y + h, 0.0f, 0.0f, 0.0f, 0.0f, r, g, b, a, 0.0f);
}

void UI_Text(UI *ui, float x, float y, float r, float g, float b, float a, const char *text)
{
    const float inv_w = 1.0f / (float)FONT_ATLAS_W;
    const float inv_h = 1.0f / (float)FONT_ATLAS_H;

    // The atlas is a pixel font sampled with GL_NEAREST: a glyph drawn from a fractional
    // origin lands between texels and resamples, which duplicates and drops whole scanlines
    // of the bitmap.  Advances and offsets are whole pixels, so snapping the pen is enough
    // to put every glyph in the string on the pixel grid.
    x = floorf(x);
    y = floorf(y);

    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        const Glyph *glyph = GlyphFor(*p);
        if (glyph->w && glyph->h) {
            const float gx = x + (float)glyph->off_x;
            const float gy = y + (float)glyph->off_y;
            PushQuad(ui, gx, gy, gx + (float)glyph->w, gy + (float)glyph->h,
                     (float)glyph->u * inv_w, (float)glyph->v * inv_h,
                     (float)(glyph->u + glyph->w) * inv_w, (float)(glyph->v + glyph->h) * inv_h,
                     r, g, b, a, 1.0f);
        }
        x += (float)glyph->adv;
    }
}

void UI_TextF(UI *ui, float x, float y, float r, float g, float b, float a, const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    UI_Text(ui, x, y, r, g, b, a, buf);
}

// Draws everything queued so far and starts a new batch.  A clip change has to flush first:
// the vertices carry no clip of their own, the scissor box applies to whole draw calls.
static void UI_Flush(UI *ui)
{
    if (ui->count == 0) {
        return;
    }

    glUseProgram(ui->program);
    glUniform2f(glGetUniformLocation(ui->program, "Viewport"), ui->width, ui->height);
    glUniform1i(glGetUniformLocation(ui->program, "Atlas"), 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ui->atlas);

    glBindVertexArray(ui->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui->vbo);
    const int bytes = ui->count * (int)sizeof(UIVertex);
    glBufferData(GL_ARRAY_BUFFER, bytes, ui->verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, ui->count);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    ui->count = 0;
    Gl_Check("UI_Flush");
}

void UI_SetClip(UI *ui, float x, float y, float w, float h)
{
    UI_Flush(ui);

    int sx = (int)floorf(x);
    int sy = (int)floorf(y);
    int sw = (int)ceilf(w);
    int sh = (int)ceilf(h);
    if (w <= 0.0f) {
        sw = 0;
    }
    if (h <= 0.0f) {
        sh = 0;
    }
    if (sx < 0) {
        sw += sx;
        sx = 0;
    }
    if (sy < 0) {
        sh += sy;
        sy = 0;
    }
    if (sw < 0) {
        sw = 0;
    }
    if (sh < 0) {
        sh = 0;
    }

    // Scissor is measured from the bottom-left of the drawable; the UI is top-left.
    glEnable(GL_SCISSOR_TEST);
    glScissor(sx, (int)ui->height - (sy + sh), sw, sh);
    ui->clip_active = 1;
}

void UI_ClearClip(UI *ui)
{
    UI_Flush(ui);
    if (ui->clip_active) {
        glDisable(GL_SCISSOR_TEST);
        ui->clip_active = 0;
    }
}

void UI_End(UI *ui)
{
    if (ui->count == 0 && !ui->clip_active) {
        return;
    }

    UI_ClearClip(ui);
    UI_Flush(ui);
    glUseProgram(0);

    glDisable(GL_BLEND);
    Gl_Check("UI_End");
}
