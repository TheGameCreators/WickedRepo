#include "RenderPath3D.h"
#include "wiRenderer.h"
#include "wiImage.h"
#include "wiHelper.h"
#include "wiTextureHelper.h"
#include "shaders/ResourceMapping.h"
#include "wiProfiler.h"

#ifdef GGREDUCED
#define DELAYEDSHADOWS
bool ImGuiHook_GetScissorArea(float* pX1, float* pY1, float* pX2, float* pY2);
extern bool g_bNoTerrainRender;
//const float maxApparentSize = 0.000015f; // make this a global performance variable, higher to cull objects more aggressively, 0 to draw everything
extern float maxApparentSize; // = 0.000002f;

wiSpinLock bindresourcesLock;

#define REMOVE_WICKED_PARTICLE
#define REMOVE_WATER_RIPPLE
#define REMOVE_TEMPORAL_AA
#define REMOVE_RAY_TRACED_SHADOW
#endif

#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
#include "optick.h"
#endif
#endif

using namespace wiGraphics;

#ifdef GGREDUCED

namespace GGTerrain {

	extern "C" void GGTerrain_Draw( const Frustum* frustum, int mode, wiGraphics::CommandList cmd );
	extern "C" void __GGTerrain_Draw_EMPTY( const Frustum* frustum, int mode, wiGraphics::CommandList cmd ) {}
	// use GGTerrain_Draw() if it is defined, otherwise use __GGTerrain_Draw_EMPTY()
	#pragma comment(linker, "/alternatename:GGTerrain_Draw=__GGTerrain_Draw_EMPTY")

	extern "C" void GGTerrain_Draw_Prepass( const Frustum* frustum, wiGraphics::CommandList cmd );
	extern "C" void __GGTerrain_Draw_Prepass_EMPTY( const Frustum* frustum, wiGraphics::CommandList cmd ) {}
	#pragma comment(linker, "/alternatename:GGTerrain_Draw_Prepass=__GGTerrain_Draw_Prepass_EMPTY")

	extern "C" void GGTerrain_Draw_Transparent( const Frustum* frustum, wiGraphics::CommandList cmd );
	extern "C" void __GGTerrain_Draw_Transparent_EMPTY( const Frustum* frustum, wiGraphics::CommandList cmd ) {}
	#pragma comment(linker, "/alternatename:GGTerrain_Draw_Transparent=__GGTerrain_Draw_Transparent_EMPTY")

	extern "C" void GGTerrain_Draw_Prepass_Reflections( const Frustum* frustum, wiGraphics::CommandList cmd );
	extern "C" void __GGTerrain_Draw_Prepass_Reflections_EMPTY( const Frustum* frustum, wiGraphics::CommandList cmd ) {}
	#pragma comment(linker, "/alternatename:GGTerrain_Draw_Prepass_Reflections=__GGTerrain_Draw_Prepass_Reflections_EMPTY")

	extern "C" void GGTerrain_VirtualTexReadBack( Texture tex, uint32_t sampleCount, wiGraphics::CommandList cmd );
	extern "C" void __GGTerrain_VirtualTexReadBack_EMPTY( Frustum* frustum, wiGraphics::CommandList cmd ) {}
	#pragma comment(linker, "/alternatename:GGTerrain_VirtualTexReadBack=__GGTerrain_VirtualTexReadBack_EMPTY")
}

namespace GGTrees {
	extern "C" void GGTrees_Draw( const Frustum* frustum, int mode, wiGraphics::CommandList cmd );
	extern "C" void __GGTrees_Draw_EMPTY( const Frustum* frustum, int mode, wiGraphics::CommandList cmd ) {}
	#pragma comment(linker, "/alternatename:GGTrees_Draw=__GGTrees_Draw_EMPTY")

	extern "C" void GGTrees_Draw_Prepass( const Frustum* frustum, int mode, wiGraphics::CommandList cmd );
	extern "C" void __GGTrees_Draw_Prepass_EMPTY( const Frustum* frustum, int mode, wiGraphics::CommandList cmd ) {}
	#pragma comment(linker, "/alternatename:GGTrees_Draw_Prepass=__GGTrees_Draw_Prepass_EMPTY")
}

namespace GGGrass {
	extern "C" void GGGrass_Draw( const Frustum* frustum, int mode, wiGraphics::CommandList cmd );
	extern "C" void __GGGrass_Draw_EMPTY( const Frustum* frustum, int mode, wiGraphics::CommandList cmd ) {}
	#pragma comment(linker, "/alternatename:GGGrass_Draw=__GGGrass_Draw_EMPTY")

	extern "C" void GGGrass_Draw_Prepass( const Frustum* frustum, int mode, wiGraphics::CommandList cmd );
	extern "C" void __GGGrass_Draw_Prepass_EMPTY( const Frustum* frustum, int mode, wiGraphics::CommandList cmd ) {}
	#pragma comment(linker, "/alternatename:GGGrass_Draw_Prepass=__GGGrass_Draw_Prepass_EMPTY")
}

namespace GPUParticles
{
	extern "C" void gpup_draw(const wiScene::CameraComponent & camera, wiGraphics::CommandList cmd);
	extern "C" void gpup_draw_init(const wiScene::CameraComponent & camera, wiGraphics::CommandList cmd);
	extern "C" void gpup_draw_bydistance(const wiScene::CameraComponent & camera, wiGraphics::CommandList cmd, float fDistanceFromCamera);
	extern "C" void __gpup_draw_EMPTY( const wiScene::CameraComponent& camera, wiGraphics::CommandList cmd ) {}
	// use gpup_draw() if it is defined, otherwise use __gpup_draw_EMPTY()
	#pragma comment(linker, "/alternatename:gpup_draw=__gpup_draw_EMPTY")
}
#endif

void RenderPath3D::ResizeBuffers()
{
	GraphicsDevice* device = wiRenderer::GetDevice();

	XMUINT2 internalResolution;
	internalResolution.x = GetWidth3D();
	internalResolution.y = GetHeight3D();

	camera->CreatePerspective((float)internalResolution.x, (float)internalResolution.y, camera->zNearP, camera->zFarP);
	
	// Render targets:

	{
		TextureDesc desc;
		//PE: BIND_UNORDERED_ACCESS do not work here if getMSAASampleCount > 1
		//PE: Only way to fix HDR postprossing is to create a rtPostprocess_HDR[1] we can use instead (for ping/pong).

		desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
		desc.Width = internalResolution.x;
		desc.Height = internalResolution.y;
		desc.SampleCount = getMSAASampleCount();

		desc.Format = FORMAT_R11G11B10_FLOAT;
		device->CreateTexture(&desc, nullptr, &rtGbuffer[GBUFFER_COLOR]);
		device->SetName(&rtGbuffer[GBUFFER_COLOR], "rtGbuffer[GBUFFER_COLOR]");

		desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
		desc.Format = FORMAT_R8G8B8A8_UNORM;
		device->CreateTexture(&desc, nullptr, &rtGbuffer[GBUFFER_NORMAL_ROUGHNESS]);
		device->SetName(&rtGbuffer[GBUFFER_NORMAL_ROUGHNESS], "rtGbuffer[GBUFFER_NORMAL_ROUGHNESS]");

		desc.Format = FORMAT_R16G16_FLOAT;
		device->CreateTexture(&desc, nullptr, &rtGbuffer[GBUFFER_VELOCITY]);
		device->SetName(&rtGbuffer[GBUFFER_VELOCITY], "rtGbuffer[GBUFFER_VELOCITY]");

#ifdef GGREDUCED
		desc.Format = FORMAT_R32_UINT;
		device->CreateTexture(&desc, nullptr, &rtVirtualTextureReadBack);
		device->SetName(&rtVirtualTextureReadBack, "rtVirtualTextureReadBack");
#endif

		if (getMSAASampleCount() > 1)
		{
			desc.SampleCount = 1;
			desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;

			desc.Format = FORMAT_R11G11B10_FLOAT;
			device->CreateTexture(&desc, nullptr, &rtGbuffer_resolved[GBUFFER_COLOR]);
			device->SetName(&rtGbuffer_resolved[GBUFFER_COLOR], "rtGbuffer_resolved[GBUFFER_COLOR]");

			desc.Format = FORMAT_R8G8B8A8_UNORM;
			device->CreateTexture(&desc, nullptr, &rtGbuffer_resolved[GBUFFER_NORMAL_ROUGHNESS]);
			device->SetName(&rtGbuffer_resolved[GBUFFER_NORMAL_ROUGHNESS], "rtGbuffer_resolved[GBUFFER_NORMAL_ROUGHNESS]");

			desc.Format = FORMAT_R16G16_FLOAT;
			device->CreateTexture(&desc, nullptr, &rtGbuffer_resolved[GBUFFER_VELOCITY]);
			device->SetName(&rtGbuffer_resolved[GBUFFER_VELOCITY], "rtGbuffer_resolved[GBUFFER_VELOCITY]");

		}
	}
#ifndef REMOVE_WICKED_PARTICLE
	{
		TextureDesc desc;
		desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
		desc.Format = FORMAT_R16G16B16A16_FLOAT;
		desc.Width = internalResolution.x;
		desc.Height = internalResolution.y;
		desc.SampleCount = getMSAASampleCount();
		device->CreateTexture(&desc, nullptr, &rtParticleDistortion);
		device->SetName(&rtParticleDistortion, "rtParticleDistortion");
		if (getMSAASampleCount() > 1)
		{
			desc.SampleCount = 1;
			device->CreateTexture(&desc, nullptr, &rtParticleDistortion_Resolved);
			device->SetName(&rtParticleDistortion_Resolved, "rtParticleDistortion_Resolved");
		}
	}
#endif

	{
		TextureDesc desc;
		desc.Format = FORMAT_R16G16B16A16_FLOAT;
		desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.Width = internalResolution.x / 4;
		desc.Height = internalResolution.y / 4;
		device->CreateTexture(&desc, nullptr, &rtVolumetricLights[0]);
		device->SetName(&rtVolumetricLights[0], "rtVolumetricLights[0]");
		device->CreateTexture(&desc, nullptr, &rtVolumetricLights[1]);
		device->SetName(&rtVolumetricLights[1], "rtVolumetricLights[1]");
	}
#ifndef REMOVE_WATER_RIPPLE
	{
		TextureDesc desc;
		desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
		desc.Format = FORMAT_R8G8_SNORM;
		desc.Width = internalResolution.x;
		desc.Height = internalResolution.y;
		device->CreateTexture(&desc, nullptr, &rtWaterRipple);
		device->SetName(&rtWaterRipple, "rtWaterRipple");
	}
#endif
	{
		TextureDesc desc;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS | BIND_RENDER_TARGET;
		desc.Format = FORMAT_R11G11B10_FLOAT;
		desc.Width = internalResolution.x / 2;
		desc.Height = internalResolution.y / 2;
		desc.MipLevels = std::min(8u, (uint32_t)std::log2(std::max(desc.Width, desc.Height)));
		device->CreateTexture(&desc, nullptr, &rtSceneCopy);
		device->SetName(&rtSceneCopy, "rtSceneCopy");
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		device->CreateTexture(&desc, nullptr, &rtSceneCopy_tmp);
		device->SetName(&rtSceneCopy_tmp, "rtSceneCopy_tmp");

		for (uint32_t i = 0; i < rtSceneCopy.GetDesc().MipLevels; ++i)
		{
			int subresource_index;
			subresource_index = device->CreateSubresource(&rtSceneCopy, SRV, 0, 1, i, 1);
			assert(subresource_index == i);
			subresource_index = device->CreateSubresource(&rtSceneCopy_tmp, SRV, 0, 1, i, 1);
			assert(subresource_index == i);
			subresource_index = device->CreateSubresource(&rtSceneCopy, UAV, 0, 1, i, 1);
			assert(subresource_index == i);
			subresource_index = device->CreateSubresource(&rtSceneCopy_tmp, UAV, 0, 1, i, 1);
			assert(subresource_index == i);
		}
	}
	{
		TextureDesc desc;
		desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
		desc.Format = FORMAT_R11G11B10_FLOAT;
		desc.Width = internalResolution.x / 2;
		desc.Height = internalResolution.y / 2;
		device->CreateTexture(&desc, nullptr, &rtReflection);
		device->SetName(&rtReflection, "rtReflection");
	}
#ifndef REMOVE_RAY_TRACED_SHADOW
	{
		TextureDesc desc;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.Format = FORMAT_R32G32B32A32_UINT;
		desc.Width = internalResolution.x / 2;
		desc.Height = internalResolution.y / 2;
		desc.layout = IMAGE_LAYOUT_SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&desc, nullptr, &rtShadow);
		device->SetName(&rtShadow, "rtShadow");
	}
