#include "MainComponent.h"
#include "RenderPath.h"
#include "wiRenderer.h"
#include "wiHelper.h"
#include "wiTimer.h"
#include "wiInput.h"
#include "wiBackLog.h"
#include "MainComponent_BindLua.h"
#include "wiVersion.h"
#include "wiEnums.h"
#include "wiTextureHelper.h"
#include "wiProfiler.h"
#include "wiInitializer.h"
#include "wiStartupArguments.h"
#include "wiFont.h"
#include "wiImage.h"
#include "wiEvent.h"

#include "wiGraphicsDevice_DX11.h"
#include "wiGraphicsDevice_DX12.h"
#include "wiGraphicsDevice_Vulkan.h"

#include "Utility/replace_new.h"

#include <sstream>
#include <algorithm>

using namespace wiGraphics;

void MainComponent::Initialize()
{
	if (initialized)
	{
		return;
	}
	initialized = true;

	wiInitializer::InitializeComponentsAsync();
}

void MainComponent::ActivatePath(RenderPath* component, float fadeSeconds, wiColor fadeColor)
{
	if (component != nullptr)
	{
		component->init(canvas);
	}

	// Fade manager will activate on fadeout
	fadeManager.Clear();
	fadeManager.Start(fadeSeconds, fadeColor, [this, component]() {

		if (GetActivePath() != nullptr)
		{
			GetActivePath()->Stop();
		}

		if (component != nullptr)
		{
			component->Start();
		}
		activePath = component;
	});

	fadeManager.Update(0); // If user calls ActivatePath without fadeout, it will be instant
}

void MainComponent::Run()
{
	if (!initialized)
	{
		// Initialize in a lazy way, so the user application doesn't have to call this explicitly
		Initialize();
		initialized = true;
	}
	if (!wiInitializer::IsInitializeFinished())
	{
		#if GGREDUCED
		// we do this elsewhere now
		#else
		// Until engine is not loaded, present initialization screen...
		CommandList cmd = wiRenderer::GetDevice()->BeginCommandList();
		wiRenderer::GetDevice()->RenderPassBegin(&swapChain, cmd);
		wiImage::SetCanvas(canvas, cmd);
		wiFont::SetCanvas(canvas, cmd);
		Viewport viewport;
		viewport.Width = (float)swapChain.desc.width;
		viewport.Height = (float)swapChain.desc.height;
		wiRenderer::GetDevice()->BindViewports(1, &viewport, cmd);
		wiFontParams params;
		params.posX = 5.f;
		params.posY = 5.f;
		std::string text = wiBackLog::getText();
		float textheight = wiFont::textHeight(text, params);
		float screenheight = canvas.GetLogicalHeight();
		if (textheight > screenheight)
		{
			params.posY = screenheight - textheight;
		}
		wiFont::Draw(text, params, cmd);
		wiRenderer::GetDevice()->RenderPassEnd(cmd);
		wiRenderer::GetDevice()->SubmitCommandLists();
		#endif
		return;
	}

#ifndef GGREDUCED
	static bool startup_script = false;
	if (!startup_script)
	{
		startup_script = true;
		wiLua::RegisterObject(MainComponent_BindLua::className, "main", new MainComponent_BindLua(this));
		wiLua::RunFile("startup.lua");
	}
#endif

	wiProfiler::BeginFrame();

	deltaTime = float(std::max(0.0, timer.elapsed() / 1000.0));
#ifdef GGREDUCED
	//LB: if leave app, timer.elapsed() is going to be huge on returning, causing weird animation issues
	// so cap this to a maximum so it reduces the glitch the user sees (without causing anything over 30fps to misbehave)
	float dThirtyFPSinMS = (1.0f / 30.0f);
	if (deltaTime > dThirtyFPSinMS) deltaTime = dThirtyFPSinMS;
#endif
	timer.record();

#ifdef GGREDUCED
	//PE: We need to run always, as we can have many windows running at the same time.
	if (1)
#else
	if (is_window_active)
#endif
	{
		// If the application is active, run Update loops:

		// Wake up the events that need to be executed on the main thread, in thread safe manner:
		wiEvent::FireEvent(SYSTEM_EVENT_THREAD_SAFE_POINT, 0);

		const float dt = framerate_lock ? (1.0f / targetFrameRate) : deltaTime;

		fadeManager.Update(dt);

		if (GetActivePath() != nullptr)
		{
			GetActivePath()->init(canvas);
			GetActivePath()->PreUpdate();
		}

		// Fixed time update:
		auto range = wiProfiler::BeginRangeCPU("Fixed Update");
		{
			if (frameskip)
			{
				deltaTimeAccumulator += dt;
				if (deltaTimeAccumulator > 10)
				{
					// application probably lost control, fixed update would take too long
					deltaTimeAccumulator = 0;
				}

				const float targetFrameRateInv = 1.0f / targetFrameRate;
				while (deltaTimeAccumulator >= targetFrameRateInv)
				{
					FixedUpdate();
					deltaTimeAccumulator -= targetFrameRateInv;
				}
			}
			else
			{
				FixedUpdate();
			}
		}
		wiProfiler::EndRange(range); // Fixed Update

		// Variable-timed update:
		Update(dt);

		Render( 0 );
	}
	else
	{
		// If the application is not active, disable Update loops:
		deltaTimeAccumulator = 0;
	}

	wiInput::Update(window);

	#ifdef GGREDUCED
	if (is_completely_loaded == false)
	{
		// we can present a loading splash while things load - done elsewhere in master.cpp
	}
	else
	{
	#endif
	CommandList cmd = wiRenderer::GetDevice()->BeginCommandList();
	wiRenderer::GetDevice()->RenderPassBegin(&swapChain, cmd);
	{
		wiImage::SetCanvas(canvas, cmd);
		wiFont::SetCanvas(canvas, cmd);
		Viewport viewport;
		viewport.Width = (float)swapChain.desc.width;
		viewport.Height = (float)swapChain.desc.height;
		wiRenderer::GetDevice()->BindViewports(1, &viewport, cmd);
		Compose(cmd);
		wiRenderer::GetDevice()->RenderPassEnd(cmd);
		wiProfiler::EndFrame(cmd);
		wiRenderer::GetDevice()->SubmitCommandLists();
	}
	#ifdef GGREDUCED
	}
	#endif
}

