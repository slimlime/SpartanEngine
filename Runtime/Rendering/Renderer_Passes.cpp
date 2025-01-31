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

//= INCLUDES ==============================
#include "Spartan.h"
#include "Renderer.h"
#include "Model.h"
#include "ShaderGBuffer.h"
#include "ShaderLight.h"
#include "Font/Font.h"
#include "Gizmos/Grid.h"
#include "Gizmos/TransformGizmo.h"
#include "../Profiling/Profiler.h"
#include "../RHI/RHI_CommandList.h"
#include "../RHI/RHI_Implementation.h"
#include "../RHI/RHI_VertexBuffer.h"
#include "../RHI/RHI_PipelineState.h"
#include "../RHI/RHI_Texture.h"
#include "../RHI/RHI_SwapChain.h"
#include "../World/Entity.h"
#include "../World/Components/Camera.h"
#include "../World/Components/Light.h"
#include "../World/Components/Transform.h"
#include "../World/Components/Renderable.h"
//=========================================

//= NAMESPACES ===============
using namespace std;
using namespace Spartan::Math;
//============================

namespace Spartan
{
    void Renderer::SetGlobalShaderResources(RHI_CommandList* cmd_list) const
    {
        // Constant buffers
        cmd_list->SetConstantBuffer(RendererBindings_Cb::frame, RHI_Shader_Vertex | RHI_Shader_Pixel | RHI_Shader_Compute, m_cb_frame_gpu);
        cmd_list->SetConstantBuffer(RendererBindings_Cb::uber,  RHI_Shader_Vertex | RHI_Shader_Pixel | RHI_Shader_Compute, m_cb_uber_gpu);
        cmd_list->SetConstantBuffer(RendererBindings_Cb::light, RHI_Shader_Compute, m_cb_light_gpu);
        cmd_list->SetConstantBuffer(RendererBindings_Cb::material, RHI_Shader_Pixel | RHI_Shader_Compute, m_cb_material_gpu);

        // Samplers
        cmd_list->SetSampler(0, m_sampler_compare_depth);
        cmd_list->SetSampler(1, m_sampler_point_clamp);
        cmd_list->SetSampler(2, m_sampler_point_wrap);
        cmd_list->SetSampler(3, m_sampler_bilinear_clamp);
        cmd_list->SetSampler(4, m_sampler_bilinear_wrap);
        cmd_list->SetSampler(5, m_sampler_trilinear_clamp);
        cmd_list->SetSampler(6, m_sampler_anisotropic_wrap);

        // Textures
        cmd_list->SetTexture(RendererBindings_Srv::noise_normal, m_tex_default_noise_normal);
        cmd_list->SetTexture(RendererBindings_Srv::noise_blue, m_tex_default_noise_blue);
    }

    void Renderer::Pass_Main(RHI_CommandList* cmd_list)
    {
        // Validate cmd list
        SP_ASSERT(cmd_list != nullptr);
        SP_ASSERT(cmd_list->GetState() == RHI_CommandListState::Recording);

        SCOPED_TIME_BLOCK(m_profiler);

        // Update frame constant buffer
        Pass_UpdateFrameBuffer(cmd_list);

        // Generate brdf specular lut (only runs once)
        Pass_BrdfSpecularLut(cmd_list);

        const bool draw_transparent_objects = !m_entities[Renderer_ObjectType::GeometryTransparent].empty();

        // Depth
        {
            Pass_Depth_Light(cmd_list, Renderer_ObjectType::GeometryOpaque);
            if (draw_transparent_objects)
            {
                Pass_Depth_Light(cmd_list, Renderer_ObjectType::GeometryTransparent);
            }
        
            if (GetOption(Render_DepthPrepass))
            {
                Pass_Depth_Prepass(cmd_list);
            }
        }

        // Acquire render targets
        RHI_Texture* rt1 = RENDER_TARGET(RendererRt::Frame_Render).get();
        RHI_Texture* rt2 = RENDER_TARGET(RendererRt::Frame_Render_2).get();

        // G-Buffer and lighting
        {
            // G-buffer
            Pass_GBuffer(cmd_list);

            // Passes which rely on the G-buffer
            Pass_Ssao(cmd_list);
            Pass_Reflections_Ssr(cmd_list);

            // Lighting
            Pass_Light(cmd_list);

            // Composition of the light buffers + ssao and volumetric fog
            Pass_Light_Composition(cmd_list, rt1);

            // Image based lighting
            Pass_Light_ImageBased(cmd_list, rt1);

            // If SSR is enabled, copy the frame so that SSR can use it to reflect from
            if ((m_options & Render_ScreenSpaceReflections) != 0)
            {
                cmd_list->Blit(rt1, rt2);
            }

            // Reflections - SSR & Environment
            Pass_Reflections(cmd_list, rt1, rt2);

            // Lighting for transparent objects (a simpler version of the above)
            if (draw_transparent_objects)
            {
                // Blit the frame so that refraction can sample from it
                cmd_list->Blit(rt1, rt2);

                const bool is_trasparent_pass = true;
                Pass_GBuffer(cmd_list, is_trasparent_pass);
                Pass_Light(cmd_list, is_trasparent_pass);
                Pass_Light_Composition(cmd_list, rt1, is_trasparent_pass);
                Pass_Light_ImageBased(cmd_list, rt1, is_trasparent_pass);
            }
        }

        Pass_PostProcess(cmd_list);
    }