#endif
	{
		TextureDesc desc;
		desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
		desc.Format = FORMAT_R11G11B10_FLOAT;
		desc.Width = internalResolution.x;
		desc.Height = internalResolution.y;
		desc.SampleCount = getMSAASampleCount();
		device->CreateTexture(&desc, nullptr, &rtSun[0]);
		device->SetName(&rtSun[0], "rtSun[0]");

		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.SampleCount = 1;
		desc.Width = internalResolution.x / 2;
		desc.Height = internalResolution.y / 2;
		device->CreateTexture(&desc, nullptr, &rtSun[1]);
		device->SetName(&rtSun[1], "rtSun[1]");

		if (getMSAASampleCount() > 1)
		{
			desc.Width = internalResolution.x;
			desc.Height = internalResolution.y;
			desc.SampleCount = 1;
			device->CreateTexture(&desc, nullptr, &rtSun_resolved);
			device->SetName(&rtSun_resolved, "rtSun_resolved");
		}
	}
#ifndef REMOVE_TEMPORAL_AA
	{
		TextureDesc desc;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.Format = FORMAT_R11G11B10_FLOAT;
		desc.Width = internalResolution.x;
		desc.Height = internalResolution.y;
		device->CreateTexture(&desc, nullptr, &rtTemporalAA[0]);
		device->SetName(&rtTemporalAA[0], "rtTemporalAA[0]");
		device->CreateTexture(&desc, nullptr, &rtTemporalAA[1]);
		device->SetName(&rtTemporalAA[1], "rtTemporalAA[1]");
	}
#endif
	{
		TextureDesc desc;
		desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.Format = FORMAT_R11G11B10_FLOAT;
		desc.Width = internalResolution.x;
		desc.Height = internalResolution.y;
		device->CreateTexture(&desc, nullptr, &rtPostprocess_HDR[0]);
		device->SetName(&rtPostprocess_HDR[0], "rtPostprocess_HDR[0]");
		device->CreateTexture(&desc, nullptr, &rtPostprocess_HDR[1]);
		device->SetName(&rtPostprocess_HDR[1], "rtPostprocess_HDR[1]");
	}
	{
		TextureDesc desc;
		desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.Format = FORMAT_R10G10B10A2_UNORM;
		desc.Width = internalResolution.x;
		desc.Height = internalResolution.y;
		device->CreateTexture(&desc, nullptr, &rtPostprocess_LDR[0]);
		device->SetName(&rtPostprocess_LDR[0], "rtPostprocess_LDR[0]");
		device->CreateTexture(&desc, nullptr, &rtPostprocess_LDR[1]);
		device->SetName(&rtPostprocess_LDR[1], "rtPostprocess_LDR[1]");

		desc.Width /= 4;
		desc.Height /= 4;
		desc.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
		device->CreateTexture(&desc, nullptr, &rtGUIBlurredBackground[0]);
		device->SetName(&rtGUIBlurredBackground[0], "rtGUIBlurredBackground[0]");

		desc.Width /= 4;
		desc.Height /= 4;
		device->CreateTexture(&desc, nullptr, &rtGUIBlurredBackground[1]);
		device->SetName(&rtGUIBlurredBackground[1], "rtGUIBlurredBackground[1]");
		device->CreateTexture(&desc, nullptr, &rtGUIBlurredBackground[2]);
		device->SetName(&rtGUIBlurredBackground[2], "rtGUIBlurredBackground[2]");
	}

	if(device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_VARIABLE_RATE_SHADING_TIER2))
	{
		uint32_t tileSize = device->GetVariableRateShadingTileSize();

		TextureDesc desc;
		desc.layout = IMAGE_LAYOUT_UNORDERED_ACCESS;
		desc.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADING_RATE;
		desc.Format = FORMAT_R8_UINT;
		desc.Width = (internalResolution.x + tileSize - 1) / tileSize;
		desc.Height = (internalResolution.y + tileSize - 1) / tileSize;

		device->CreateTexture(&desc, nullptr, &rtShadingRate);
		device->SetName(&rtShadingRate, "rtShadingRate");
	}

	// Depth buffers:
	{
		TextureDesc desc;
		desc.Width = internalResolution.x;
		desc.Height = internalResolution.y;

		desc.SampleCount = getMSAASampleCount();
		desc.layout = IMAGE_LAYOUT_DEPTHSTENCIL_READONLY;
		desc.Format = FORMAT_R32G8X24_TYPELESS;
		desc.BindFlags = BIND_DEPTH_STENCIL | BIND_SHADER_RESOURCE;
		device->CreateTexture(&desc, nullptr, &depthBuffer_Main);
		device->SetName(&depthBuffer_Main, "depthBuffer_Main");

		desc.layout = IMAGE_LAYOUT_SHADER_RESOURCE_COMPUTE;
		desc.Format = FORMAT_R32_FLOAT;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.SampleCount = 1;
		desc.MipLevels = 3;
		device->CreateTexture(&desc, nullptr, &depthBuffer_Copy);
		device->SetName(&depthBuffer_Copy, "depthBuffer_Copy");
		device->CreateTexture(&desc, nullptr, &depthBuffer_Copy1);
		device->SetName(&depthBuffer_Copy1, "depthBuffer_Copy1");

		for (uint32_t i = 0; i < depthBuffer_Copy.desc.MipLevels; ++i)
		{
			int subresource = 0;
			subresource = device->CreateSubresource(&depthBuffer_Copy, SRV, 0, 1, i, 1);
			assert(subresource == i);
			subresource = device->CreateSubresource(&depthBuffer_Copy, UAV, 0, 1, i, 1);
			assert(subresource == i);
			subresource = device->CreateSubresource(&depthBuffer_Copy1, SRV, 0, 1, i, 1);
			assert(subresource == i);
			subresource = device->CreateSubresource(&depthBuffer_Copy1, UAV, 0, 1, i, 1);
			assert(subresource == i);
		}
	}
	{
		TextureDesc desc;
		desc.BindFlags = BIND_DEPTH_STENCIL | BIND_SHADER_RESOURCE;
		desc.Format = FORMAT_R32_TYPELESS;
		desc.Width = internalResolution.x / 2;
		desc.Height = internalResolution.y / 2;
		desc.layout = IMAGE_LAYOUT_DEPTHSTENCIL_READONLY;
		device->CreateTexture(&desc, nullptr, &depthBuffer_Reflection);
		device->SetName(&depthBuffer_Reflection, "depthBuffer_Reflection");
	}
	{
		TextureDesc desc;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.Format = FORMAT_R32_FLOAT;
		desc.Width = internalResolution.x;
		desc.Height = internalResolution.y;
		desc.MipLevels = 6;
		desc.layout = IMAGE_LAYOUT_SHADER_RESOURCE_COMPUTE;
		device->CreateTexture(&desc, nullptr, &rtLinearDepth);
		device->SetName(&rtLinearDepth, "rtLinearDepth");

		for (uint32_t i = 0; i < desc.MipLevels; ++i)
		{
			int subresource_index;
			subresource_index = device->CreateSubresource(&rtLinearDepth, SRV, 0, 1, i, 1);
			assert(subresource_index == i);
			subresource_index = device->CreateSubresource(&rtLinearDepth, UAV, 0, 1, i, 1);
			assert(subresource_index == i);
		}
	}

	// Render passes:
	{
		RenderPassDesc desc;
		desc.attachments.push_back(
			RenderPassAttachment::DepthStencil(
				&depthBuffer_Main,
				RenderPassAttachment::LOADOP_CLEAR,
				RenderPassAttachment::STOREOP_STORE,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
				IMAGE_LAYOUT_DEPTHSTENCIL,
				IMAGE_LAYOUT_SHADER_RESOURCE
			)
		);
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtGbuffer[GBUFFER_VELOCITY], RenderPassAttachment::LOADOP_DONTCARE));
		if (getMSAASampleCount() > 1)
		{
			desc.attachments.push_back(RenderPassAttachment::Resolve(GetGbuffer_Read(GBUFFER_VELOCITY)));
		}
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtVirtualTextureReadBack, RenderPassAttachment::LOADOP_CLEAR));
		device->CreateRenderPass(&desc, &renderpass_depthprepass);

		desc.attachments.clear();
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtGbuffer[GBUFFER_COLOR], RenderPassAttachment::LOADOP_DONTCARE));
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtGbuffer[GBUFFER_NORMAL_ROUGHNESS], RenderPassAttachment::LOADOP_DONTCARE));
		desc.attachments.push_back(
			RenderPassAttachment::DepthStencil(
				&depthBuffer_Main,
				RenderPassAttachment::LOADOP_LOAD,
				RenderPassAttachment::STOREOP_STORE,
				IMAGE_LAYOUT_SHADER_RESOURCE,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY
			)
		);
		if (getMSAASampleCount() > 1)
		{
			desc.attachments.push_back(RenderPassAttachment::Resolve(GetGbuffer_Read(GBUFFER_COLOR)));
			desc.attachments.push_back(RenderPassAttachment::Resolve(GetGbuffer_Read(GBUFFER_NORMAL_ROUGHNESS)));
		}

		if (device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_VARIABLE_RATE_SHADING_TIER2))
		{
			desc.attachments.push_back(RenderPassAttachment::ShadingRateSource(&rtShadingRate, IMAGE_LAYOUT_UNORDERED_ACCESS, IMAGE_LAYOUT_UNORDERED_ACCESS));
		}

		device->CreateRenderPass(&desc, &renderpass_main);
	}
	{
		RenderPassDesc desc;
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtGbuffer[GBUFFER_COLOR], RenderPassAttachment::LOADOP_LOAD));
		desc.attachments.push_back(
			RenderPassAttachment::DepthStencil(
				&depthBuffer_Main,
				RenderPassAttachment::LOADOP_LOAD,
				RenderPassAttachment::STOREOP_STORE,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
				IMAGE_LAYOUT_DEPTHSTENCIL,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY
			)
		);
		if (getMSAASampleCount() > 1)
		{
			desc.attachments.push_back(RenderPassAttachment::Resolve(&rtGbuffer_resolved[GBUFFER_COLOR]));
		}
		device->CreateRenderPass(&desc, &renderpass_transparent);
	}
	{
		RenderPassDesc desc;
		desc.attachments.push_back(
			RenderPassAttachment::DepthStencil(
				&depthBuffer_Reflection,
				RenderPassAttachment::LOADOP_CLEAR,
				RenderPassAttachment::STOREOP_STORE,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
				IMAGE_LAYOUT_DEPTHSTENCIL,
				IMAGE_LAYOUT_SHADER_RESOURCE
			)
		);

		device->CreateRenderPass(&desc, &renderpass_reflection_depthprepass);
	}
	{
		RenderPassDesc desc;
		desc.attachments.push_back(
			RenderPassAttachment::RenderTarget(
				&rtReflection,
				RenderPassAttachment::LOADOP_DONTCARE,
				RenderPassAttachment::STOREOP_STORE,
				IMAGE_LAYOUT_SHADER_RESOURCE,
				IMAGE_LAYOUT_RENDERTARGET,
				IMAGE_LAYOUT_SHADER_RESOURCE
			)
		);
		desc.attachments.push_back(
			RenderPassAttachment::DepthStencil(
				&depthBuffer_Reflection, 
				RenderPassAttachment::LOADOP_LOAD, 
				RenderPassAttachment::STOREOP_DONTCARE,
				IMAGE_LAYOUT_SHADER_RESOURCE,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY
			)
		);

		device->CreateRenderPass(&desc, &renderpass_reflection);
	}
	{
		RenderPassDesc desc;
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtSceneCopy, RenderPassAttachment::LOADOP_DONTCARE));

		device->CreateRenderPass(&desc, &renderpass_downsamplescene);
	}
	{
		RenderPassDesc desc;
		desc.attachments.push_back(
			RenderPassAttachment::DepthStencil(
				&depthBuffer_Main,
				RenderPassAttachment::LOADOP_LOAD,
				RenderPassAttachment::STOREOP_STORE,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY
			)
		);
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtSun[0], RenderPassAttachment::LOADOP_CLEAR));
		if (getMSAASampleCount() > 1)
		{
			desc.attachments.back().storeop = RenderPassAttachment::STOREOP_DONTCARE;
			desc.attachments.push_back(RenderPassAttachment::Resolve(&rtSun_resolved));
		}

		device->CreateRenderPass(&desc, &renderpass_lightshafts);
	}
	{
		RenderPassDesc desc;
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtVolumetricLights[0], RenderPassAttachment::LOADOP_CLEAR));

		device->CreateRenderPass(&desc, &renderpass_volumetriclight);
	}