void MainComponent::Update(float dt)
{
	auto range = wiProfiler::BeginRangeCPU("Update");

#ifndef GGREDUCED
	wiLua::SetDeltaTime(double(dt));
	wiLua::Update();
#endif

	if (GetActivePath() != nullptr)
	{
		GetActivePath()->Update(dt);
		GetActivePath()->PostUpdate();
	}

	wiProfiler::EndRange(range); // Update
}

void MainComponent::FixedUpdate()
{
	wiBackLog::Update(canvas);
#ifndef GGREDUCED
	wiLua::FixedUpdate();
#endif

	if (GetActivePath() != nullptr)
	{
		GetActivePath()->FixedUpdate();
	}
}

void MainComponent::Render( int mode )
{
	auto range = wiProfiler::BeginRangeCPU("Render");

#ifndef GGREDUCED
	wiLua::Render();
#endif

	if (GetActivePath() != nullptr)
	{
		GetActivePath()->Render( mode );
	}

	wiProfiler::EndRange(range); // Render
}

void MainComponent::Compose(CommandList cmd)
{
	auto range = wiProfiler::BeginRangeCPU("Compose");

	if (GetActivePath() != nullptr)
	{
		GetActivePath()->Compose(cmd);
	}

	GraphicsDevice* device = wiRenderer::GetDevice();

	if (fadeManager.IsActive())
	{
		// display fade rect
		static wiImageParams fx;
		fx.siz.x = canvas.GetLogicalWidth();
		fx.siz.y = canvas.GetLogicalHeight();
		fx.opacity = fadeManager.opacity;
		wiImage::Draw(wiTextureHelper::getColor(fadeManager.color), fx, cmd);
	}

	// Draw the information display
	if (infoDisplay.active)
	{
		std::stringstream ss("");
		if (infoDisplay.watermark)
		{
			ss << "Wicked Engine " << wiVersion::GetVersionString() << " ";

#if defined(_ARM)
			ss << "[ARM]";
#elif defined(_WIN64)
			ss << "[64-bit]";
#elif defined(_WIN32)
			ss << "[32-bit]";
#endif

#ifdef PLATFORM_UWP
			ss << "[UWP]";
#endif

#ifdef WICKEDENGINE_BUILD_DX11
			if (dynamic_cast<GraphicsDevice_DX11*>(device))
			{
				ss << "[DX11]";
			}
#endif
#ifdef WICKEDENGINE_BUILD_DX12
			if (dynamic_cast<GraphicsDevice_DX12*>(device))
			{
				ss << "[DX12]";
			}
#endif
#ifdef WICKEDENGINE_BUILD_VULKAN
			if (dynamic_cast<GraphicsDevice_Vulkan*>(device))
			{
				ss << "[Vulkan]";
			}
#endif

#ifdef _DEBUG
			ss << "[DEBUG]";
#endif
			if (device->IsDebugDevice())
			{
				ss << "[debugdevice]";
			}
			ss << std::endl;
		}
		if (infoDisplay.resolution)
		{
			ss << "Resolution: " << canvas.GetPhysicalWidth() << " x " << canvas.GetPhysicalHeight() << " (" << canvas.GetDPI() << " dpi)" << std::endl;
		}
		if (infoDisplay.fpsinfo)
		{
			deltatimes[fps_avg_counter++ % arraysize(deltatimes)] = deltaTime;
			float displaydeltatime = deltaTime;
			if (fps_avg_counter > arraysize(deltatimes))
			{
				float avg_time = 0;
				for (int i = 0; i < arraysize(deltatimes); ++i)
				{
					avg_time += deltatimes[i];
				}
				displaydeltatime = avg_time / arraysize(deltatimes);
			}

			ss.precision(2);
			ss << std::fixed << 1.0f / displaydeltatime << " FPS" << std::endl;
		}
		if (infoDisplay.heap_allocation_counter)
		{
			ss << "Heap allocations per frame: " << number_of_allocs.load() << std::endl;
			number_of_allocs.store(0);
		}

#ifdef _DEBUG
		ss << "Warning: This is a [DEBUG] build, performance will be slow!" << std::endl;
#endif
		if (wiRenderer::GetDevice()->IsDebugDevice())
		{
			ss << "Warning: Graphics is in [debugdevice] mode, performance will be slow!" << std::endl;
		}

		ss.precision(2);
		wiFont::Draw(ss.str(), wiFontParams(4, 4, infoDisplay.size, WIFALIGN_LEFT, WIFALIGN_TOP, wiColor(255,255,255,255), wiColor(0,0,0,255)), cmd);

		if (infoDisplay.colorgrading_helper)
		{
			wiImage::Draw(wiTextureHelper::getColorGradeDefault(), wiImageParams(0, 0, 256.0f / canvas.GetDPIScaling(), 16.0f / canvas.GetDPIScaling()), cmd);
		}
	}

	wiProfiler::DrawData(canvas, 4, 120, cmd);

	wiBackLog::Draw(canvas, cmd);

	wiProfiler::EndRange(range); // Compose
}

