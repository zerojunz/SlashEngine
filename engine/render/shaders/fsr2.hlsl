// What the passes of AMD's FidelityFX Super Resolution 2.2 share, as Godot 4.6 runs it for its
// scaling_3d mode "fsr2" (thirdparty/amd-fsr2): the uniforms, ffx_fsr2_common.h's helpers and
// ffx_fsr2_sample.h's filters. Godot's FSR 2 is a compute shader per pass; here each pass draws one
// triangle over its target (the reconstruct pass alone is a compute shader, for its atomics), and
// loads texels and filters them in code, keeping every load inside its texture, so every device
// draws the same values. Godot's settings: HDR input, reversed depth, motion vectors at the render
// size, no auto exposure of FSR 2's own, a pre-exposure of 1. Keep these files in step with
// src/fsr2.cpp, their CPU twin.
//
// Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software
// and associated documentation files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef FSR2_UNIFORMS
#define FSR2_UNIFORMS register(b0, space3)
#endif

// FSR 2's constant buffer, as the passes read it (slash::render::detail::Fsr2Uniforms).
cbuffer Fsr2 : FSR2_UNIFORMS {
    int4 sizes;          // the render size, then the display size
    float4 jitter_scale; // the jitter in render pixels, then the render size over the display's
    float4 view_depth;   // deviceToViewDepth: device depth to view space depth and position
    float4 frame;        // the exposure, the jitter's phases, the frame's index, RCAS's sharpness
    // The shading change mip's size, the reconstruct pass's step (0 clears, 1 scatters), and 1
    // for an orthogonal camera, whose depth FSR 2's perspective formulas take as linear here.
    int4 options;
};

int2 RenderSize() {
    return sizes.xy;
}
int2 DisplaySize() {
    return sizes.zw;
}
float2 Jitter() {
    return jitter_scale.xy;
}
float2 DownscaleFactor() {
    return jitter_scale.zw;
}
float4 DeviceToViewSpaceTransformFactors() {
    return view_depth;
}
// FSR 2's exposure texture's value, 1 for none (an exposure of 0).
float Exposure() {
    return frame.x == 0.0 ? 1.0 : frame.x;
}
float PreExposure() {
    return 1.0;
}
float PreviousFramePreExposure() {
    return 1.0;
}
float JitterSequenceLength() {
    return frame.y;
}
int FrameIndex() {
    return (int)frame.z;
}
int2 LumaMipDimensions() {
    return options.xy;
}

static const float FSR2_FP16_MAX = 65504.0;
static const float FSR2_EPSILON = 1e-03;
static const float FSR2_TONEMAP_EPSILON = 1.0 / FSR2_FP16_MAX;
static const float FSR2_FLT_MAX = 3.402823466e+38;
static const float fReconstructedDepthBilinearWeightThreshold = 0.01;
static const float fUpsampleLanczosWeightScale = 1.0 / 12.0;
static const float fMaxAccumulationLanczosWeight = 1.0;
static const float fAverageLanczosWeightPerFrame = 0.74 * fUpsampleLanczosWeightScale;
static const int LOCK_LIFETIME_REMAINING = 0;
static const int LOCK_TEMPORAL_LUMA = 1;

// x clamped to 0 to 1.
float ffxSaturate(float x) {
    return clamp(x, 0.0, 1.0);
}
float3 ffxSaturate(float3 x) {
    return clamp(x, 0.0, 1.0);
}
// a to b by t, written out so every device rounds alike.
float ffxLerp(float a, float b, float t) {
    return a + (b - a) * t;
}
float2 ffxLerp(float2 a, float2 b, float t) {
    return a + (b - a) * t;
}
float3 ffxLerp(float3 a, float3 b, float3 t) {
    return a + (b - a) * t;
}
float3 ffxLerp(float3 a, float3 b, float t) {
    return a + (b - a) * t;
}
float4 ffxLerp(float4 a, float4 b, float t) {
    return a + (b - a) * t;
}
// x rounded to the nearest whole number, halves up.
float ffxRound(float x) {
    return floor(x + 0.5);
}
// x to the power of y for x of 0 or more, as AMD's ffxPow; 0 to any power is 0, without the
// exp2(y * log2(0)) GPUs leave undefined.
float ffxPow(float x, float y) {
    return x <= 0.0 ? 0.0 : exp2(y * log2(x));
}