#ifndef REMOVE_WICKED_PARTICLE
	{
		RenderPassDesc desc;
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtParticleDistortion, RenderPassAttachment::LOADOP_CLEAR));
		desc.attachments.push_back(
			RenderPassAttachment::DepthStencil(
				&depthBuffer_Main,
				RenderPassAttachment::LOADOP_LOAD,
				RenderPassAttachment::STOREOP_STORE,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY
			)
		);

		if (getMSAASampleCount() > 1)
		{
			desc.attachments.push_back(RenderPassAttachment::Resolve(&rtParticleDistortion_Resolved));
		}

		device->CreateRenderPass(&desc, &renderpass_particledistortion);
	}
#endif

#ifndef REMOVE_WATER_RIPPLE
	{
		RenderPassDesc desc;
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtWaterRipple, RenderPassAttachment::LOADOP_CLEAR));

		device->CreateRenderPass(&desc, &renderpass_waterripples);
	}
#endif

	// Other resources:
	{
		TextureDesc desc;
		desc.Width = internalResolution.x;
		desc.Height = internalResolution.y;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = FORMAT_R8G8B8A8_UNORM;
		desc.SampleCount = 1;
		desc.Usage = USAGE_DEFAULT;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;

		device->CreateTexture(&desc, nullptr, &debugUAV);
		device->SetName(&debugUAV, "debugUAV");
	}
	wiRenderer::CreateTiledLightResources(tiledLightResources, internalResolution);
	wiRenderer::CreateTiledLightResources(tiledLightResources_planarReflection, internalResolution);
	wiRenderer::CreateLuminanceResources(luminanceResources, internalResolution);
	wiRenderer::CreateScreenSpaceShadowResources(screenspaceshadowResources, internalResolution);
	wiRenderer::CreateDepthOfFieldResources(depthoffieldResources, internalResolution);
	wiRenderer::CreateMotionBlurResources(motionblurResources, internalResolution);
	wiRenderer::CreateVolumetricCloudResources(volumetriccloudResources[0], internalResolution);
	wiRenderer::CreateVolumetricCloudResources(volumetriccloudResources[1], internalResolution);
	wiRenderer::CreateVolumetricCloudResources(volumetriccloudResources_reflection[0], XMUINT2(depthBuffer_Reflection.desc.Width, depthBuffer_Reflection.desc.Height));
	wiRenderer::CreateVolumetricCloudResources(volumetriccloudResources_reflection[1], XMUINT2(depthBuffer_Reflection.desc.Width, depthBuffer_Reflection.desc.Height));
	wiRenderer::CreateBloomResources(bloomResources, internalResolution);

#ifndef REMOVE_RAY_TRACED_SHADOW
	if (device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_INLINE))
	{
		wiRenderer::CreateRTShadowResources(rtshadowResources, internalResolution);
	}
#endif

	setAO(ao);
	setSSREnabled(ssrEnabled);
	setRaytracedReflectionsEnabled(raytracedReflectionsEnabled);
	setFSREnabled(fsrEnabled);

	RenderPath2D::ResizeBuffers();
}

// desired 3D resolution, may be reduced internally due to FSR
void RenderPath3D::Set3DResolution( float width, float height, bool resizebuffers)
{
	if ( width == 0 || height == 0 ) return;
	if ( width3D == width && height3D == height ) return;

	width3D = width / fsrUpScale;
	height3D = height / fsrUpScale;
	if(resizebuffers)
		ResizeBuffers();
}

void RenderPath3D::SetFSRScale( float scale )
{
	if ( fsrUpScale == scale ) return;
	if ( fsrUpScale == 0 ) return;
	
	float origWidth3D = width3D * fsrUpScale;
	float origHeight3D = height3D * fsrUpScale;
	fsrUpScale = scale;
	width3D = origWidth3D / fsrUpScale;
	height3D = origHeight3D / fsrUpScale;
}

void RenderPath3D::PreUpdate()
{
	camera_previous = *camera;
	camera_reflection_previous = camera_reflection;
}

void RenderPath3D::StorePreviousLeft()
{
	camera_previousLeft = *camera;
	camera_reflection_previousLeft = camera_reflection;
}

void RenderPath3D::StorePreviousRight()
{
	camera_previousRight = *camera;
	camera_reflection_previousRight = camera_reflection;
}

void RenderPath3D::Update(float dt)
{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
	OPTICK_EVENT();
#endif
#endif
	if (rtGbuffer[GBUFFER_COLOR].desc.SampleCount != msaaSampleCount)
	{
		ResizeBuffers();
	}

	RenderPath2D::Update(dt);

	if (getSceneUpdateEnabled())
	{
		scene->Update(dt * wiRenderer::GetGameSpeed());
		if (wiRenderer::GetRaytracedShadowsEnabled() ||
			getAO() == AO_RTAO ||
			getRaytracedReflectionEnabled())
		{
			scene->SetAccelerationStructureUpdateRequested(true);
		}
	}

	// Frustum culling for main camera:
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
	OPTICK_EVENT("wiRenderer::UpdateVisibility");
#endif
#endif
	visibility_main.layerMask = getLayerMask();
	visibility_main.scene = scene;
	visibility_main.camera = camera;
	visibility_main.flags = wiRenderer::Visibility::ALLOW_EVERYTHING;
	wiRenderer::UpdateVisibility(visibility_main, maxApparentSize); 

	if (visibility_main.planar_reflection_visible && getReflectionsEnabled())
	{
		// Frustum culling for planar reflections:
		camera_reflection = *camera;
		camera_reflection.jitter = XMFLOAT2(0, 0);
		camera_reflection.Reflect(visibility_main.reflectionPlane);
		visibility_reflection.layerMask = getLayerMask();
		visibility_reflection.scene = scene;
		visibility_reflection.camera = &camera_reflection;
		visibility_reflection.flags = wiRenderer::Visibility::ALLOW_OBJECTS;
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT("wiRenderer::UpdateVisibility getReflectionsEnabled");
#endif
#endif
		wiRenderer::UpdateVisibility( visibility_reflection, std::max(maxApparentSize, 0.002f) ); // reflections cull more agressively by default
	}

	XMUINT2 internalResolution;
	internalResolution.x = GetWidth3D();
	internalResolution.y = GetHeight3D();

#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
	OPTICK_EVENT("wiRenderer::UpdatePerFrameData");
#endif
#endif
	wiRenderer::UpdatePerFrameData(
		*scene,
		visibility_main,
		frameCB,
		internalResolution,
		*this,
		dt
	);

	if (wiRenderer::GetTemporalAAEnabled())
	{
		const XMFLOAT4& halton = wiMath::GetHaltonSequence(wiRenderer::GetDevice()->GetFrameCount() % 256);
		camera->jitter.x = (halton.x * 2 - 1) / (float)internalResolution.x;
		camera->jitter.y = (halton.y * 2 - 1) / (float)internalResolution.y;
	}
	else
	{
		camera->jitter = XMFLOAT2(0, 0);
	}

#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
	OPTICK_EVENT("camera->UpdateCamera");
#endif
#endif
	camera->UpdateCamera();

	if (getAO() != AO_RTAO)
	{
		rtaoResources.frame = 0;
	}
#ifndef REMOVE_RAY_TRACED_SHADOW
	if (!wiRenderer::GetRaytracedShadowsEnabled())
	{
		rtshadowResources.frame = 0;
	}
#endif
	std::swap(depthBuffer_Copy, depthBuffer_Copy1);
}

