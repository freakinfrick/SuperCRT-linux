// GLSL ports of the reference implementation's HLSL effect files.
//
// The HLSL files are compiled per-technique; here every technique becomes one program.
// Texture-orientation note: CRTSim renders with D3D's convention where v = 0 is the top
// of the image.  This port keeps the same *sample-space* mappings by uploading textures
// top-row-first (the captured screen as-is, BMPs row-reversed) and using natural quad
// UVs, so the mesh UVs and every shader body port over untouched.
#pragma once

// Oversized-triangle fullscreen quad, matching MakeFullscreenQuad in the reference.
#define kVS_Fullscreen \
    "#version 330 core\n" \
    "layout(location = 0) in vec2 aPos;\n" \
    "out vec2 vUV;\n" \
    "void main() {\n" \
    "    vUV = aPos * 0.5 + 0.5;\n" \
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n" \
    "}\n"

// composite.fx -- temporally stale bloom/bleed, NTSC artifacts and unsharp masking,
// evaluated in the source buffer's pixel grid.
#define kFS_Composite \
    "#version 330 core\n" \
    "in vec2 vUV;\n" \
    "out vec4 oColor;\n" \
    "uniform vec2 RcpScrWidth;\n" \
    "uniform vec2 RcpScrHeight;\n" \
    "uniform float Tuning_Sharp;\n" \
    "uniform vec4 Tuning_Persistence;\n" \
    "uniform float Tuning_Bleed;\n" \
    "uniform float Tuning_Artifacts;\n" \
    "uniform sampler2D curFrameMap;\n" \
    "uniform sampler2D prevFrameMap;\n" \
    "uniform sampler2D NTSCArtifactTex;\n" \
    "uniform float NTSCLerp;\n" \
    "const float SharpWeight[3] = float[3](1.0, -0.3162277, 0.1);\n" \
    "float Brightness(vec4 v) { return dot(v, vec4(0.299, 0.587, 0.114, 0.0)); }\n" \
    "void main() {\n" \
    "    vec4 NTSCArtifact1 = texture(NTSCArtifactTex, vUV);\n" \
    "    vec4 NTSCArtifact2 = texture(NTSCArtifactTex, vUV + RcpScrHeight);\n" \
    "    vec4 NTSCArtifact = mix(NTSCArtifact1, NTSCArtifact2, NTSCLerp);\n" \
    "    vec2 LeftUV = vUV - RcpScrWidth;\n" \
    "    vec2 RightUV = vUV + RcpScrWidth;\n" \
    "    vec4 Cur_Left = texture(curFrameMap, LeftUV);\n" \
    "    vec4 Cur_Local = texture(curFrameMap, vUV);\n" \
    "    vec4 Cur_Right = texture(curFrameMap, RightUV);\n" \
    "    vec4 TunedNTSC = NTSCArtifact * Tuning_Artifacts;\n" \
    "    vec4 Prev_Left = texture(prevFrameMap, LeftUV);\n" \
    "    vec4 Prev_Local = texture(prevFrameMap, vUV);\n" \
    "    vec4 Prev_Right = texture(prevFrameMap, RightUV);\n" \
    "    Cur_Local = clamp(Cur_Local + (((Cur_Left - Cur_Local) + (Cur_Right - Cur_Local)) * TunedNTSC), 0.0, 1.0);\n" \
    "    float curBrt = Brightness(Cur_Local);\n" \
    "    float offset = 0.0;\n" \
    "    for (int i = 0; i < 3; ++i) {\n" \
    "        // Upstream hardcodes 1/256 here; the actual source width is used instead so\n" \
    "        // that non-default capture widths step by one texel.\n" \
    "        vec2 StepSize = vec2(RcpScrWidth.x, 0.0) * float(i + 1);\n" \
    "        vec4 neighborleft = texture(curFrameMap, vUV - StepSize);\n" \
    "        vec4 neighborright = texture(curFrameMap, vUV + StepSize);\n" \
    "        float NBrtL = Brightness(neighborleft);\n" \
    "        float NBrtR = Brightness(neighborright);\n" \
    "        offset += ((curBrt - NBrtL) + (curBrt - NBrtR)) * SharpWeight[i];\n" \
    "    }\n" \
    "    Cur_Local = clamp(Cur_Local + (offset * Tuning_Sharp * mix(vec4(1.0), NTSCArtifact, Tuning_Artifacts)), 0.0, 1.0);\n" \
    "    Cur_Local = clamp(max(Cur_Local, Tuning_Persistence * (1.0 / (1.0 + (2.0 * Tuning_Bleed))) * (Prev_Local + ((Prev_Left + Prev_Right) * Tuning_Bleed))), 0.0, 1.0);\n" \
    "    oColor = Cur_Local;\n" \
    "}\n"

