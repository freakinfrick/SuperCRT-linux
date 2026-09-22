// See params.h.
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "params.h"

Params g_params;

void Params_Defaults(Params *p)
{
    memset(p, 0, sizeof(*p));

    p->SrcX = 64;
    p->SrcY = 64;
    p->SrcWidth = 256;
    p->SrcHeight = 224;

    p->DstWidth = 1600;
    p->DstHeight = 900;
    p->Fullscreen = 0;
    p->VSync = 1;
    p->CaptureOutline = 1;
    p->AlwaysOnTop = 0;
    p->ClickThrough = 0;
    p->IgnoreSelf = 1;

    p->Sharp = 0.8f;
    p->Persistence[0] = 0.7f;
    p->Persistence[1] = 0.525f;
    p->Persistence[2] = 0.42f;
    p->Persistence[3] = 0.0f;
    p->Bleed = 0.5f;
    p->Artifacts = 0.5f;

    p->PixelRatio = 8.0f / 7.0f;
    p->Overscan = 1.0f;
    p->Dimming = 0.5f;
    p->Saturation = 1.35f;
    p->ReflectionScalar = 0.3f;
    p->Barrel = -0.115f;
    p->MaskBrightness = 0.45f;
    p->MaskOpacity = 1.0f;

    p->DiffuseBrightness = 0.5f;
    p->SpecularBrightness = 0.35f;
    p->SpecularPower = 50.0f;
    p->FresnelBrightness = 1.0f;
    p->LightPos[0] = -10.0f;
    p->LightPos[1] = -5.0f;
    p->LightPos[2] = 10.0f;
    p->FrameColor[0] = 0.06f;
    p->FrameColor[1] = 0.06f;
    p->FrameColor[2] = 0.06f;

    p->BloomDownsampleSpread = 0.025f;
    p->BloomUpsampleSpread = 0.025f;
    p->BloomIntensity = 0.25f;
    p->BloomPower = 2.0f;
}

