// FSR 3's prepare reactivity pass (ffx_fsr3upscaler_prepare_reactivity.h): for each render pixel,
// how far its surface was hidden in the frame before (from the reconstruct pass's depths), how much
// its shading changed, how its motion differs from where it came from, and how many frames it has
// gathered, which it keeps for the next frame. Godot gives no reactive or transparency and
// composition mask, so those are 0. The thin features this pass locks AMD scatters to the display
// pixels they land on; fsr3_lock.hlsl gathers them there instead. Keep it in step with
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

// The prepare inputs pass's motion vectors and depths, the shading change pass's changes and the
// accumulation the frame before kept.
Texture2D<float4> dilated_motion : register(t0, space2);
Texture2D<float4> dilated_depth : register(t1, space2);
Texture2D<float4> shading_change : register(t2, space2);
Texture2D<float4> previous_accumulation : register(t3, space2);
SamplerState dilated_motion_sampler : register(s0, space2);
SamplerState dilated_depth_sampler : register(s1, space2);
SamplerState shading_change_sampler : register(s2, space2);
SamplerState previous_accumulation_sampler : register(s3, space2);

// The reconstruct pass's depths as bits. Metal numbers storage buffers after the uniforms, and
// WebGPU's go in a bind group of their own.
#if defined(SLASH_MSL)
StructuredBuffer<uint> reconstructed : register(t1, space2);
#elif defined(SLASH_WGSL)
StructuredBuffer<uint> reconstructed : register(t0, space0);
#else
StructuredBuffer<uint> reconstructed : register(t4, space2);
#endif

float2 LoadDilatedMotionVector(int2 iPxPos) {
    return dilated_motion.Load(int3(Inside(iPxPos, RenderSize()), 0)).xy;
}
float LoadDilatedDepth(int2 iPxPos) {
    return dilated_depth.Load(int3(Inside(iPxPos, RenderSize()), 0)).r;
}
float LoadReconstructedPrevDepth(int2 iPxPos) {
    int2 iPos = Inside(iPxPos, RenderSize());
    return asfloat(reconstructed[iPos.y * RenderSize().x + iPos.x]);
}
#define FETCH_SHADING_CHANGE(p) shading_change.Load(int3(p, 0))
#define FETCH_ACCUMULATION(p) previous_accumulation.Load(int3(p, 0))
FSR3_LINEAR_SAMPLE(SampleShadingChangeTexel, FETCH_SHADING_CHANGE)
FSR3_LINEAR_SAMPLE(SampleAccumulationTexel, FETCH_ACCUMULATION)

float ComputeDisocclusions(float2 fUv, float2 fMotionVector, float fCurrentDepthViewSpace) {
    float fNearestDepthInMeters =
        min(fCurrentDepthViewSpace * ViewSpaceToMetersFactor(), FSR3UPSCALER_FP16_MAX);
    float fReconstructedDeptMvThreshold = ReconstructedDepthMvPxThreshold(fNearestDepthInMeters);
    fMotionVector *= float(Get4KVelocity(fMotionVector) > fReconstructedDeptMvThreshold);
    float2 fReprojectedUv = fUv + fMotionVector;
    BilinearSamplingData bilinearInfo = GetBilinearSamplingData(fReprojectedUv, RenderSize());
    float fDisocclusion = 0.0;
    float fWeightSum = 0.0;
    bool bPotentialDisocclusion = true;
    for (int iSampleIndex = 0; iSampleIndex < 4 && bPotentialDisocclusion; iSampleIndex++) {
        int2 iOffset = bilinearInfo.iOffsets[iSampleIndex];
        int2 iSamplePos = ClampLoad(bilinearInfo.iBasePos, iOffset, RenderSize());
        if (IsOnScreen(iSamplePos, RenderSize())) {
            float fWeight = bilinearInfo.fWeights[iSampleIndex];
            if (fWeight > fReconstructedDepthBilinearWeightThreshold) {
                float fPrevNearestDepthViewSpace =
                    GetViewSpaceDepth(LoadReconstructedPrevDepth(iSamplePos));
                float fDepthDifference = fCurrentDepthViewSpace - fPrevNearestDepthViewSpace;
                bPotentialDisocclusion =
                    bPotentialDisocclusion && (fDepthDifference > FSR3UPSCALER_FP32_MIN);
                if (bPotentialDisocclusion) {
                    float fHalfViewportWidth = length(float2(RenderSize()) * 0.5);
                    float fDepthThreshold = max(fCurrentDepthViewSpace, fPrevNearestDepthViewSpace);
                    const float Ksep = 1.37e-05;
                    float fRequiredDepthSeparation = Ksep * fHalfViewportWidth * fDepthThreshold;
                    fDisocclusion +=
                        ffxSaturate(fRequiredDepthSeparation / fDepthDifference) * fWeight;
                    fWeightSum += fWeight;
                }
            }
        }
    }
    return (bPotentialDisocclusion && fWeightSum > 0.0)
               ? ffxSaturate(1.0 - fDisocclusion / fWeightSum)
               : 0.0;
}

