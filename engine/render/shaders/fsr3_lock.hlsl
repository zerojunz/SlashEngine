// FSR 3's new locks, at the display size: how strongly each display pixel locks onto a thin feature
// found this frame (ComputeThinFeatureConfidence in ffx_fsr3upscaler_prepare_reactivity.h). AMD's
// prepare reactivity pass scatters each render pixel's lock to the display pixel it lands on, and
// its accumulate pass reads and clears it; this pass gathers the render pixel that lands on each
// display pixel instead, which gives the same locks with no atomics. Keep it in step with
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

// The prepare inputs pass's luma.
Texture2D<float4> current_luma : register(t0, space2);
SamplerState current_luma_sampler : register(s0, space2);

float LoadCurrentLuma(int2 iPxPos) {
    return current_luma.Load(int3(Inside(iPxPos, RenderSize()), 0)).r;
}

// AMD's, which divides by the range of the lumas around: where they are all one luma, every one is
// dissimilar through a NaN, so nothing is a ridge. That is written out here, as some GPUs' fast
// math doesn't keep NaNs.
float ComputeThinFeatureConfidence(int2 iPxPos) {
    //  1 2 3
    //  4 0 5
    //  6 7 8
    const int iNucleusIndex = 0;
    const int iSampleCount = 9;
    const int2 iSampleOffsets[9] = {int2(+0, +0), int2(-1, -1), int2(+0, -1),
                                    int2(+1, -1), int2(-1, +0), int2(+1, +0),
                                    int2(-1, +1), int2(+0, +1), int2(+1, +1)};
    float fSamples[9];
    float fLumaMin = FSR3UPSCALER_FP32_MAX;
    float fLumaMax = FSR3UPSCALER_FP32_MIN;
    for (int iSampleIndex = 0; iSampleIndex < iSampleCount; ++iSampleIndex) {
        int2 iPxSamplePos = ClampLoad(iPxPos, iSampleOffsets[iSampleIndex], RenderSize());
        fSamples[iSampleIndex] = LoadCurrentLuma(iPxSamplePos) * Exposure();
        fLumaMin = min(fLumaMin, fSamples[iSampleIndex]);
        fLumaMax = max(fLumaMax, fSamples[iSampleIndex]);
    }
    float fRange = fLumaMax - fLumaMin;
    if (fRange == 0.0) {
        return 0.0;
    }
    const float fThreshold = 0.9;
    float fDissimilarLumaMin = FSR3UPSCALER_FP32_MAX;
    float fDissimilarLumaMax = 0.0;
    uint uPatternMask = 1u << iNucleusIndex; // the nucleus is similar
    const uint uRejectionMasks[4] = {
        (1u << 1) | (1u << 2) | (1u << 4) | (1u << iNucleusIndex), // upper left
        (1u << 2) | (1u << 3) | (1u << 5) | (1u << iNucleusIndex), // upper right
        (1u << 4) | (1u << 6) | (1u << 7) | (1u << iNucleusIndex), // lower left
        (1u << 5) | (1u << 7) | (1u << 8) | (1u << iNucleusIndex)  // lower right
    };
    for (int iSample = 1; iSample < iSampleCount; ++iSample) {
        float fDifference = abs(fSamples[iSample] - fSamples[iNucleusIndex]) / fRange;
        if (fDifference < fThreshold) {
            uPatternMask |= 1u << uint(iSample);
        } else {
            fDissimilarLumaMin = min(fDissimilarLumaMin, fSamples[iSample]);
            fDissimilarLumaMax = max(fDissimilarLumaMax, fSamples[iSample]);
        }
    }
    bool bIsRidge = fSamples[iNucleusIndex] > fDissimilarLumaMax ||
                    fSamples[iNucleusIndex] < fDissimilarLumaMin;
    if (!bIsRidge) {
        return 0.0;
    }
    for (int i = 0; i < 4; i++) {
        if ((uPatternMask & uRejectionMasks[i]) == uRejectionMasks[i]) {
            return 0.0;
        }
    }
    return 1.0 - fLumaMin / fLumaMax;
}

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    int2 iPxHrPos = int2(position.xy);
    // The render pixels that ComputeHrPosFromLrPos takes to this display pixel lie within half a
    // render pixel of this point: one at most, as display pixels are no larger.
    float2 fCenter = (float2(iPxHrPos) + 0.5) * float2(RenderSize()) / float2(UpscaleSize()) -
                     0.5 + Jitter();
    int2 iBase = int2(floor(fCenter));
    float fLock = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            int2 iPxLrPos = iBase + int2(x, y);
            if (IsOnScreen(iPxLrPos, RenderSize()) &&
                all(ComputeHrPosFromLrPos(iPxLrPos) == iPxHrPos)) {
                float fLockStrength = ComputeThinFeatureConfidence(iPxLrPos);
                if (fLockStrength > (1.0 / 100.0)) {
                    fLock = fLockStrength;
                }
            }
        }
    }
    return float4(fLock, 0.0, 0.0, 0.0);
}