void RenderPath3D::Render(int mode) const
{
#ifdef OPTICK_ENABLE
	OPTICK_EVENT();
#endif
	GraphicsDevice* device = wiRenderer::GetDevice();
	wiJobSystem::context ctx;
	CommandList cmd;

	int cloudIndex = 0;
	const wiScene::CameraComponent* previousCamera;
	const wiScene::CameraComponent* previousCameraReflection;
	switch (mode)
	{
	case EYE_LEFT:
	{
		previousCamera = &camera_previousLeft;
		previousCameraReflection = &camera_reflection_previousLeft;
	} break;

	case EYE_RIGHT:
	{
		previousCamera = &camera_previousRight;
		previousCameraReflection = &camera_reflection_previousRight;
		cloudIndex = 1;
	} break;

	default:
	{
		previousCamera = &camera_previous;
		previousCameraReflection = &camera_reflection_previous;
	}
	}

	// Preparing the frame:
	cmd = device->BeginCommandList();
	CommandList cmd_prepareframe = cmd;

#ifdef DELAYEDSHADOWS
	extern bool g_bDelayedShadows;
	wiJobSystem::Execute(ctx, [this, cmd](wiJobArgs args) {
		RenderFrameSetUp(cmd);
		});
#else

	wiJobSystem::Execute(ctx, [this, cmd](wiJobArgs args) {
		RenderFrameSetUp(cmd);
		});
#endif

	if (scene->IsAccelerationStructureUpdateRequested())
	{
		// Acceleration structures:
		//	async compute parallel with depth prepass
		cmd = device->BeginCommandList(QUEUE_COMPUTE);
		device->WaitCommandList(cmd, cmd_prepareframe);
		wiJobSystem::Execute(ctx, [this, cmd](wiJobArgs args) {

			wiRenderer::UpdateRaytracingAccelerationStructures(*scene, cmd);

			});
	}

	static const uint32_t drawscene_flags =
		wiRenderer::DRAWSCENE_OPAQUE |
		wiRenderer::DRAWSCENE_HAIRPARTICLE |
		wiRenderer::DRAWSCENE_TESSELLATION |
		wiRenderer::DRAWSCENE_OCCLUSIONCULLING
		;
	static const uint32_t drawscene_flags_reflections =
		wiRenderer::DRAWSCENE_OPAQUE |
		wiRenderer::DRAWSCENE_REFLECTIONS
		;

	// Main camera depth prepass + occlusion culling:
	cmd = device->BeginCommandList();
	CommandList cmd_maincamera_prepass = cmd;
	wiJobSystem::Execute(ctx, [this, cmd, previousCamera, mode](wiJobArgs args) {

		GraphicsDevice* device = wiRenderer::GetDevice();

		wiRenderer::UpdateCameraCB(
			*camera,
			*previousCamera,
			camera_reflection,
			cmd
		);

		device->RenderPassBegin(&renderpass_depthprepass, cmd);

		device->EventBegin("Opaque Z-prepass", cmd);
		
		Viewport vp;
		vp.Width = (float)depthBuffer_Main.GetDesc().Width;
		vp.Height = (float)depthBuffer_Main.GetDesc().Height;
		device->BindViewports(1, &vp, cmd);

		auto range = wiProfiler::BeginRangeGPU("Z-Prepass - Scene", cmd);
		wiRenderer::DrawScene(visibility_main, RENDERPASS_PREPASS, cmd, drawscene_flags);

		// write depth for transparent objects that are solid (opaque=100%) like guns and solid doors with windows in		
		wiRenderer::DrawScene(visibility_main, RENDERPASS_PREPASS, cmd, wiRenderer::DRAWSCENE_TRANSPARENT);

		wiProfiler::EndRange(range);

#ifdef GGREDUCED
		if (!g_bNoTerrainRender)
		{
			auto range2 = wiProfiler::BeginRangeGPU("Z-Prepass - Terrain", cmd);
			GGTerrain::GGTerrain_Draw_Prepass(&camera->frustum, cmd);
			wiProfiler::EndRange(range2);

			GGTrees::GGTrees_Draw_Prepass(&camera->frustum, 0, cmd);

			GGGrass::GGGrass_Draw_Prepass(&camera->frustum, 0, cmd);
		}
#endif

		range = wiProfiler::BeginRangeGPU("Z-Prepass - Sky Velocity", cmd);
		wiRenderer::DrawSkyVelocity(cmd);
		wiProfiler::EndRange(range);
		
		device->EventEnd(cmd);

		if (getOcclusionCullingEnabled())
		{
			wiRenderer::OcclusionCulling_Render(*camera, visibility_main, cmd);
		}

		device->RenderPassEnd(cmd);

#ifdef GGREDUCED
		if (!g_bNoTerrainRender)
		{
			if ( mode != EYE_RIGHT ) GGTerrain::GGTerrain_VirtualTexReadBack(rtVirtualTextureReadBack, getMSAASampleCount(), cmd);
		}
#endif
		});

	// Main camera compute effects:
	//	(async compute, parallel to "shadow maps" and "update textures",
	//	must finish before "main scene opaque color pass")
	cmd = device->BeginCommandList(QUEUE_COMPUTE);
	device->WaitCommandList(cmd, cmd_maincamera_prepass);
	CommandList cmd_maincamera_compute_effects = cmd;
	wiJobSystem::Execute(ctx, [this, cmd, previousCamera, cloudIndex](wiJobArgs args) {

		GraphicsDevice* device = wiRenderer::GetDevice();

		wiRenderer::UpdateCameraCB(
			*camera,
			*previousCamera,
			camera_reflection,
			cmd
		);

		// Create the top mip of depth pyramid from main depth buffer:
		if (getMSAASampleCount() > 1)
		{
			wiRenderer::ResolveMSAADepthBuffer(depthBuffer_Copy, depthBuffer_Main, cmd);
		}
		else
		{
			wiRenderer::CopyTexture2D(depthBuffer_Copy, 0, 0, 0, depthBuffer_Main, 0, cmd);
		}

		wiRenderer::Postprocess_DepthPyramid(depthBuffer_Copy, rtLinearDepth, cmd);

		RenderAO(cmd);

		if (wiRenderer::GetVariableRateShadingClassification() && device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_VARIABLE_RATE_SHADING_TIER2))
		{
			wiRenderer::ComputeShadingRateClassification(
				GetGbuffer_Read(),
				rtLinearDepth,
				rtShadingRate,
				debugUAV,
				cmd
			);
		}

		if (!g_bNoTerrainRender)
		{

			if (scene->weather.IsVolumetricClouds())
			{
				wiScene::CameraComponent cameraClouds = *camera;
				cameraClouds.Eye.x *= 0.0254f;
				cameraClouds.Eye.y *= 0.0254f;
				cameraClouds.Eye.z *= 0.0254f;
				cameraClouds.zNearP *= 0.0254f;
				cameraClouds.zFarP *= 0.0254f;
				cameraClouds.Projection.m[3][2] *= 0.0254f; // if the projection matrix is custom then this will correct it, if it isn't then it will be overwritten anyway
				cameraClouds.UpdateCamera();

				wiScene::CameraComponent cameraCloudsPrev = *previousCamera;
				cameraCloudsPrev.Eye.x *= 0.0254f;
				cameraCloudsPrev.Eye.y *= 0.0254f;
				cameraCloudsPrev.Eye.z *= 0.0254f;
				cameraCloudsPrev.zNearP *= 0.0254f;
				cameraCloudsPrev.zFarP *= 0.0254f;
				cameraCloudsPrev.Projection.m[3][2] *= 0.0254f; // if the projection matrix is custom then this will correct it, if it isn't then it will be overwritten anyway
				cameraCloudsPrev.UpdateCamera();

				wiRenderer::UpdateCameraCB(
					cameraClouds,
					cameraCloudsPrev,
					camera_reflection,
					cmd
				);

				wiRenderer::Postprocess_VolumetricClouds(
					volumetriccloudResources[cloudIndex],
					depthBuffer_Copy,
					cmd
				);

				wiRenderer::UpdateCameraCB(
					*camera,
					*previousCamera,
					camera_reflection,
					cmd
				);
			}
		}
	{
			auto range = wiProfiler::BeginRangeGPU("Entity Culling", cmd);
			wiRenderer::ComputeTiledLightCulling(
				tiledLightResources,
				depthBuffer_Copy,
				debugUAV,
				cmd
			);
			wiProfiler::EndRange(range);
		}

		RenderSSR(cmd);

#ifndef REMOVE_RAY_TRACED_SHADOW
		if (wiRenderer::GetScreenSpaceShadowsEnabled())
		{
			wiRenderer::Postprocess_ScreenSpaceShadow(
				screenspaceshadowResources,
				depthBuffer_Copy,
				rtLinearDepth,
				tiledLightResources.entityTiles_Opaque,
				rtShadow,
				cmd,
				getScreenSpaceShadowRange(),
				getScreenSpaceShadowSampleCount()
			);
		}

		if (wiRenderer::GetRaytracedShadowsEnabled())
		{
			wiRenderer::Postprocess_RTShadow(
				rtshadowResources,
				*scene,
				depthBuffer_Copy,
				rtLinearDepth,
				depthBuffer_Copy1,
				tiledLightResources.entityTiles_Opaque,
				GetGbuffer_Read(),
				rtShadow,
				cmd
			);
		}
#endif

			});

	// Shadow maps:
#ifdef DELAYEDSHADOWS
	if (g_bDelayedShadows) //PE: dependencies.