float3 YCoCgToRGB(float3 fYCoCg) {
    return float3(fYCoCg.x + fYCoCg.y - fYCoCg.z, fYCoCg.x + fYCoCg.z,
                  fYCoCg.x - fYCoCg.y - fYCoCg.z);
}
float3 RGBToYCoCg(float3 fRgb) {
    return float3(0.25 * fRgb.r + 0.5 * fRgb.g + 0.25 * fRgb.b, 0.5 * fRgb.r - 0.5 * fRgb.b,
                  -0.25 * fRgb.r + 0.5 * fRgb.g - 0.25 * fRgb.b);
}
float RGBToLuma(float3 fLinearRgb) {
    return dot(fLinearRgb, float3(0.2126, 0.7152, 0.0722));
}
float RGBToPerceivedLuma(float3 fLinearRgb) {
    float fLuminance = RGBToLuma(fLinearRgb);
    float fPercievedLuminance = 0.0;
    if (fLuminance <= 216.0 / 24389.0) {
        fPercievedLuminance = fLuminance * (24389.0 / 27.0);
    } else {
        fPercievedLuminance = ffxPow(fLuminance, 1.0 / 3.0) * 116.0 - 16.0;
    }
    return fPercievedLuminance * 0.01;
}
float3 Tonemap(float3 fRgb) {
    return fRgb / (max(max(0.0, fRgb.r), max(fRgb.g, fRgb.b)) + 1.0);
}
float3 InverseTonemap(float3 fRgb) {
    return fRgb / max(FSR2_TONEMAP_EPSILON, 1.0 - max(fRgb.r, max(fRgb.g, fRgb.b)));
}

int2 ClampLoad(int2 iPxSample, int2 iPxOffset, int2 iTextureSize) {
    int2 result = iPxSample + iPxOffset;
    result.x = (iPxOffset.x < 0) ? max(result.x, 0) : result.x;
    result.x = (iPxOffset.x > 0) ? min(result.x, iTextureSize.x - 1) : result.x;
    result.y = (iPxOffset.y < 0) ? max(result.y, 0) : result.y;
    result.y = (iPxOffset.y > 0) ? min(result.y, iTextureSize.y - 1) : result.y;
    return result;
}
float2 ClampUv(float2 fUv, int2 iTextureSize, int2 iResourceSize) {
    float2 fSampleLocation = fUv * float2(iTextureSize);
    float2 fClampedLocation =
        max(float2(0.5, 0.5), min(fSampleLocation, float2(iTextureSize) - float2(0.5, 0.5)));
    return fClampedLocation / float2(iResourceSize);
}
bool IsOnScreen(int2 pos, int2 size) {
    return all(pos >= int2(0, 0)) && all(pos < size);
}
// A texel inside a texture of size, for loads.
int2 Inside(int2 pos, int2 size) {
    return clamp(pos, int2(0, 0), size - int2(1, 1));
}

int2 ComputeHrPosFromLrPos(int2 iPxLrPos) {
    float2 fSrcJitteredPos = float2(iPxLrPos) + 0.5 - Jitter();
    float2 fLrPosInHr = (fSrcJitteredPos / float2(RenderSize())) * float2(DisplaySize());
    return int2(floor(fLrPosInHr));
}

float2 ComputeNdc(float2 fPxPos, int2 iSize) {
    return fPxPos / float2(iSize) * float2(2.0, -2.0) + float2(-1.0, 1.0);
}
// Device depth to view space depth: AMD's c1 / (d - c0) for a perspective camera, and
// (d - c0) / c1 for an orthogonal one, whose depth is linear.
float GetViewSpaceDepth(float fDeviceDepth) {
    float4 fDeviceToViewDepth = DeviceToViewSpaceTransformFactors();
    if (options.w != 0) {
        return (fDeviceDepth - fDeviceToViewDepth[0]) / fDeviceToViewDepth[1];
    }
    return fDeviceToViewDepth[1] / (fDeviceDepth - fDeviceToViewDepth[0]);
}
float GetViewSpaceDepthInMeters(float fDeviceDepth) {
    return GetViewSpaceDepth(fDeviceDepth);
}
float3 GetViewSpacePosition(int2 iViewportPos, int2 iViewportSize, float fDeviceDepth) {
    float4 fDeviceToViewDepth = DeviceToViewSpaceTransformFactors();
    float Z = GetViewSpaceDepth(fDeviceDepth);
    float2 fNdcPos = ComputeNdc(float2(iViewportPos), iViewportSize);
    // An orthogonal camera's rays are parallel: its x and y don't grow with depth.
    float scale = options.w != 0 ? 1.0 : Z;
    float X = fDeviceToViewDepth[2] * fNdcPos.x * scale;
    float Y = fDeviceToViewDepth[3] * fNdcPos.y * scale;
    return float3(X, Y, Z);
}
float GetMaxDistanceInMeters() {
    return GetViewSpaceDepth(0.0);
}

