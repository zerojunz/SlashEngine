// FSR 2's depth clip pass (ffx_fsr2_depth_clip.h): how far each render pixel's surface was hidden
// in the frame before, from the reconstruct pass's depths, with the world's color prepared for the
// accumulate pass (YCoCg); and the dilated reactive masks. Godot gives FSR 2 no transparency and
// composition mask and draws no reactive mask here yet, so the masks are 0 and the second only
// carries the motion divergence. Keep it in step with src/fsr2.cpp, its CPU twin.
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

#include "fsr2.hlsl"

// The dilate pass's motion vectors and depths, the world, the frame before's dilated motion vectors
// and the prepass's motion vectors and depth, which make the sky's motion vectors.
Texture2D<float4> dilated_motion : register(t0, space2);
Texture2D<float4> dilated_depth : register(t1, space2);
Texture2D<float4> input_color : register(t2, space2);
Texture2D<float4> previous_dilated_motion : register(t3, space2);
Texture2D<float4> input_motion : register(t4, space2);
Texture2D<float4> input_depth : register(t5, space2);
SamplerState dilated_motion_sampler : register(s0, space2);
SamplerState dilated_depth_sampler : register(s1, space2);
SamplerState input_color_sampler : register(s2, space2);
SamplerState previous_dilated_motion_sampler : register(s3, space2);
SamplerState input_motion_sampler : register(s4, space2);
SamplerState input_depth_sampler : register(s5, space2);

// The reconstruct pass's depths as bits. Metal numbers storage buffers after the uniforms, and
// WebGPU's go in a bind group of their own.
#if defined(SLASH_MSL)
StructuredBuffer<uint> reconstructed : register(t1, space2);
#elif defined(SLASH_WGSL)
StructuredBuffer<uint> reconstructed : register(t0, space0);
#else
StructuredBuffer<uint> reconstructed : register(t6, space2);
#endif

float2 LoadDilatedMotionVector(int2 iPxPos) {
    return dilated_motion.Load(int3(Inside(iPxPos, RenderSize()), 0)).xy;
}
float LoadDilatedDepth(int2 iPxPos) {
    return dilated_depth.Load(int3(Inside(iPxPos, RenderSize()), 0)).r;
}
float3 LoadInputColor(int2 iPxPos) {
    return input_color.Load(int3(Inside(iPxPos, RenderSize()), 0)).rgb;
}
float2 LoadPreviousDilatedMotionVector(int2 iPxPos) {
    return previous_dilated_motion.Load(int3(Inside(iPxPos, RenderSize()), 0)).xy;
}
float2 LoadInputMotionVector(int2 iPxPos) {
    int2 iPos = Inside(iPxPos, RenderSize());
    return ValidMotionVector(input_motion.Load(int3(iPos, 0)).xy, iPos,
                             input_depth.Load(int3(iPos, 0)).r);
}
float LoadReconstructedPrevDepth(int2 iPxPos) {
    int2 iPos = Inside(iPxPos, RenderSize());
    return asfloat(reconstructed[iPos.y * RenderSize().x + iPos.x]);
}
// SamplePreviousDilatedMotionVector: filtered linearly at uv, clamped to the edges.
float2 SamplePreviousDilatedMotionVector(float2 fUv) {
    int2 iBase;
    float2 fFrac = LinearWeights(fUv, RenderSize(), iBase);
    float2 fTop = ffxLerp(LoadPreviousDilatedMotionVector(iBase),
                          LoadPreviousDilatedMotionVector(iBase + int2(1, 0)), fFrac.x);
    float2 fBottom = ffxLerp(LoadPreviousDilatedMotionVector(iBase + int2(0, 1)),
                             LoadPreviousDilatedMotionVector(iBase + int2(1, 1)), fFrac.x);
    return ffxLerp(fTop, fBottom, fFrac.y);
}

static const float DepthClipBaseScale = 4.0;

