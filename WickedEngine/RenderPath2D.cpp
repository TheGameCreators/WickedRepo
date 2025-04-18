#include "RenderPath2D.h"
#include "wiResourceManager.h"
#include "wiSprite.h"
#include "wiSpriteFont.h"
#include "wiRenderer.h"

#ifdef GGREDUCED
//PE: We do not use any text/sprite/gui function in wicked so:
#define DISABLERTFINAL
#ifdef OPTICK_ENABLE
#include "optick.h"
#endif
#endif

#ifdef GGREDUCED
void ImGuiHook_RenderCall(void* ctx);

namespace GGTerrain {
	extern "C" void GGTerrain_Draw_Debug( wiGraphics::CommandList cmd );
	extern "C" void __GGTerrain_Draw_Debug_EMPTY( wiGraphics::CommandList cmd ) {}
	// use GGTerrain_Draw_Debug() if it is defined, otherwise use __GGTerrain_Draw_Debug_EMPTY()
	#pragma comment(linker, "/alternatename:GGTerrain_Draw_Debug=__GGTerrain_Draw_Debug_EMPTY")

	extern "C" void GGTerrain_Draw_Overlay( wiGraphics::CommandList cmd );
	extern "C" void __GGTerrain_Draw_Overlay_EMPTY( wiGraphics::CommandList cmd ) {}
	#pragma comment(linker, "/alternatename:GGTerrain_Draw_Overlay=__GGTerrain_Draw_Overlay_EMPTY")
}
#endif

using namespace wiGraphics;

void RenderPath2D::ResizeBuffers()
{
	current_buffersize = GetInternalResolution();
	current_layoutscale = 0; // invalidate layout

	GraphicsDevice* device = wiRenderer::GetDevice();

	const Texture* dsv = GetDepthStencil();
	if(dsv != nullptr && (resolutionScale != 1.0f ||  dsv->GetDesc().SampleCount > 1))
	{
		TextureDesc desc = GetDepthStencil()->GetDesc();
		desc.layout = IMAGE_LAYOUT_SHADER_RESOURCE;
		desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
		desc.Format = FORMAT_R8G8B8A8_UNORM;
		device->CreateTexture(&desc, nullptr, &rtStenciled);
		device->SetName(&rtStenciled, "rtStenciled");

		if (desc.SampleCount > 1)
		{
			desc.SampleCount = 1;
			device->CreateTexture(&desc, nullptr, &rtStenciled_resolved);
			device->SetName(&rtStenciled_resolved, "rtStenciled_resolved");
		}
	}
	else
	{
		rtStenciled = Texture(); // this will be deleted here
	}

#ifndef DISABLERTFINAL
	{
		TextureDesc desc;
		desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
		desc.Format = FORMAT_R8G8B8A8_UNORM;
		desc.Width = GetPhysicalWidth();
		desc.Height = GetPhysicalHeight();
		device->CreateTexture(&desc, nullptr, &rtFinal);
		device->SetName(&rtFinal, "rtFinal");
	}

	if (rtStenciled.IsValid())
	{
		RenderPassDesc desc;
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtStenciled, RenderPassAttachment::LOADOP_CLEAR));
		desc.attachments.push_back(
			RenderPassAttachment::DepthStencil(
				dsv,
				RenderPassAttachment::LOADOP_LOAD,
				RenderPassAttachment::STOREOP_STORE,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
				IMAGE_LAYOUT_DEPTHSTENCIL_READONLY
			)
		);

		if (rtStenciled.GetDesc().SampleCount > 1)
		{
			desc.attachments.push_back(RenderPassAttachment::Resolve(&rtStenciled_resolved));
		}

		device->CreateRenderPass(&desc, &renderpass_stenciled);

		dsv = nullptr;
	}
	{
		RenderPassDesc desc;
		desc.attachments.push_back(RenderPassAttachment::RenderTarget(&rtFinal, RenderPassAttachment::LOADOP_CLEAR));
		
		if(dsv != nullptr && !rtStenciled.IsValid())
		{
			desc.attachments.push_back(
				RenderPassAttachment::DepthStencil(
					dsv,
					RenderPassAttachment::LOADOP_LOAD,
					RenderPassAttachment::STOREOP_STORE,
					IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
					IMAGE_LAYOUT_DEPTHSTENCIL_READONLY,
					IMAGE_LAYOUT_DEPTHSTENCIL_READONLY
				)
			);
		}

		device->CreateRenderPass(&desc, &renderpass_final);
	}