#endif
		wiJobSystem::Wait(ctx);
	if (getShadowsEnabled())
	{
		cmd = device->BeginCommandList();
		wiJobSystem::Execute(ctx, [this, cmd](wiJobArgs args) {
			wiRenderer::DrawShadowmaps(visibility_main, cmd);
			});
	}

	// Updating textures:
	cmd = device->BeginCommandList();
	wiJobSystem::Execute(ctx, [cmd, this](wiJobArgs args) {
		wiRenderer::BindCommonResources(cmd);
		wiRenderer::RefreshDecalAtlas(*scene, cmd);
		wiRenderer::RefreshLightmapAtlas(*scene, cmd);
		wiRenderer::RefreshEnvProbes(visibility_main, cmd);
		wiRenderer::RefreshImpostors(*scene, cmd);
		});

	// Voxel GI:
	if (wiRenderer::GetVoxelRadianceEnabled())
	{
		cmd = device->BeginCommandList();
		wiJobSystem::Execute(ctx, [cmd, this](wiJobArgs args) {
			wiRenderer::VoxelRadiance(visibility_main, cmd);
			});
	}

	if (visibility_main.IsRequestedPlanarReflections() && getReflectionsEnabled())
	{
		// Planar reflections depth prepass:
		cmd = device->BeginCommandList();
		wiJobSystem::Execute(ctx, [cmd, this, previousCameraReflection, cloudIndex](wiJobArgs args) {

			GraphicsDevice* device = wiRenderer::GetDevice();

			wiRenderer::UpdateCameraCB(
				camera_reflection,
				*previousCameraReflection,
				camera_reflection,
				cmd
			);

			device->EventBegin("Planar reflections Z-Prepass", cmd);
			auto range = wiProfiler::BeginRangeGPU("Planar Reflections Z-Prepass", cmd);

			Viewport vp;
			vp.Width = (float)depthBuffer_Reflection.GetDesc().Width;
			vp.Height = (float)depthBuffer_Reflection.GetDesc().Height;
			device->BindViewports(1, &vp, cmd);

			device->RenderPassBegin(&renderpass_reflection_depthprepass, cmd);

			wiRenderer::DrawScene(visibility_reflection, RENDERPASS_PREPASS, cmd, drawscene_flags_reflections);

#ifdef GGREDUCED
			if (!g_bNoTerrainRender)
			{
				auto range2 = wiProfiler::BeginRangeGPU("Planar Reflections Z-Prepass - Terrain", cmd);
				GGTerrain::GGTerrain_Draw_Prepass_Reflections(&camera_reflection.frustum, cmd);
				wiProfiler::EndRange(range2);

				GGTrees::GGTrees_Draw_Prepass(&camera_reflection.frustum, 1, cmd);
			}
#endif

			device->RenderPassEnd(cmd);

			if (!g_bNoTerrainRender)
			{
				if (scene->weather.IsVolumetricClouds())
				{
					wiScene::CameraComponent cameraClouds = camera_reflection;
					cameraClouds.Eye.x *= 0.0254f;
					cameraClouds.Eye.y *= 0.0254f;
					cameraClouds.Eye.z *= 0.0254f;
					cameraClouds.zNearP *= 0.0254f;
					cameraClouds.zFarP *= 0.0254f;
					cameraClouds.Projection.m[3][2] *= 0.0254f; // if the projection matrix is custom then this will correct it, if it isn't then it will be overwritten anyway
					cameraClouds.UpdateCamera();

					wiScene::CameraComponent cameraCloudsPrev = *previousCameraReflection;
					cameraCloudsPrev.Eye.x *= 0.0254f;
					cameraCloudsPrev.Eye.y *= 0.0254f;
					cameraCloudsPrev.Eye.z *= 0.0254f;
					cameraCloudsPrev.zNearP *= 0.0254f;
					cameraCloudsPrev.zFarP *= 0.0254f;
					cameraCloudsPrev.Projection.m[3][2] *= 0.0254f; // if the projection matrix is custom then this will correct it, if it isn't then it will be overwritten anyway
					cameraCloudsPrev.UpdateCamera();

					wiRenderer::UpdateCameraCB(
						cameraClouds,
						cameraCloudsPrev,
						camera_reflection,
						cmd
					);

					wiRenderer::Postprocess_VolumetricClouds(
						volumetriccloudResources_reflection[cloudIndex],
						depthBuffer_Reflection,
						cmd
					);

					wiRenderer::UpdateCameraCB(
						camera_reflection,
						*previousCameraReflection,
						camera_reflection,
						cmd
					);
				}
			}
			wiProfiler::EndRange(range); // Planar Reflections
			device->EventEnd(cmd);

			});

		//PE: This crash a lot try to make the mainthread run it. (Looks ok).
		// Planar reflections opaque color pass:
		cmd = device->BeginCommandList();
		//wiJobSystem::Execute(ctx, [cmd, this, previousCameraReflection, cloudIndex](wiJobArgs args) {

			GraphicsDevice* device = wiRenderer::GetDevice();

			wiRenderer::UpdateCameraCB(
				camera_reflection,
				*previousCameraReflection,
				camera_reflection,
				cmd
			);

			wiRenderer::ComputeTiledLightCulling(
				tiledLightResources_planarReflection,
				depthBuffer_Reflection,
				Texture(),
				cmd
			);

			device->EventBegin("Planar reflections", cmd);
			auto range = wiProfiler::BeginRangeGPU("Planar Reflections", cmd);

			Viewport vp;
			vp.Width = (float)depthBuffer_Reflection.GetDesc().Width;
			vp.Height = (float)depthBuffer_Reflection.GetDesc().Height;
			device->BindViewports(1, &vp, cmd);

			device->UnbindResources(TEXSLOT_DEPTH, 1, cmd);

			device->RenderPassBegin(&renderpass_reflection, cmd);

			device->BindResource(PS, &tiledLightResources_planarReflection.entityTiles_Opaque, TEXSLOT_RENDERPATH_ENTITYTILES, cmd);

			device->BindResource(PS, wiTextureHelper::getTransparent(), TEXSLOT_RENDERPATH_REFLECTION, cmd);

//#ifdef GGREDUCED
//			// this can fail and crash (maybe race condition?)
//			const Texture* white = wiTextureHelper::getWhite();
//			if( !(white != nullptr && white->IsValid()))
//			{
//				//__debugbreak();
//			}
//			else
//			{
//				device->BindResource(PS, white, TEXSLOT_RENDERPATH_AO, cmd);
//			}
//#else
			device->BindResource(PS, wiTextureHelper::getWhite(), TEXSLOT_RENDERPATH_AO, cmd);
//#endif

			device->BindResource(PS, wiTextureHelper::getTransparent(), TEXSLOT_RENDERPATH_SSR, cmd);
			device->BindResource(PS, wiTextureHelper::getUINT4(), TEXSLOT_RENDERPATH_RTSHADOW, cmd);
			wiRenderer::DrawScene(visibility_reflection, RENDERPASS_MAIN, cmd, drawscene_flags_reflections);

#ifdef GGREDUCED
			if (!g_bNoTerrainRender)
			{
				auto range2 = wiProfiler::BeginRangeGPU("Planar Reflections - Terrain", cmd);
				GGTerrain::GGTerrain_Draw(&camera_reflection.frustum, 1, cmd);
				wiProfiler::EndRange(range2);

				GGTrees::GGTrees_Draw(&camera_reflection.frustum, 1, cmd);
			}
#endif
#ifdef GGREDUCED
			if (!g_bNoTerrainRender)
			{
				wiRenderer::DrawSky(*scene, cmd);

				// Blend the volumetric clouds on top:
				if (scene->weather.IsVolumetricClouds())
				{
					device->EventBegin("Volumetric Clouds Reflection Blend", cmd);
					wiImageParams fx;
					fx.enableFullScreen();
					wiImage::Draw(&volumetriccloudResources_reflection[cloudIndex].texture_temporal[device->GetFrameCount() % 2], fx, cmd);
					device->EventEnd(cmd);
				}
			}
#endif
			device->RenderPassEnd(cmd);

			wiProfiler::EndRange(range); // Planar Reflections
			device->EventEnd(cmd);
			//});
	}

	// Main camera opaque color pass:
	cmd = device->BeginCommandList();
	device->WaitCommandList(cmd, cmd_maincamera_compute_effects);
	wiJobSystem::Execute(ctx, [this, cmd, previousCamera, cloudIndex](wiJobArgs args) {

		GraphicsDevice* device = wiRenderer::GetDevice();
		device->EventBegin("Opaque Scene", cmd);

		wiRenderer::UpdateCameraCB(
			*camera,
			*previousCamera,
			camera_reflection,
			cmd
		);

		// This can't run in "main camera compute effects" async compute,
		//	because it depends on shadow maps, and envmaps
		if (getRaytracedReflectionEnabled())
		{
			wiRenderer::Postprocess_RTReflection(
				rtreflectionResources,
				*scene,
				depthBuffer_Copy,
				depthBuffer_Copy1,
				GetGbuffer_Read(),
				rtSSR,
				cmd
			);
		}

		device->RenderPassBegin(&renderpass_main, cmd);

		

		Viewport vp;
		vp.Width = (float)depthBuffer_Main.GetDesc().Width;
		vp.Height = (float)depthBuffer_Main.GetDesc().Height;
		device->BindViewports(1, &vp, cmd);

		auto range = wiProfiler::BeginRangeGPU("Opaque - Scene", cmd);

		bindresourcesLock.lock();

		#ifndef REMOVE_RAY_TRACED_SHADOW
		if (wiRenderer::GetRaytracedShadowsEnabled() || wiRenderer::GetScreenSpaceShadowsEnabled())
		{
			GPUBarrier barrier = GPUBarrier::Image(&rtShadow, rtShadow.desc.layout, IMAGE_LAYOUT_SHADER_RESOURCE);
			device->Barrier(&barrier, 1, cmd);
			device->BindResource(PS, &rtShadow, TEXSLOT_RENDERPATH_RTSHADOW, cmd);
		}
		else
		{
		#endif
			device->BindResource(PS, wiTextureHelper::getUINT4(), TEXSLOT_RENDERPATH_RTSHADOW, cmd);
		#ifndef REMOVE_RAY_TRACED_SHADOW
		}
		#endif

		device->BindResource(PS, &tiledLightResources.entityTiles_Opaque, TEXSLOT_RENDERPATH_ENTITYTILES, cmd);
//#ifdef GGREDUCED
//		const Texture* transparent = wiTextureHelper::getTransparent();
//		if (!(transparent != nullptr && transparent->IsValid()))
//		{
//			__debugbreak();
//		}
//		else
//			device->BindResource(PS, getReflectionsEnabled() ? &rtReflection : transparent, TEXSLOT_RENDERPATH_REFLECTION, cmd);
//#else
		device->BindResource(PS, getReflectionsEnabled() ? &rtReflection : wiTextureHelper::getTransparent(), TEXSLOT_RENDERPATH_REFLECTION, cmd);
//#endif

//#ifdef GGREDUCED
//		// this can fail and crash (maybe race condition?)
//		const Texture* white = wiTextureHelper::getWhite();
//		if( !(white != nullptr && white->IsValid()))
//		{
//			__debugbreak();
//		}
//		else
//		{
//			device->BindResource(PS, getAOEnabled() ? &rtAO : white, TEXSLOT_RENDERPATH_AO, cmd);
//		}
//#else
		device->BindResource(PS, getAOEnabled() ? &rtAO : wiTextureHelper::getWhite(), TEXSLOT_RENDERPATH_AO, cmd);
//#endif
//#ifdef GGREDUCED
//		if ((transparent != nullptr && transparent->IsValid()))
//			device->BindResource(PS, getSSREnabled() || getRaytracedReflectionEnabled() ? &rtSSR : transparent, TEXSLOT_RENDERPATH_SSR, cmd);
//#else
		device->BindResource(PS, getSSREnabled() || getRaytracedReflectionEnabled() ? &rtSSR : wiTextureHelper::getTransparent(), TEXSLOT_RENDERPATH_SSR, cmd);
//#endif

		bindresourcesLock.unlock();

		wiRenderer::DrawScene(visibility_main, RENDERPASS_MAIN, cmd, drawscene_flags);
		wiProfiler::EndRange(range); // Opaque Scene

#ifdef GGREDUCED
		if (!g_bNoTerrainRender)
		{
			auto range2 = wiProfiler::BeginRangeGPU("Opaque - Terrain", cmd);
			GGTerrain::GGTerrain_Draw(&camera->frustum, 0, cmd);
			wiProfiler::EndRange(range2);

			GGTrees::GGTrees_Draw(&camera->frustum, 0, cmd);

			GGGrass::GGGrass_Draw(&camera->frustum, 0, cmd);
		}
#endif
#ifdef GGREDUCED
		if (!g_bNoTerrainRender)
		{
			range = wiProfiler::BeginRangeGPU("Opaque - Sky", cmd);
			wiRenderer::DrawSky(*scene, cmd);
			wiProfiler::EndRange(range);
		}
#endif
		RenderOutline(cmd);

		// Upsample + Blend the volumetric clouds on top:
#ifdef GGREDUCED
		if (!g_bNoTerrainRender)
		{
			if (scene->weather.IsVolumetricClouds() && camera->Eye.y <= (scene->weather.volumetricCloudParameters.CloudStartHeight * 39.37f) + 500)
			{
				device->EventBegin("Volumetric Clouds Upsample + Blend", cmd);
				wiRenderer::Postprocess_Upsample_Bilateral(
					volumetriccloudResources[cloudIndex].texture_temporal[device->GetFrameCount() % 2],
					rtLinearDepth,
					*GetGbuffer_Read(GBUFFER_COLOR), // only desc is taken if pixel shader upsampling is used
					cmd,
					true // pixel shader upsampling
				);
				device->EventEnd(cmd);
			}
		}
#endif
		float waterHieght = visibility_main.scene->weather.oceanParameters.waterHeight + visibility_main.scene->weather.oceanParameters.wave_amplitude;

		wiScene::CameraComponent clippedCamera = *camera;

		if(clippedCamera.Eye.y > waterHieght)
			clippedCamera.clipPlane = XMFLOAT4(0, clippedCamera.Eye.y > waterHieght ? -1 : 1, 0, waterHieght);

		wiRenderer::UpdateCameraCB(clippedCamera, *previousCamera, camera_reflection, cmd);
		wiRenderer::DrawScene(visibility_main, RENDERPASS_MAIN, cmd, (drawscene_flags & ~wiRenderer::DRAWSCENE_OPAQUE) | wiRenderer::DRAWSCENE_TRANSPARENT);
		wiRenderer::UpdateCameraCB(*camera, *previousCamera, camera_reflection, cmd);
		device->RenderPassEnd(cmd);

#ifndef REMOVE_RAY_TRACED_SHADOW
		if (wiRenderer::GetRaytracedShadowsEnabled() || wiRenderer::GetScreenSpaceShadowsEnabled())
		{
			GPUBarrier barrier = GPUBarrier::Image(&rtShadow, IMAGE_LAYOUT_SHADER_RESOURCE, rtShadow.desc.layout);
			device->Barrier(&barrier, 1, cmd);
		}
#endif
		device->EventEnd(cmd);
		});

	// Transparents, post processes, etc:
	cmd = device->BeginCommandList();
	wiJobSystem::Execute(ctx, [this, cmd, previousCamera, mode](wiJobArgs args) {

		GraphicsDevice* device = wiRenderer::GetDevice();

		wiRenderer::UpdateCameraCB(
			*camera,
			*previousCamera,
			camera_reflection,
			cmd
		);
		wiRenderer::BindCommonResources(cmd);

#ifdef GGREDUCED
		Viewport vp;
		vp.Width = GetWidth3D();
		vp.Height = GetHeight3D();
		device->BindViewports(1, &vp, cmd);
#endif

		RenderLightShafts(cmd, mode);

		RenderVolumetrics(cmd);

		RenderSceneMIPChain(cmd);

		RenderTransparents(cmd, mode);

		RenderPostprocessChain(cmd);

#ifdef GGREDUCED
		RenderOutlineHighlighers(cmd);
#endif

		});

	//PE: I get a crash when generating mip maps. ( ProcessDeferredMipGenRequests ).
	//wiJobSystem::WaitSleep(ctx,1); //PE: This just make everyting slow, Added wiSpinLock to see if that solve it.
	wiJobSystem::Wait(ctx);

	RenderPath2D::Render( mode );

	wiJobSystem::Wait(ctx);
}

