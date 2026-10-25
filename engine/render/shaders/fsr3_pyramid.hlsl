// The first level of FSR 3's two pyramids, at half the render size: ffx_fsr3upscaler_luma_pyramid.h's
// farthest depth (its second mip, which is all of that pyramid FSR 3 reads without auto exposure),
// and ffx_fsr3upscaler_shading_change_pyramid.h's first level, how the luma changed since the
// frame before. AMD makes both with its single pass downsampler, each texel the average of the four
// under it; fsr3_reduce.hlsl draws the shading change pyramid's next levels from this one. Keep it
// in step with src/fsr3.cpp, its CPU twin.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
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

#include "fsr3.hlsl"

// The prepare inputs pass's farthest depths, motion vectors and luma, and the frame before's luma.
Texture2D<float4> farthest_depth : register(t0, space2);
Texture2D<float4> dilated_motion : register(t1, space2);
Texture2D<float4> current_luma : register(t2, space2);
Texture2D<float4> previous_luma : register(t3, space2);
SamplerState farthest_depth_sampler : register(s0, space2);
SamplerState dilated_motion_sampler : register(s1, space2);
SamplerState current_luma_sampler : register(s2, space2);
SamplerState previous_luma_sampler : register(s3, space2);

float LoadFarthestDepth(int2 iPxPos) {
    return farthest_depth.Load(int3(Inside(iPxPos, RenderSize()), 0)).r;
}
float2 LoadDilatedMotionVector(int2 iPxPos) {
    return dilated_motion.Load(int3(Inside(iPxPos, RenderSize()), 0)).xy;
}
float LoadCurrentLuma(int2 iPxPos) {
    return current_luma.Load(int3(Inside(iPxPos, RenderSize()), 0)).r;
}
float LoadPreviousLuma(int2 iPxPos) {
    return previous_luma.Load(int3(Inside(iPxPos, PreviousFrameRenderSize()), 0)).r;
}

#define SHADING_CHANGE_SET_SIZE 5

struct SampleSet {
    float fSamples[SHADING_CHANGE_SET_SIZE];
};

static const int2 iSampleOffsets[5] = {int2(+0, +0), int2(-1, +0), int2(+1, +0), int2(+0, -1),
                                       int2(+0, +1)};

void CompareSwap(inout SampleSet fSet, int i, int j) {
    float fTmp = min(fSet.fSamples[i], fSet.fSamples[j]);
    fSet.fSamples[j] = max(fSet.fSamples[i], fSet.fSamples[j]);
    fSet.fSamples[i] = fTmp;
}

void SortSet(inout SampleSet fSet) {
    CompareSwap(fSet, 0, 3);
    CompareSwap(fSet, 1, 4);
    CompareSwap(fSet, 0, 2);
    CompareSwap(fSet, 1, 3);
    CompareSwap(fSet, 0, 1);
    CompareSwap(fSet, 2, 4);
    CompareSwap(fSet, 1, 2);
    CompareSwap(fSet, 3, 4);
    CompareSwap(fSet, 2, 3);
}

// AMD's walk through both sorted sets, which moves b on from the a it has just moved. On the last
// step a can pass the end before b reads it, which changes nothing after: the read is kept inside.
float ComputeMinimumDifference(SampleSet fSet0, SampleSet fSet1) {
    float fMinDiff = FSR3UPSCALER_FP16_MAX - 1.0;
    int a = 0;
    int b = 0;
    SortSet(fSet0);
    SortSet(fSet1);
    float fMax = min(fSet0.fSamples[SHADING_CHANGE_SET_SIZE - 1],
                     fSet1.fSamples[SHADING_CHANGE_SET_SIZE - 1]);
    if (fMax > FSR3UPSCALER_FP32_MIN) {
        for (int i = 0; i < SHADING_CHANGE_SET_SIZE && (fMinDiff < FSR3UPSCALER_FP16_MAX); i++) {
            float fDiff = fSet0.fSamples[a] - fSet1.fSamples[b];
            if (abs(fDiff) > FSR3UPSCALER_FP16_MIN) {
                fDiff = Sign(fDiff) * (1.0 - MinDividedByMax(fSet0.fSamples[a], fSet1.fSamples[b]));
                fMinDiff = (abs(fDiff) < abs(fMinDiff)) ? fDiff : fMinDiff;
                a += int(fSet0.fSamples[a] < fSet1.fSamples[b]);
                b += int(fSet0.fSamples[min(a, SHADING_CHANGE_SET_SIZE - 1)] >= fSet1.fSamples[b]);
            } else {
                fMinDiff = FSR3UPSCALER_FP16_MAX;
            }
        }
    }
    return fMinDiff * float(fMinDiff < (FSR3UPSCALER_FP16_MAX - 1.0));
}