#endif

}
void RenderPath2D::ResizeLayout()
{
	current_layoutscale = GetDPIScaling();
}

void RenderPath2D::Update(float dt)
{
	XMUINT2 internalResolution = GetInternalResolution();

	if (current_buffersize.x != internalResolution.x || current_buffersize.y != internalResolution.y)
	{
		ResizeBuffers();
	}
	if (current_layoutscale != GetDPIScaling())
	{
		ResizeLayout();
	}

	GetGUI().Update(*this, dt);

	for (auto& x : layers)
	{
		for (auto& y : x.items)
		{
			switch (y.type)
			{
			default:
			case RenderItem2D::SPRITE:
				if (y.sprite != nullptr)
				{
					y.sprite->Update(dt);
				}
				break;
			case RenderItem2D::FONT:
				if (y.font != nullptr)
				{
					y.font->Update(dt);
				}
				break;
			}
		}
	}

	RenderPath::Update(dt);
}
void RenderPath2D::FixedUpdate()
{
	for (auto& x : layers)
	{
		for (auto& y : x.items)
		{
			switch (y.type)
			{
			default:
			case RenderItem2D::SPRITE:
				if (y.sprite != nullptr)
				{
					y.sprite->FixedUpdate();
				}
				break;
			case RenderItem2D::FONT:
				if (y.font != nullptr)
				{
					y.font->FixedUpdate();
				}
				break;
			}
		}
	}

	RenderPath::FixedUpdate();
}
void RenderPath2D::Render( int mode ) const
{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
	OPTICK_EVENT();
#endif
#endif

	GraphicsDevice* device = wiRenderer::GetDevice();
	CommandList cmd = device->BeginCommandList();
	wiImage::SetCanvas(*this, cmd);
	wiFont::SetCanvas(*this, cmd);

	wiRenderer::ProcessDeferredMipGenRequests(cmd);

	if (GetGUIBlurredBackground() != nullptr)
	{
		wiImage::SetBackground(*GetGUIBlurredBackground(), cmd);
	}

	// Special care for internal resolution, because stencil buffer is of internal resolution, 
	//	so we might need to render stencil sprites to separate render target that matches internal resolution!
#ifndef DISABLERTFINAL
	if (rtStenciled.IsValid())
	{
		device->RenderPassBegin(&renderpass_stenciled, cmd);

		Viewport vp;
		vp.Width = (float)rtStenciled.GetDesc().Width;
		vp.Height = (float)rtStenciled.GetDesc().Height;
		device->BindViewports(1, &vp, cmd);

		wiRenderer::GetDevice()->EventBegin("STENCIL Sprite Layers", cmd);
		for (auto& x : layers)
		{
			for (auto& y : x.items)
			{
				if (y.type == RenderItem2D::SPRITE &&
					y.sprite != nullptr &&
					y.sprite->params.stencilComp != STENCILMODE_DISABLED)
				{
					y.sprite->Draw(cmd);
				}
			}
		}
		wiRenderer::GetDevice()->EventEnd(cmd);

		device->RenderPassEnd(cmd);
	}
#endif

#ifndef DISABLERTFINAL
	device->RenderPassBegin(&renderpass_final, cmd);

	Viewport vp;
	vp.Width = (float)rtFinal.GetDesc().Width;
	vp.Height = (float)rtFinal.GetDesc().Height;
	device->BindViewports(1, &vp, cmd);

	if (GetDepthStencil() != nullptr)
	{
		if (rtStenciled.IsValid())
		{
			wiRenderer::GetDevice()->EventBegin("Copy STENCIL Sprite Layers", cmd);
			wiImageParams fx;
			fx.enableFullScreen();
			if (rtStenciled.GetDesc().SampleCount > 1)
			{
				wiImage::Draw(&rtStenciled_resolved, fx, cmd);
			}
			else
			{
				wiImage::Draw(&rtStenciled, fx, cmd);
			}
			wiRenderer::GetDevice()->EventEnd(cmd);
		}
		else
		{
			wiRenderer::GetDevice()->EventBegin("STENCIL Sprite Layers", cmd);
			for (auto& x : layers)
			{
				for (auto& y : x.items)
				{
					if (y.type == RenderItem2D::SPRITE &&
						y.sprite != nullptr &&
						y.sprite->params.stencilComp != STENCILMODE_DISABLED)
					{
						y.sprite->Draw(cmd);
					}
				}
			}
			wiRenderer::GetDevice()->EventEnd(cmd);
		}
	}

	wiRenderer::GetDevice()->EventBegin("Sprite Layers", cmd);
	for (auto& x : layers)
	{
		for (auto& y : x.items)
		{
			switch (y.type)
			{
			default:
			case RenderItem2D::SPRITE:
				if (y.sprite != nullptr && y.sprite->params.stencilComp == STENCILMODE_DISABLED)
				{
					y.sprite->Draw(cmd);
				}
				break;
			case RenderItem2D::FONT:
				if (y.font != nullptr)
				{
					y.font->Draw(cmd);
				}
				break;
			}
		}
	}
	wiRenderer::GetDevice()->EventEnd(cmd);

	GetGUI().Render(*this, cmd);

	device->RenderPassEnd(cmd);
#endif

	RenderPath::Render( mode );
}
void RenderPath2D::Compose(CommandList cmd) const
{
#ifdef GGREDUCED
//  Moved to ImGuiHook_RenderCall so we get overlay.
//	extern bool g_bNo2DRender;
//	if (g_bNo2DRender)
//		return;
#endif

#ifndef DISABLERTFINAL
	wiImageParams fx;
	fx.enableFullScreen();
	fx.blendFlag = BLENDMODE_PREMULTIPLIED;

	wiImage::Draw(&rtFinal, fx, cmd);
#endif

#ifdef GGREDUCED

	void Wicked_Render_Opaque_Scene(CommandList cmd);
	Wicked_Render_Opaque_Scene(cmd);

	extern bool g_bNoTerrainRender;
	if (!g_bNoTerrainRender)
	{
		GGTerrain::GGTerrain_Draw_Overlay(cmd);
	}

	// hook back to main app to allow it to render IMGUI IDE
	GraphicsDevice* device = wiRenderer::GetDevice();
	ImGuiHook_RenderCall((void*)device->GetDeviceContext(cmd));

	if (!g_bNoTerrainRender)
	{
		GGTerrain::GGTerrain_Draw_Debug(cmd);
	}
#endif

	RenderPath::Compose(cmd);
}