// crtbase.fx + screen.fx / frame.fx shared vertex stage.  Attributes line up with the
// .m3d streams (position, normal, D3DCOLOR, texcoord0, texcoord1).
#define kVS_Mesh \
    "#version 330 core\n" \
    "layout(location = 0) in vec3 aPos;\n" \
    "layout(location = 1) in vec3 aNormal;\n" \
    "layout(location = 2) in vec4 aColor;\n" \
    "layout(location = 3) in vec2 aUV;\n" \
    "layout(location = 4) in float aBlend;\n" \
    "uniform mat4 wvpMat;\n" \
    "uniform vec3 camPos;\n" \
    "uniform vec3 Tuning_LightPos;\n" \
    "out vec4 vColor;\n" \
    "out vec2 vUV;\n" \
    "out vec3 vNormal;\n" \
    "out vec3 vCamDir;\n" \
    "out vec3 vLightDir;\n" \
    "out float vBlend;\n" \
    "void main() {\n" \
    "    // The reference multiplies by worldMat and its inverse-transpose; worldMat is\n" \
    "    // always identity in this application, so positions are already world space.\n" \
    "    gl_Position = wvpMat * vec4(aPos, 1.0);\n" \
    "    vColor = aColor;\n" \
    "    vUV = aUV;\n" \
    "    vNormal = aNormal;\n" \
    "    vCamDir = camPos - aPos;\n" \
    "    vLightDir = Tuning_LightPos - aPos;\n" \
    "    vBlend = aBlend;\n" \
    "}\n"

// crtbase.fx's SampleCRT(), shared by the screen and frame pixel shaders.
#define SUPERCRT_GLSL_SAMPLE_CRT                                                              \
    "uniform vec2 UVScalar;\n"                                                                \
    "uniform vec2 UVOffset;\n"                                                                \
    "uniform vec2 CRTMask_Scale;\n"                                                           \
    "uniform float Tuning_Overscan;\n"                                                        \
    "uniform float Tuning_Satur;\n"                                                           \
    "uniform float Tuning_Barrel;\n"                                                          \
    "uniform float Tuning_Mask_Brightness;\n"                                                 \
    "uniform float Tuning_Mask_Opacity;\n"                                                    \
    "uniform sampler2D compFrameMap;\n"                                                       \
    "uniform sampler2D shadowMaskMap;\n"                                                      \
    "vec4 SampleCRT(vec2 uv) {\n"                                                             \
    "    vec2 ScaledUV = uv * UVScalar + UVOffset;\n"                                         \
    "    vec2 scanuv = ScaledUV * CRTMask_Scale;\n"                                           \
    "    vec3 scantex = texture(shadowMaskMap, scanuv).rgb;\n"                                \
    "    scantex += Tuning_Mask_Brightness;\n"                                                \
    "    scantex = mix(vec3(1.0), scantex, Tuning_Mask_Opacity);\n"                           \
    "    vec2 overscanuv = (ScaledUV * Tuning_Overscan) - ((Tuning_Overscan - 1.0) * 0.5);\n" \
    "    overscanuv = overscanuv - vec2(0.5, 0.5);\n"                                         \
    "    float rsq = (overscanuv.x * overscanuv.x) + (overscanuv.y * overscanuv.y);\n"        \
    "    overscanuv = overscanuv + (overscanuv * (Tuning_Barrel * rsq)) + vec2(0.5, 0.5);\n"  \
    "    vec3 comptex = texture(compFrameMap, overscanuv).rgb;\n"                             \
    "    vec4 emissive = vec4(comptex * scantex, 1.0);\n"                                     \
    "    float desat = dot(vec4(0.299, 0.587, 0.114, 0.0), emissive);\n"                      \
    "    emissive = mix(vec4(desat, desat, desat, 1.0), emissive, Tuning_Satur);\n"           \
    "    return emissive;\n"                                                                  \
    "}\n"