float ComputeDepthClip(float2 fUvSample, float fCurrentDepthSample) {
    float fCurrentDepthViewSpace = GetViewSpaceDepth(fCurrentDepthSample);
    BilinearSamplingData bilinearInfo = GetBilinearSamplingData(fUvSample, RenderSize());
    float fDepth = 0.0;
    float fWeightSum = 0.0;
    for (int iSampleIndex = 0; iSampleIndex < 4; iSampleIndex++) {
        int2 iOffset = bilinearInfo.iOffsets[iSampleIndex];
        int2 iSamplePos = bilinearInfo.iBasePos + iOffset;
        if (IsOnScreen(iSamplePos, RenderSize())) {
            float fWeight = bilinearInfo.fWeights[iSampleIndex];
            if (fWeight > fReconstructedDepthBilinearWeightThreshold) {
                float fPrevDepthSample = LoadReconstructedPrevDepth(iSamplePos);
                float fPrevNearestDepthViewSpace = GetViewSpaceDepth(fPrevDepthSample);
                float fDepthDiff = fCurrentDepthViewSpace - fPrevNearestDepthViewSpace;
                if (fDepthDiff > 0.0) {
                    float fPlaneDepth = min(fPrevDepthSample, fCurrentDepthSample);
                    float3 fCenter = GetViewSpacePosition(int2(float2(RenderSize()) * 0.5),
                                                          RenderSize(), fPlaneDepth);
                    float3 fCorner = GetViewSpacePosition(int2(0, 0), RenderSize(), fPlaneDepth);
                    float fHalfViewportWidth = length(float2(RenderSize()));
                    float fDepthThreshold = max(fCurrentDepthViewSpace, fPrevNearestDepthViewSpace);
                    const float Ksep = 1.37e-05;
                    float Kfov = length(fCorner) / length(fCenter);
                    float fRequiredDepthSeparation = Ksep * Kfov * fHalfViewportWidth * fDepthThreshold;
                    float fResolutionFactor = ffxSaturate(length(float2(RenderSize())) /
                                                          length(float2(1920.0, 1080.0)));
                    float fPower = ffxLerp(1.0, 3.0, fResolutionFactor);
                    fDepth += ffxPow(ffxSaturate(fRequiredDepthSeparation / fDepthDiff), fPower) *
                              fWeight;
                    fWeightSum += fWeight;
                }
            }
        }
    }
    return (fWeightSum > 0.0) ? ffxSaturate(1.0 - fDepth / fWeightSum) : 0.0;
}

float ComputeMotionDivergence(int2 iPxPos, int2 iPxInputMotionVectorSize) {
    float minconvergence = 1.0;
    float2 fMotionVectorNucleus = LoadInputMotionVector(iPxPos);
    float fNucleusVelocityLr = length(fMotionVectorNucleus * float2(RenderSize()));
    float fMaxVelocityUv = length(fMotionVectorNucleus);
    const float MotionVectorVelocityEpsilon = 1e-02;
    if (fNucleusVelocityLr > MotionVectorVelocityEpsilon) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                int2 sp = ClampLoad(iPxPos, int2(x, y), iPxInputMotionVectorSize);
                float2 fMotionVector = LoadInputMotionVector(sp);
                float fVelocityUv = length(fMotionVector);
                fMaxVelocityUv = max(fVelocityUv, fMaxVelocityUv);
                fVelocityUv = max(fVelocityUv, fMaxVelocityUv);
                minconvergence = min(minconvergence, dot(fMotionVector / fVelocityUv,
                                                         fMotionVectorNucleus / fVelocityUv));
            }
        }
    }
    return ffxSaturate(1.0 - minconvergence) * ffxSaturate(fMaxVelocityUv / 0.01);
}