// AMD's divides velocities by a velocity of 0 where nothing moves, which D3D's saturate turns into
// a confidence of 0: written out here, as other GPUs leave a NaN as it is.
float ComputeMotionDivergence(float2 fUv, float2 fMotionVector, float fCurrentDepthSample) {
    int2 iPxReprojectedPos = int2((fUv + fMotionVector) * float2(RenderSize()));
    float fReprojectedDepth = LoadDilatedDepth(iPxReprojectedPos);
    float2 fReprojectedMotionVector = LoadDilatedMotionVector(iPxReprojectedPos);
    float fReprojectedVelocity = Get4KVelocity(fReprojectedMotionVector);
    float f4KVelocity = Get4KVelocity(fMotionVector);
    if (f4KVelocity == 0.0) {
        return 0.0;
    }
    float fNucleusDepthInMeters = GetViewSpaceDepthInMeters(fReprojectedDepth);
    float fCurrentDepthInMeters = GetViewSpaceDepthInMeters(fCurrentDepthSample);
    float fDistanceFactor = MinDividedByMax(fNucleusDepthInMeters, fCurrentDepthInMeters);
    float fVelocityFactor = ffxSaturate(f4KVelocity / 10.0);
    return (1.0 - ffxSaturate(fReprojectedVelocity / f4KVelocity)) * fDistanceFactor *
           fVelocityFactor;
}

struct Reactivity {
    float4 masks : SV_Target0;
    float4 accumulation : SV_Target1;
};

Reactivity fs_main(float4 position : SV_Position) {
    int2 iPxPos = int2(position.xy);
    float2 fUv = (float2(iPxPos) + 0.5) / float2(RenderSize());
    float2 fMotionVector = LoadDilatedMotionVector(iPxPos);
    float fDilatedDepth = LoadDilatedDepth(iPxPos);
    float fDisocclusion =
        ComputeDisocclusions(fUv, fMotionVector, GetViewSpaceDepth(fDilatedDepth));
    // ComputeShadingChange, which puts the jitter back that the accumulate pass takes off; the
    // reactive mask it is the most of is 0.
    float2 fShadingChangeUv = ClampUv(fUv - Jitter() / float2(RenderSize()),
                                      ShadingChangeRenderSize(), ShadingChangeRenderSize());
    float fShadingChange =
        ffxSaturate(SampleShadingChangeTexel(fShadingChangeUv, ShadingChangeRenderSize()).r);
    float fFinalReactiveness = max(ComputeMotionDivergence(fUv, fMotionVector, fDilatedDepth), 0.0);
    // UpdateAccumulation
    float2 fReprojectedUv = fUv + fMotionVector;
    float fAccumulation = 0.0;
    if (IsUvInside(fReprojectedUv)) {
        float2 fReprojectedUv_HW =
            ClampUv(fReprojectedUv, PreviousFrameRenderSize(), MaxRenderSize());
        fAccumulation =
            ffxSaturate(SampleAccumulationTexel(fReprojectedUv_HW, PreviousFrameRenderSize()).r);
    }
    fAccumulation = ffxLerp(fAccumulation, 0.0, fShadingChange);
    fAccumulation =
        ffxLerp(fAccumulation, min(MinDisocclusionAccumulation(), fAccumulation), fDisocclusion);
    fAccumulation *= float(RoundsAboveOne(fAccumulation * 100.0));
    Reactivity result;
    result.masks = float4(fFinalReactiveness, fDisocclusion, fShadingChange, fAccumulation);
    result.accumulation =
        float4(ffxSaturate(fAccumulation + AccumulationAddedPerFrame()), 0.0, 0.0, 0.0);
    return result;
}