#define kFS_Screen \
    "#version 330 core\n" \
    "in vec4 vColor;\n" \
    "in vec2 vUV;\n" \
    "in vec3 vNormal;\n" \
    "in vec3 vCamDir;\n" \
    "in vec3 vLightDir;\n" \
    "in float vBlend;\n" \
    "out vec4 oColor;\n" \
    SUPERCRT_GLSL_SAMPLE_CRT \
    "uniform float Tuning_Dimming;\n" \
    "uniform float Tuning_Diff_Brightness;\n" \
    "uniform float Tuning_Spec_Brightness;\n" \
    "uniform float Tuning_Spec_Power;\n" \
    "uniform float Tuning_Fres_Brightness;\n" \
    "void main() {\n" \
    "    vec3 norm = normalize(vNormal);\n" \
    "    vec3 camDir = normalize(vCamDir);\n" \
    "    vec3 lightDir = normalize(vLightDir);\n" \
    "    float diffuse = clamp(dot(norm, lightDir), 0.0, 1.0);\n" \
    "    vec4 colordiff = vec4(0.175, 0.15, 0.2, 1.0) * diffuse * Tuning_Diff_Brightness;\n" \
    "    vec3 halfVec = normalize(lightDir + camDir);\n" \
    "    float spec = pow(clamp(dot(norm, halfVec), 0.0, 1.0), Tuning_Spec_Power);\n" \
    "    vec4 colorspec = vec4(0.25, 0.25, 0.25, 1.0) * spec * Tuning_Spec_Brightness;\n" \
    "    float fres = 1.0 - dot(camDir, norm);\n" \
    "    fres = (fres * fres) * Tuning_Fres_Brightness;\n" \
    "    vec4 colorfres = vec4(0.45, 0.4, 0.5, 1.0) * fres;\n" \
    "    vec4 emissive = SampleCRT(vUV);\n" \
    "    vec4 nearfinal = colorfres + colordiff + colorspec + emissive;\n" \
    "    oColor = nearfinal * mix(vec4(1.0), vColor, Tuning_Dimming);\n" \
    "}\n"

#define kFS_Frame \
    "#version 330 core\n" \
    "in vec4 vColor;\n" \
    "in vec2 vUV;\n" \
    "in vec3 vNormal;\n" \
    "in vec3 vCamDir;\n" \
    "in vec3 vLightDir;\n" \
    "in float vBlend;\n" \
    "out vec4 oColor;\n" \
    SUPERCRT_GLSL_SAMPLE_CRT \
    "uniform float Tuning_Dimming;\n" \
    "uniform float Tuning_ReflScalar;\n" \
    "uniform float Tuning_Diff_Brightness;\n" \
    "uniform float Tuning_Spec_Brightness;\n" \
    "uniform float Tuning_Spec_Power;\n" \
    "uniform float Tuning_Fres_Brightness;\n" \
    "uniform vec3 Tuning_FrameColor;\n" \
    "void main() {\n" \
    "    vec3 norm = normalize(vNormal);\n" \
    "    vec3 camDir = normalize(vCamDir);\n" \
    "    vec3 lightDir = normalize(vLightDir);\n" \
    "    float diffuse = clamp(dot(norm, lightDir), 0.0, 1.0);\n" \
    "    vec3 worldUp = vec3(0.0, 0.0, 1.0);\n" \
    "    float hemi = dot(norm, worldUp) * 0.5 + 0.5;\n" \
    "    hemi = hemi * 0.4 + 0.3;\n" \
    "    vec4 colordiff = vec4(Tuning_FrameColor, 1.0) * (diffuse + hemi) * Tuning_Diff_Brightness;\n" \
    "    vec3 halfVec = normalize(lightDir + camDir);\n" \
    "    float spec = pow(clamp(dot(norm, halfVec), 0.0, 1.0), Tuning_Spec_Power);\n" \
    "    vec4 colorspec = vec4(0.25, 0.25, 0.25, 1.0) * spec * Tuning_Spec_Brightness;\n" \
    "    vec4 emissive = SampleCRT(vUV);\n" \
    "    colorspec += (emissive * vBlend * Tuning_ReflScalar);\n" \
    "    float fres = 1.0 - dot(camDir, norm);\n" \
    "    fres = (fres * fres) * Tuning_Fres_Brightness;\n" \
    "    vec4 colorfres = vec4(0.15, 0.15, 0.15, 1.0) * fres;\n" \
    "    vec4 nearfinal = (colorfres + colordiff + colorspec);\n" \
    "    oColor = nearfinal * mix(vec4(1.0), vColor, Tuning_Dimming);\n" \
    "}\n"