float ComputeDepthDivergence(int2 iPxPos) {
    float fMaxDistInMeters = GetMaxDistanceInMeters();
    float fDepthMax = 0.0;
    float fDepthMin = fMaxDistInMeters;
    int iMaxDistFound = 0;
    for (int y = -1; y < 2; y++) {
        for (int x = -1; x < 2; x++) {
            int2 iSamplePos = iPxPos + int2(x, y);
            float fOnScreenFactor = IsOnScreen(iSamplePos, RenderSize()) ? 1.0 : 0.0;
            float fDepth = GetViewSpaceDepthInMeters(LoadDilatedDepth(iSamplePos)) * fOnScreenFactor;
            iMaxDistFound |= int(fMaxDistInMeters == fDepth);
            fDepthMin = min(fDepthMin, fDepth);
            fDepthMax = max(fDepthMax, fDepth);
        }
    }
    return (1.0 - fDepthMin / fDepthMax) * (iMaxDistFound != 0 ? 0.0 : 1.0);
}

float ComputeTemporalMotionDivergence(int2 iPxPos) {
    float2 fUv = (float2(iPxPos) + 0.5) / float2(RenderSize());
    float2 fMotionVector = LoadDilatedMotionVector(iPxPos);
    float2 fReprojectedUv = fUv + fMotionVector;
    fReprojectedUv = ClampUv(fReprojectedUv, RenderSize(), RenderSize());
    float2 fPrevMotionVector = SamplePreviousDilatedMotionVector(fReprojectedUv);
    float fPxDistance = length(fMotionVector * float2(DisplaySize()));
    return fPxDistance > 1.0
               ? ffxLerp(0.0, 1.0 - ffxSaturate(length(fPrevMotionVector) / length(fMotionVector)),
                         ffxSaturate(ffxPow(fPxDistance / 20.0, 3.0)))
               : 0.0;
}

float3 ComputePreparedInputColor(int2 iPxLrPos) {
    float3 fRgb = max(float3(0.0, 0.0, 0.0), LoadInputColor(iPxLrPos));
    fRgb = PrepareRgb(fRgb, Exposure(), PreExposure());
    return RGBToYCoCg(fRgb);
}

float EvaluateSurface(int2 iPxPos, float2 fMotionVector) {
    float d0 = GetViewSpaceDepth(LoadReconstructedPrevDepth(iPxPos + int2(0, -1)));
    float d1 = GetViewSpaceDepth(LoadReconstructedPrevDepth(iPxPos + int2(0, 0)));
    float d2 = GetViewSpaceDepth(LoadReconstructedPrevDepth(iPxPos + int2(0, 1)));
    return 1.0 - float(((d0 - d1) > (d1 * 0.01)) && ((d1 - d2) > (d2 * 0.01)));
}

struct Clipped {
    float4 prepared : SV_Target0;
    float4 reactive : SV_Target1;
};

Clipped fs_main(float4 position : SV_Position) {
    int2 iPxPos = int2(position.xy);
    float2 fDepthUv = (float2(iPxPos) + 0.5) / float2(RenderSize());
    float2 fMotionVector = LoadDilatedMotionVector(iPxPos);
    // Discard tiny mvs
    fMotionVector *= float(length(fMotionVector * float2(DisplaySize())) > 0.01);
    float2 fDilatedUv = fDepthUv + fMotionVector;
    float fDilatedDepth = LoadDilatedDepth(iPxPos);
    float fDepthClip =
        ComputeDepthClip(fDilatedUv, fDilatedDepth) * EvaluateSurface(iPxPos, fMotionVector);
    Clipped result;
    result.prepared = float4(ComputePreparedInputColor(iPxPos), fDepthClip);
    float fMotionDivergence = ComputeMotionDivergence(iPxPos, RenderSize());
    float fTemporalMotionDifference =
        ffxSaturate(ComputeTemporalMotionDivergence(iPxPos) - ComputeDepthDivergence(iPxPos));
    // PreProcessReactiveMasks with both masks 0.
    result.reactive = float4(0.0, max(fTemporalMotionDifference, fMotionDivergence), 0.0, 0.0);
    return result;
}