float3 PrepareRgb(float3 fRgb, float fExposure, float fPreExposure) {
    fRgb /= fPreExposure;
    fRgb *= fExposure;
    return clamp(fRgb, 0.0, FSR2_FP16_MAX);
}
float3 UnprepareRgb(float3 fRgb, float fExposure) {
    fRgb /= fExposure;
    fRgb *= PreExposure();
    return fRgb;
}

float MinDividedByMax(float v0, float v1) {
    float m = max(v0, v1);
    return m != 0.0 ? min(v0, v1) / m : 0.0;
}

struct BilinearSamplingData {
    int2 iOffsets[4];
    float fWeights[4];
    int2 iBasePos;
};

BilinearSamplingData GetBilinearSamplingData(float2 fUv, int2 iSize) {
    BilinearSamplingData data;
    float2 fPxSample = (fUv * float2(iSize)) - float2(0.5, 0.5);
    data.iBasePos = int2(floor(fPxSample));
    float2 fPxFrac = fPxSample - floor(fPxSample);
    data.iOffsets[0] = int2(0, 0);
    data.iOffsets[1] = int2(1, 0);
    data.iOffsets[2] = int2(0, 1);
    data.iOffsets[3] = int2(1, 1);
    data.fWeights[0] = (1.0 - fPxFrac.x) * (1.0 - fPxFrac.y);
    data.fWeights[1] = (fPxFrac.x) * (1.0 - fPxFrac.y);
    data.fWeights[2] = (1.0 - fPxFrac.x) * (fPxFrac.y);
    data.fWeights[3] = (fPxFrac.x) * (fPxFrac.y);
    return data;
}

// A linear sampler with clamped edges, as the GPU's filters a texture of size at uv: the four
// texels around it, kept inside the texture, blended.
float2 LinearWeights(float2 fUv, int2 iSize, out int2 iBase) {
    float2 fPx = (fUv * float2(iSize)) - 0.5;
    float2 fBase = floor(fPx);
    iBase = int2(fBase);
    return fPx - fBase;
}

float Lanczos2NoClamp(float x) {
    const float PI = 3.141592653589793;
    return abs(x) < FSR2_EPSILON ? 1.0
                                 : (sin(PI * x) / (PI * x)) * (sin(0.5 * PI * x) / (0.5 * PI * x));
}
float Lanczos2(float x) {
    x = min(abs(x), 2.0);
    return Lanczos2NoClamp(x);
}
float4 Lanczos2(float4 fColor0, float4 fColor1, float4 fColor2, float4 fColor3, float t) {
    float fWeight0 = Lanczos2(-1.0 - t);
    float fWeight1 = Lanczos2(-0.0 - t);
    float fWeight2 = Lanczos2(+1.0 - t);
    float fWeight3 = Lanczos2(+2.0 - t);
    return (fWeight0 * fColor0 + fWeight1 * fColor1 + fWeight2 * fColor2 + fWeight3 * fColor3) /
           (fWeight0 + fWeight1 + fWeight2 + fWeight3);
}
// FSR 1's Lanczos approximation, of x squared, which must be 4 or less.
float Lanczos2ApproxSqNoClamp(float x2) {
    float a = (2.0 / 5.0) * x2 - 1.0;
    float b = (1.0 / 4.0) * x2 - 1.0;
    return ((25.0 / 16.0) * a * a - (25.0 / 16.0 - 1.0)) * (b * b);
}
float Lanczos2ApproxSq(float x2) {
    x2 = min(x2, 4.0);
    return Lanczos2ApproxSqNoClamp(x2);
}