    void Renderer::Pass_UpdateFrameBuffer(RHI_CommandList* cmd_list)
    {
        // Set render state
        static RHI_PipelineState pso;
        pso.pass_name = "Pass_UpdateFrameBuffer";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            Update_Cb_Frame(cmd_list);
            cmd_list->EndRenderPass();
        }

    }

    void Renderer::Pass_Depth_Light(RHI_CommandList* cmd_list, const Renderer_ObjectType object_type)
    {
        // All opaque objects are rendered from the lights point of view.
        // Opaque objects write their depth information to a depth buffer, using just a vertex shader.
        // Transparent objects, read the opaque depth but don't write their own, instead, they write their color information using a pixel shader.

        // Acquire shader
        RHI_Shader* shader_v = m_shaders[RendererShader::Depth_Light_V].get();
        RHI_Shader* shader_p = m_shaders[RendererShader::Depth_Light_P].get();
        if (!shader_v->IsCompiled() || !shader_p->IsCompiled())
            return;

        // Get entities
        const auto& entities = m_entities[object_type];
        if (entities.empty())
            return;

        const bool transparent_pass = object_type == Renderer_ObjectType::GeometryTransparent;

        // Go through all of the lights
        const auto& entities_light = m_entities[Renderer_ObjectType::Light];
        for (uint32_t light_index = 0; light_index < entities_light.size(); light_index++)
        {
            const Light* light = entities_light[light_index]->GetComponent<Light>();

            // Can happen when loading a new scene and the lights get deleted
            if (!light)
                continue;

            // Skip lights which don't cast shadows or have an intensity of zero
            if (!light->GetShadowsEnabled() || light->GetIntensity() == 0.0f)
                continue;

            // Skip lights that don't cast transparent shadows (if this is a transparent pass)
            if (transparent_pass && !light->GetShadowsTransparentEnabled())
                continue;

            // Acquire light's shadow maps
            RHI_Texture* tex_depth = light->GetDepthTexture();
            RHI_Texture* tex_color = light->GetColorTexture();
            if (!tex_depth)
                continue;

            // Set render state
            static RHI_PipelineState pso;
            pso.shader_vertex                    = shader_v;
            pso.vertex_buffer_stride             = static_cast<uint32_t>(sizeof(RHI_Vertex_PosTexNorTan)); // assume all vertex buffers have the same stride (which they do)
            pso.shader_pixel                     = transparent_pass ? shader_p : nullptr;
            pso.blend_state                      = transparent_pass ? m_blend_alpha.get() : m_blend_disabled.get();
            pso.depth_stencil_state              = transparent_pass ? m_depth_stencil_r_off.get() : m_depth_stencil_rw_off.get();
            pso.render_target_color_textures[0]  = tex_color; // always bind so we can clear to white (in case there are now transparent objects)
            pso.render_target_depth_texture      = tex_depth;
            pso.clear_stencil                    = rhi_stencil_dont_care;
            pso.viewport                         = tex_depth->GetViewport();
            pso.primitive_topology               = RHI_PrimitiveTopology_Mode::TriangleList;
            pso.pass_name                        = transparent_pass ? "Pass_Depth_Light_Transparent" : "Pass_Depth_Light";

            for (uint32_t array_index = 0; array_index < tex_depth->GetArrayLength(); array_index++)
            {
                // Set render target texture array index
                pso.render_target_color_texture_array_index          = array_index;
                pso.render_target_depth_stencil_texture_array_index  = array_index;

                // Set clear values
                pso.clear_color[0] = Vector4::One;
                pso.clear_depth    = transparent_pass ? rhi_depth_load : GetClearDepth();

                const Matrix& view_projection = light->GetViewMatrix(array_index) * light->GetProjectionMatrix(array_index);

                // Set appropriate rasterizer state
                if (light->GetLightType() == LightType::Directional)
                {
                    // "Pancaking" - https://www.gamedev.net/forums/topic/639036-shadow-mapping-and-high-up-objects/
                    // It's basically a way to capture the silhouettes of potential shadow casters behind the light's view point.
                    // Of course we also have to make sure that the light doesn't cull them in the first place (this is done automatically by the light)
                    pso.rasterizer_state = m_rasterizer_light_directional.get();
                }
                else
                {
                    pso.rasterizer_state = m_rasterizer_light_point_spot.get();
                }

                // State tracking
                bool render_pass_active     = false;
                uint32_t m_set_material_id  = 0;

                for (uint32_t entity_index = 0; entity_index < static_cast<uint32_t>(entities.size()); entity_index++)
                {
                    Entity* entity = entities[entity_index];

                    // Acquire renderable component
                    Renderable* renderable = entity->GetRenderable();
                    if (!renderable)
                        continue;

                    // Skip meshes that don't cast shadows
                    if (!renderable->GetCastShadows())
                        continue;

                    // Acquire geometry
                    Model* model = renderable->GeometryModel();
                    if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
                        continue;

                    // Acquire material
                    Material* material = renderable->GetMaterial();
                    if (!material)
                        continue;

                    // Skip objects outside of the view frustum
                    if (!light->IsInViewFrustrum(renderable, array_index))
                        continue;

                    if (!render_pass_active)
                    {
                        render_pass_active = cmd_list->BeginRenderPass(pso);
                    }

                    // Bind material
                    if (transparent_pass && m_set_material_id != material->GetObjectId())
                    {
                        // Bind material textures
                        RHI_Texture* tex_albedo = material->GetTexture_Ptr(Material_Color);
                        cmd_list->SetTexture(RendererBindings_Srv::tex, tex_albedo ? tex_albedo : m_tex_default_white.get());

                        // Update uber buffer with material properties
                        m_cb_uber_cpu.mat_albedo    = material->GetColorAlbedo();
                        m_cb_uber_cpu.mat_tiling_uv = material->GetTiling();
                        m_cb_uber_cpu.mat_offset_uv = material->GetOffset();

                        m_set_material_id = material->GetObjectId();
                    }

                    // Bind geometry
                    cmd_list->SetBufferIndex(model->GetIndexBuffer());
                    cmd_list->SetBufferVertex(model->GetVertexBuffer());

                    // Update uber buffer with cascade transform
                    m_cb_uber_cpu.transform = entity->GetTransform()->GetMatrix() * view_projection;
                    if (!Update_Cb_Uber(cmd_list))
                        continue;

                    cmd_list->DrawIndexed(renderable->GeometryIndexCount(), renderable->GeometryIndexOffset(), renderable->GeometryVertexOffset());
                }

                if (render_pass_active)
                {
                    cmd_list->EndRenderPass();
                }
            }
        }
    }

    void Renderer::Pass_Depth_Prepass(RHI_CommandList* cmd_list)
    {
        // Acquire shaders
        RHI_Shader* shader_v = m_shaders[RendererShader::Depth_Prepass_V].get();
        RHI_Shader* shader_p = m_shaders[RendererShader::Depth_Prepass_P].get();
        if (!shader_v->IsCompiled() || !shader_p->IsCompiled())
            return;

        RHI_Texture* tex_depth = RENDER_TARGET(RendererRt::Gbuffer_Depth).get();
        const auto& entities = m_entities[Renderer_ObjectType::GeometryOpaque];

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_vertex               = shader_v;
        pso.shader_pixel                = shader_p; // alpha testing
        pso.rasterizer_state            = m_rasterizer_cull_back_solid.get();
        pso.blend_state                 = m_blend_disabled.get();
        pso.depth_stencil_state         = m_depth_stencil_rw_off.get();
        pso.render_target_depth_texture = tex_depth;
        pso.clear_depth                 = GetClearDepth();
        pso.viewport                    = tex_depth->GetViewport();
        pso.primitive_topology          = RHI_PrimitiveTopology_Mode::TriangleList;
        pso.vertex_buffer_stride        = static_cast<uint32_t>(sizeof(RHI_Vertex_PosTex));
        pso.pass_name                   = "Pass_Depth_Prepass";

        // Record commands
        if (cmd_list->BeginRenderPass(pso))
        { 
            // Variables that help reduce state changes
            uint32_t currently_bound_geometry = 0;
            
            // Draw opaque
            for (const auto& entity : entities)
            {
                // Get renderable
                Renderable* renderable = entity->GetRenderable();
                if (!renderable)
                    continue;

                // Get material
                Material* material = renderable->GetMaterial();
                if (!material)
                    continue;

                // Get geometry
                Model* model = renderable->GeometryModel();
                if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
                    continue;

                // Get transform
                Transform* transform = entity->GetTransform();
                if (!transform)
                    continue;

                // Skip objects outside of the view frustum
                if (!m_camera->IsInViewFrustum(renderable))
                    continue;
            
                // Bind geometry
                if (currently_bound_geometry != model->GetObjectId())
                {
                    cmd_list->SetBufferIndex(model->GetIndexBuffer());
                    cmd_list->SetBufferVertex(model->GetVertexBuffer());
                    currently_bound_geometry = model->GetObjectId();
                }

                // Bind alpha testing textures
                cmd_list->SetTexture(RendererBindings_Srv::material_albedo,  material->GetTexture_Ptr(Material_Color));
                cmd_list->SetTexture(RendererBindings_Srv::material_mask,    material->GetTexture_Ptr(Material_AlphaMask));

                // Update uber buffer
                m_cb_uber_cpu.transform             = transform->GetMatrix();
                m_cb_uber_cpu.color.w               = material->HasTexture(Material_Color) ? 1.0f : 0.0f;
                m_cb_uber_cpu.is_transparent_pass   = material->HasTexture(Material_AlphaMask);
                Update_Cb_Uber(cmd_list);
            
                // Draw
                cmd_list->DrawIndexed(renderable->GeometryIndexCount(), renderable->GeometryIndexOffset(), renderable->GeometryVertexOffset());
            }

            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_GBuffer(RHI_CommandList* cmd_list, const bool is_transparent_pass /*= false*/)
    {
        // Acquire required resources/shaders
        RHI_Texture* tex_albedo     = RENDER_TARGET(RendererRt::Gbuffer_Albedo).get();
        RHI_Texture* tex_normal     = RENDER_TARGET(RendererRt::Gbuffer_Normal).get();
        RHI_Texture* tex_material   = RENDER_TARGET(RendererRt::Gbuffer_Material).get();
        RHI_Texture* tex_velocity   = RENDER_TARGET(RendererRt::Gbuffer_Velocity).get();
        RHI_Texture* tex_depth      = RENDER_TARGET(RendererRt::Gbuffer_Depth).get();
        RHI_Shader* shader_v        = m_shaders[RendererShader::Gbuffer_V].get();
        ShaderGBuffer* shader_p     = static_cast<ShaderGBuffer*>(m_shaders[RendererShader::Gbuffer_P].get());

        // Validate that the shader has compiled
        if (!shader_v->IsCompiled())
            return;

        bool depth_prepass  = GetOption(Render_DepthPrepass);
        bool wireframe      = GetOption(Render_Debug_Wireframe);

        // Set render state
        RHI_PipelineState pso;
        pso.shader_vertex                   = shader_v;
        pso.blend_state                     = m_blend_disabled.get();
        pso.rasterizer_state                = wireframe ? m_rasterizer_cull_back_wireframe.get() : m_rasterizer_cull_back_solid.get();
        pso.depth_stencil_state             = is_transparent_pass ? m_depth_stencil_rw_w.get() : (depth_prepass ? m_depth_stencil_r_off.get() : m_depth_stencil_rw_off.get());
        pso.render_target_color_textures[0] = tex_albedo;
        pso.clear_color[0]                  = !is_transparent_pass ? Vector4::Zero : rhi_color_load;
        pso.render_target_color_textures[1] = tex_normal;
        pso.clear_color[1]                  = !is_transparent_pass ? Vector4::Zero : rhi_color_load;
        pso.render_target_color_textures[2] = tex_material;
        pso.clear_color[2]                  = !is_transparent_pass ? Vector4::Zero : rhi_color_load;
        pso.render_target_color_textures[3] = tex_velocity;
        pso.clear_color[3]                  = !is_transparent_pass ? Vector4::Zero : rhi_color_load;
        pso.render_target_depth_texture     = tex_depth;
        pso.clear_depth                     = (is_transparent_pass || depth_prepass) ? rhi_depth_load : GetClearDepth();
        pso.clear_stencil                   = !is_transparent_pass ? 0 : rhi_stencil_dont_care;
        pso.viewport                        = tex_albedo->GetViewport();
        pso.vertex_buffer_stride            = static_cast<uint32_t>(sizeof(RHI_Vertex_PosTexNorTan));
        pso.primitive_topology              = RHI_PrimitiveTopology_Mode::TriangleList;

        uint32_t material_index = 0;
        uint32_t material_bound_id = 0;
        m_material_instances.fill(nullptr);

        // Iterate through all the G-Buffer shader variations
        for (const auto& it : ShaderGBuffer::GetVariations())
        {
            // Set pixel shader
            pso.shader_pixel = static_cast<RHI_Shader*>(it.second.get());

            // Skip the shader it failed to compiled or hasn't compiled yet
            if (!pso.shader_pixel->IsCompiled())
                continue;

            // Set pass name
            pso.pass_name = is_transparent_pass ? "GBuffer_Transparent" : "GBuffer_Opaque";

            bool render_pass_active = false;
            auto& entities = m_entities[is_transparent_pass ? Renderer_ObjectType::GeometryTransparent : Renderer_ObjectType::GeometryOpaque];

            // Record commands
            if (cmd_list->BeginRenderPass(pso))
            { 
                for (uint32_t i = 0; i < static_cast<uint32_t>(entities.size()); i++)
                {
                    Entity* entity = entities[i];

                    // Get renderable
                    Renderable* renderable = entity->GetRenderable();
                    if (!renderable)
                        continue;

                    // Get material
                    Material* material = renderable->GetMaterial();
                    if (!material)
                        continue;

                    // Skip objects with different shader requirements
                    if (!static_cast<ShaderGBuffer*>(pso.shader_pixel)->IsSuitable(material->GetFlags()))
                        continue;

                    // Skip transparent objects that won't contribute
                    if (material->GetColorAlbedo().w == 0 && is_transparent_pass)
                        continue;

                    // Get geometry
                    Model* model = renderable->GeometryModel();
                    if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
                        continue;

                    // Skip objects outside of the view frustum
                    if (!m_camera->IsInViewFrustum(renderable))
                        continue;

                    // Set geometry (will only happen if not already set)
                    cmd_list->SetBufferIndex(model->GetIndexBuffer());
                    cmd_list->SetBufferVertex(model->GetVertexBuffer());

                    // Bind material
                    const bool firs_run       = material_index == 0;
                    const bool new_material   = material_bound_id != material->GetObjectId();
                    if (firs_run || new_material)
                    {
                        material_bound_id = material->GetObjectId();

                        // Keep track of used material instances (they get mapped to shaders)
                        if (material_index + 1 < m_material_instances.size())
                        {
                            // Advance index (0 is reserved for the sky)
                            material_index++;

                            // Keep reference
                            m_material_instances[material_index] = material;
                        }
                        else
                        {
                            LOG_ERROR("Material instance array has reached it's maximum capacity of %d elements. Consider increasing the size.", m_max_material_instances);
                        }

                        // Bind material textures
                        cmd_list->SetTexture(RendererBindings_Srv::material_albedo,      material->GetTexture_Ptr(Material_Color));
                        cmd_list->SetTexture(RendererBindings_Srv::material_roughness,   material->GetTexture_Ptr(Material_Roughness));
                        cmd_list->SetTexture(RendererBindings_Srv::material_metallic,    material->GetTexture_Ptr(Material_Metallic));
                        cmd_list->SetTexture(RendererBindings_Srv::material_normal,      material->GetTexture_Ptr(Material_Normal));
                        cmd_list->SetTexture(RendererBindings_Srv::material_height,      material->GetTexture_Ptr(Material_Height));
                        cmd_list->SetTexture(RendererBindings_Srv::material_occlusion,   material->GetTexture_Ptr(Material_Occlusion));
                        cmd_list->SetTexture(RendererBindings_Srv::material_emission,    material->GetTexture_Ptr(Material_Emission));
                        cmd_list->SetTexture(RendererBindings_Srv::material_mask,        material->GetTexture_Ptr(Material_AlphaMask));
                    
                        // Update uber buffer with material properties
                        m_cb_uber_cpu.mat_id            = material_index;
                        m_cb_uber_cpu.mat_albedo        = material->GetColorAlbedo();
                        m_cb_uber_cpu.mat_tiling_uv     = material->GetTiling();
                        m_cb_uber_cpu.mat_offset_uv     = material->GetOffset();
                        m_cb_uber_cpu.mat_roughness_mul = material->GetProperty(Material_Roughness);
                        m_cb_uber_cpu.mat_metallic_mul  = material->GetProperty(Material_Metallic);
                        m_cb_uber_cpu.mat_normal_mul    = material->GetProperty(Material_Normal);
                        m_cb_uber_cpu.mat_height_mul    = material->GetProperty(Material_Height);
                    }

                    // Update uber buffer with entity transform
                    if (Transform* transform = entity->GetTransform())
                    {
                        m_cb_uber_cpu.transform             = transform->GetMatrix();
                        m_cb_uber_cpu.transform_previous    = transform->GetMatrixPrevious();

                        // Save matrix for velocity computation
                        transform->SetWvpLastFrame(m_cb_uber_cpu.transform);

                        // Update object buffer
                        if (!Update_Cb_Uber(cmd_list))
                            continue;
                    }

                    // Render
                    cmd_list->DrawIndexed(renderable->GeometryIndexCount(), renderable->GeometryIndexOffset(), renderable->GeometryVertexOffset());
                    m_profiler->m_renderer_meshes_rendered++;
                }

                cmd_list->EndRenderPass();

                // Reset clear values after the first render pass
                pso.ResetClearValues();
            }
        }
    }

    void Renderer::Pass_Ssao(RHI_CommandList* cmd_list)
    {
        if ((m_options & Render_Ssao) == 0)
            return;

        bool do_gi = GetOptionValue<bool>(Renderer_Option_Value::Ssao_Gi);

        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[do_gi ? RendererShader::Ssao_Gi_C : RendererShader::Ssao_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Acquire textures
        shared_ptr<RHI_Texture>& tex_ssao_noisy     = RENDER_TARGET(RendererRt::Ssao);
        shared_ptr<RHI_Texture>& tex_ssao_blurred   = RENDER_TARGET(RendererRt::Ssao_Blurred);
        RHI_Texture* tex_depth                      = RENDER_TARGET(RendererRt::Gbuffer_Depth).get();
        RHI_Texture* tex_normal                     = RENDER_TARGET(RendererRt::Gbuffer_Normal).get();
        RHI_Texture* tex_albedo                     = RENDER_TARGET(RendererRt::Gbuffer_Albedo).get();
        RHI_Texture* tex_velocity                   = RENDER_TARGET(RendererRt::Gbuffer_Velocity).get();
        RHI_Texture* tex_diffuse                    = RENDER_TARGET(RendererRt::Light_Diffuse).get();

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute   = shader_c;
        pso.pass_name        = "Pass_Ssao";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_ssao_noisy->GetWidth()), static_cast<float>(tex_ssao_noisy->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_ssao_noisy->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_ssao_noisy->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z = 1;
            const bool async = false;

            cmd_list->SetTexture(do_gi ? RendererBindings_Uav::rgba : RendererBindings_Uav::r, tex_ssao_noisy);
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_normal, tex_normal);
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth, tex_depth);
            if (do_gi)
            {
                cmd_list->SetTexture(RendererBindings_Srv::gbuffer_albedo,   tex_albedo);
                cmd_list->SetTexture(RendererBindings_Srv::gbuffer_velocity, tex_velocity);
                cmd_list->SetTexture(RendererBindings_Srv::light_diffuse,    tex_diffuse);
            }

            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }

        // Bilateral blur
        const auto sigma = 2.0f;
        const auto pixel_stride = 2.0f;
        Pass_Blur_BilateralGaussian(cmd_list, tex_ssao_noisy, tex_ssao_blurred, sigma, pixel_stride, false);
    }

    void Renderer::Pass_Reflections_Ssr(RHI_CommandList* cmd_list)
    {
        if ((m_options & Render_ScreenSpaceReflections) == 0)
            return;

        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[RendererShader::Ssr_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Acquire render targets
        RHI_Texture* tex_out = RENDER_TARGET(RendererRt::Ssr).get();

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute   = shader_c;
        pso.pass_name        = "Pass_Reflections_Ssr";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z = 1;
            const bool async = false;

            cmd_list->SetTexture(RendererBindings_Uav::rgba,             tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_normal,   RENDER_TARGET(RendererRt::Gbuffer_Normal));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth,    RENDER_TARGET(RendererRt::Gbuffer_Depth));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_material, RENDER_TARGET(RendererRt::Gbuffer_Material));
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_Reflections(RHI_CommandList* cmd_list, RHI_Texture* tex_out, RHI_Texture* tex_reflections)
    {
        // Acquire shaders
        RHI_Shader* shader_v = m_shaders[RendererShader::Quad_V].get();
        RHI_Shader* shader_p = m_shaders[RendererShader::Reflections_P].get();
        if (!shader_v->IsCompiled() || !shader_p->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_vertex                   = shader_v;
        pso.shader_pixel                    = shader_p;
        pso.rasterizer_state                = m_rasterizer_cull_back_solid.get();
        pso.depth_stencil_state             = m_depth_stencil_off_off.get();
        pso.blend_state                     = m_blend_additive.get();
        pso.render_target_color_textures[0] = tex_out;
        pso.clear_color[0]                  = rhi_color_load;
        pso.render_target_depth_texture     = nullptr;
        pso.clear_depth                     = rhi_depth_dont_care;
        pso.clear_stencil                   = rhi_stencil_dont_care;
        pso.viewport                        = tex_out->GetViewport();
        pso.vertex_buffer_stride            = m_viewport_quad.GetVertexBuffer()->GetStride();
        pso.primitive_topology              = RHI_PrimitiveTopology_Mode::TriangleList;
        pso.pass_name                       = "Pass_Reflections";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_albedo,   RENDER_TARGET(RendererRt::Gbuffer_Albedo));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_normal,   RENDER_TARGET(RendererRt::Gbuffer_Normal));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_material, RENDER_TARGET(RendererRt::Gbuffer_Material));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth,    RENDER_TARGET(RendererRt::Gbuffer_Depth));
            cmd_list->SetTexture(RendererBindings_Srv::ssr,              RENDER_TARGET(RendererRt::Ssr));
            cmd_list->SetTexture(RendererBindings_Srv::frame,            tex_reflections);
            cmd_list->SetTexture(RendererBindings_Srv::environment,      GetEnvironmentTexture());

            cmd_list->SetBufferVertex(m_viewport_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_viewport_quad.GetIndexBuffer());
            cmd_list->DrawIndexed(Rectangle::GetIndexCount());
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_Light(RHI_CommandList* cmd_list, const bool is_transparent_pass /*= false*/)
    {
        // Acquire lights
        const vector<Entity*>& entities = m_entities[Renderer_ObjectType::Light];
        if (entities.empty())
            return;

        // Acquire render targets
        RHI_Texture* tex_diffuse    = is_transparent_pass ? RENDER_TARGET(RendererRt::Light_Diffuse_Transparent).get()   : RENDER_TARGET(RendererRt::Light_Diffuse).get();
        RHI_Texture* tex_specular   = is_transparent_pass ? RENDER_TARGET(RendererRt::Light_Specular_Transparent).get()  : RENDER_TARGET(RendererRt::Light_Specular).get();
        RHI_Texture* tex_volumetric = RENDER_TARGET(RendererRt::Light_Volumetric).get();

        // Clear render targets
        cmd_list->ClearRenderTarget(tex_diffuse,    0, 0, true, Vector4::Zero);
        cmd_list->ClearRenderTarget(tex_specular,   0, 0, true, Vector4::Zero);
        cmd_list->ClearRenderTarget(tex_volumetric, 0, 0, true, Vector4::Zero);

        // Set render state
        static RHI_PipelineState pso;
        pso.pass_name = is_transparent_pass ? "Pass_Light_Transparent" : "Pass_Light_Opaque";

        // Iterate through all the light entities
        for (const auto& entity : entities)
        {
            if (Light* light = entity->GetComponent<Light>())
            {
                if (light->GetIntensity() != 0)
                {
                    // Set pixel shader
                    pso.shader_compute = static_cast<RHI_Shader*>(ShaderLight::GetVariation(m_context, light, m_options));

                    // Skip the shader it failed to compiled or hasn't compiled yet
                    if (!pso.shader_compute->IsCompiled())
                        continue;

                    // Draw
                    if (cmd_list->BeginRenderPass(pso))
                    {
                        // Update materials structured buffer (light pass will access it using material IDs)
                        Update_Cb_Material(cmd_list);

                        cmd_list->SetTexture(RendererBindings_Uav::rgb,              tex_diffuse);
                        cmd_list->SetTexture(RendererBindings_Uav::rgb2,             tex_specular);
                        cmd_list->SetTexture(RendererBindings_Uav::rgb3,             tex_volumetric);
                        cmd_list->SetTexture(RendererBindings_Srv::gbuffer_albedo,   RENDER_TARGET(RendererRt::Gbuffer_Albedo));
                        cmd_list->SetTexture(RendererBindings_Srv::gbuffer_normal,   RENDER_TARGET(RendererRt::Gbuffer_Normal));
                        cmd_list->SetTexture(RendererBindings_Srv::gbuffer_material, RENDER_TARGET(RendererRt::Gbuffer_Material));
                        cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth,    RENDER_TARGET(RendererRt::Gbuffer_Depth));
                        cmd_list->SetTexture(RendererBindings_Srv::ssao,             RENDER_TARGET(RendererRt::Ssao_Blurred));

                        // Set shadow map
                        if (light->GetShadowsEnabled())
                        {
                            RHI_Texture* tex_depth = light->GetDepthTexture();
                            RHI_Texture* tex_color = light->GetShadowsTransparentEnabled() ? light->GetColorTexture() : m_tex_default_white.get();

                            if (light->GetLightType() == LightType::Directional)
                            {
                                cmd_list->SetTexture(RendererBindings_Srv::light_directional_depth, tex_depth);
                                cmd_list->SetTexture(RendererBindings_Srv::light_directional_color, tex_color);
                            }
                            else if (light->GetLightType() == LightType::Point)
                            {
                                cmd_list->SetTexture(RendererBindings_Srv::light_point_depth, tex_depth);
                                cmd_list->SetTexture(RendererBindings_Srv::light_point_color, tex_color);
                            }
                            else if (light->GetLightType() == LightType::Spot)
                            {
                                cmd_list->SetTexture(RendererBindings_Srv::light_spot_depth, tex_depth);
                                cmd_list->SetTexture(RendererBindings_Srv::light_spot_color, tex_color);
                            }
                        }

                        // Update light buffer
                        Update_Cb_Light(cmd_list, light);

                        // Update uber buffer
                        m_cb_uber_cpu.resolution            = Vector2(static_cast<float>(tex_diffuse->GetWidth()), static_cast<float>(tex_diffuse->GetHeight()));
                        m_cb_uber_cpu.is_transparent_pass   = is_transparent_pass;
                        Update_Cb_Uber(cmd_list);

                        const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_diffuse->GetWidth()) / m_thread_group_count));
                        const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_diffuse->GetHeight()) / m_thread_group_count));
                        const uint32_t thread_group_count_z = 1;
                        const bool async = false;

                        cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
                        cmd_list->EndRenderPass();
                    }
                }
            }
        }
    }

    void Renderer::Pass_Light_Composition(RHI_CommandList* cmd_list, RHI_Texture* tex_out, const bool is_transparent_pass /*= false*/)
    {
        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[RendererShader::Light_Composition_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute   = shader_c;
        pso.pass_name        = is_transparent_pass ? "Pass_Light_Composition_Transparent" : "Pass_Light_Composition_Opaque";

        // Begin commands
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution            = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            m_cb_uber_cpu.is_transparent_pass   = is_transparent_pass;
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z = 1;
            const bool async = false;

            // Setup command list
            cmd_list->SetTexture(RendererBindings_Uav::rgba,             tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_albedo,   RENDER_TARGET(RendererRt::Gbuffer_Albedo));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_material, RENDER_TARGET(RendererRt::Gbuffer_Material));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_normal,   RENDER_TARGET(RendererRt::Gbuffer_Normal));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth,    RENDER_TARGET(RendererRt::Gbuffer_Depth));
            cmd_list->SetTexture(RendererBindings_Srv::light_diffuse,    is_transparent_pass ? RENDER_TARGET(RendererRt::Light_Diffuse_Transparent).get() : RENDER_TARGET(RendererRt::Light_Diffuse).get());
            cmd_list->SetTexture(RendererBindings_Srv::light_specular,   is_transparent_pass ? RENDER_TARGET(RendererRt::Light_Specular_Transparent).get() : RENDER_TARGET(RendererRt::Light_Specular).get());
            cmd_list->SetTexture(RendererBindings_Srv::light_volumetric, RENDER_TARGET(RendererRt::Light_Volumetric));
            cmd_list->SetTexture(RendererBindings_Srv::frame,            RENDER_TARGET(RendererRt::Frame_Render_2)); // refraction
            cmd_list->SetTexture(RendererBindings_Srv::ssao,             RENDER_TARGET(RendererRt::Ssao_Blurred));
            cmd_list->SetTexture(RendererBindings_Srv::environment,      GetEnvironmentTexture());

            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_Light_ImageBased(RHI_CommandList* cmd_list, RHI_Texture* tex_out, const bool is_transparent_pass /*= false*/)
    {
        // The directional light's intensity is used to modulate the environment texture.
        // So, if the intensity is zero, then there is no need to do image based lighting.
        if (m_cb_frame_cpu.directional_light_intensity == 0.0f)
            return;

        // Acquire shaders
        RHI_Shader* shader_v = m_shaders[RendererShader::Quad_V].get();
        RHI_Shader* shader_p = m_shaders[RendererShader::Light_ImageBased_P].get();
        if (!shader_v->IsCompiled() || !shader_p->IsCompiled())
            return;

        RHI_Texture* tex_depth = RENDER_TARGET(RendererRt::Gbuffer_Depth).get();

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_vertex                           = shader_v;
        pso.shader_pixel                            = shader_p;
        pso.rasterizer_state                        = m_rasterizer_cull_back_solid.get();
        pso.depth_stencil_state                     = is_transparent_pass ? m_depth_stencil_off_r.get() : m_depth_stencil_off_off.get();
        pso.blend_state                             = m_blend_additive.get();
        pso.render_target_color_textures[0]         = tex_out;
        pso.clear_color[0]                          = rhi_color_load;
        pso.render_target_depth_texture             = is_transparent_pass ? tex_depth : nullptr;
        pso.render_target_depth_texture_read_only   = is_transparent_pass;
        pso.clear_depth                             = rhi_depth_load;
        pso.clear_stencil                           = rhi_stencil_load;
        pso.viewport                                = tex_out->GetViewport();
        pso.vertex_buffer_stride                    = m_viewport_quad.GetVertexBuffer()->GetStride();
        pso.primitive_topology                      = RHI_PrimitiveTopology_Mode::TriangleList;
        pso.pass_name                               = is_transparent_pass ? "Pass_Light_ImageBased_Transparent" : "Pass_Light_ImageBased_Opaque";

        // Begin commands
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution            = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            m_cb_uber_cpu.is_transparent_pass   = is_transparent_pass;
            Update_Cb_Uber(cmd_list);

            // Setup command list
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_albedo,   RENDER_TARGET(RendererRt::Gbuffer_Albedo));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_normal,   RENDER_TARGET(RendererRt::Gbuffer_Normal));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_material, RENDER_TARGET(RendererRt::Gbuffer_Material));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth,    tex_depth);
            cmd_list->SetTexture(RendererBindings_Srv::ssao,             (m_options & Render_Ssao) ? RENDER_TARGET(RendererRt::Ssao_Blurred) : m_tex_default_white);
            cmd_list->SetTexture(RendererBindings_Srv::lutIbl,           RENDER_TARGET(RendererRt::Brdf_Specular_Lut));
            cmd_list->SetTexture(RendererBindings_Srv::environment,      GetEnvironmentTexture());

            cmd_list->SetBufferVertex(m_viewport_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_viewport_quad.GetIndexBuffer());
            cmd_list->DrawIndexed(Rectangle::GetIndexCount());
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_Blur_Box(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out, const float sigma, const float pixel_stride, const bool use_stencil)
    {
        // Acquire shaders
        const auto& shader_v = m_shaders[RendererShader::Quad_V];
        const auto& shader_p = m_shaders[RendererShader::BlurBox_P];
        if (!shader_v->IsCompiled() || !shader_p->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso         = {};
        pso.shader_vertex                    = shader_v.get();
        pso.shader_pixel                     = shader_p.get();
        pso.rasterizer_state                 = m_rasterizer_cull_back_solid.get();
        pso.blend_state                      = m_blend_disabled.get();
        pso.depth_stencil_state              = use_stencil ? m_depth_stencil_off_r.get() : m_depth_stencil_off_off.get();
        pso.vertex_buffer_stride             = m_viewport_quad.GetVertexBuffer()->GetStride();
        pso.render_target_color_textures[0]  = tex_out.get();
        pso.clear_color[0]                   = rhi_color_dont_care;
        pso.render_target_depth_texture      = use_stencil ? RENDER_TARGET(RendererRt::Gbuffer_Depth).get() : nullptr;
        pso.viewport                         = tex_out->GetViewport();
        pso.primitive_topology               = RHI_PrimitiveTopology_Mode::TriangleList;
        pso.pass_name                        = "Pass_Blur_Box";

        // Record commands
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution        = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            m_cb_uber_cpu.blur_direction    = Vector2(pixel_stride, 0.0f);
            m_cb_uber_cpu.blur_sigma        = sigma;
            Update_Cb_Uber(cmd_list);

            cmd_list->SetBufferVertex(m_viewport_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_viewport_quad.GetIndexBuffer());
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->DrawIndexed(m_viewport_quad.GetIndexCount());
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_Blur_Gaussian(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out, const float sigma, const float pixel_stride)
    {
        if (tex_in->GetWidth() != tex_out->GetWidth() || tex_in->GetHeight() != tex_out->GetHeight() || tex_in->GetFormat() != tex_out->GetFormat())
        {
            LOG_ERROR("Invalid parameters, textures must match because they will get swapped");
            return;
        }

        // Acquire shaders
        const auto& shader_v = m_shaders[RendererShader::Quad_V];
        const auto& shader_p = m_shaders[RendererShader::BlurGaussian_P];
        if (!shader_v->IsCompiled() || !shader_p->IsCompiled())
            return;

        // Set render state for horizontal pass
        static RHI_PipelineState pso_horizontal;
        pso_horizontal.shader_vertex                     = shader_v.get();
        pso_horizontal.shader_pixel                      = shader_p.get();
        pso_horizontal.rasterizer_state                  = m_rasterizer_cull_back_solid.get();
        pso_horizontal.blend_state                       = m_blend_disabled.get();
        pso_horizontal.depth_stencil_state               = m_depth_stencil_off_off.get();
        pso_horizontal.vertex_buffer_stride              = m_viewport_quad.GetVertexBuffer()->GetStride();
        pso_horizontal.render_target_color_textures[0]   = tex_out.get();
        pso_horizontal.clear_color[0]                    = rhi_color_dont_care;
        pso_horizontal.viewport                          = tex_out->GetViewport();
        pso_horizontal.primitive_topology                = RHI_PrimitiveTopology_Mode::TriangleList;
        pso_horizontal.pass_name                         = "Pass_Blur_Gaussian_Horizontal";

        // Record commands for horizontal pass
        if (cmd_list->BeginRenderPass(pso_horizontal))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution        = Vector2(static_cast<float>(tex_in->GetWidth()), static_cast<float>(tex_in->GetHeight()));
            m_cb_uber_cpu.blur_direction    = Vector2(pixel_stride, 0.0f);
            m_cb_uber_cpu.blur_sigma        = sigma;
            Update_Cb_Uber(cmd_list);
        
            cmd_list->SetBufferVertex(m_viewport_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_viewport_quad.GetIndexBuffer());
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->DrawIndexed(Rectangle::GetIndexCount());
            cmd_list->EndRenderPass();
        }
        
        // Set render state for vertical pass
        static RHI_PipelineState pso_vertical;
        pso_vertical.shader_vertex                   = shader_v.get();
        pso_vertical.shader_pixel                    = shader_p.get();
        pso_vertical.rasterizer_state                = m_rasterizer_cull_back_solid.get();
        pso_vertical.blend_state                     = m_blend_disabled.get();
        pso_vertical.depth_stencil_state             = m_depth_stencil_off_off.get();
        pso_vertical.vertex_buffer_stride            = m_viewport_quad.GetVertexBuffer()->GetStride();
        pso_vertical.render_target_color_textures[0] = tex_in.get();
        pso_vertical.clear_color[0]                  = rhi_color_dont_care;
        pso_vertical.viewport                        = tex_in->GetViewport();
        pso_vertical.primitive_topology              = RHI_PrimitiveTopology_Mode::TriangleList;
        pso_vertical.pass_name                       = "Pass_Blur_Gaussian_Vertical";

        // Record commands for vertical pass
        if (cmd_list->BeginRenderPass(pso_vertical))
        {
            m_cb_uber_cpu.blur_direction    = Vector2(0.0f, pixel_stride);
            m_cb_uber_cpu.blur_sigma        = sigma;
            Update_Cb_Uber(cmd_list);
        
            cmd_list->SetBufferVertex(m_viewport_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_viewport_quad.GetIndexBuffer());
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_out);
            cmd_list->DrawIndexed(Rectangle::GetIndexCount());
            cmd_list->EndRenderPass();
        }

        // Swap textures
        tex_in.swap(tex_out);
    }

    void Renderer::Pass_Blur_BilateralGaussian(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out, const float sigma, const float pixel_stride, const bool use_stencil)
    {
        if (tex_in->GetWidth() != tex_out->GetWidth() || tex_in->GetHeight() != tex_out->GetHeight() || tex_in->GetFormat() != tex_out->GetFormat())
        {
            LOG_ERROR("Invalid parameters, textures must match because they will get swapped.");
            return;
        }

        // Acquire shaders
        const auto& shader_v = m_shaders[RendererShader::Quad_V];
        const auto& shader_p = m_shaders[RendererShader::BlurGaussianBilateral_P];
        if (!shader_v->IsCompiled() || !shader_p->IsCompiled())
            return;

        // Acquire render targets
        RHI_Texture* tex_depth     = RENDER_TARGET(RendererRt::Gbuffer_Depth).get();
        RHI_Texture* tex_normal    = RENDER_TARGET(RendererRt::Gbuffer_Normal).get();

        // Set render state for horizontal pass
        static RHI_PipelineState pso;
        pso.shader_vertex                     = shader_v.get();
        pso.shader_pixel                      = shader_p.get();
        pso.rasterizer_state                  = m_rasterizer_cull_back_solid.get();
        pso.blend_state                       = m_blend_disabled.get();
        pso.depth_stencil_state               = use_stencil ? m_depth_stencil_off_r.get() : m_depth_stencil_off_off.get();
        pso.vertex_buffer_stride              = m_viewport_quad.GetVertexBuffer()->GetStride();
        pso.render_target_color_textures[0]   = tex_out.get();
        pso.clear_color[0]                    = rhi_color_dont_care;
        pso.render_target_depth_texture       = use_stencil ? tex_depth : nullptr;
        pso.clear_stencil                     = use_stencil ? rhi_stencil_load : rhi_stencil_dont_care;
        pso.viewport                          = tex_out->GetViewport();
        pso.primitive_topology                = RHI_PrimitiveTopology_Mode::TriangleList;
        pso.pass_name                         = "Pass_Blur_BilateralGaussian_Horizontal";

        // Record commands for horizontal pass
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution        = Vector2(static_cast<float>(tex_in->GetWidth()), static_cast<float>(tex_in->GetHeight()));
            m_cb_uber_cpu.blur_direction    = Vector2(pixel_stride, 0.0f);
            m_cb_uber_cpu.blur_sigma        = sigma;
            Update_Cb_Uber(cmd_list);

            cmd_list->SetBufferVertex(m_viewport_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_viewport_quad.GetIndexBuffer());
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth, tex_depth);
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_normal, tex_normal);
            cmd_list->DrawIndexed(m_viewport_quad.GetIndexCount());
            cmd_list->EndRenderPass();
        }

        // Set render state for vertical pass
        pso.render_target_color_textures[0] = tex_in.get();
        pso.viewport                        = tex_in->GetViewport();
        pso.pass_name                       = "Pass_Blur_BilateralGaussian_Vertical";

        // Record commands for vertical pass
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.blur_direction    = Vector2(0.0f, pixel_stride);
            m_cb_uber_cpu.blur_sigma        = sigma;
            Update_Cb_Uber(cmd_list);

            cmd_list->SetBufferVertex(m_viewport_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_viewport_quad.GetIndexBuffer());
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth, tex_depth);
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_normal, tex_normal);
            cmd_list->DrawIndexed(m_viewport_quad.GetIndexCount());
            cmd_list->EndRenderPass();
        }

        // Swap textures
        tex_in.swap(tex_out);
    }

    void Renderer::Pass_PostProcess(RHI_CommandList* cmd_list)
    {
        // IN:  RenderTarget_Composition_Hdr
        // OUT: RenderTarget_Composition_Ldr

        // Acquire render targets
        shared_ptr<RHI_Texture>& rt_frame_render_in         = RENDER_TARGET(RendererRt::Frame_Render);   // render res
        shared_ptr<RHI_Texture>& rt_frame_render_out        = RENDER_TARGET(RendererRt::Frame_Render_2); // render res
        shared_ptr<RHI_Texture>& rt_frame_render_output_in  = RENDER_TARGET(RendererRt::Frame_Output);   // output res
        shared_ptr<RHI_Texture>& rt_frame_render_output_out = RENDER_TARGET(RendererRt::Frame_Output_2); // output res

        // Depth of Field
        if (GetOption(Render_DepthOfField))
        {
            Pass_PostProcess_DepthOfField(cmd_list, rt_frame_render_in, rt_frame_render_out);
            rt_frame_render_in.swap(rt_frame_render_out);
        }

        // Upsampling vars
        bool upsampled                   = false;
        bool resolution_output_larger    = m_resolution_output.x > m_resolution_render.x || m_resolution_output.y > m_resolution_render.y;
        bool resolution_output_different = m_resolution_output != m_resolution_render;

        // TAA
        if (GetOption(Render_AntiAliasing_Taa))
        {
            if (GetOption(Render_Upsample_TAA) && resolution_output_larger)
            {
                Pass_PostProcess_TAA(cmd_list, rt_frame_render_in, rt_frame_render_output_in);
                upsampled = true; // taa writes directly in the high res buffer
            }
            else
            {
                Pass_PostProcess_TAA(cmd_list, rt_frame_render_in, rt_frame_render_out);
                rt_frame_render_in.swap(rt_frame_render_out);
            }
        }

        // Upsample - AMD FidelityFX SuperResolution - TODO: This needs to be in perceptual space and normalised to 0, 1 range.
        if (GetOption(Render_Upsample_AMD_FidelityFX_SuperResolution) && resolution_output_larger)
        {
            Pass_AMD_FidelityFX_SuperResolution(cmd_list, rt_frame_render_in.get(), rt_frame_render_output_in.get(), rt_frame_render_output_out.get());
            upsampled = true;
        }

        // If we haven't upsampled, do a bilinear upscale (different output resolution) or a blit (same output resolution)
        if (!upsampled)
        {
            if (resolution_output_different)
            {
                // D3D11 luggage, can't blit to different resolution
                Pass_CopyBilinear(cmd_list, rt_frame_render_in.get(), rt_frame_render_output_in.get());
            }
            else
            {
                cmd_list->Blit(rt_frame_render_in, rt_frame_render_output_in);
            }
        }

        // Motion Blur
        if (GetOption(Render_MotionBlur))
        {
            Pass_PostProcess_MotionBlur(cmd_list, rt_frame_render_output_in, rt_frame_render_output_out);
            rt_frame_render_output_in.swap(rt_frame_render_output_out);
        }

        // Bloom
        if (GetOption(Render_Bloom))
        {
            Pass_PostProcess_Bloom(cmd_list, rt_frame_render_output_in, rt_frame_render_output_out);
            rt_frame_render_output_in.swap(rt_frame_render_output_out);
        }

        // Sharpening
        if (GetOption(Render_Sharpening_AMD_FidelityFX_ContrastAdaptiveSharpening))
        {
            Pass_AMD_FidelityFX_ContrastAdaptiveSharpening(cmd_list, rt_frame_render_output_in, rt_frame_render_output_out);
            rt_frame_render_output_in.swap(rt_frame_render_output_out);
        }

        // Tone-Mapping
        if (m_option_values[Renderer_Option_Value::Tonemapping] != 0)
        {
            Pass_PostProcess_ToneMapping(cmd_list, rt_frame_render_output_in, rt_frame_render_output_out);
            rt_frame_render_output_in.swap(rt_frame_render_output_out);
        }

        // FXAA
        if (GetOption(Render_AntiAliasing_Fxaa))
        {
            Pass_PostProcess_Fxaa(cmd_list, rt_frame_render_output_in, rt_frame_render_output_out);
            rt_frame_render_output_in.swap(rt_frame_render_output_out);
        }

        // Dithering
        if (GetOption(Render_Dithering))
        {
            Pass_PostProcess_Dithering(cmd_list, rt_frame_render_output_in, rt_frame_render_output_out);
            rt_frame_render_output_in.swap(rt_frame_render_output_out);
        }

        // Film grain
        if (GetOption(Render_FilmGrain))
        {
            Pass_PostProcess_FilmGrain(cmd_list, rt_frame_render_output_in, rt_frame_render_output_out);
            rt_frame_render_output_in.swap(rt_frame_render_output_out);
        }

        // Chromatic aberration
        if (GetOption(Render_ChromaticAberration))
        {
            Pass_PostProcess_ChromaticAberration(cmd_list, rt_frame_render_output_in, rt_frame_render_output_out);
            rt_frame_render_output_in.swap(rt_frame_render_output_out);
        }

        // Gamma correction
        Pass_PostProcess_GammaCorrection(cmd_list, rt_frame_render_output_in, rt_frame_render_output_out);

        // Passes that render on top of each other
        Pass_Outline(cmd_list, rt_frame_render_output_out.get());
        Pass_TransformHandle(cmd_list, rt_frame_render_output_out.get());
        Pass_Lines(cmd_list, rt_frame_render_output_out.get());
        Pass_Icons(cmd_list, rt_frame_render_output_out.get());
        Pass_DebugBuffer(cmd_list, rt_frame_render_output_out.get());
        Pass_Text(cmd_list, rt_frame_render_output_out.get());

        // Swap textures
        rt_frame_render_output_in.swap(rt_frame_render_output_out);
    }

    void Renderer::Pass_PostProcess_TAA(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[RendererShader::Taa_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Acquire history texture
        RHI_Texture* tex_history = RENDER_TARGET(RendererRt::Taa_History).get();

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute   = shader_c;
        pso.pass_name        = "Pass_PostProcess_TAA";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z = 1;
            const bool async = false;

            cmd_list->SetTexture(RendererBindings_Srv::tex,              tex_history);
            cmd_list->SetTexture(RendererBindings_Srv::tex2,             tex_in);
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_velocity, RENDER_TARGET(RendererRt::Gbuffer_Velocity));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth,    RENDER_TARGET(RendererRt::Gbuffer_Depth));
            cmd_list->SetTexture(RendererBindings_Uav::rgb,              tex_out);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }

        // Update history buffer
        if (tex_out->GetViewport() == tex_history->GetViewport())
        {
            cmd_list->Blit(tex_out.get(), tex_history);
        }
        else
        {
            Pass_CopyBilinear(cmd_list, tex_in.get(), tex_out.get());
        }
    }

    void Renderer::Pass_PostProcess_Bloom(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader_luminance        = m_shaders[RendererShader::BloomLuminance_C].get();
        RHI_Shader* shader_upsampleBlendMip = m_shaders[RendererShader::BloomUpsampleBlendMip_C].get();
        RHI_Shader* shader_blendFrame       = m_shaders[RendererShader::BloomBlendFrame_C].get();

        if (!shader_luminance->IsCompiled() || !shader_upsampleBlendMip->IsCompiled() || !shader_blendFrame->IsCompiled())
            return;

        // Acquire render target
        RHI_Texture* tex_bloom = RENDER_TARGET(RendererRt::Bloom).get();

        // Luminance
        {
            // Set render state
            static RHI_PipelineState pso;
            pso.shader_compute = shader_luminance;
            pso.pass_name      = "Pass_PostProcess_BloomLuminance";

            // Draw
            if (cmd_list->BeginRenderPass(pso))
            {
                // Update uber buffer
                m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_bloom->GetWidth()), static_cast<float>(tex_bloom->GetHeight()));
                Update_Cb_Uber(cmd_list);

                const uint32_t thread_group_count_x   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_bloom->GetWidth()) / m_thread_group_count));
                const uint32_t thread_group_count_y   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_bloom->GetHeight()) / m_thread_group_count));
                const uint32_t thread_group_count_z   = 1;
                const bool async                      = false;

                cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_bloom);
                cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
                cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
                cmd_list->EndRenderPass();
            }
        }

        // Generate mips
        Pass_AMD_FidelityFX_SinglePassDowsnampler(cmd_list, tex_bloom);

        // Starting from the lowest mip, upsample and blend with the higher one
        {
            // Set render state
            static RHI_PipelineState pso;
            pso.shader_compute  = shader_upsampleBlendMip;
            pso.pass_name       = "Pass_PostProcess_BloomUpsampleBlendMip";

            // Draw
            if (cmd_list->BeginRenderPass(pso))
            {
                for (int i = static_cast<int>(tex_bloom->GetMipCount() - 1); i > 0; i--)
                {
                    int mip_index_small     = i;
                    int mip_index_big       = i - 1;
                    int mip_width_large     = tex_bloom->GetWidth() >> mip_index_big;
                    int mip_height_height   = tex_bloom->GetHeight() >> mip_index_big;

                    // Update uber buffer
                    m_cb_uber_cpu.resolution = Vector2(static_cast<float>(mip_width_large), static_cast<float>(mip_height_height));
                    Update_Cb_Uber(cmd_list);

                    const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(mip_width_large) / m_thread_group_count));
                    const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(mip_height_height) / m_thread_group_count));
                    const uint32_t thread_group_count_z = 1;
                    const bool async = false;

                    cmd_list->SetTexture(RendererBindings_Srv::tex, tex_bloom, mip_index_small);
                    cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_bloom, mip_index_big);
                    cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
                }

                cmd_list->EndRenderPass();
            }
        }

        // Blend with the frame
        {
            // Set render state
            static RHI_PipelineState pso;
            pso.shader_compute   = shader_blendFrame;
            pso.pass_name        = "Pass_PostProcess_BloomBlendFrame";

            // Draw
            if (cmd_list->BeginRenderPass(pso))
            {
                // Update uber buffer
                m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
                Update_Cb_Uber(cmd_list);

                const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
                const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
                const uint32_t thread_group_count_z = 1;
                const bool async = false;

                cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out.get());
                cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
                cmd_list->SetTexture(RendererBindings_Srv::tex2, tex_bloom, 0);
                cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
                cmd_list->EndRenderPass();
            }
        }
    }

    void Renderer::Pass_PostProcess_ToneMapping(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[RendererShader::ToneMapping_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute = shader_c;
        pso.pass_name      = "Pass_PostProcess_ToneMapping";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z = 1;
            const bool async = false;

            cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_PostProcess_GammaCorrection(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[RendererShader::GammaCorrection_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute   = shader_c;
        pso.pass_name        = "Pass_PostProcess_GammaCorrection";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z   = 1;
            const bool async                      = false;

            cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_PostProcess_Fxaa(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[RendererShader::Fxaa_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute   = shader_c;
        pso.pass_name        = "Pass_PostProcess_FXAA";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Compute thread count
            const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z = 1;
            const bool async = false;

            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_PostProcess_ChromaticAberration(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[RendererShader::ChromaticAberration_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute = shader_c;
        pso.pass_name      = "Pass_PostProcess_ChromaticAberration";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z   = 1;
            const bool async                      = false;

            cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_PostProcess_MotionBlur(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[RendererShader::MotionBlur_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute = shader_c;
        pso.pass_name      = "Pass_PostProcess_MotionBlur";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z   = 1;
            const bool async                      = false;

            cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_velocity, RENDER_TARGET(RendererRt::Gbuffer_Velocity));
            cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth,    RENDER_TARGET(RendererRt::Gbuffer_Depth));
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_PostProcess_DepthOfField(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader_downsampleCoc    = m_shaders[RendererShader::Dof_DownsampleCoc_C].get();
        RHI_Shader* shader_bokeh            = m_shaders[RendererShader::Dof_Bokeh_C].get();
        RHI_Shader* shader_tent             = m_shaders[RendererShader::Dof_Tent_C].get();
        RHI_Shader* shader_upsampleBlend    = m_shaders[RendererShader::Dof_UpscaleBlend_C].get();
        if (!shader_downsampleCoc->IsCompiled() || !shader_bokeh->IsCompiled() || !shader_tent->IsCompiled() || !shader_upsampleBlend->IsCompiled())
            return;

        // Acquire render targets
        RHI_Texture* tex_bokeh_half     = RENDER_TARGET(RendererRt::Dof_Half).get();
        RHI_Texture* tex_bokeh_half_2   = RENDER_TARGET(RendererRt::Dof_Half_2).get();
        RHI_Texture* tex_depth          = RENDER_TARGET(RendererRt::Gbuffer_Depth).get();

        // Downsample and compute circle of confusion
        {
            // Set render state
            static RHI_PipelineState pso;
            pso.shader_compute = shader_downsampleCoc;
            pso.pass_name      = "Pass_PostProcess_Dof_DownsampleCoc";

            // Draw
            if (cmd_list->BeginRenderPass(pso))
            {
                // Update uber buffer
                m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_bokeh_half->GetWidth()), static_cast<float>(tex_bokeh_half->GetHeight()));
                Update_Cb_Uber(cmd_list);

                const uint32_t thread_group_count_x   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_bokeh_half->GetWidth()) / m_thread_group_count));
                const uint32_t thread_group_count_y   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_bokeh_half->GetHeight()) / m_thread_group_count));
                const uint32_t thread_group_count_z   = 1;
                const bool async                      = false;

                cmd_list->SetTexture(RendererBindings_Uav::rgba, tex_bokeh_half);
                cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth, tex_depth);
                cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
                cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
                cmd_list->EndRenderPass();
            }
        }

        // Bokeh
        {
            // Set render state
            static RHI_PipelineState pso;
            pso.shader_compute   = shader_bokeh;
            pso.pass_name        = "Pass_PostProcess_Dof_Bokeh";

            // Draw
            if (cmd_list->BeginRenderPass(pso))
            {
                // Update uber buffer
                m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_bokeh_half_2->GetWidth()), static_cast<float>(tex_bokeh_half_2->GetHeight()));
                Update_Cb_Uber(cmd_list);

                const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_bokeh_half_2->GetWidth()) / m_thread_group_count));
                const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_bokeh_half_2->GetHeight()) / m_thread_group_count));
                const uint32_t thread_group_count_z = 1;
                const bool async = false;

                cmd_list->SetTexture(RendererBindings_Uav::rgba, tex_bokeh_half_2);
                cmd_list->SetTexture(RendererBindings_Srv::tex, tex_bokeh_half);
                cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
                cmd_list->EndRenderPass();
            }
        }

        // Blur the bokeh using a tent filter
        {
            // Set render state
            static RHI_PipelineState pso;
            pso.shader_compute   = shader_tent;
            pso.pass_name        = "Pass_PostProcess_Dof_Tent";

            // Draw
            if (cmd_list->BeginRenderPass(pso))
            {
                // Update uber buffer
                m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_bokeh_half->GetWidth()), static_cast<float>(tex_bokeh_half->GetHeight()));
                Update_Cb_Uber(cmd_list);

                const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_bokeh_half->GetWidth()) / m_thread_group_count));
                const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_bokeh_half->GetHeight()) / m_thread_group_count));
                const uint32_t thread_group_count_z = 1;
                const bool async = false;

                cmd_list->SetTexture(RendererBindings_Uav::rgba, tex_bokeh_half);
                cmd_list->SetTexture(RendererBindings_Srv::tex, tex_bokeh_half_2);
                cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
                cmd_list->EndRenderPass();
            }
        }

        // Upscale & Blend
        {
            // Set render state
            static RHI_PipelineState pso;
            pso.shader_compute   = shader_upsampleBlend;
            pso.pass_name        = "Pass_PostProcess_Dof_UpscaleBlend";

            // Draw
            if (cmd_list->BeginRenderPass(pso))
            {
                // Update uber buffer
                m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
                Update_Cb_Uber(cmd_list);

                const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
                const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
                const uint32_t thread_group_count_z = 1;
                const bool async = false;

                cmd_list->SetTexture(RendererBindings_Uav::rgba, tex_out);
                cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth, tex_depth);
                cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
                cmd_list->SetTexture(RendererBindings_Srv::tex2, tex_bokeh_half);
                cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
                cmd_list->EndRenderPass();
            }
        }
    }

    void Renderer::Pass_PostProcess_Dithering(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader = m_shaders[RendererShader::Dithering_C].get();
        if (!shader->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute   = shader;
        pso.pass_name        = "Pass_PostProcess_Dithering";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z = 1;
            const bool async = false;

            cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_PostProcess_FilmGrain(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[RendererShader::FilmGrain_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute = shader_c;
        pso.pass_name      = "Pass_PostProcess_FilmGrain";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z   = 1;
            const bool async                      = false;

            cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_AMD_FidelityFX_ContrastAdaptiveSharpening(RHI_CommandList* cmd_list, shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[RendererShader::AMD_FidelityFX_CAS_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute = shader_c;
        pso.pass_name      = "Pass_AMD_FidelityFX_ContrastAdaptiveSharpening";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y   = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z   = 1;
            const bool async                      = false;

            cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_AMD_FidelityFX_SinglePassDowsnampler(RHI_CommandList* cmd_list, RHI_Texture* tex)
    {
        // AMD FidelityFX Single Pass Downsampler.
        // Provides an RDNA�-optimized solution for generating up to 12 MIP levels of a texture.
        // GitHub:          https://github.com/GPUOpen-Effects/FidelityFX-SPD
        // Documentation:   https://github.com/GPUOpen-Effects/FidelityFX-SPD/blob/master/docs/FidelityFX_SPD.pdf

        SP_ASSERT(tex->HasPerMipView());

        // Acquire shader
        RHI_Shader* shader = m_shaders[RendererShader::AMD_FidelityFX_SPD_C].get();

        if (!shader->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute  = shader;
        pso.pass_name       = "Pass_AMD_FidelityFX_SinglePassDowsnampler";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            uint32_t generated_mips_count = tex->GetMipCount() - 1; // all mips but the top

            // As per documentation (page 22)
            SP_ASSERT(generated_mips_count <= 12);

            // As per documentation (page 22)
            const uint32_t thread_group_count_x = (tex->GetWidth() + 63) >> 6;
            const uint32_t thread_group_count_y = (tex->GetHeight() + 63) >> 6;
            const uint32_t thread_group_count_z = 1;
            const bool async                    = false;
        
            // Update uber buffer
            m_cb_uber_cpu.resolution        = Vector2(static_cast<float>(tex->GetWidth()), static_cast<float>(tex->GetHeight()));
            m_cb_uber_cpu.mip_count         = generated_mips_count;
            m_cb_uber_cpu.work_group_count  = thread_group_count_x * thread_group_count_y * thread_group_count_z;
            Update_Cb_Uber(cmd_list);
        
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex, 0); // top mip
            cmd_list->SetTexture(RendererBindings_Uav::rgba_mips, tex, 1, true); // rest of the mips
            cmd_list->SetStructuredBuffer(RendererBindings_Sb::counter, m_sb_counter);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_AMD_FidelityFX_SuperResolution(RHI_CommandList* cmd_list, RHI_Texture* tex_in, RHI_Texture* tex_out, RHI_Texture* tex_out_scratch)
    {
        // Acquire shaders
        RHI_Shader* shader_upsample_c   = m_shaders[RendererShader::AMD_FidelityFX_FSR_Upsample_C].get();
        RHI_Shader* shader_sharpen_c    = m_shaders[RendererShader::AMD_FidelityFX_FSR_Sharpen_C].get();
        if (!shader_upsample_c->IsCompiled() || !shader_sharpen_c->IsCompiled())
            return;

        // Upsample
        {
            // Set render state
            static RHI_PipelineState pso;
            pso.shader_compute = shader_upsample_c;
            pso.pass_name      = "Pass_AMD_FidelityFX_SuperResolution_Upsample";

            if (cmd_list->BeginRenderPass(pso))
            {
                static const int thread_group_work_region_dim   = 16;
                const uint32_t thread_group_count_x             = (tex_out->GetWidth() + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim;
                const uint32_t thread_group_count_y             = (tex_out->GetHeight() + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim;
                const uint32_t thread_group_count_z             = 1;
                const bool async                                = false;

                cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out_scratch);
                cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
                cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
                cmd_list->EndRenderPass();
            }
        }

        // Sharpen
        {
            // Set render state
            static RHI_PipelineState pso;
            pso.shader_compute  = shader_sharpen_c;
            pso.pass_name       = "Pass_AMD_FidelityFX_SuperResolution_Sharpen";

            if (cmd_list->BeginRenderPass(pso))
            {
                static const int thread_group_work_region_dim   = 16;
                const uint32_t thread_group_count_x             = (tex_out->GetWidth() + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim;
                const uint32_t thread_group_count_y             = (tex_out->GetHeight() + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim;
                const uint32_t thread_group_count_z             = 1;
                const bool async                                = false;

                cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out);
                cmd_list->SetTexture(RendererBindings_Srv::tex, tex_out_scratch);
                cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
                cmd_list->EndRenderPass();
            }
        }
    }

    void Renderer::Pass_Lines(RHI_CommandList* cmd_list, RHI_Texture* tex_out)
    {
        const bool draw_picking_ray = m_options & Render_Debug_PickingRay;
        const bool draw_aabb        = m_options & Render_Debug_Aabb;
        const bool draw_grid        = m_options & Render_Debug_Grid;
        const bool draw_lights      = m_options & Render_Debug_Lights;
        const bool draw_lines       = !m_lines_depth_disabled.empty() || !m_lines_depth_enabled.empty(); // Any kind of lines, physics, user debug, etc.
        const bool draw             = draw_picking_ray || draw_aabb || draw_grid || draw_lines || draw_lights;
        if (!draw)
            return;

        // Acquire color shaders
        RHI_Shader* shader_color_v = m_shaders[RendererShader::Color_V].get();
        RHI_Shader* shader_color_p = m_shaders[RendererShader::Color_P].get();
        if (!shader_color_v->IsCompiled() || !shader_color_p->IsCompiled())
            return;

        // Grid
        if (draw_grid)
        {
            // Set render state
            static RHI_PipelineState pso;
            pso.shader_vertex                    = shader_color_v;
            pso.shader_pixel                     = shader_color_p;
            pso.rasterizer_state                 = m_rasterizer_cull_back_wireframe.get();
            pso.blend_state                      = m_blend_alpha.get();
            pso.depth_stencil_state              = m_depth_stencil_r_off.get();
            pso.vertex_buffer_stride             = m_gizmo_grid->GetVertexBuffer()->GetStride();
            pso.render_target_color_textures[0]  = tex_out;
            pso.render_target_depth_texture      = RENDER_TARGET(RendererRt::Gbuffer_Depth).get();
            pso.viewport                         = tex_out->GetViewport();
            pso.primitive_topology               = RHI_PrimitiveTopology_Mode::LineList;
            pso.pass_name                        = "Pass_Lines_Grid";
        
            // Create and submit command list
            if (cmd_list->BeginRenderPass(pso))
            {
                // Update uber buffer
                m_cb_uber_cpu.resolution    = m_resolution_render;
                m_cb_uber_cpu.transform     = m_gizmo_grid->ComputeWorldMatrix(m_camera->GetTransform()) * m_cb_frame_cpu.view_projection_unjittered;
                Update_Cb_Uber(cmd_list);
        
                cmd_list->SetBufferIndex(m_gizmo_grid->GetIndexBuffer().get());
                cmd_list->SetBufferVertex(m_gizmo_grid->GetVertexBuffer().get());
                cmd_list->DrawIndexed(m_gizmo_grid->GetIndexCount());
                cmd_list->EndRenderPass();
            }
        }

        // Generate lines for debug primitives supported by the renderer
        {
            // Picking ray
            if (draw_picking_ray)
            {
                const auto& ray = m_camera->GetPickingRay();
                DrawLine(ray.GetStart(), ray.GetStart() + ray.GetDirection() * m_camera->GetFarPlane(), Vector4(0, 1, 0, 1));
            }

            // Lights
            if (draw_lights)
            {
                auto& lights = m_entities[Renderer_ObjectType::Light];
                for (const auto& entity : lights)
                {
                    const Entity* entity_selected = m_transform_handle->GetSelectedEntity();
                    if (entity_selected && entity_selected->GetObjectId() == entity->GetObjectId())
                    { 
                        Light* light = entity->GetComponent<Light>();

                        if (light->GetLightType() == LightType::Directional)
                        {
                            Vector3 pos_start   = light->GetTransform()->GetPosition();
                            Vector3 pos_end     = -pos_start;
                            DrawLine(pos_start, pos_end);

                        }
                        else if (light->GetLightType() == LightType::Point)
                        {
                            Vector3 center          = light->GetTransform()->GetPosition();
                            float radius            = light->GetRange();
                            uint32_t segment_count  = 64;

                            DrawCircle(center, Vector3::Up, radius, segment_count);
                            DrawCircle(center, Vector3::Right, radius, segment_count);
                            DrawCircle(center, Vector3::Forward, radius, segment_count);
                        }
                        else if (light->GetLightType() == LightType::Spot)
                        {
                            // tan(angle) = opposite/adjacent
                            // opposite = adjacent * tan(angle)
                            float opposite  = light->GetRange() * Math::Helper::Tan(light->GetAngle());

                            Vector3 pos_end_center  = light->GetTransform()->GetForward() * light->GetRange();
                            Vector3 pos_end_up      = pos_end_center + light->GetTransform()->GetUp()      * opposite;
                            Vector3 pos_end_right   = pos_end_center + light->GetTransform()->GetRight()   * opposite;
                            Vector3 pos_end_down    = pos_end_center + light->GetTransform()->GetDown()    * opposite;
                            Vector3 pos_end_left    = pos_end_center + light->GetTransform()->GetLeft()    * opposite;

                            Vector3 pos_start = light->GetTransform()->GetPosition();
                            DrawLine(pos_start, pos_start + pos_end_center);
                            DrawLine(pos_start, pos_start + pos_end_up);
                            DrawLine(pos_start, pos_start + pos_end_right);
                            DrawLine(pos_start, pos_start + pos_end_down);
                            DrawLine(pos_start, pos_start + pos_end_left);
                        }
                    }
                }
            }

            // AABBs
            if (draw_aabb)
            {
                for (const auto& entity : m_entities[Renderer_ObjectType::GeometryOpaque])
                {
                    if (auto renderable = entity->GetRenderable())
                    {
                        DrawBox(renderable->GetAabb(), Vector4(0.41f, 0.86f, 1.0f, 1.0f));
                    }
                }

                for (const auto& entity : m_entities[Renderer_ObjectType::GeometryTransparent])
                {
                    if (auto renderable = entity->GetRenderable())
                    {
                        DrawBox(renderable->GetAabb(), Vector4(0.41f, 0.86f, 1.0f, 1.0f));
                    }
                }
            }
        }

        // Draw lines
        {
            // Width depth
            uint32_t line_vertex_buffer_size = static_cast<uint32_t>(m_lines_depth_enabled.size());
            if (line_vertex_buffer_size != 0)
            {
                // Grow vertex buffer (if needed)
                if (line_vertex_buffer_size > m_vertex_buffer_lines->GetVertexCount())
                {
                    m_vertex_buffer_lines->CreateDynamic<RHI_Vertex_PosCol>(line_vertex_buffer_size);
                }

                // Update vertex buffer
                RHI_Vertex_PosCol* buffer = static_cast<RHI_Vertex_PosCol*>(m_vertex_buffer_lines->Map());
                std::copy(m_lines_depth_enabled.begin(), m_lines_depth_enabled.end(), buffer);
                m_vertex_buffer_lines->Unmap();

                // Set render state
                static RHI_PipelineState pso;
                pso.shader_vertex                    = shader_color_v;
                pso.shader_pixel                     = shader_color_p;
                pso.rasterizer_state                 = m_rasterizer_cull_back_wireframe.get();
                pso.blend_state                      = m_blend_alpha.get();
                pso.depth_stencil_state              = m_depth_stencil_r_off.get();
                pso.vertex_buffer_stride             = m_vertex_buffer_lines->GetStride();
                pso.render_target_color_textures[0]  = tex_out;
                pso.render_target_depth_texture      = RENDER_TARGET(RendererRt::Gbuffer_Depth).get();
                pso.viewport                         = tex_out->GetViewport();
                pso.primitive_topology               = RHI_PrimitiveTopology_Mode::LineList;
                pso.pass_name                        = "Pass_Lines";

                // Create and submit command list
                if (cmd_list->BeginRenderPass(pso))
                {
                    cmd_list->SetBufferVertex(m_vertex_buffer_lines.get());
                    cmd_list->Draw(line_vertex_buffer_size);
                    cmd_list->EndRenderPass();
                }
            }

            // Without depth
            line_vertex_buffer_size = static_cast<uint32_t>(m_lines_depth_disabled.size());
            if (line_vertex_buffer_size != 0)
            {
                // Grow vertex buffer (if needed)
                if (line_vertex_buffer_size > m_vertex_buffer_lines->GetVertexCount())
                {
                    m_vertex_buffer_lines->CreateDynamic<RHI_Vertex_PosCol>(line_vertex_buffer_size);
                }

                // Update vertex buffer
                RHI_Vertex_PosCol* buffer = static_cast<RHI_Vertex_PosCol*>(m_vertex_buffer_lines->Map());
                std::copy(m_lines_depth_disabled.begin(), m_lines_depth_disabled.end(), buffer);
                m_vertex_buffer_lines->Unmap();

                // Set render state
                static RHI_PipelineState pso;
                pso.shader_vertex                    = shader_color_v;
                pso.shader_pixel                     = shader_color_p;
                pso.rasterizer_state                 = m_rasterizer_cull_back_wireframe.get();
                pso.blend_state                      = m_blend_disabled.get();
                pso.depth_stencil_state              = m_depth_stencil_off_off.get();
                pso.vertex_buffer_stride             = m_vertex_buffer_lines->GetStride();
                pso.render_target_color_textures[0]  = tex_out;
                pso.viewport                         = tex_out->GetViewport();
                pso.primitive_topology               = RHI_PrimitiveTopology_Mode::LineList;
                pso.pass_name                        = "Pass_Lines_No_Depth";

                // Create and submit command list
                if (cmd_list->BeginRenderPass(pso))
                {
                    cmd_list->SetBufferVertex(m_vertex_buffer_lines.get());
                    cmd_list->Draw(line_vertex_buffer_size);
                    cmd_list->EndRenderPass();
                }
            }
        }
    }

    void Renderer::Pass_Icons(RHI_CommandList* cmd_list, RHI_Texture* tex_out)
    {
        if (!(m_options & Render_Debug_Lights))
            return;

        // Acquire resources
        auto& lights                    = m_entities[Renderer_ObjectType::Light];
        const auto& shader_quad_v       = m_shaders[RendererShader::Quad_V];
        const auto& shader_texture_p    = m_shaders[RendererShader::Copy_Bilinear_P];
        if (lights.empty() || !shader_quad_v->IsCompiled() || !shader_texture_p->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_vertex                    = shader_quad_v.get();
        pso.shader_pixel                     = shader_texture_p.get();
        pso.rasterizer_state                 = m_rasterizer_cull_back_solid.get();
        pso.blend_state                      = m_blend_alpha.get();
        pso.depth_stencil_state              = m_depth_stencil_off_off.get();
        pso.vertex_buffer_stride             = m_viewport_quad.GetVertexBuffer()->GetStride(); // stride matches rect
        pso.render_target_color_textures[0]  = tex_out;
        pso.primitive_topology               = RHI_PrimitiveTopology_Mode::TriangleList;
        pso.viewport                         = tex_out->GetViewport();
        pso.pass_name                        = "Pass_Icons";

        // For each light
        for (const auto& entity : lights)
        {
            if (cmd_list->BeginRenderPass(pso))
            {
                // Light can be null if it just got removed and our buffer doesn't update till the next frame
                if (Light* light = entity->GetComponent<Light>())
                {
                    Vector3 position_light_world        = entity->GetTransform()->GetPosition();
                    Vector3 position_camera_world       = m_camera->GetTransform()->GetPosition();
                    Vector3 direction_camera_to_light   = (position_light_world - position_camera_world).Normalized();
                    const float v_dot_l                 = Vector3::Dot(m_camera->GetTransform()->GetForward(), direction_camera_to_light);

                    // Only draw if it's inside our view
                    if (v_dot_l > 0.5f)
                    {
                        // Compute light screen space position and scale (based on distance from the camera)
                        const Vector2 position_light_screen = m_camera->Project(position_light_world);
                        const float distance                = (position_camera_world - position_light_world).Length() + Helper::EPSILON;
                        float scale                         = m_gizmo_size_max / distance;
                        scale                               = Helper::Clamp(scale, m_gizmo_size_min, m_gizmo_size_max);

                        // Choose texture based on light type
                        shared_ptr<RHI_Texture> light_tex = nullptr;
                        const LightType type = light->GetLightType();
                        if (type == LightType::Directional) light_tex = m_tex_gizmo_light_directional;
                        else if (type == LightType::Point)  light_tex = m_tex_gizmo_light_point;
                        else if (type == LightType::Spot)   light_tex = m_tex_gizmo_light_spot;

                        // Construct appropriate rectangle
                        const float tex_width = light_tex->GetWidth() * scale;
                        const float tex_height = light_tex->GetHeight() * scale;
                        Math::Rectangle rectangle = Math::Rectangle
                        (
                            position_light_screen.x - tex_width * 0.5f,
                            position_light_screen.y - tex_height * 0.5f,
                            position_light_screen.x + tex_width,
                            position_light_screen.y + tex_height
                        );

                        if (rectangle != m_gizmo_light_rect)
                        {
                            m_gizmo_light_rect = rectangle;
                            m_gizmo_light_rect.CreateBuffers(this);
                        }

                        // Update uber buffer
                        m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_width), static_cast<float>(tex_width));
                        m_cb_uber_cpu.transform = m_cb_frame_cpu.view_projection_ortho;
                        Update_Cb_Uber(cmd_list);

                        cmd_list->SetTexture(RendererBindings_Srv::tex, light_tex);
                        cmd_list->SetBufferIndex(m_gizmo_light_rect.GetIndexBuffer());
                        cmd_list->SetBufferVertex(m_gizmo_light_rect.GetVertexBuffer());
                        cmd_list->DrawIndexed(Rectangle::GetIndexCount());
                    }
                }
                cmd_list->EndRenderPass();
            }
        }
    }

    void Renderer::Pass_TransformHandle(RHI_CommandList* cmd_list, RHI_Texture* tex_out)
    {
        if (!GetOption(Render_Debug_Transform))
            return;

        // Acquire resources
        RHI_Shader* shader_gizmo_transform_v    = m_shaders[RendererShader::Entity_V].get();
        RHI_Shader* shader_gizmo_transform_p    = m_shaders[RendererShader::Entity_Transform_P].get();
        if (!shader_gizmo_transform_v->IsCompiled() || !shader_gizmo_transform_p->IsCompiled())
            return;

        // Transform
        if (m_transform_handle->Tick(m_camera.get(), m_gizmo_transform_size, m_gizmo_transform_speed))
        {
            // Set render state
            static RHI_PipelineState pso;
            pso.shader_vertex                    = shader_gizmo_transform_v;
            pso.shader_pixel                     = shader_gizmo_transform_p;
            pso.rasterizer_state                 = m_rasterizer_cull_back_solid.get();
            pso.blend_state                      = m_blend_alpha.get();
            pso.depth_stencil_state              = m_depth_stencil_off_off.get();
            pso.vertex_buffer_stride             = m_transform_handle->GetVertexBuffer()->GetStride();
            pso.render_target_color_textures[0]  = tex_out;
            pso.primitive_topology               = RHI_PrimitiveTopology_Mode::TriangleList;
            pso.viewport                         = tex_out->GetViewport();

            // Axis - X
            pso.pass_name = "Pass_Handle_Axis_X";
            if (cmd_list->BeginRenderPass(pso))
            {
                m_cb_uber_cpu.transform         = m_transform_handle->GetHandle()->GetTransform(Vector3::Right);
                m_cb_uber_cpu.transform_axis    = m_transform_handle->GetHandle()->GetColor(Vector3::Right);
                Update_Cb_Uber(cmd_list);
            
                cmd_list->SetBufferIndex(m_transform_handle->GetIndexBuffer());
                cmd_list->SetBufferVertex(m_transform_handle->GetVertexBuffer());
                cmd_list->DrawIndexed(m_transform_handle->GetIndexCount());
                cmd_list->EndRenderPass();
            }
            
            // Axis - Y
            pso.pass_name = "Pass_Handle_Axis_Y";
            if (cmd_list->BeginRenderPass(pso))
            {
                m_cb_uber_cpu.transform         = m_transform_handle->GetHandle()->GetTransform(Vector3::Up);
                m_cb_uber_cpu.transform_axis    = m_transform_handle->GetHandle()->GetColor(Vector3::Up);
                Update_Cb_Uber(cmd_list);

                cmd_list->SetBufferIndex(m_transform_handle->GetIndexBuffer());
                cmd_list->SetBufferVertex(m_transform_handle->GetVertexBuffer());
                cmd_list->DrawIndexed(m_transform_handle->GetIndexCount());
                cmd_list->EndRenderPass();
            }
            
            // Axis - Z
            pso.pass_name = "Pass_Handle_Axis_Z";
            if (cmd_list->BeginRenderPass(pso))
            {
                m_cb_uber_cpu.transform         = m_transform_handle->GetHandle()->GetTransform(Vector3::Forward);
                m_cb_uber_cpu.transform_axis    = m_transform_handle->GetHandle()->GetColor(Vector3::Forward);
                Update_Cb_Uber(cmd_list);

                cmd_list->SetBufferIndex(m_transform_handle->GetIndexBuffer());
                cmd_list->SetBufferVertex(m_transform_handle->GetVertexBuffer());
                cmd_list->DrawIndexed(m_transform_handle->GetIndexCount());
                cmd_list->EndRenderPass();
            }
            
            // Axes - XYZ
            if (m_transform_handle->DrawXYZ())
            {
                pso.pass_name = "Pass_Gizmos_Axis_XYZ";
                if (cmd_list->BeginRenderPass(pso))
                {
                    m_cb_uber_cpu.transform         = m_transform_handle->GetHandle()->GetTransform(Vector3::One);
                    m_cb_uber_cpu.transform_axis    = m_transform_handle->GetHandle()->GetColor(Vector3::One);
                    Update_Cb_Uber(cmd_list);

                    cmd_list->SetBufferIndex(m_transform_handle->GetIndexBuffer());
                    cmd_list->SetBufferVertex(m_transform_handle->GetVertexBuffer());
                    cmd_list->DrawIndexed(m_transform_handle->GetIndexCount());
                    cmd_list->EndRenderPass();
                }
            }
        }
    }

    void Renderer::Pass_Outline(RHI_CommandList* cmd_list, RHI_Texture* tex_out)
    {
        if (!GetOption(Render_Debug_SelectionOutline))
            return;

        if (const Entity* entity = m_transform_handle->GetSelectedEntity())
        {
            // Get renderable
            const Renderable* renderable = entity->GetRenderable();
            if (!renderable)
                return;

            // Get material
            const Material* material = renderable->GetMaterial();
            if (!material)
                return;

            // Get geometry
            const Model* model = renderable->GeometryModel();
            if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
                return;

            // Acquire shaders
            const auto& shader_v = m_shaders[RendererShader::Entity_V];
            const auto& shader_p = m_shaders[RendererShader::Entity_Outline_P];
            if (!shader_v->IsCompiled() || !shader_p->IsCompiled())
                return;

            RHI_Texture* tex_depth  = RENDER_TARGET(RendererRt::Gbuffer_Depth).get();
            RHI_Texture* tex_normal = RENDER_TARGET(RendererRt::Gbuffer_Normal).get();

            // Set render state
            static RHI_PipelineState pso;
            pso.shader_vertex                            = shader_v.get();
            pso.shader_pixel                             = shader_p.get();
            pso.rasterizer_state                         = m_rasterizer_cull_back_solid.get();
            pso.blend_state                              = m_blend_alpha.get();
            pso.depth_stencil_state                      = m_depth_stencil_r_off.get();
            pso.vertex_buffer_stride                     = model->GetVertexBuffer()->GetStride();
            pso.render_target_color_textures[0]          = tex_out;
            pso.render_target_depth_texture              = tex_depth;
            pso.render_target_depth_texture_read_only    = true;
            pso.primitive_topology                       = RHI_PrimitiveTopology_Mode::TriangleList;
            pso.viewport                                 = tex_out->GetViewport();
            pso.pass_name                                = "Pass_Outline";

            // Record commands
            if (cmd_list->BeginRenderPass(pso))
            {
                 // Update uber buffer with entity transform
                if (Transform* transform = entity->GetTransform())
                {
                    m_cb_uber_cpu.transform     = transform->GetMatrix();
                    m_cb_uber_cpu.resolution    = Vector2(tex_out->GetWidth(), tex_out->GetHeight());
                    Update_Cb_Uber(cmd_list);
                }

                cmd_list->SetTexture(RendererBindings_Srv::gbuffer_depth, tex_depth);
                cmd_list->SetTexture(RendererBindings_Srv::gbuffer_normal, tex_normal);
                cmd_list->SetBufferVertex(model->GetVertexBuffer());
                cmd_list->SetBufferIndex(model->GetIndexBuffer());
                cmd_list->DrawIndexed(renderable->GeometryIndexCount(), renderable->GeometryIndexOffset(), renderable->GeometryVertexOffset());
                cmd_list->EndRenderPass();
            }
        }
    }

    void Renderer::Pass_Text(RHI_CommandList* cmd_list, RHI_Texture* tex_out)
    {
        // Early exit cases
        const bool draw         = m_options & Render_Debug_PerformanceMetrics;
        const bool empty        = m_profiler->GetMetrics().empty();
        const auto& shader_v    = m_shaders[RendererShader::Font_V];
        const auto& shader_p    = m_shaders[RendererShader::Font_P];
        if (!draw || empty || !shader_v->IsCompiled() || !shader_p->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_vertex                    = shader_v.get();
        pso.shader_pixel                     = shader_p.get();
        pso.rasterizer_state                 = m_rasterizer_cull_back_solid.get();
        pso.blend_state                      = m_blend_alpha.get();
        pso.depth_stencil_state              = m_depth_stencil_off_off.get();
        pso.vertex_buffer_stride             = m_font->GetVertexBuffer()->GetStride();
        pso.render_target_color_textures[0]  = tex_out;
        pso.primitive_topology               = RHI_PrimitiveTopology_Mode::TriangleList;
        pso.viewport                         = tex_out->GetViewport();
        pso.pass_name                        = "Pass_Text";

        // Update text
        const Vector2 text_pos = Vector2(-m_viewport.width * 0.5f + 5.0f, m_viewport.height * 0.5f - m_font->GetSize() - 2.0f);
        m_font->SetText(m_profiler->GetMetrics(), text_pos);

        // Draw outline
        if (m_font->GetOutline() != Font_Outline_None && m_font->GetOutlineSize() != 0)
        { 
            if (cmd_list->BeginRenderPass(pso))
            {
                // Update uber buffer
                m_cb_uber_cpu.resolution    = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
                m_cb_uber_cpu.color         = m_font->GetColorOutline();
                Update_Cb_Uber(cmd_list);

                cmd_list->SetBufferIndex(m_font->GetIndexBuffer());
                cmd_list->SetBufferVertex(m_font->GetVertexBuffer());
                cmd_list->SetTexture(RendererBindings_Srv::font_atlas, m_font->GetAtlasOutline());
                cmd_list->DrawIndexed(m_font->GetIndexCount());
                cmd_list->EndRenderPass();
            }
        }

        // Draw 
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution    = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            m_cb_uber_cpu.color         = m_font->GetColor();
            Update_Cb_Uber(cmd_list);

            cmd_list->SetBufferIndex(m_font->GetIndexBuffer());
            cmd_list->SetBufferVertex(m_font->GetVertexBuffer());
            cmd_list->SetTexture(RendererBindings_Srv::font_atlas, m_font->GetAtlas());
            cmd_list->DrawIndexed(m_font->GetIndexCount());
            cmd_list->EndRenderPass();
        }
    }

    bool Renderer::Pass_DebugBuffer(RHI_CommandList* cmd_list, RHI_Texture* tex_out)
    {
        if (m_render_target_debug == RendererRt::Undefined)
            return true;

        // Bind correct texture & shader pass
        RHI_Texture* texture          = RENDER_TARGET(m_render_target_debug).get();
        RendererShader shader_type    = RendererShader::Copy_Point_C;

        if (m_render_target_debug == RendererRt::Gbuffer_Albedo)
        {
            shader_type = RendererShader::DebugChannelRgbGammaCorrect_C;
        }

        if (m_render_target_debug == RendererRt::Gbuffer_Normal)
        {
            shader_type = RendererShader::DebugNormal_C;
        }

        if (m_render_target_debug == RendererRt::Light_Diffuse || m_render_target_debug == RendererRt::Light_Diffuse_Transparent)
        {
            shader_type = RendererShader::DebugChannelRgbGammaCorrect_C;
        }

        if (m_render_target_debug == RendererRt::Light_Specular || m_render_target_debug == RendererRt::Light_Specular_Transparent)
        {
            shader_type = RendererShader::DebugChannelRgbGammaCorrect_C;
        }

        if (m_render_target_debug == RendererRt::Gbuffer_Velocity)
        {
            shader_type = RendererShader::DebugVelocity_C;
        }

        if (m_render_target_debug == RendererRt::Gbuffer_Depth)
        {
            shader_type = RendererShader::DebugChannelR_C;
        }

        if (m_render_target_debug == RendererRt::Ssao)
        {
            texture = m_options & Render_Ssao ? RENDER_TARGET(RendererRt::Ssao).get() : m_tex_default_white.get();
            bool do_gi = GetOptionValue<bool>(Renderer_Option_Value::Ssao_Gi);
            shader_type = do_gi ? RendererShader::DebugChannelRgbGammaCorrect_C : RendererShader::DebugChannelR_C;
        }

        if (m_render_target_debug == RendererRt::Ssao_Blurred)
        {
            texture = m_options & Render_Ssao ? RENDER_TARGET(RendererRt::Ssao_Blurred).get() : m_tex_default_white.get();
            bool do_gi = GetOptionValue<bool>(Renderer_Option_Value::Ssao_Gi);
            shader_type = do_gi ? RendererShader::DebugChannelRgbGammaCorrect_C : RendererShader::DebugChannelR_C;
        }

        if (m_render_target_debug == RendererRt::Ssr)
        {
            shader_type = RendererShader::DebugChannelRgbGammaCorrect_C;
        }

        if (m_render_target_debug == RendererRt::Dof_Half)
        {
            texture = RENDER_TARGET(RendererRt::Dof_Half).get();
            shader_type = RendererShader::DebugChannelRgbGammaCorrect_C;
        }

        if (m_render_target_debug == RendererRt::Dof_Half_2)
        {
            texture = RENDER_TARGET(RendererRt::Dof_Half_2).get();
            shader_type = RendererShader::DebugChannelRgbGammaCorrect_C;
        }

        if (m_render_target_debug == RendererRt::Light_Volumetric)
        {
            shader_type = RendererShader::DebugChannelRgbGammaCorrect_C;
        }

        if (m_render_target_debug == RendererRt::Bloom)
        {
            shader_type = RendererShader::DebugChannelRgbGammaCorrect_C;
        }

        // Acquire shaders
        RHI_Shader* shader = m_shaders[shader_type].get();
        if (!shader->IsCompiled())
            return false;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute   = shader;
        pso.pass_name        = "Pass_DebugBuffer";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution    = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            m_cb_uber_cpu.transform     = m_cb_frame_cpu.view_projection_ortho;
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z = 1;
            const bool async = false;

            cmd_list->SetTexture(RendererBindings_Uav::rgba, tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::tex, texture);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }

        return true;
    }

    void Renderer::Pass_BrdfSpecularLut(RHI_CommandList* cmd_list)
    {
        if (m_brdf_specular_lut_rendered)
            return;

        // Acquire shaders
        RHI_Shader* shader = m_shaders[RendererShader::BrdfSpecularLut_C].get();
        if (!shader->IsCompiled())
            return;

        // Acquire render target
        RHI_Texture* render_target = RENDER_TARGET(RendererRt::Brdf_Specular_Lut).get();

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute   = shader;
        pso.pass_name        = "Pass_BrdfSpecularLut";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(render_target->GetWidth()), static_cast<float>(render_target->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(render_target->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(render_target->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z = 1;
            const bool async = false;

            cmd_list->SetTexture(RendererBindings_Uav::rg, render_target);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();

            m_brdf_specular_lut_rendered = true;
        }
    }

    void Renderer::Pass_CopyBilinear(RHI_CommandList* cmd_list, RHI_Texture* tex_in, RHI_Texture* tex_out)
    {
        // Acquire shaders
        RHI_Shader* shader_c = m_shaders[RendererShader::Copy_Bilinear_C].get();
        if (!shader_c->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso;
        pso.shader_compute  = shader_c;
        pso.pass_name       = "Pass_CopyBilinear";

        // Draw
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(tex_out->GetWidth()), static_cast<float>(tex_out->GetHeight()));
            Update_Cb_Uber(cmd_list);

            const uint32_t thread_group_count_x = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetWidth()) / m_thread_group_count));
            const uint32_t thread_group_count_y = static_cast<uint32_t>(Math::Helper::Ceil(static_cast<float>(tex_out->GetHeight()) / m_thread_group_count));
            const uint32_t thread_group_count_z = 1;
            const bool async = false;

            cmd_list->SetTexture(RendererBindings_Uav::rgb, tex_out);
            cmd_list->SetTexture(RendererBindings_Srv::tex, tex_in);
            cmd_list->Dispatch(thread_group_count_x, thread_group_count_y, thread_group_count_z, async);
            cmd_list->EndRenderPass();
        }
    }

    void Renderer::Pass_CopyToBackbuffer(RHI_CommandList* cmd_list)
    {
        // Acquire shaders
        RHI_Shader* shader_v = m_shaders[RendererShader::Quad_V].get();
        RHI_Shader* shader_p = m_shaders[RendererShader::Copy_Point_P].get();
        if (!shader_v->IsCompiled() || !shader_p->IsCompiled())
            return;

        // Set render state
        static RHI_PipelineState pso = {};
        pso.shader_vertex            = shader_v;
        pso.shader_pixel             = shader_p;
        pso.rasterizer_state         = m_rasterizer_cull_back_solid.get();
        pso.blend_state              = m_blend_disabled.get();
        pso.depth_stencil_state      = m_depth_stencil_off_off.get();
        pso.vertex_buffer_stride     = m_viewport_quad.GetVertexBuffer()->GetStride();
        pso.render_target_swapchain  = m_swap_chain.get();
        pso.clear_color[0]           = rhi_color_dont_care;
        pso.primitive_topology       = RHI_PrimitiveTopology_Mode::TriangleList;
        pso.viewport                 = m_viewport;
        pso.pass_name                = "Pass_CopyToBackbuffer";

        // Record commands
        if (cmd_list->BeginRenderPass(pso))
        {
            // Update uber buffer
            m_cb_uber_cpu.resolution = Vector2(static_cast<float>(m_swap_chain->GetWidth()), static_cast<float>(m_swap_chain->GetHeight()));
            Update_Cb_Uber(cmd_list);

            cmd_list->SetTexture(RendererBindings_Srv::tex, RENDER_TARGET(RendererRt::Frame_Output).get());
            cmd_list->SetBufferVertex(m_viewport_quad.GetVertexBuffer());
            cmd_list->SetBufferIndex(m_viewport_quad.GetIndexBuffer());
            cmd_list->DrawIndexed(m_viewport_quad.GetIndexCount());
            cmd_list->EndRenderPass();
        }
    }
}