void RenderPath2D::AddSprite(wiSprite* sprite, const std::string& layer)
{
	for (auto& x : layers)
	{
		if (!x.name.compare(layer))
		{
			x.items.push_back(RenderItem2D());
			x.items.back().type = RenderItem2D::SPRITE;
			x.items.back().sprite = sprite;
		}
	}
	SortLayers();
}
void RenderPath2D::RemoveSprite(wiSprite* sprite)
{
	for (auto& x : layers)
	{
		for (auto& y : x.items)
		{
			if (y.type == RenderItem2D::SPRITE && y.sprite == sprite)
			{
				y.sprite = nullptr;
			}
		}
	}
	CleanLayers();
}
void RenderPath2D::ClearSprites()
{
	for (auto& x : layers)
	{
		for (auto& y : x.items)
		{
			if (y.type == RenderItem2D::SPRITE)
			{
				y.sprite = nullptr;
			}
		}
	}
	CleanLayers();
}
int RenderPath2D::GetSpriteOrder(wiSprite* sprite)
{
	for (auto& x : layers)
	{
		for (auto& y : x.items)
		{
			if (y.sprite == sprite)
			{
				return y.order;
			}
		}
	}
	return 0;
}

void RenderPath2D::AddFont(wiSpriteFont* font, const std::string& layer)
{
	for (auto& x : layers)
	{
		if (!x.name.compare(layer))
		{
			x.items.push_back(RenderItem2D());
			x.items.back().type = RenderItem2D::FONT;
			x.items.back().font = font;
		}
	}
	SortLayers();
}
void RenderPath2D::RemoveFont(wiSpriteFont* font)
{
	for (auto& x : layers)
	{
		for (auto& y : x.items)
		{
			if (y.type == RenderItem2D::FONT && y.font == font)
			{
				y.font = nullptr;
			}
		}
	}
	CleanLayers();
}
void RenderPath2D::ClearFonts()
{
	for (auto& x : layers)
	{
		for (auto& y : x.items)
		{
			if (y.type == RenderItem2D::FONT)
			{
				y.font = nullptr;
			}
		}
	}
	CleanLayers();
}
int RenderPath2D::GetFontOrder(wiSpriteFont* font)
{
	for (auto& x : layers)
	{
		for (auto& y : x.items)
		{
			if (y.font == font)
			{
				return y.order;
			}
		}
	}
	return 0;
}