// post.fx -- the same seven-tap Poisson blur serves downsample and upsample.
#define kFS_Post \
    "#version 330 core\n" \
    "in vec2 vUV;\n" \
    "out vec4 oColor;\n" \
    "uniform sampler2D uSource;\n" \
    "uniform vec2 BloomScale;\n" \
    "uniform int Upsample;\n" \
    "const vec2 Poisson[7] = vec2[7](\n" \
    "    vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0), vec2(-0.866025, 0.5),\n" \
    "    vec2(-0.866025, -0.5), vec2(0.866025, 0.5), vec2(0.866025, -0.5));\n" \
    "void main() {\n" \
    "    vec4 bloom = vec4(0.0);\n" \
    "    for (int i = 0; i < 7; ++i) {\n" \
    "        // The upsample pass swaps X and Y to reduce sampling artifacts.\n" \
    "        vec2 offset = (Upsample != 0) ? Poisson[i].yx : Poisson[i];\n" \
    "        bloom += texture(uSource, vUV + offset * BloomScale);\n" \
    "    }\n" \
    "    oColor = bloom * (1.0 / 7.0);\n" \
    "}\n"

// present.fx -- bloom composite onto the swap chain.
#define kFS_Present \
    "#version 330 core\n" \
    "in vec2 vUV;\n" \
    "out vec4 oColor;\n" \
    "uniform sampler2D PreBloomBuffer;\n" \
    "uniform sampler2D UpsampledBuffer;\n" \
    "uniform float BloomScalar;\n" \
    "uniform float BloomPower;\n" \
    "vec4 ColorPow(vec4 InColor, float InPower) {\n" \
    "    vec4 RefLuma = vec4(0.299, 0.587, 0.114, 0.0);\n" \
    "    float ActLuma = dot(InColor, RefLuma);\n" \
    "    if (ActLuma < 1e-5) {\n" \
    "        // Upstream divides by ActLuma unguarded (see its TODO); a black blur tap\n" \
    "        // yields 0/0 there.  For luma this small the result is black either way.\n" \
    "        return vec4(0.0, 0.0, 0.0, InColor.a);\n" \
    "    }\n" \
    "    vec4 ActColor = InColor / ActLuma;\n" \
    "    return ActColor * pow(ActLuma, InPower);\n" \
    "}\n" \
    "void main() {\n" \
    "    vec4 PreBloom = texture(PreBloomBuffer, vUV);\n" \
    "    vec4 Blurred = texture(UpsampledBuffer, vUV);\n" \
    "    oColor = PreBloom + (ColorPow(Blurred, BloomPower) * BloomScalar);\n" \
    "}\n"

// Overlay chrome: solid rectangles and font-atlas text, in window pixel coordinates
// with the origin at the top-left.
#define kVS_UI \
    "#version 330 core\n" \
    "layout(location = 0) in vec2 aPos;\n" \
    "layout(location = 1) in vec2 aUV;\n" \
    "layout(location = 2) in vec4 aColor;\n" \
    "layout(location = 3) in float aTextured;\n" \
    "uniform vec2 Viewport;\n" \
    "out vec2 vUV;\n" \
    "out vec4 vColor;\n" \
    "out float vTextured;\n" \
    "void main() {\n" \
    "    vec2 ndc = vec2(aPos.x / Viewport.x * 2.0 - 1.0, 1.0 - aPos.y / Viewport.y * 2.0);\n" \
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n" \
    "    vUV = aUV;\n" \
    "    vColor = aColor;\n" \
    "    vTextured = aTextured;\n" \
    "}\n"

#define kFS_UI \
    "#version 330 core\n" \
    "in vec2 vUV;\n" \
    "in vec4 vColor;\n" \
    "in float vTextured;\n" \
    "out vec4 oColor;\n" \
    "uniform sampler2D Atlas;\n" \
    "void main() {\n" \
    "    if (vTextured > 0.5) {\n" \
    "        oColor = vec4(vColor.rgb, vColor.a * texture(Atlas, vUV).r);\n" \
    "    } else {\n" \
    "        oColor = vColor;\n" \
    "    }\n" \
    "}\n"