void MainComponent::SetWindow(wiPlatform::window_type window, bool fullscreen)
{
	this->window = window;

	// User can also create a graphics device if custom logic is desired, but they must do before this function!
	if (wiRenderer::GetDevice() == nullptr)
	{
		bool debugdevice = false;//wiStartupArguments::HasArgument("debugdevice");
		bool gpuvalidation = false;//wiStartupArguments::HasArgument("gpuvalidation");

#ifdef _DEBUG
		debugdevice = true;
#endif

		//bool use_dx11 = wiStartupArguments::HasArgument("dx11");
		//bool use_dx12 = wiStartupArguments::HasArgument("dx12");
		//bool use_vulkan = wiStartupArguments::HasArgument("vulkan");

		bool use_dx11 = false;
		bool use_dx12 = true;
		bool use_vulkan = false;

#ifndef WICKEDENGINE_BUILD_DX11
		if (use_dx11) {
			wiHelper::messageBox("The engine was built without DX11 support!", "Error");
			use_dx11 = false;
		}
#endif
#ifndef WICKEDENGINE_BUILD_DX12
		if (use_dx12) {
			wiHelper::messageBox("The engine was built without DX12 support!", "Error");
			use_dx12 = false;
		}
#endif
#ifndef WICKEDENGINE_BUILD_VULKAN
		if (use_vulkan) {
			wiHelper::messageBox("The engine was built without Vulkan support!", "Error");
			use_vulkan = false;
		}
#endif

		if (!use_dx11 && !use_dx12 && !use_vulkan)
		{
#if defined(WICKEDENGINE_BUILD_DX11)
			use_dx11 = true;
#elif defined(WICKEDENGINE_BUILD_DX12)
			use_dx12 = true;
#elif defined(WICKEDENGINE_BUILD_VULKAN)
			use_vulkan = true;
#else
			wiBackLog::post("No rendering backend is enabled! Please enable at least one so we can use it as default");
			assert(false);
#endif
		}
		assert(use_dx11 || use_dx12 || use_vulkan);

		if (use_vulkan)
		{
#ifdef WICKEDENGINE_BUILD_VULKAN
			wiRenderer::SetShaderPath(wiRenderer::GetShaderPath() + "spirv/");
			wiRenderer::SetDevice(std::make_shared<GraphicsDevice_Vulkan>(window, debugdevice));
#endif
		}
		else if (use_dx12)
		{
#ifdef WICKEDENGINE_BUILD_DX12
			wiRenderer::SetShaderPath(wiRenderer::GetShaderPath() + "hlsl6/");
			wiRenderer::SetDevice(std::make_shared<GraphicsDevice_DX12>(debugdevice, gpuvalidation));
#endif
		}
		else if (use_dx11)
		{
#ifdef WICKEDENGINE_BUILD_DX11
			wiRenderer::SetShaderPath(wiRenderer::GetShaderPath() + "hlsl5/");
			wiRenderer::SetDevice(std::make_shared<GraphicsDevice_DX11>(debugdevice));
#endif
		}
	}

	canvas.init(window);

	SwapChainDesc desc;
	desc.width = canvas.GetPhysicalWidth();
	desc.height = canvas.GetPhysicalHeight();
	desc.buffercount = 3;
	desc.format = FORMAT_R10G10B10A2_UNORM;
	desc.vsync = swapChain.desc.vsync;
	bool success = wiRenderer::GetDevice()->CreateSwapChain(&desc, window, &swapChain);
	assert(success);

	swapChainVsyncChangeEvent = wiEvent::Subscribe(SYSTEM_EVENT_SET_VSYNC, [this](uint64_t userdata) {
		SwapChainDesc desc = swapChain.desc;
		desc.vsync = userdata != 0;
		bool success = wiRenderer::GetDevice()->CreateSwapChain(&desc, nullptr, &swapChain);
		assert(success);
	});
}
