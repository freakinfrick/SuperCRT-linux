// Tunables + INI config, ported from CRTSim's Parameters.h / Parameters.cpp.
//
// Field names map 1:1 onto the reference implementation's statics so the HLSL->GLSL port
// can be diffed against the original by eye.  Defaults are the upstream values.
#pragma once

typedef struct {
    // --- Capture region (the reference's 256x224 "source window" at 64,64) ---
    int   SrcX, SrcY, SrcWidth, SrcHeight;

    // --- Output window ("destination window") ---
    int   DstWidth, DstHeight;
    int   Fullscreen;        // 0 = windowed, 1 = Fullscreen, 2 = FullscreenWindowed
    int   VSync;
    int   CaptureOutline;    // draw the click-through capture-region frame
    int   AlwaysOnTop;       // re-raise the output window in fullscreen modes

    // --- Composite pass ---
    float Sharp;             // Tuning_Sharp
    float Persistence[4];    // Tuning_Persistence (a is unused by the shader)
    float Bleed;             // Tuning_Bleed
    float Artifacts;         // Tuning_Artifacts

    // --- Screen / frame meshes ---
    float PixelRatio;        // Tuning_PixelRatio
    float Overscan;          // Tuning_Overscan
    float Dimming;           // Tuning_Dimming
    float Saturation;        // Tuning_Satur
    float ReflectionScalar;  // Tuning_ReflScalar
    float Barrel;            // Tuning_Barrel
    float MaskBrightness;    // Tuning_Mask_Brightness
    float MaskOpacity;       // Tuning_Mask_Opacity
    float DiffuseBrightness; // Tuning_Diff_Brightness
    float SpecularBrightness;// Tuning_Spec_Brightness
    float SpecularPower;     // Tuning_Spec_Power
    float FresnelBrightness; // Tuning_Fres_Brightness
    float LightPos[3];       // Tuning_LightPos (xyz; w is unused)
    float FrameColor[3];     // Tuning_FrameColor (rgb; a is 1)

    // --- Bloom ---
    float BloomDownsampleSpread; // Tuning_Bloom_Downsample_Spread
    float BloomUpsampleSpread;   // Tuning_Bloom_Upsample_Spread
    float BloomIntensity;        // Tuning_Bloom_Intensity
    float BloomPower;            // Tuning_Bloom_Power
} Params;

extern Params g_params;

typedef struct {
    const char *label;
    void       *ptr;
    int         is_int;
    float       step, coarse, min, max;
    const char *fmt;
} Tunable;

void Params_Defaults(Params *p);

// Loads over the defaults.  Returns 1 when the file was read, 0 when absent (not an
// error), -1 when present but unreadable.
int Params_Load(Params *p, const char *path);

// Writes every key.  Creates parent directories.  Returns 0 on success.
int Params_Save(const Params *p, const char *path);

// $XDG_CONFIG_HOME/supercrt/supercrt.ini, or ~/.config/supercrt/supercrt.ini.
const char *Params_XdgConfigPath(void);

// Path of the INI shipped next to the executable, or NULL when there is none.
const char *Params_SidecarConfigPath(void);

// Directory containing the running executable, or NULL.
const char *Params_ExeDir(void);

// The overlay's editable rows, in display order.  *count receives the length.
const Tunable *Params_Tunables(int *count);
