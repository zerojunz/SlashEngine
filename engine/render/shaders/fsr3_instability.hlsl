// FSR 3's luma instability pass (ffx_fsr3upscaler_luma_instability.h): each render pixel's luma
// over its last four frames, and whether this frame's flickers back to one of them, which lets the
// accumulate pass keep history that would otherwise be clamped away. Keep it in step with
// src/fsr3.cpp, its CPU twin.
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

// The prepare inputs pass's motion vectors and luma, the prepare reactivity pass's masks and the
// lumas the frame before kept.
Texture2D<float4> dilated_motion : register(t0, space2);
Texture2D<float4> reactive_masks : register(t1, space2);
Texture2D<float4> current_luma : register(t2, space2);
Texture2D<float4> luma_history : register(t3, space2);
SamplerState dilated_motion_sampler : register(s0, space2);
SamplerState reactive_masks_sampler : register(s1, space2);
SamplerState current_luma_sampler : register(s2, space2);
SamplerState luma_history_sampler : register(s3, space2);

float2 LoadDilatedMotionVector(int2 iPxPos) {
    return dilated_motion.Load(int3(Inside(iPxPos, RenderSize()), 0)).xy;
}
#define FETCH_MASKS(p) reactive_masks.Load(int3(p, 0))
#define FETCH_LUMA(p) current_luma.Load(int3(p, 0))
#define FETCH_HISTORY(p) luma_history.Load(int3(p, 0))
FSR3_LINEAR_SAMPLE(SampleDilatedReactiveMasks, FETCH_MASKS)
FSR3_LINEAR_SAMPLE(SampleCurrentLuma, FETCH_LUMA)
FSR3_LINEAR_SAMPLE(SampleLumaHistory, FETCH_HISTORY)

struct LumaInstabilityFactorData {
    float4 fLumaHistory;
    float fLumaInstabilityFactor;
};

LumaInstabilityFactorData ComputeLumaInstabilityFactor(LumaInstabilityFactorData data,
                                                       float fCurrentFrameLuma) {
    const int N_MINUS_1 = 0;
    const int N_MINUS_2 = 1;
    const int N_MINUS_4 = 3;
    float fLumaInstability = 0.0;
    float fDiffs0 = fCurrentFrameLuma - data.fLumaHistory[N_MINUS_1];
    float fSimilarity0 = MinDividedByMax(fCurrentFrameLuma, data.fLumaHistory[N_MINUS_1], 1.0);
    float fMaxSimilarity = fSimilarity0;
    if (fSimilarity0 < 1.0) {
        for (int i = N_MINUS_2; i <= N_MINUS_4; i++) {
            float fDiffs1 = fCurrentFrameLuma - data.fLumaHistory[i];
            float fSimilarity1 = MinDividedByMax(fCurrentFrameLuma, data.fLumaHistory[i]);
            if (Sign(fDiffs0) == Sign(fDiffs1)) {
                fMaxSimilarity = max(fMaxSimilarity, fSimilarity1);
            }
        }
        fLumaInstability = float(fMaxSimilarity > fSimilarity0);
    }
    // Shift the history.
    data.fLumaHistory = float4(fCurrentFrameLuma, data.fLumaHistory.xyz);
    data.fLumaHistory /= Exposure();
    data.fLumaInstabilityFactor = fLumaInstability * float(data.fLumaHistory[N_MINUS_4] != 0.0);
    return data;
}

struct Instability {
    float4 luma_history : SV_Target0;
    float4 instability : SV_Target1;
};

Instability fs_main(float4 position : SV_Position) {
    int2 iPxPos = int2(position.xy);
    LumaInstabilityFactorData data;
    data.fLumaInstabilityFactor = 0.0;
    data.fLumaHistory = float4(0.0, 0.0, 0.0, 0.0);
    float2 fDilatedMotionVector = LoadDilatedMotionVector(iPxPos);
    float2 fUv = (float2(iPxPos) + 0.5) / float2(RenderSize());
    float2 fUvCurrFrameJittered = fUv + Jitter() / float2(RenderSize());
    float2 fUvPrevFrameJittered =
        fUv + PreviousFrameJitter() / float2(PreviousFrameRenderSize());
    float2 fReprojectedUv = fUvPrevFrameJittered + fDilatedMotionVector;
    if (IsUvInside(fReprojectedUv)) {
        float2 fUvReactive_HW = ClampUv(fUvCurrFrameJittered, RenderSize(), MaxRenderSize());
        float4 fDilatedReactiveMasks = SampleDilatedReactiveMasks(fUvReactive_HW, RenderSize());
        float fReactiveMask = ffxSaturate(fDilatedReactiveMasks[REACTIVE]);
        float fDisocclusion = ffxSaturate(fDilatedReactiveMasks[DISOCCLUSION]);
        float fShadingChange = ffxSaturate(fDilatedReactiveMasks[SHADING_CHANGE]);
        float fAccumulation = ffxSaturate(fDilatedReactiveMasks[ACCUMULAION]);
        if (fAccumulation > 0.9) {
            float2 fUv_HW = ClampUv(fUvCurrFrameJittered, RenderSize(), MaxRenderSize());
            float fCurrentFrameLuma = SampleCurrentLuma(fUv_HW, RenderSize()).r * Exposure();
            float2 fReprojectedUv_HW =
                ClampUv(fReprojectedUv, PreviousFrameRenderSize(), MaxRenderSize());
            data.fLumaHistory = SampleLumaHistory(fReprojectedUv_HW, PreviousFrameRenderSize()) *
                                DeltaPreExposure() * Exposure();
            data = ComputeLumaInstabilityFactor(data, fCurrentFrameLuma);
            float fVelocityWeight =
                1.0 - ffxSaturate(Get4KVelocity(fDilatedMotionVector) / 20.0);
            data.fLumaInstabilityFactor *= fVelocityWeight * (1.0 - fDisocclusion) *
                                           (1.0 - fReactiveMask) * (1.0 - fShadingChange);
        }
    }
    Instability result;
    result.luma_history = data.fLumaHistory;
    result.instability = float4(data.fLumaInstabilityFactor, 0.0, 0.0, 0.0);
    return result;
}
