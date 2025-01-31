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

//= INCLUDES =====================
#include "../Core/SpartanObject.h"
#include "RHI_Descriptor.h"
//================================

namespace Spartan
{
    class SPARTAN_CLASS RHI_DescriptorSetLayoutCache : public SpartanObject
    {
    public:
        RHI_DescriptorSetLayoutCache(const RHI_Device* rhi_device);
        ~RHI_DescriptorSetLayoutCache();

        void SetPipelineState(RHI_PipelineState& pipeline_state);
        void Reset(uint32_t descriptor_set_capacity = 0);

        // Descriptor resource updating
        void SetConstantBuffer(const uint32_t slot, RHI_ConstantBuffer* constant_buffer);
        void SetSampler(const uint32_t slot, RHI_Sampler* sampler);
        void SetTexture(const uint32_t slot, RHI_Texture* texture, const int mip, const bool ranged);
        void SetStructuredBuffer(const uint32_t slot, RHI_StructuredBuffer* structured_buffer);
        void RemoveConstantBuffer(RHI_ConstantBuffer* constant_buffer);
        void RemoveTexture(RHI_Texture* texture, const int mip);
        void RemoveSampler(RHI_Sampler* sampler);

        RHI_DescriptorSetLayout* GetCurrentDescriptorSetLayout() const { return m_descriptor_layout_current; }
        bool GetDescriptorSet(RHI_DescriptorSet*& descriptor_set);
        void* GetResource_DescriptorPool() const { return m_descriptor_pool; }

        // Capacity
        bool HasEnoughCapacity() const { return m_descriptor_set_capacity > GetDescriptorSetCount(); }
        void GrowIfNeeded();

    private:
        uint32_t GetDescriptorSetCount() const;
        void SetDescriptorSetCapacity(uint32_t descriptor_capacity);
        bool CreateDescriptorPool(uint32_t descriptor_set_capacity);
        void GetDescriptors(RHI_PipelineState& pipeline_state, std::vector<RHI_Descriptor>& descriptors);

        // Descriptor set layouts 
        std::unordered_map<std::size_t, std::shared_ptr<RHI_DescriptorSetLayout>> m_descriptor_set_layouts;
        RHI_DescriptorSetLayout* m_descriptor_layout_current = nullptr;
        std::vector<RHI_Descriptor> m_descriptors;

        // Descriptor pool
        uint32_t m_descriptor_set_capacity = 0;
        void* m_descriptor_pool = nullptr;

        // Misc
        std::atomic<bool> m_descriptor_set_layouts_being_cleared = false;
        const RHI_Device* m_rhi_device;
    };
}