void RenderPath3D::Compose(CommandList cmd) const
{
	GraphicsDevice* device = wiRenderer::GetDevice();

	wiImageParams fx;
	fx.blendFlag = BLENDMODE_OPAQUE;
	fx.quality = QUALITY_LINEAR;
	fx.enableFullScreen();

	device->EventBegin("Composition", cmd);

#ifdef GGREDUCED
	XMFLOAT4 area;
	if (ImGuiHook_GetScissorArea(&area.x, &area.y, &area.z, &area.w) == true)
		device->SetScissorArea(cmd, area);
#endif

	wiImage::Draw(GetLastPostprocessRT(), fx, cmd);
	device->EventEnd(cmd);

	if (wiRenderer::GetDebugLightCulling() || wiRenderer::GetVariableRateShadingClassificationDebug())
	{
		fx.enableFullScreen();
		fx.blendFlag = BLENDMODE_PREMULTIPLIED;
		wiImage::Draw(&debugUAV, fx, cmd);
	}

#ifdef GGREDUCED
	XMUINT2 resolution;
	resolution.x = GetWidth3D();
	resolution.y = GetHeight3D();
	area = { 0, 0, (float)resolution.x, (float)resolution.y };
	device->SetScissorArea(cmd, area);
#endif

	RenderPath2D::Compose(cmd);
}

#ifdef GGREDUCED
void RenderPath3D::ComposeSimple(CommandList cmd) const
{
	GraphicsDevice* device = wiRenderer::GetDevice();

	wiImageParams fx;
	fx.blendFlag = BLENDMODE_OPAQUE;
	fx.quality = QUALITY_LINEAR;
	fx.enableFullScreen();

	wiImage::Draw(GetLastPostprocessRT(), fx, cmd);
}
void RenderPath3D::ComposeSimple2D(CommandList cmd) const
{
	RenderPath2D::Compose(cmd);
}
#endif

void RenderPath3D::RenderFrameSetUp(CommandList cmd) const
{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
	OPTICK_EVENT();
#endif
#endif
	GraphicsDevice* device = wiRenderer::GetDevice();

	device->BindResource(CS, &depthBuffer_Copy1, TEXSLOT_DEPTH, cmd);
	wiRenderer::UpdateRenderData(visibility_main, frameCB, cmd);
}