// AMD raises each luma to fShadingChangeSamplePow, which is 1.
SampleSet GetCurrentLumaBilinearSamples(float2 fUv) {
    float2 fUvJittered = fUv + Jitter() / float2(RenderSize());
    int2 iBasePos = int2(floor(fUvJittered * float2(RenderSize())));
    SampleSet fSet;
    for (int iSampleIndex = 0; iSampleIndex < SHADING_CHANGE_SET_SIZE; iSampleIndex++) {
        int2 iSamplePos = ClampLoad(iBasePos, iSampleOffsets[iSampleIndex], RenderSize());
        fSet.fSamples[iSampleIndex] =
            max(LoadCurrentLuma(iSamplePos) * Exposure(), FSR3UPSCALER_EPSILON);
    }
    return fSet;
}

float ComputeDiff(float2 fUv, float2 fMotionVector) {
    SampleSet fCurrentSamples = GetCurrentLumaBilinearSamples(fUv);
    // GetPreviousLumaBilinearSamples
    float2 fUvJittered = fUv + PreviousFrameJitter() / float2(PreviousFrameRenderSize());
    float2 fReprojectedUv = fUvJittered + fMotionVector;
    if (!IsUvInside(fReprojectedUv)) {
        return 0.0;
    }
    int2 iBasePos = int2(floor(fReprojectedUv * float2(PreviousFrameRenderSize())));
    SampleSet fPreviousSamples;
    for (int iSampleIndex = 0; iSampleIndex < SHADING_CHANGE_SET_SIZE; iSampleIndex++) {
        int2 iSamplePos =
            ClampLoad(iBasePos, iSampleOffsets[iSampleIndex], PreviousFrameRenderSize());
        fPreviousSamples.fSamples[iSampleIndex] = max(
            LoadPreviousLuma(iSamplePos) * DeltaPreExposure() * Exposure(), FSR3UPSCALER_EPSILON);
    }
    return ComputeMinimumDifference(fCurrentSamples, fPreviousSamples);
}

// The shading change pyramid's source texel: the signed luma difference and its sign.
float2 ShadingChangeSource(int2 iPxPos) {
    int2 iPxSamplePos = ClampLoad(iPxPos, int2(0, 0), RenderSize());
    float2 fDilatedMotionVector = LoadDilatedMotionVector(iPxSamplePos);
    float2 fUv = (float2(iPxSamplePos) + 0.5) / float2(RenderSize());
    float fScaledAndSignedLumaDiff = ComputeDiff(fUv, fDilatedMotionVector);
    return float2(fScaledAndSignedLumaDiff, Sign(fScaledAndSignedLumaDiff));
}

struct Level {
    float4 farthest_depth : SV_Target0;
    float4 shading_change : SV_Target1;
};

// SpdReduceLoadSourceImage: the four source texels from (0, 0), (0, 1), (1, 0) and (1, 1),
// averaged in that order.
Level fs_main(float4 position : SV_Position) {
    int2 iBase = int2(position.xy) * 2;
    const int2 iOffsets[4] = {int2(0, 0), int2(0, 1), int2(1, 0), int2(1, 1)};
    float fDepthSum = 0.0;
    float2 fChangeSum = float2(0.0, 0.0);
    for (int i = 0; i < 4; ++i) {
        int2 iPos = iBase + iOffsets[i];
        fDepthSum += LoadFarthestDepth(ClampLoad(iPos, int2(0, 0), RenderSize()));
        fChangeSum += ShadingChangeSource(iPos);
    }
    Level result;
    result.farthest_depth = float4(fDepthSum * 0.25, 0.0, 0.0, 0.0);
    result.shading_change = float4(fChangeSum * 0.25, 0.0, 0.0);
    return result;
}
