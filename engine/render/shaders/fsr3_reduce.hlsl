// A level of FSR 3's shading change pyramid after the first (ffx_fsr3upscaler_shading_change_pyramid.h):
// each texel the average of the four of the level before under it, as AMD's single pass
// downsampler's SpdReduce4 makes them. AMD's keeps the levels it averages in 32-bit floats; this
// pass reads the level before as stored, in half floats. options.x is the level it draws. Keep it
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

// The level before.
Texture2D<float4> level_before : register(t0, space2);
SamplerState level_before_sampler : register(s0, space2);

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    int2 iBase = int2(position.xy) * 2;
    int2 iSize = GetSPDMipDimensions(options.x - 1);
    float2 v0 = level_before.Load(int3(Inside(iBase, iSize), 0)).xy;
    float2 v1 = level_before.Load(int3(Inside(iBase + int2(0, 1), iSize), 0)).xy;
    float2 v2 = level_before.Load(int3(Inside(iBase + int2(1, 0), iSize), 0)).xy;
    float2 v3 = level_before.Load(int3(Inside(iBase + int2(1, 1), iSize), 0)).xy;
    return float4((v0 + v1 + v2 + v3) * 0.25, 0.0, 0.0);
}
