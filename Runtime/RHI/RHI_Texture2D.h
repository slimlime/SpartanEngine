/*
Copyright(c) 2016-2021 Panos Karabelas

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

#pragma once

//= INCLUDES ============
#include "RHI_Texture.h"
#include "RHI_Viewport.h"
//=======================

namespace Spartan
{
    class SPARTAN_CLASS RHI_Texture2D : public RHI_Texture
    {
    public:
        // Creates a texture without data (intended for manual loading)
        RHI_Texture2D(Context* context, const bool generate_mipmaps = false, const char* name = nullptr) : RHI_Texture(context)
        {
            m_resource_type = ResourceType::Texture2d;
            m_array_length  = 0;
            m_mip_count     = 0;
            m_flags         = RHI_Texture_Sampled;
            m_flags         |= generate_mipmaps ? RHI_Texture_GenerateMipsWhenLoading : 0;

            if (name != nullptr)
            {
                m_object_name = name;
            }
        }

        // Creates a texture from data (intended for sampling)
        RHI_Texture2D(Context* context, const uint32_t width, const uint32_t height, const RHI_Format format, const std::vector<RHI_Texture_Slice>& data) : RHI_Texture(context)
        {
            m_resource_type = ResourceType::Texture2d;
            m_width         = width;
            m_height        = height;
            m_viewport      = RHI_Viewport(0, 0, static_cast<float>(width), static_cast<float>(height));
            m_channel_count = GetChannelCountFromFormat(format);
            m_format        = format;
            m_data          = data;
            m_array_length  = 1;
            m_mip_count     = GetSlice(0).GetMipCount();
            m_flags         = RHI_Texture_Sampled;

            RHI_Texture2D::CreateResourceGpu();
        }

        // Creates a texture without any data (intended for usage as a render target)
        RHI_Texture2D(Context* context, const uint32_t width, const uint32_t height, const uint32_t mip_count, const RHI_Format format, const uint16_t flags = 0, const char* name = nullptr) : RHI_Texture(context)
        {
            m_resource_type = ResourceType::Texture2d;
            m_width         = width;
            m_height        = height;
            m_channel_count = GetChannelCountFromFormat(format);
            m_viewport      = RHI_Viewport(0, 0, static_cast<float>(width), static_cast<float>(height));
            m_format        = format;
            m_array_length  = 1;
            m_mip_count     = mip_count;
            m_flags         = flags;
            m_flags         |= RHI_Texture_Sampled;
            m_flags         |= IsDepthFormat() ? RHI_Texture_DepthStencil : (RHI_Texture_RenderTarget | RHI_Texture_Storage); // Need to optimize that, not every rt is used in a compute shader

            if (name != nullptr)
            {
                m_object_name = name;
            }

            CreateResourceGpu();
        }

        ~RHI_Texture2D() = default;
    };
}