static const Tunable kTunables[] = {
    { "SrcX",                  &g_params.SrcX,                  1, 1,    16,   -16384, 16384, "%d" },
    { "SrcY",                  &g_params.SrcY,                  1, 1,    16,   -16384, 16384, "%d" },
    { "SrcWidth",              &g_params.SrcWidth,              1, 1,    16,   16,     7680,  "%d" },
    { "SrcHeight",             &g_params.SrcHeight,             1, 1,    16,   16,     7680,  "%d" },
    { "Sharp",                 &g_params.Sharp,                 0, 0.01f, 0.1f, 0.0f,   1.0f,  "%.3f" },
    { "PersistenceR",          &g_params.Persistence[0],        0, 0.01f, 0.1f, 0.0f,   1.0f,  "%.3f" },
    { "PersistenceG",          &g_params.Persistence[1],        0, 0.01f, 0.1f, 0.0f,   1.0f,  "%.3f" },
    { "PersistenceB",          &g_params.Persistence[2],        0, 0.01f, 0.1f, 0.0f,   1.0f,  "%.3f" },
    { "Bleed",                 &g_params.Bleed,                 0, 0.01f, 0.1f, 0.0f,   1.0f,  "%.3f" },
    { "Artifacts",             &g_params.Artifacts,             0, 0.01f, 0.1f, 0.0f,   1.0f,  "%.3f" },
    { "PixelRatio",            &g_params.PixelRatio,            0, 0.01f, 0.1f, 0.25f,  2.0f,  "%.4f" },
    { "Overscan",              &g_params.Overscan,              0, 0.01f, 0.1f, 0.5f,   2.0f,  "%.3f" },
    { "Dimming",               &g_params.Dimming,               0, 0.01f, 0.1f, 0.0f,   1.0f,  "%.3f" },
    { "Saturation",            &g_params.Saturation,            0, 0.01f, 0.1f, 0.0f,   3.0f,  "%.3f" },
    { "ReflectionScalar",      &g_params.ReflectionScalar,      0, 0.01f, 0.1f, 0.0f,   2.0f,  "%.3f" },
    { "Barrel",                &g_params.Barrel,                0, 0.005f,0.05f,-1.0f,  1.0f,  "%.4f" },
    { "MaskBrightness",        &g_params.MaskBrightness,        0, 0.01f, 0.1f, -1.0f,  1.0f,  "%.3f" },
    { "MaskOpacity",           &g_params.MaskOpacity,           0, 0.01f, 0.1f, 0.0f,   1.0f,  "%.3f" },
    { "DiffuseBrightness",     &g_params.DiffuseBrightness,     0, 0.01f, 0.1f, 0.0f,   2.0f,  "%.3f" },
    { "SpecularBrightness",    &g_params.SpecularBrightness,    0, 0.01f, 0.1f, 0.0f,   2.0f,  "%.3f" },
    { "SpecularPower",         &g_params.SpecularPower,         0, 1.0f,  10.0f, 1.0f,  200.0f,"%.1f" },
    { "FresnelBrightness",     &g_params.FresnelBrightness,     0, 0.05f, 0.5f, 0.0f,   4.0f,  "%.3f" },
    { "LightPosX",             &g_params.LightPos[0],           0, 0.5f,  5.0f, -50.0f, 50.0f, "%.2f" },
    { "LightPosY",             &g_params.LightPos[1],           0, 0.5f,  5.0f, -50.0f, 50.0f, "%.2f" },
    { "LightPosZ",             &g_params.LightPos[2],           0, 0.5f,  5.0f, -50.0f, 50.0f, "%.2f" },
    { "FrameColorR",           &g_params.FrameColor[0],         0, 0.01f, 0.1f, 0.0f,   1.0f,  "%.3f" },
    { "FrameColorG",           &g_params.FrameColor[1],         0, 0.01f, 0.1f, 0.0f,   1.0f,  "%.3f" },
    { "FrameColorB",           &g_params.FrameColor[2],         0, 0.01f, 0.1f, 0.0f,   1.0f,  "%.3f" },
    { "BloomDownsampleSpread", &g_params.BloomDownsampleSpread, 0, 0.002f,0.02f,0.0f,  0.5f,  "%.4f" },
    { "BloomUpsampleSpread",   &g_params.BloomUpsampleSpread,   0, 0.002f,0.02f,0.0f,  0.5f,  "%.4f" },
    { "BloomIntensity",        &g_params.BloomIntensity,        0, 0.01f, 0.1f, 0.0f,   2.0f,  "%.3f" },
    { "BloomPower",            &g_params.BloomPower,            0, 0.05f, 0.5f, 0.5f,   5.0f,  "%.3f" },
};

const Tunable *Params_Tunables(int *count)
{
    *count = (int)(sizeof(kTunables) / sizeof(kTunables[0]));
    return kTunables;
}

static Tunable *FindTunable(const char *key)
{
    int n = 0;
    const Tunable *t = Params_Tunables(&n);
    for (int i = 0; i < n; ++i) {
        if (strcasecmp(t[i].label, key) == 0) {
            return (Tunable *)&t[i];
        }
    }
    return NULL;
}

static char *Trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        ++s;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static int ParseBool(const char *v)
{
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on")) {
        return 1;
    }
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off")) {
        return 0;
    }
    return atoi(v) != 0;
}

static int ApplyKey(Params *p, const char *key, const char *value)
{
    if (!strcasecmp(key, "DstWidth"))         { p->DstWidth = atoi(value); return 1; }
    if (!strcasecmp(key, "DstHeight"))        { p->DstHeight = atoi(value); return 1; }
    if (!strcasecmp(key, "Fullscreen"))       { p->Fullscreen = atoi(value) ? 1 : 0; return 1; }
    if (!strcasecmp(key, "FullscreenWindowed")) { p->Fullscreen = ParseBool(value) ? 2 : 0; return 1; }
    if (!strcasecmp(key, "VSync"))            { p->VSync = ParseBool(value); return 1; }
    if (!strcasecmp(key, "CaptureOutline"))   { p->CaptureOutline = ParseBool(value); return 1; }
    if (!strcasecmp(key, "AlwaysOnTop"))      { p->AlwaysOnTop = ParseBool(value); return 1; }
    if (!strcasecmp(key, "ClickThrough"))     { p->ClickThrough = ParseBool(value); return 1; }
    if (!strcasecmp(key, "IgnoreSelf"))       { p->IgnoreSelf = ParseBool(value); return 1; }

    // Accept the reference's packed spelling as well as the per-channel keys.
    if (!strcasecmp(key, "Persistence")) {
        const char *cursor = value;
        for (int i = 0; i < 4 && *cursor; ++i) {
            char *end = NULL;
            float v = strtof(cursor, &end);
            if (end == cursor) {
                return 0;
            }
            p->Persistence[i] = v;
            cursor = end;
            while (*cursor == ',' || *cursor == ' ' || *cursor == '\t') {
                ++cursor;
            }
        }
        return 1;
    }

    Tunable *t = FindTunable(key);
    if (!t) {
        return 0;
    }
    if (t->is_int) {
        *(int *)t->ptr = atoi(value);
    } else {
        *(float *)t->ptr = strtof(value, NULL);
    }
    return 1;
}