void RenderPath2D::AddLayer(const std::string& name)
{
	for (auto& x : layers)
	{
		if (!x.name.compare(name))
			return;
	}
	RenderLayer2D layer;
	layer.name = name;
	layer.order = (int)layers.size();
	layers.push_back(layer);
	layers.back().items.clear();
}
void RenderPath2D::SetLayerOrder(const std::string& name, int order)
{
	for (auto& x : layers)
	{
		if (!x.name.compare(name))
		{
			x.order = order;
			break;
		}
	}
	SortLayers();
}
void RenderPath2D::SetSpriteOrder(wiSprite* sprite, int order)
{
	for (auto& x : layers)
	{
		for (auto& y : x.items)
		{
			if (y.type == RenderItem2D::SPRITE && y.sprite == sprite)
			{
				y.order = order;
			}
		}
	}
	SortLayers();
}
void RenderPath2D::SetFontOrder(wiSpriteFont* font, int order)
{
	for (auto& x : layers)
	{
		for (auto& y : x.items)
		{
			if (y.type == RenderItem2D::FONT && y.font == font)
			{
				y.order = order;
			}
		}
	}
	SortLayers();
}
void RenderPath2D::SortLayers()
{
	if (layers.empty())
	{
		return;
	}

	for (size_t i = 0; i < layers.size() - 1; ++i)
	{
		for (size_t j = i + 1; j < layers.size(); ++j)
		{
			if (layers[i].order > layers[j].order)
			{
				RenderLayer2D swap = layers[i];
				layers[i] = layers[j];
				layers[j] = swap;
			}
		}
	}
	for (auto& x : layers)
	{
		if (x.items.empty())
		{
			continue;
		}
		for (size_t i = 0; i < x.items.size() - 1; ++i)
		{
			for (size_t j = i + 1; j < x.items.size(); ++j)
			{
				if (x.items[i].order > x.items[j].order)
				{
					RenderItem2D swap = x.items[i];
					x.items[i] = x.items[j];
					x.items[j] = swap;
				}
			}
		}
	}
}

void RenderPath2D::CleanLayers()
{
	for (auto& x : layers)
	{
		if (x.items.empty())
		{
			continue;
		}
		std::vector<RenderItem2D> itemsToRetain(0);
		for (auto& y : x.items)
		{
			if (y.sprite != nullptr || y.font!=nullptr)
			{
				itemsToRetain.push_back(y);
			}
		}
		x.items.clear();
		x.items = itemsToRetain;
	}
}
