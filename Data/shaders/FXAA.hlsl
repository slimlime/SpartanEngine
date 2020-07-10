/*
Copyright(c) 2016-2020 Panos Karabelas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//= INCLUDES ==================
#include "Common.hlsl"
#if FXAA
#define FXAA_PC 1
#define FXAA_HLSL_5 1
#define FXAA_QUALITY__PRESET 39
#include "FXAA.h"
#endif
//=============================


float4 mainPS(Pixel_PosUv input) : SV_TARGET
{
    float4 color = 0.0f;

     // Encodue luminance into alpha channel which is optimal for FXAA
#if LUMINANCE
    color = tex.Sample(sampler_point_clamp, input.uv);
    color.a = luminance(color.rgb);  
#endif

     // Actual FXAA
#if FXAA
    static const float fxaa_subPix           = 0.75f;
    static const float fxaa_edgeThreshold    = 0.166f;
    static const float fxaa_edgeThresholdMin = 0.0833f;
    
    FxaaTex fxaa_tex = { sampler_bilinear_clamp, tex };
    float2 fxaaQualityRcpFrame = g_texel_size;

    color.rgb = FxaaPixelShader
    (
        input.uv, 0, fxaa_tex, fxaa_tex, fxaa_tex,
        fxaaQualityRcpFrame, 0, 0, 0,
        fxaa_subPix,
        fxaa_edgeThreshold,
        fxaa_edgeThresholdMin,
        0, 0, 0, 0
    ).rgb;
    color.a = 1.0f; 
#endif
    
    return color;
}