void RenderPath3D::RenderAO(CommandList cmd) const
{
	wiRenderer::GetDevice()->UnbindResources(TEXSLOT_RENDERPATH_AO, 1, cmd);

	if (getAOEnabled())
	{
		switch (getAO())
		{
		case AO_SSAO:
			wiRenderer::Postprocess_SSAO(
				ssaoResources,
				depthBuffer_Copy,
				rtLinearDepth,
				rtAO,
				cmd,
				getAORange(),
				getAOSampleCount(),
				getAOPower()
				);
			break;
		case AO_HBAO:
			wiRenderer::Postprocess_HBAO(
				ssaoResources,
				*camera,
				rtLinearDepth,
				rtAO,
				cmd,
				getAOPower()
				);
			break;
		case AO_MSAO:
			wiRenderer::Postprocess_MSAO(
				msaoResources,
				*camera,
				rtLinearDepth,
				rtAO,
				cmd,
				getAOPower()
				);
			break;
		case AO_RTAO:
			wiRenderer::Postprocess_RTAO(
				rtaoResources,
				*scene,
				depthBuffer_Copy,
				rtLinearDepth,
				depthBuffer_Copy1,
				GetGbuffer_Read(),
				rtAO,
				cmd,
				getAORange(),
				getAOPower()
			);
			break;
		}
	}
}
void RenderPath3D::RenderSSR(CommandList cmd) const
{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
	OPTICK_EVENT();
#endif
#endif
	if (getSSREnabled() && !getRaytracedReflectionEnabled())
	{
		wiRenderer::Postprocess_SSR(
			ssrResources,
			rtSceneCopy, 
			depthBuffer_Copy, 
			rtLinearDepth,
			depthBuffer_Copy1,
			GetGbuffer_Read(),
			rtSSR, 
			cmd
		);
	}
}
void RenderPath3D::RenderOutline(CommandList cmd) const
{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
	OPTICK_EVENT();
#endif
#endif
	if (getOutlineEnabled())
	{
		wiRenderer::Postprocess_Outline(rtLinearDepth, cmd, getOutlineThreshold(), getOutlineThickness(), getOutlineColor());
	}
}
void RenderPath3D::RenderLightShafts(CommandList cmd, int mode) const
{
	int cloudIndex = 0;
	if ( mode == EYE_RIGHT ) cloudIndex = 1;

	XMVECTOR sunDirection = XMLoadFloat3(&scene->weather.sunDirection);
	if (getLightShaftsEnabled() && XMVectorGetX(XMVector3Dot(sunDirection, camera->GetAt())) > 0)
	{
		GraphicsDevice* device = wiRenderer::GetDevice();

		device->EventBegin("Light Shafts", cmd);
		device->UnbindResources(TEXSLOT_ONDEMAND0, TEXSLOT_ONDEMAND_COUNT, cmd);

		// Render sun stencil cutout:
		{
			device->RenderPassBegin(&renderpass_lightshafts, cmd);

			Viewport vp;
			vp.Width = (float)depthBuffer_Main.GetDesc().Width;
			vp.Height = (float)depthBuffer_Main.GetDesc().Height;
			device->BindViewports(1, &vp, cmd);

			wiRenderer::DrawSun(cmd);
			if (!g_bNoTerrainRender)
			{

				if (scene->weather.IsVolumetricClouds())
				{
					device->EventBegin("Volumetric cloud occlusion mask", cmd);
					wiImageParams fx;
					fx.enableFullScreen();
					fx.blendFlag = BLENDMODE_MULTIPLY;
					wiImage::Draw(&volumetriccloudResources[cloudIndex].texture_cloudMask, fx, cmd);
					device->EventEnd(cmd);
				}
			}
			device->RenderPassEnd(cmd);
		}

		// Radial blur on the sun:
		{
//			XMVECTOR sunPos = XMVector3Project(sunDirection * 100000, 0, 0,
//				1.0f, 1.0f, 0.1f, 1.0f,
//				camera->GetProjection(), camera->GetView(), XMMatrixIdentity());
			#ifdef GGREDUCED
			//PE: We need a larger direction or the 2D calculation fail when the camera pos has large values like, -40000,3000,5000 (x will be unprecise).
			XMVECTOR sunPos = XMVector3Project(sunDirection * 700000, 0, 0,
				1.0f, 1.0f, 0.1f, 1.0f,
				camera->GetProjection(), camera->GetView(), XMMatrixIdentity());
			#endif
			{
				XMFLOAT2 sun;
				XMStoreFloat2(&sun, sunPos);
				wiRenderer::Postprocess_LightShafts(*renderpass_lightshafts.desc.attachments.back().texture, rtSun[1], cmd, sun);
			}
		}
		device->EventEnd(cmd);
	}
}
void RenderPath3D::RenderVolumetrics(CommandList cmd) const
{
	if (getVolumeLightsEnabled() && visibility_main.IsRequestedVolumetricLights())
	{
		auto range = wiProfiler::BeginRangeGPU("Volumetric Lights", cmd);

		GraphicsDevice* device = wiRenderer::GetDevice();

		device->RenderPassBegin(&renderpass_volumetriclight, cmd);

		Viewport vp;
		vp.Width = (float)rtVolumetricLights[0].GetDesc().Width;
		vp.Height = (float)rtVolumetricLights[0].GetDesc().Height;
		device->BindViewports(1, &vp, cmd);

		wiRenderer::DrawVolumeLights(visibility_main, depthBuffer_Copy, cmd);

		device->RenderPassEnd(cmd);

		wiRenderer::Postprocess_Blur_Bilateral(
			rtVolumetricLights[0],
			rtLinearDepth,
			rtVolumetricLights[1],
			rtVolumetricLights[0],
			cmd
		);

		wiProfiler::EndRange(range);
	}
}
void RenderPath3D::RenderSceneMIPChain(CommandList cmd) const
{
	GraphicsDevice* device = wiRenderer::GetDevice();

	auto range = wiProfiler::BeginRangeGPU("Scene MIP Chain", cmd);
	device->EventBegin("RenderSceneMIPChain", cmd);

	device->RenderPassBegin(&renderpass_downsamplescene, cmd);

	Viewport vp;
	vp.Width = (float)rtSceneCopy.GetDesc().Width;
	vp.Height = (float)rtSceneCopy.GetDesc().Height;
	device->BindViewports(1, &vp, cmd);

	wiImageParams fx;
	fx.enableFullScreen();
	fx.sampleFlag = SAMPLEMODE_CLAMP;
	fx.quality = QUALITY_LINEAR;
	fx.blendFlag = BLENDMODE_OPAQUE;
	wiImage::Draw(GetGbuffer_Read(GBUFFER_COLOR), fx, cmd);

	device->RenderPassEnd(cmd);

	wiRenderer::MIPGEN_OPTIONS mipopt;
	mipopt.gaussian_temp = &rtSceneCopy_tmp;
	wiRenderer::GenerateMipChain(rtSceneCopy, wiRenderer::MIPGENFILTER_GAUSSIAN, cmd, mipopt);

	device->EventEnd(cmd);
	wiProfiler::EndRange(range);
}
#ifdef GGREDUCED
void RenderPath3D::RenderOutlineHighlighers(wiGraphics::CommandList cmd) const
{
	GraphicsDevice* device = wiRenderer::GetDevice();
}
#endif
void RenderPath3D::RenderTransparents(CommandList cmd, int mode) const
{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
	OPTICK_EVENT();
#endif
#endif
	GraphicsDevice* device = wiRenderer::GetDevice();

	int cloudIndex = 0;
	if ( mode == EYE_RIGHT ) cloudIndex = 1;

#ifdef GGREDUCED
#ifndef REMOVE_WATER_RIPPLE
	if (!g_bNoTerrainRender)
	{
		// Water ripple rendering:
		if (!scene->waterRipples.empty())
		{
			device->RenderPassBegin(&renderpass_waterripples, cmd);

			Viewport vp;
			vp.Width = (float)rtWaterRipple.GetDesc().Width;
			vp.Height = (float)rtWaterRipple.GetDesc().Height;
			device->BindViewports(1, &vp, cmd);

			wiRenderer::DrawWaterRipples(visibility_main, cmd);

			device->RenderPassEnd(cmd);
		}
	}
#endif
#endif
	device->UnbindResources(TEXSLOT_GBUFFER0, 1, cmd);
	device->UnbindResources(TEXSLOT_ONDEMAND0, TEXSLOT_ONDEMAND_COUNT, cmd);

	device->RenderPassBegin(&renderpass_transparent, cmd);

	Viewport vp;
	vp.Width = (float)depthBuffer_Main.GetDesc().Width;
	vp.Height = (float)depthBuffer_Main.GetDesc().Height;
	device->BindViewports(1, &vp, cmd);

	wiRenderer::DrawLightVisualizers(visibility_main, cmd);

	// Transparent scene
	{

		auto range = wiProfiler::BeginRangeGPU("Transparent Scene", cmd);
		device->EventBegin("Transparent Scene", cmd);

		//PE: Binding to the same slot from 2 threads seams to crash. even if diffeerent cmd..
		bindresourcesLock.lock();

		device->BindResource(VS, &rtLinearDepth, TEXSLOT_LINEARDEPTH, cmd);

		device->BindResource(PS, &tiledLightResources.entityTiles_Transparent, TEXSLOT_RENDERPATH_ENTITYTILES, cmd);
		device->BindResource(PS, &rtLinearDepth, TEXSLOT_LINEARDEPTH, cmd);
		device->BindResource(PS, getReflectionsEnabled() ? &rtReflection : wiTextureHelper::getTransparent(), TEXSLOT_RENDERPATH_REFLECTION, cmd);
		device->BindResource(PS, &rtSceneCopy, TEXSLOT_RENDERPATH_REFRACTION, cmd);
#ifndef REMOVE_WATER_RIPPLE
		device->BindResource(PS, &rtWaterRipple, TEXSLOT_RENDERPATH_WATERRIPPLES, cmd);
#endif

		bindresourcesLock.unlock();

#ifdef GGREDUCED
		// Moved from below to avoid corruption caused when transparent objects/maybe are not drawn
		if (getLensFlareEnabled())
		{
			wiRenderer::DrawLensFlares(
				visibility_main,
				depthBuffer_Copy,
				cmd,
				scene->weather.IsVolumetricClouds() ? &volumetriccloudResources[cloudIndex].texture_cloudMask : nullptr
			);
		}
#endif

#ifdef GGREDUCED
		auto particlerange = wiProfiler::BeginRangeGPU("Particles - Init Render", cmd);
		GPUParticles::gpup_draw_init(wiScene::GetCamera(), cmd);
		wiProfiler::EndRange(particlerange);
#endif

		uint32_t drawscene_flags = 0;
		drawscene_flags |= wiRenderer::DRAWSCENE_TRANSPARENT;
		drawscene_flags |= wiRenderer::DRAWSCENE_OCCLUSIONCULLING;
		drawscene_flags |= wiRenderer::DRAWSCENE_HAIRPARTICLE;
		drawscene_flags |= wiRenderer::DRAWSCENE_TESSELLATION;
		drawscene_flags |= wiRenderer::DRAWSCENE_OCEAN;
		wiRenderer::DrawScene(visibility_main, RENDERPASS_MAIN, cmd, drawscene_flags);

#ifdef GGREDUCED
		GPUParticles::gpup_draw_bydistance(wiScene::GetCamera(), cmd, 0.0f);
		// repair constant buffers changed by particle shader
		//BindCommonResources(cmd);
		//BindConstantBuffers(VS, cmd);
		//BindConstantBuffers(PS, cmd);
#endif

		device->EventEnd(cmd);
		wiProfiler::EndRange(range); // Transparent Scene

#ifdef GGREDUCED
		if (!g_bNoTerrainRender)
		{
			auto range2 = wiProfiler::BeginRangeGPU("Transparent - Terrain", cmd);
			GGTerrain::GGTerrain_Draw_Transparent(&camera->frustum, cmd);
			wiProfiler::EndRange(range2);
		}
#endif
	}

	if (!g_bNoTerrainRender)
	{
		
		if (scene->weather.IsVolumetricClouds() && camera->Eye.y > (scene->weather.volumetricCloudParameters.CloudStartHeight * 39.37) + 500)
		{
			device->EventBegin("Volumetric Clouds Upsample + Blend", cmd);
			wiRenderer::Postprocess_Upsample_Bilateral(
				volumetriccloudResources[cloudIndex].texture_temporal[device->GetFrameCount() % 2],
				rtLinearDepth,
				*GetGbuffer_Read(GBUFFER_COLOR), // only desc is taken if pixel shader upsampling is used
				cmd,
				true // pixel shader upsampling
			);
			device->EventEnd(cmd);
		}
		
	}

	if (getVolumeLightsEnabled() && visibility_main.IsRequestedVolumetricLights())
	{
		device->EventBegin("Contribute Volumetric Lights", cmd);
		wiRenderer::Postprocess_Upsample_Bilateral(rtVolumetricLights[0], rtLinearDepth,
			*renderpass_transparent.desc.attachments[0].texture, cmd, true, 1.5f);
		device->EventEnd(cmd);
	}

	XMVECTOR sunDirection = XMLoadFloat3(&scene->weather.sunDirection);
	if (getLightShaftsEnabled() && XMVectorGetX(XMVector3Dot(sunDirection, camera->GetAt())) > 0)
	{
		device->EventBegin("Contribute LightShafts", cmd);
		wiImageParams fx;
		fx.enableFullScreen();
		fx.blendFlag = BLENDMODE_ADDITIVE;
		wiImage::Draw(&rtSun[1], fx, cmd);
		device->EventEnd(cmd);
	}

#ifdef GGREDUCED
	// Moved this to above the transparent objects draw stuff - fixes issue with lensflare disappearing when no transparents
	if (getLensFlareEnabled())
	{
		wiRenderer::DrawLensFlares(
			visibility_main,
			depthBuffer_Copy,
			cmd,
			scene->weather.IsVolumetricClouds() ? &volumetriccloudResources[cloudIndex].texture_cloudMask : nullptr
		);
	}
#endif

	wiRenderer::DrawDebugWorld(*scene, *camera, *this, cmd);
	#ifdef GGREDUCED
	#else
	auto range = wiProfiler::BeginRangeGPU("EmittedParticles - Render", cmd);
	#endif

	#ifndef REMOVE_WICKED_PARTICLE
	wiRenderer::DrawSoftParticles(visibility_main, rtLinearDepth, false, cmd);
	#endif
	#ifdef GGREDUCED
	//PE: strange things happen when placed above, moved here, draw order looks fine.
	//GPUParticles::gpup_draw(wiScene::GetCamera(), cmd); moved further up and incorporated into the DrawScene as particles need to be rendered back to front WITH other transparent objects (ie. window, window, particle, window)
	#else
	wiProfiler::EndRange(range);
	#endif

	device->RenderPassEnd(cmd);

#ifndef REMOVE_WICKED_PARTICLE
	// Distortion particles:
	{
#ifdef GGREDUCED
		auto range = wiProfiler::BeginRangeGPU("Particles - Render (Distortion)", cmd);
#else
		auto range = wiProfiler::BeginRangeGPU("EmittedParticles - Render (Distortion)", cmd);
#endif
		device->RenderPassBegin(&renderpass_particledistortion, cmd);

		Viewport vp;
		vp.Width = (float)rtParticleDistortion.GetDesc().Width;
		vp.Height = (float)rtParticleDistortion.GetDesc().Height;
		device->BindViewports(1, &vp, cmd);

		wiRenderer::DrawSoftParticles(visibility_main, rtLinearDepth, true, cmd);

		device->RenderPassEnd(cmd);

		// device->MSAAResolve missing and had to add this!!
		wiProfiler::EndRange(range);
	}
#endif
}
void RenderPath3D::RenderPostprocessChain(CommandList cmd) const
{
	GraphicsDevice* device = wiRenderer::GetDevice();

	const Texture* rt_first = nullptr; // not ping-ponged with read / write
	const Texture* rt_read = GetGbuffer_Read(GBUFFER_COLOR);
	//GetGbuffer_Read
	const Texture* rt_write = &rtPostprocess_HDR[0];
	const Texture* rt_write2 = &rtPostprocess_HDR[1];

	// 1.) HDR post process chain
	{
#ifndef REMOVE_TEMPORAL_AA
		if (wiRenderer::GetTemporalAAEnabled() && !wiRenderer::GetTemporalAADebugEnabled())
		{
			GraphicsDevice* device = wiRenderer::GetDevice();

			int output = device->GetFrameCount() % 2;
			int history = 1 - output;
			wiRenderer::Postprocess_TemporalAA(
				*rt_read, rtTemporalAA[history], 
				rtLinearDepth,
				depthBuffer_Copy1,
				GetGbuffer_Read(),
				rtTemporalAA[output], 
				cmd
			);
			rt_first = &rtTemporalAA[output];
		}
#endif

		if (getDepthOfFieldEnabled() && camera->aperture_size > 0 && getDepthOfFieldStrength() > 0)
		{
			wiRenderer::Postprocess_DepthOfField(
				depthoffieldResources,
				rt_first == nullptr ? *rt_read : *rt_first,
				*rt_write,
				rtLinearDepth,
				cmd,
				getDepthOfFieldStrength()
			);
			rt_first = nullptr;
			if (rt_read == GetGbuffer_Read(GBUFFER_COLOR))
				rt_read = rt_write2;
			std::swap(rt_read, rt_write);
			//rt_write2

			device->UnbindResources(TEXSLOT_ONDEMAND0, 1, cmd);
		}

		if (getMotionBlurEnabled() && getMotionBlurStrength() > 0)
		{
			wiRenderer::Postprocess_MotionBlur(
				motionblurResources,
				rt_first == nullptr ? *rt_read : *rt_first,
				rtLinearDepth,
				GetGbuffer_Read(),
				*rt_write,
				cmd,
				getMotionBlurStrength()
			);
			rt_first = nullptr;
			if (rt_read == GetGbuffer_Read(GBUFFER_COLOR))
				rt_read = rt_write2;
			std::swap(rt_read, rt_write);
			device->UnbindResources(TEXSLOT_ONDEMAND0, 1, cmd);
		}

		if (getBloomEnabled())
		{
			wiRenderer::Postprocess_Bloom(
				bloomResources,
				rt_first == nullptr ? *rt_read : *rt_first,
				*rt_write,
				cmd,
				getBloomThreshold(),
				getBloomStrength()
			);
			rt_first = nullptr;

			if (rt_read == GetGbuffer_Read(GBUFFER_COLOR))
				rt_read = rt_write2;
			std::swap(rt_read, rt_write);
			device->UnbindResources(TEXSLOT_ONDEMAND0, 1, cmd);
		}
	}

	// 2.) Tone mapping HDR -> LDR
	{
		rt_write = &rtPostprocess_LDR[0];

#ifdef GGREDUCED
		if (!g_bNoTerrainRender)
		{
#endif
#ifndef REMOVE_WICKED_PARTICLE
			wiRenderer::Postprocess_Tonemap(
				rt_first == nullptr ? *rt_read : *rt_first,
				*rt_write,
				cmd,
				getExposure(),
				getDitherEnabled(),
				getColorGradingEnabled() ? (scene->weather.colorGradingMap == nullptr ? nullptr : &scene->weather.colorGradingMap->texture) : nullptr,
				getMSAASampleCount() > 1 ? &rtParticleDistortion_Resolved : &rtParticleDistortion,
				getEyeAdaptionEnabled() ? wiRenderer::ComputeLuminance(luminanceResources, *GetGbuffer_Read(GBUFFER_COLOR), cmd, getEyeAdaptionRate()) : nullptr,
				getEyeAdaptionKey(),
				&rtLinearDepth
			);
#else
			wiRenderer::Postprocess_Tonemap(
				rt_first == nullptr ? *rt_read : *rt_first,
				*rt_write,
				cmd,
				getExposure(),
				getDitherEnabled(),
				getColorGradingEnabled() ? (scene->weather.colorGradingMap == nullptr ? nullptr : &scene->weather.colorGradingMap->texture) : nullptr,
				nullptr,
				getEyeAdaptionEnabled() ? wiRenderer::ComputeLuminance(luminanceResources, *GetGbuffer_Read(GBUFFER_COLOR), cmd, getEyeAdaptionRate()) : nullptr,
				getEyeAdaptionKey(),
				&rtLinearDepth
			);
#endif
#ifdef GGREDUCED
		}
		else
		{
			//PE: Needed.
			wiRenderer::Postprocess_Tonemap(
				rt_first == nullptr ? *rt_read : *rt_first,
				*rt_write,
				cmd,
				getExposure(),
				getDitherEnabled(),
				nullptr,
				getMSAASampleCount() > 1 ? &rtParticleDistortion_Resolved : &rtParticleDistortion,
				getEyeAdaptionEnabled() ? wiRenderer::ComputeLuminance(luminanceResources, *GetGbuffer_Read(GBUFFER_COLOR), cmd, getEyeAdaptionRate()) : nullptr,
				getEyeAdaptionKey(),
				&rtLinearDepth
			);
		}
#endif

		rt_first = nullptr;
		rt_read = rt_write;
		rt_write = &rtPostprocess_LDR[1];
		device->UnbindResources(TEXSLOT_ONDEMAND0, 1, cmd);
	}

	// 3.) LDR post process chain
	{
		if (getSharpenFilterEnabled())
		{
			wiRenderer::Postprocess_Sharpen(*rt_read, *rt_write, cmd, getSharpenFilterAmount());

			std::swap(rt_read, rt_write);
			device->UnbindResources(TEXSLOT_ONDEMAND0, 1, cmd);
		}

		#ifdef GGREDUCED
		if (getRainEnabled())
		{
			//PE: Test rain shader.
			if (!(rainTextureMap == nullptr || rainNormalMap == nullptr))
			{
				wiRenderer::Postprocess_Rain(*rt_read, *rt_write, cmd, rainTextureMap->texture, rainNormalMap->texture, rainOpacity, rainScaleX, rainScaleY, rainOffsetX, rainOffsetY, rainRefreactionScale);
				std::swap(rt_read, rt_write);
				device->UnbindResources(TEXSLOT_ONDEMAND0, 1, cmd);
				device->UnbindResources(TEXSLOT_ONDEMAND1, 1, cmd);
				device->UnbindResources(TEXSLOT_ONDEMAND2, 1, cmd);
			}
		}
		if (getSnowEnabled())
		{
			//PE: Test snow shader.
			wiRenderer::Postprocess_Snow(*rt_read, *rt_write, cmd, snowOpacity, snowLayers, snowDepth, snowWindiness, snowSpeed, snowOffset);
			std::swap(rt_read, rt_write);
			device->UnbindResources(TEXSLOT_ONDEMAND0, 1, cmd);
			device->UnbindResources(TEXSLOT_ONDEMAND1, 1, cmd);
			device->UnbindResources(TEXSLOT_ONDEMAND2, 1, cmd);
		}
		#endif

		if (getFXAAEnabled())
		{
			wiRenderer::Postprocess_FXAA(*rt_read, *rt_write, cmd);

			std::swap(rt_read, rt_write);
			device->UnbindResources(TEXSLOT_ONDEMAND0, 1, cmd);
		}

		if (getChromaticAberrationEnabled())
		{
			wiRenderer::Postprocess_Chromatic_Aberration(*rt_read, *rt_write, cmd, getChromaticAberrationAmount());

			std::swap(rt_read, rt_write);
			device->UnbindResources(TEXSLOT_ONDEMAND0, 1, cmd);
		}

#ifndef GGREDUCED
		// GUI Background blurring:
		{
			auto range = wiProfiler::BeginRangeGPU("GUI Background Blur", cmd);
			device->EventBegin("GUI Background Blur", cmd);
			wiRenderer::Postprocess_Downsample4x(*rt_read, rtGUIBlurredBackground[0], cmd);
			wiRenderer::Postprocess_Downsample4x(rtGUIBlurredBackground[0], rtGUIBlurredBackground[2], cmd);
			wiRenderer::Postprocess_Blur_Gaussian(rtGUIBlurredBackground[2], rtGUIBlurredBackground[1], rtGUIBlurredBackground[2], cmd, -1, -1, true);
			device->EventEnd(cmd);
			wiProfiler::EndRange(range);
		}
#endif

		if (rtFSR[0].IsValid() && getFSREnabled())
		{
			wiRenderer::Postprocess_FSR(*rt_read, rtFSR[1], rtFSR[0], cmd, getFSRSharpness());

			device->UnbindResources(TEXSLOT_ONDEMAND0, 1, cmd);
		}
	}
}

