// FSR 2's lock pass (ffx_fsr2_lock.h): render pixels on thin features (a ridge of luma brighter or
// darker than all around it) lock the display pixel they land on, so the accumulate pass keeps
// their detail. AMD's pass runs per render pixel and writes the display pixels it locks; this one
// draws every display pixel and looks for the render pixel that lands on it, which comes to the
// same locks with no writes to clear. AMD's pass also clears the reconstructed depths for the next
// frame, which the reconstruct pass does here. Keep it in step with src/fsr2.cpp, its CPU twin.
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

// The dilate pass's luma for locks.
Texture2D<float4> lock_input_luma : register(t0, space2);
SamplerState lock_input_luma_sampler : register(s0, space2);

float LoadLockInputLuma(int2 iPxPos) {
    return lock_input_luma.Load(int3(Inside(iPxPos, RenderSize()), 0)).r;
}

bool ComputeThinFeatureConfidence(int2 pos) {
    const int RADIUS = 1;
    float fNucleus = LoadLockInputLuma(pos);
    float similar_threshold = 1.05;
    float dissimilarLumaMin = FSR2_FLT_MAX;
    float dissimilarLumaMax = 0.0;
    //  0 1 2
    //  3 4 5
    //  6 7 8
    uint mask = 1u << 4; // the nucleus is similar
    const uint uRejectionMasks[4] = {
        (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4), // upper left
        (1u << 1) | (1u << 2) | (1u << 4) | (1u << 5), // upper right
        (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7), // lower left
        (1u << 4) | (1u << 5) | (1u << 7) | (1u << 8), // lower right
    };
    int idx = 0;
    for (int y = -RADIUS; y <= RADIUS; y++) {
        for (int x = -RADIUS; x <= RADIUS; x++, idx++) {
            if (x == 0 && y == 0) {
                continue;
            }
            int2 samplePos = ClampLoad(pos, int2(x, y), RenderSize());
            float sampleLuma = LoadLockInputLuma(samplePos);
            // AMD's max / min, which is infinite or NaN, so dissimilar, where min is 0.
            float fMin = min(sampleLuma, fNucleus);
            float fMax = max(sampleLuma, fNucleus);
            if (fMin > 0.0 && (fMax / fMin) < similar_threshold) {
                mask |= 1u << uint(idx);
            } else {
                dissimilarLumaMin = min(dissimilarLumaMin, sampleLuma);
                dissimilarLumaMax = max(dissimilarLumaMax, sampleLuma);
            }
        }
    }
    bool isRidge = fNucleus > dissimilarLumaMax || fNucleus < dissimilarLumaMin;
    if (!isRidge) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if ((mask & uRejectionMasks[i]) == uRejectionMasks[i]) {
            return false;
        }
    }
    return true;
}

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    int2 iPxHrPos = int2(position.xy);
    // The render pixels that ComputeHrPosFromLrPos takes to this display pixel lie within half a
    // render pixel of this point: one at most, as display pixels are no larger, but any that does
    // locks it, as AMD's writes would.
    float2 fCenter = (float2(iPxHrPos) + 0.5) * float2(RenderSize()) / float2(DisplaySize()) - 0.5 +
                     Jitter();
    int2 iBase = int2(floor(fCenter));
    float fLock = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            int2 iPxLrPos = iBase + int2(x, y);
            if (IsOnScreen(iPxLrPos, RenderSize()) &&
                all(ComputeHrPosFromLrPos(iPxLrPos) == iPxHrPos) &&
                ComputeThinFeatureConfidence(iPxLrPos)) {
                fLock = 1.0;
            }
        }
    }
    return float4(fLock, 0.0, 0.0, 0.0);
}
