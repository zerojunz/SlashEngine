// FSR 3's prepare inputs pass, the drawing half of ffx_fsr3upscaler_prepare_inputs.h: each render
// pixel's nearest depth of the 3x3 around it, the motion vector where that depth is, the farthest
// depth found on the way in meters, and the world's luma. The other half, which scatters the depths
// to where they were in the frame before, is fsr3_reconstruct.hlsl, a compute shader for its
// atomics. Keep it in step with src/fsr3.cpp, its CPU twin.
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

// The prepass's depth (reversed: 1 at the near plane) and motion vectors, and the world.
Texture2D<float4> input_depth : register(t0, space2);
Texture2D<float4> input_motion : register(t1, space2);
Texture2D<float4> input_color : register(t2, space2);
SamplerState input_depth_sampler : register(s0, space2);
SamplerState input_motion_sampler : register(s1, space2);
SamplerState input_color_sampler : register(s2, space2);

float LoadInputDepth(int2 iPxPos) {
    return input_depth.Load(int3(Inside(iPxPos, RenderSize()), 0)).r;
}
// The motion vectors are already in texture coordinates: AMD's scale is 1.
float2 LoadInputMotionVector(int2 iPxPos) {
    int2 iPos = Inside(iPxPos, RenderSize());
    return ValidMotionVector(input_motion.Load(int3(iPos, 0)).xy, iPos, LoadInputDepth(iPos));
}
float3 LoadInputColor(int2 iPxPos) {
    return input_color.Load(int3(Inside(iPxPos, RenderSize()), 0)).rgb;
}

struct DepthExtents {
    float fNearest;
    int2 fNearestCoord;
    float fFarthest;
};

// AMD's FindDepthExtents for reversed depth, which moves the farthest only where it finds a
// nearer depth.
DepthExtents FindDepthExtents(int2 iPxPos) {
    const int2 iSampleOffsets[9] = {int2(+0, +0), int2(+1, +0), int2(+0, +1),
                                    int2(+0, -1), int2(-1, +0), int2(-1, +1),
                                    int2(+1, +1), int2(-1, -1), int2(+1, -1)};
    DepthExtents extents;
    extents.fNearestCoord = iPxPos;
    extents.fNearest = LoadInputDepth(iPxPos);
    extents.fFarthest = extents.fNearest;
    for (int iSampleIndex = 1; iSampleIndex < 9; ++iSampleIndex) {
        int2 iPos = iPxPos + iSampleOffsets[iSampleIndex];
        if (IsOnScreen(iPos, RenderSize())) {
            float fNdDepth = LoadInputDepth(iPos);
            if (fNdDepth > extents.fNearest) {
                extents.fFarthest = min(extents.fFarthest, fNdDepth);
                extents.fNearestCoord = iPos;
                extents.fNearest = fNdDepth;
            }
        }
    }
    return extents;
}

struct Prepared {
    float4 dilated_depth : SV_Target0;
    float4 dilated_motion : SV_Target1;
    float4 farthest_depth : SV_Target2;
    float4 current_luma : SV_Target3;
};

Prepared fs_main(float4 position : SV_Position) {
    int2 iPxPos = int2(position.xy);
    DepthExtents depthExtents = FindDepthExtents(iPxPos);
    // FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS: the motion vector at the nearest.
    float2 fDilatedMotionVector = LoadInputMotionVector(depthExtents.fNearestCoord);
    float fFarthestDepthInMeters =
        min(GetViewSpaceDepthInMeters(depthExtents.fFarthest), FSR3UPSCALER_FP16_MAX);
    // GetCurrentFrameLuma: the world is linear.
    float3 fRgb = max(float3(0.0, 0.0, 0.0), LoadInputColor(iPxPos));
    Prepared result;
    result.dilated_depth = float4(depthExtents.fNearest, 0.0, 0.0, 0.0);
    result.dilated_motion = float4(fDilatedMotionVector, 0.0, 0.0);
    result.farthest_depth = float4(fFarthestDepthInMeters, 0.0, 0.0, 0.0);
    result.current_luma = float4(RGBToLuma(fRgb), 0.0, 0.0, 0.0);
    return result;
}