// ffx_fsr2_sample.h's DeclareCustomTextureSample with a Lanczos2 filter over 4x4 texels, the
// texels each fetched by FETCH(int2) from a texture of iTextureSize, with ClampCoord's clamping.
// FETCH is a macro the pass defines.
#define FSR2_LANCZOS2_SAMPLE(Name, FETCH)                                                          \
    float4 Name(float2 fUvSample, int2 iTextureSize) {                                             \
        float2 fPxSample = (fUvSample * float2(iTextureSize)) - float2(0.5, 0.5);                  \
        fPxSample.x = max(0.0, min(float(iTextureSize.x), fPxSample.x));                           \
        fPxSample.y = max(0.0, min(float(iTextureSize.y), fPxSample.y));                           \
        int2 iPxSample = int2(floor(fPxSample));                                                   \
        float2 fPxFrac = fPxSample - floor(fPxSample);                                             \
        float4 fRows[4];                                                                           \
        float4 fDeringingMin = float4(0.0, 0.0, 0.0, 0.0);                                         \
        float4 fDeringingMax = float4(0.0, 0.0, 0.0, 0.0);                                         \
        for (int y = 0; y < 4; ++y) {                                                              \
            float4 fColors[4];                                                                     \
            for (int x = 0; x < 4; ++x) {                                                          \
                fColors[x] = FETCH(ClampCoord(iPxSample, int2(x - 1, y - 1), iTextureSize));       \
            }                                                                                      \
            fRows[y] = Lanczos2(fColors[0], fColors[1], fColors[2], fColors[3], fPxFrac.x);        \
            if (y == 1) {                                                                          \
                fDeringingMin = min(fColors[1], fColors[2]);                                       \
                fDeringingMax = max(fColors[1], fColors[2]);                                       \
            } else if (y == 2) {                                                                   \
                fDeringingMin = min(fDeringingMin, min(fColors[1], fColors[2]));                   \
                fDeringingMax = max(fDeringingMax, max(fColors[1], fColors[2]));                   \
            }                                                                                      \
        }                                                                                          \
        float4 fColorXY = Lanczos2(fRows[0], fRows[1], fRows[2], fRows[3], fPxFrac.y);             \
        return clamp(fColorXY, fDeringingMin, fDeringingMax);                                      \
    }

// ffx_fsr2_sample.h's DeclareCustomTextureSample with its Bilinear filter over 2x2 texels, fetched
// as FSR2_LANCZOS2_SAMPLE fetches them.
#define FSR2_BILINEAR_SAMPLE(Name, FETCH)                                                          \
    float4 Name(float2 fUvSample, int2 iTextureSize) {                                             \
        float2 fPxSample = (fUvSample * float2(iTextureSize)) - float2(0.5, 0.5);                  \
        fPxSample.x = max(0.0, min(float(iTextureSize.x), fPxSample.x));                           \
        fPxSample.y = max(0.0, min(float(iTextureSize.y), fPxSample.y));                           \
        int2 iPxSample = int2(floor(fPxSample));                                                   \
        float2 fPxFrac = fPxSample - floor(fPxSample);                                             \
        float4 fColor00 = FETCH(ClampCoord(iPxSample, int2(0, 0), iTextureSize));                  \
        float4 fColor10 = FETCH(ClampCoord(iPxSample, int2(1, 0), iTextureSize));                  \
        float4 fColor01 = FETCH(ClampCoord(iPxSample, int2(0, 1), iTextureSize));                  \
        float4 fColor11 = FETCH(ClampCoord(iPxSample, int2(1, 1), iTextureSize));                  \
        float4 fColorX0 = fColor00 + (fColor10 - fColor00) * fPxFrac.x;                            \
        float4 fColorX1 = fColor01 + (fColor11 - fColor01) * fPxFrac.x;                            \
        return fColorX0 + (fColorX1 - fColorX0) * fPxFrac.y;                                       \
    }

// ffx_fsr2_sample.h's ClampCoord: by the offset's direction, then inside the texture, where AMD's
// leaves a texel past the edge to the hardware.
int2 ClampCoord(int2 iPxSample, int2 iPxOffset, int2 iTextureSize) {
    int2 result = iPxSample + iPxOffset;
    result.x = (iPxOffset.x < 0) ? max(result.x, 0) : result.x;
    result.x = (iPxOffset.x > 0) ? min(result.x, iTextureSize.x - 1) : result.x;
    result.y = (iPxOffset.y < 0) ? max(result.y, 0) : result.y;
    result.y = (iPxOffset.y > 0) ? min(result.y, iTextureSize.y - 1) : result.y;
    return Inside(result, iTextureSize);
}

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}