void RenderPath3D::setAO(AO value)
{
	ao = value;

	rtAO = {};
	ssaoResources = {};
	msaoResources = {};
	rtaoResources = {};

	if (ao == AO_DISABLED)
	{
		return;
	}

	XMUINT2 internalResolution;
	internalResolution.x = GetWidth3D();
	internalResolution.y = GetHeight3D();

	TextureDesc desc;
	desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
	desc.Format = FORMAT_R8_UNORM;
	desc.layout = IMAGE_LAYOUT_SHADER_RESOURCE_COMPUTE;

	switch (ao)
	{
	case RenderPath3D::AO_SSAO:
	case RenderPath3D::AO_HBAO:
		desc.Width = internalResolution.x / 2;
		desc.Height = internalResolution.y / 2;
		wiRenderer::CreateSSAOResources(ssaoResources, internalResolution);
		break;
	case RenderPath3D::AO_MSAO:
		desc.Width = internalResolution.x;
		desc.Height = internalResolution.y;
		wiRenderer::CreateMSAOResources(msaoResources, internalResolution);
		break;
	case RenderPath3D::AO_RTAO:
		desc.Width = internalResolution.x / 2;
		desc.Height = internalResolution.y / 2;
		wiRenderer::CreateRTAOResources(rtaoResources, internalResolution);
		break;
	default:
		break;
	}

	GraphicsDevice* device = wiRenderer::GetDevice();
	device->CreateTexture(&desc, nullptr, &rtAO);
	device->SetName(&rtAO, "rtAO");
}

void RenderPath3D::setRainTextures(char *tex, char *normal)
{
	if (tex && normal)
	{
		rainTextureMap = wiResourceManager::Load(tex);
		rainNormalMap = wiResourceManager::Load(normal);
	}
}

void RenderPath3D::setSSREnabled(bool value)
{
	ssrEnabled = value;

	if (value)
	{
		GraphicsDevice* device = wiRenderer::GetDevice();
		XMUINT2 internalResolution;
		internalResolution.x = GetWidth3D();
		internalResolution.y = GetHeight3D();

		TextureDesc desc;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.Format = FORMAT_R16G16B16A16_FLOAT;
		desc.Width = internalResolution.x / 2;
		desc.Height = internalResolution.y / 2;
		device->CreateTexture(&desc, nullptr, &rtSSR);
		device->SetName(&rtSSR, "rtSSR");

		wiRenderer::CreateSSRResources(ssrResources, internalResolution);
	}
	else
	{
		ssrResources = {};
	}
}

void RenderPath3D::setRaytracedReflectionsEnabled(bool value)
{
	raytracedReflectionsEnabled = value;

	if (value)
	{
		GraphicsDevice* device = wiRenderer::GetDevice();
		XMUINT2 internalResolution;
		internalResolution.x = GetWidth3D();
		internalResolution.y = GetHeight3D();

		TextureDesc desc;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.Format = FORMAT_R11G11B10_FLOAT;
		desc.Width = internalResolution.x / 2;
		desc.Height = internalResolution.y / 2;
		device->CreateTexture(&desc, nullptr, &rtSSR);
		device->SetName(&rtSSR, "rtSSR");

		wiRenderer::CreateRTReflectionResources(rtreflectionResources, internalResolution);
	}
	else
	{
		rtreflectionResources = {};
	}
}

void RenderPath3D::setFSREnabled(bool value)
{
	fsrEnabled = value;
	
	//if (resolutionScale < 1.0f && fsrEnabled)
	if (GetFSRScale() != 1.0f && fsrEnabled) //PE: GGREDUCED
	{
		GraphicsDevice* device = wiRenderer::GetDevice();

		TextureDesc desc;
		desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
		desc.Format = rtPostprocess_LDR[0].desc.Format;
		desc.Width = GetWidth3D() * GetFSRScale();
		desc.Height = GetHeight3D() * GetFSRScale();
		device->CreateTexture(&desc, nullptr, &rtFSR[0]);
		device->SetName(&rtFSR[0], "rtFSR[0]");
		device->CreateTexture(&desc, nullptr, &rtFSR[1]);
		device->SetName(&rtFSR[1], "rtFSR[1]");
	}
	else
	{
		rtFSR[0] = {};
		rtFSR[1] = {};
	}
}