int Params_Load(Params *p, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }

    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        ++lineno;
        char *s = Trim(line);
        if (*s == '\0' || *s == '#' || *s == ';' || *s == '[') {
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = Trim(s);
        char *value = Trim(eq + 1);
        if (!ApplyKey(p, key, value)) {
            fprintf(stderr, "supercrt: %s:%d: ignoring unknown key '%s'\n", path, lineno, key);
        }
    }

    fclose(f);
    return 1;
}

static void MkdirParents(const char *path)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *s = buf + 1; *s; ++s) {
        if (*s == '/') {
            *s = '\0';
            mkdir(buf, 0700);
            *s = '/';
        }
    }
}

int Params_Save(const Params *p, const char *path)
{
    MkdirParents(path);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "supercrt: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(f, "# SuperCRT (Linux port) settings.  Esc in the sim window to edit live.\n\n");

    fprintf(f, "[Capture]\n");
    fprintf(f, "SrcX=%d\nSrcY=%d\nSrcWidth=%d\nSrcHeight=%d\n\n",
            p->SrcX, p->SrcY, p->SrcWidth, p->SrcHeight);

    fprintf(f, "[Window]\n");
    fprintf(f, "DstWidth=%d\nDstHeight=%d\n", p->DstWidth, p->DstHeight);
    fprintf(f, "Fullscreen=%d\n", p->Fullscreen == 1 ? 1 : 0);
    fprintf(f, "FullscreenWindowed=%s\n", p->Fullscreen == 2 ? "true" : "false");
    fprintf(f, "VSync=%s\n", p->VSync ? "true" : "false");
    fprintf(f, "CaptureOutline=%s\n", p->CaptureOutline ? "true" : "false");
    fprintf(f, "AlwaysOnTop=%s\n", p->AlwaysOnTop ? "true" : "false");
    fprintf(f, "ClickThrough=%s\n", p->ClickThrough ? "true" : "false");
    fprintf(f, "IgnoreSelf=%s\n\n", p->IgnoreSelf ? "true" : "false");

    fprintf(f, "[CRT]\n");
    int n = 0;
    const Tunable *t = Params_Tunables(&n);
    for (int i = 0; i < n; ++i) {
        // Already written under [Capture]; repeating them would create two sources of
        // truth for one value.
        if (!strncmp(t[i].label, "Src", 3)) {
            continue;
        }
        if (t[i].is_int) {
            fprintf(f, "%s=%d\n", t[i].label, *(const int *)t[i].ptr);
        } else {
            fprintf(f, "%s=%.6g\n", t[i].label, (double)*(const float *)t[i].ptr);
        }
    }
    fprintf(f, "PersistenceA=%.6g\n", (double)p->Persistence[3]);

    fclose(f);
    return 0;
}

const char *Params_XdgConfigPath(void)
{
    static char path[4608];
    const char *base = getenv("XDG_CONFIG_HOME");
    if (base && *base) {
        snprintf(path, sizeof(path), "%s/supercrt/supercrt.ini", base);
        return path;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/.config/supercrt/supercrt.ini", home);
    return path;
}

// Path of the running executable's directory, or NULL.
const char *Params_ExeDir(void)
{
    static char dir[4096];
    ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
    if (n <= 0) {
        return NULL;
    }
    dir[n] = '\0';
    char *slash = strrchr(dir, '/');
    if (!slash) {
        return NULL;
    }
    *slash = '\0';
    return dir;
}

const char *Params_SidecarConfigPath(void)
{
    static char path[4608];
    const char *dir = Params_ExeDir();
    if (!dir) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/supercrt.ini", dir);
    return path;
}
