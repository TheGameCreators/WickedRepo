#pragma once
#include "RenderPath2D.h"
#include "wiRenderer.h"
#include "wiGraphicsDevice.h"
#include "wiResourceManager.h"
#include "wiScene.h"

#include <memory>

#define EYE_LEFT 1
#define EYE_RIGHT 2

class RenderPath3D :
	public RenderPath2D
{
public:
	enum AO 
	{
		AO_DISABLED,	// no ambient occlusion
		AO_SSAO,		// simple brute force screen space ambient occlusion
		AO_HBAO,		// horizon based screen space ambient occlusion
		AO_MSAO,		// multi scale screen space ambient occlusion
		AO_RTAO,		// ray traced ambient occlusion
		// Don't alter order! (bound to lua manually)
	};
private:
	float exposure = 1.0f;
	float bloomThreshold = 1.0f;
	float bloomStrength = 1.0f;
	float motionBlurStrength = 100.0f;
	float dofStrength = 10.0f;
	float sharpenFilterAmount = 0.28f;
	float outlineThreshold = 0.2f;
	float outlineThickness = 1.0f;
	XMFLOAT4 outlineColor = XMFLOAT4(0, 0, 0, 1);
	float aoRange = 40.0f;
	uint32_t aoSampleCount = 16;
	float aoPower = 1.0f;
	float chromaticAberrationAmount = 2.0f;
	uint32_t screenSpaceShadowSampleCount = 16;
	float screenSpaceShadowRange = 1;
	float eyeadaptionKey = 0.115f;
	float eyeadaptionRate = 1;
	float fsrSharpness = 1.0f;

#ifdef GGREDUCED
	std::shared_ptr<wiResource> rainTextureMap;
	std::shared_ptr<wiResource> rainNormalMap;
	float rainOpacity = 1.0;
	float rainScaleX = 1.0;
	float rainScaleY = 1.0;
	float rainOffsetX = 0.0;
	float rainOffsetY = 0.0;
	float rainRefreactionScale = 0.01;
	bool rainEnabled = false;

	bool snowEnabled = false;
	float snowLayers = 15.0;
	float snowDepth = 0.5;
	float snowWindiness = 0.5;
	float snowSpeed = 0.15;
	float snowOpacity = 1.0;
	float snowOffset = 0.0;

	// 3D resolution is different from screen resolution when rendering VR, and maybe in other cases like FSR
	float width3D;
	float height3D;
	float fsrUpScale = 1.0f;
#endif	

	AO ao = AO_DISABLED;
	bool fxaaEnabled = false;
	bool ssrEnabled = false;
	bool raytracedReflectionsEnabled = false;
	bool reflectionsEnabled = true;
	bool shadowsEnabled = true;
	bool bloomEnabled = true;
	bool colorGradingEnabled = true;
	bool volumeLightsEnabled = true;
	bool lightShaftsEnabled = false;
	bool lensFlareEnabled = true;
	bool motionBlurEnabled = false;
	bool depthOfFieldEnabled = true;
	bool eyeAdaptionEnabled = false;
	bool sharpenFilterEnabled = false;
	bool outlineEnabled = false;
	bool chromaticAberrationEnabled = false;
	bool ditherEnabled = true;
	bool occlusionCullingEnabled = true;
	bool sceneUpdateEnabled = true;
	bool fsrEnabled = true;

	uint32_t msaaSampleCount = 1;

public:
	wiGraphics::Texture rtGbuffer[GBUFFER_COUNT];
	wiGraphics::Texture rtGbuffer_resolved[GBUFFER_COUNT];
	wiGraphics::Texture rtReflection; // contains the scene rendered for planar reflections
	wiGraphics::Texture rtSSR; // standard screen-space reflection results
	wiGraphics::Texture rtSceneCopy; // contains the rendered scene that can be fed into transparent pass for distortion effect
	wiGraphics::Texture rtSceneCopy_tmp; // temporary for gaussian mipchain
	wiGraphics::Texture rtWaterRipple; // water ripple sprite normal maps are rendered into this
	wiGraphics::Texture rtParticleDistortion; // contains distortive particles
	wiGraphics::Texture rtParticleDistortion_Resolved; // contains distortive particles
	wiGraphics::Texture rtVolumetricLights[2]; // contains the volumetric light results
	wiGraphics::Texture rtTemporalAA[2]; // temporal AA history buffer
	wiGraphics::Texture rtBloom; // contains the bright parts of the image + mipchain
	wiGraphics::Texture rtBloom_tmp; // temporary for bloom downsampling
	wiGraphics::Texture rtAO; // full res AO
	wiGraphics::Texture rtShadow; // raytraced shadows mask
	wiGraphics::Texture rtSun[2]; // 0: sun render target used for lightshafts (can be MSAA), 1: radial blurred lightshafts
	wiGraphics::Texture rtSun_resolved; // sun render target, but the resolved version if MSAA is enabled
	wiGraphics::Texture rtGUIBlurredBackground[3];	// downsampled, gaussian blurred scene for GUI
	wiGraphics::Texture rtShadingRate; // UINT8 shading rate per tile
	wiGraphics::Texture rtFSR[2]; // FSR upscaling result (full resolution LDR)

	wiGraphics::Texture rtPostprocess_HDR[2]; // ping-pong with main scene RT in HDR post-process chain
	wiGraphics::Texture rtPostprocess_LDR[2]; // ping-pong with itself in LDR post-process chain

#ifdef GGREDUCED
	wiGraphics::Texture rtVirtualTextureReadBack;
#endif

	wiGraphics::Texture depthBuffer_Main; // used for depth-testing, can be MSAA
	wiGraphics::Texture depthBuffer_Copy; // used for shader resource, single sample
	wiGraphics::Texture depthBuffer_Copy1; // used for disocclusion check
	wiGraphics::Texture depthBuffer_Reflection; // used for reflection, single sample
	wiGraphics::Texture rtLinearDepth; // linear depth result + mipchain (max filter)

	wiGraphics::RenderPass renderpass_depthprepass;
	wiGraphics::RenderPass renderpass_main;
	wiGraphics::RenderPass renderpass_transparent;
	wiGraphics::RenderPass renderpass_reflection_depthprepass;
	wiGraphics::RenderPass renderpass_reflection;
	wiGraphics::RenderPass renderpass_downsamplescene;
	wiGraphics::RenderPass renderpass_lightshafts;
	wiGraphics::RenderPass renderpass_volumetriclight;
	wiGraphics::RenderPass renderpass_particledistortion;
	wiGraphics::RenderPass renderpass_waterripples;

	wiGraphics::Texture debugUAV; // debug UAV can be used by some shaders...
	wiRenderer::TiledLightResources tiledLightResources;
	wiRenderer::TiledLightResources tiledLightResources_planarReflection;
	wiRenderer::LuminanceResources luminanceResources;
	wiRenderer::SSAOResources ssaoResources;
	wiRenderer::MSAOResources msaoResources;
	wiRenderer::RTAOResources rtaoResources;
	wiRenderer::RTReflectionResources rtreflectionResources;
	wiRenderer::SSRResources ssrResources;
	wiRenderer::RTShadowResources rtshadowResources;
	wiRenderer::ScreenSpaceShadowResources screenspaceshadowResources;
	wiRenderer::DepthOfFieldResources depthoffieldResources;
	wiRenderer::MotionBlurResources motionblurResources;
	wiRenderer::VolumetricCloudResources volumetriccloudResources[2]; // one each for left and right eyes
	wiRenderer::VolumetricCloudResources volumetriccloudResources_reflection[2]; // one each for left and right eyes
	wiRenderer::BloomResources bloomResources;

	const constexpr wiGraphics::Texture* GetGbuffer_Read() const
	{
		if (getMSAASampleCount() > 1)
		{
			return rtGbuffer_resolved;
		}
		else
		{
			return rtGbuffer;
		}
	}
	const constexpr wiGraphics::Texture* GetGbuffer_Read(GBUFFER i) const
	{
		if (getMSAASampleCount() > 1)
		{
			return &rtGbuffer_resolved[i];
		}
		else
		{
			return &rtGbuffer[i];
		}
	}

	// Post-processes are ping-ponged, this function helps to obtain the last postprocess render target that was written
	const wiGraphics::Texture* GetLastPostprocessRT() const
	{
		if (rtFSR[0].IsValid() && getFSREnabled())
		{
			return &rtFSR[0];
		}
		int ldr_postprocess_count = 0;
		ldr_postprocess_count += getSharpenFilterEnabled() ? 1 : 0;
#ifdef GGREDUCED
		ldr_postprocess_count += getRainEnabled() ? 1 : 0; //Rain
		ldr_postprocess_count += getSnowEnabled() ? 1 : 0; //Rain
#endif
		ldr_postprocess_count += getFXAAEnabled() ? 1 : 0;
		ldr_postprocess_count += getChromaticAberrationEnabled() ? 1 : 0;
		int rt_index = ldr_postprocess_count % 2;
		return &rtPostprocess_LDR[rt_index];
	}

	virtual void RenderFrameSetUp(wiGraphics::CommandList cmd) const;
	virtual void RenderAO(wiGraphics::CommandList cmd) const;
	virtual void RenderSSR(wiGraphics::CommandList cmd) const;
	virtual void RenderOutline(wiGraphics::CommandList cmd) const;
	virtual void RenderLightShafts(wiGraphics::CommandList cmd, int mode) const;
	virtual void RenderVolumetrics(wiGraphics::CommandList cmd) const;
	virtual void RenderSceneMIPChain(wiGraphics::CommandList cmd) const;
	virtual void RenderTransparents(wiGraphics::CommandList cmd, int mode) const;
	virtual void RenderPostprocessChain(wiGraphics::CommandList cmd) const;
#ifdef GGREDUCED
	virtual void RenderOutlineHighlighers(wiGraphics::CommandList cmd) const;
#endif

	void ResizeBuffers() override;

	wiScene::CameraComponent* camera = &wiScene::GetCamera();
	wiScene::CameraComponent camera_previous;
	wiScene::CameraComponent camera_previousLeft;
	wiScene::CameraComponent camera_previousRight;
	wiScene::CameraComponent camera_reflection;
	wiScene::CameraComponent camera_reflection_previous;
	wiScene::CameraComponent camera_reflection_previousLeft;
	wiScene::CameraComponent camera_reflection_previousRight;

	wiScene::Scene* scene = &wiScene::GetScene();
	wiRenderer::Visibility visibility_main;
	wiRenderer::Visibility visibility_reflection;

	FrameCB frameCB = {};

	const wiGraphics::Texture* GetDepthStencil() const override { return &depthBuffer_Main; }
	const wiGraphics::Texture* GetGUIBlurredBackground() const override { return &rtGUIBlurredBackground[2]; }

	constexpr float getExposure() const { return exposure; }
	constexpr float getBloomThreshold() const { return bloomThreshold; }
	constexpr float getBloomStrength() const { return bloomStrength; }
	constexpr float getMotionBlurStrength() const { return motionBlurStrength; }
	constexpr float getDepthOfFieldStrength() const { return dofStrength; }
	constexpr float getSharpenFilterAmount() const { return sharpenFilterAmount; }
	constexpr float getOutlineThreshold() const { return outlineThreshold; }
	constexpr float getOutlineThickness() const { return outlineThickness; }
	constexpr XMFLOAT4 getOutlineColor() const { return outlineColor; }
	constexpr float getAORange() const { return aoRange; }
	constexpr uint32_t getAOSampleCount() const { return aoSampleCount; }
	constexpr float getAOPower() const { return aoPower; }
	constexpr float getChromaticAberrationAmount() const { return chromaticAberrationAmount; }
	constexpr uint32_t getScreenSpaceShadowSampleCount() const { return screenSpaceShadowSampleCount; }
	constexpr float getScreenSpaceShadowRange() const { return screenSpaceShadowRange; }
	constexpr float getEyeAdaptionKey() const { return eyeadaptionKey; }
	constexpr float getEyeAdaptionRate() const { return eyeadaptionRate; }
	constexpr float getFSRSharpness() const { return fsrSharpness; }

	constexpr bool getAOEnabled() const { return ao != AO_DISABLED; }
	constexpr AO getAO() const { return ao; }
	constexpr bool getSSREnabled() const { return ssrEnabled; }
	constexpr bool getRaytracedReflectionEnabled() const { return raytracedReflectionsEnabled; }
	constexpr bool getShadowsEnabled() const { return shadowsEnabled; }
	constexpr bool getReflectionsEnabled() const { return reflectionsEnabled; }
	constexpr bool getFXAAEnabled() const { return fxaaEnabled; }
#ifdef GGREDUCED
	constexpr bool getRainEnabled() const { return rainEnabled; }
	constexpr bool getSnowEnabled() const { return snowEnabled; }
#endif
	constexpr bool getBloomEnabled() const { return bloomEnabled; }
	constexpr bool getColorGradingEnabled() const { return colorGradingEnabled; }
	constexpr bool getVolumeLightsEnabled() const { return volumeLightsEnabled; }
	constexpr bool getLightShaftsEnabled() const { return lightShaftsEnabled; }
	constexpr bool getLensFlareEnabled() const { return lensFlareEnabled; }
	constexpr bool getMotionBlurEnabled() const { return motionBlurEnabled; }
	constexpr bool getDepthOfFieldEnabled() const { return depthOfFieldEnabled; }
	constexpr bool getEyeAdaptionEnabled() const { return eyeAdaptionEnabled; }
	constexpr bool getSharpenFilterEnabled() const { return sharpenFilterEnabled && getSharpenFilterAmount() > 0; }
	constexpr bool getOutlineEnabled() const { return outlineEnabled; }
	constexpr bool getChromaticAberrationEnabled() const { return chromaticAberrationEnabled; }
	constexpr bool getDitherEnabled() const { return ditherEnabled; }
	constexpr bool getOcclusionCullingEnabled() const { return occlusionCullingEnabled; }
	constexpr bool getSceneUpdateEnabled() const { return sceneUpdateEnabled; }
	constexpr bool getFSREnabled() const { return fsrEnabled; }

	constexpr uint32_t getMSAASampleCount() const { return msaaSampleCount; }

	constexpr void setExposure(float value) { exposure = value; }
	constexpr void setBloomThreshold(float value){ bloomThreshold = value; }
	constexpr void setBloomStrength(float value){ bloomStrength = value; }
	constexpr void setMotionBlurStrength(float value) { motionBlurStrength = value; }
	constexpr void setDepthOfFieldStrength(float value) { dofStrength = value; }
	constexpr void setSharpenFilterAmount(float value) { sharpenFilterAmount = value; }
	constexpr void setOutlineThreshold(float value) { outlineThreshold = value; }
	constexpr void setOutlineThickness(float value) { outlineThickness = value; }
	constexpr void setOutlineColor(const XMFLOAT4& value) { outlineColor = value; }
	constexpr void setAORange(float value) { aoRange = value; }
	constexpr void setAOSampleCount(uint32_t value) { aoSampleCount = value; }
	constexpr void setAOPower(float value) { aoPower = value; }
	constexpr void setChromaticAberrationAmount(float value) { chromaticAberrationAmount = value; }
	constexpr void setScreenSpaceShadowSampleCount(uint32_t value) { screenSpaceShadowSampleCount = value; }
	constexpr void setScreenSpaceShadowRange(float value) { screenSpaceShadowRange = value; }
	constexpr void setEyeAdaptionKey(float value) { eyeadaptionKey = value; }
	constexpr void setEyeAdaptionRate(float value) { eyeadaptionRate = value; }
	constexpr void setFSRSharpness(float value) { fsrSharpness = value; }

	void setAO(AO value);
	void setSSREnabled(bool value);
	void setRaytracedReflectionsEnabled(bool value);
	constexpr void setShadowsEnabled(bool value){ shadowsEnabled = value; }
	constexpr void setReflectionsEnabled(bool value){ reflectionsEnabled = value; }
	constexpr void setFXAAEnabled(bool value){ fxaaEnabled = value; }
#ifdef GGREDUCED
	constexpr void setRainEnabled(bool value) { rainEnabled = value; }
	constexpr void setRainOpacity(float value) { rainOpacity = value; }
	constexpr void setRainScaleX(float value) { rainScaleX = value; }
	constexpr void setRainScaleY(float value) { rainScaleY = value; }
	constexpr void setRainOffsetX(float value) { rainOffsetX = value; }
	constexpr void setRainOffsetY(float value) { rainOffsetY = value; }
	constexpr void setRainRefreactionScale(float value) { rainRefreactionScale = value; }
	
	void setRainTextures(char *tex, char *normal);

	constexpr void setSnowEnabled(bool value) { snowEnabled = value; }
	constexpr void setSnowLayers(float value) { snowLayers = value; }
	constexpr void setSnowDepth(float value) { snowDepth = value; }
	constexpr void setSnowWindiness(float value) { snowWindiness = value; }
	constexpr void setSnowSpeed(float value) { snowSpeed = value; }
	constexpr void setSnowOpacity(float value) { snowOpacity = value; }
	constexpr void setSnowOffset(float value) { snowOffset = value; }

#endif

	constexpr void setBloomEnabled(bool value){ bloomEnabled = value; }
	constexpr void setColorGradingEnabled(bool value){ colorGradingEnabled = value; }
	constexpr void setVolumeLightsEnabled(bool value){ volumeLightsEnabled = value; }
	constexpr void setLightShaftsEnabled(bool value){ lightShaftsEnabled = value; }
	constexpr void setLensFlareEnabled(bool value){ lensFlareEnabled = value; }
	constexpr void setMotionBlurEnabled(bool value){ motionBlurEnabled = value; }
	constexpr void setDepthOfFieldEnabled(bool value){ depthOfFieldEnabled = value; }
	constexpr void setEyeAdaptionEnabled(bool value) { eyeAdaptionEnabled = value; }
	constexpr void setSharpenFilterEnabled(bool value) { sharpenFilterEnabled = value; }
	constexpr void setOutlineEnabled(bool value) { outlineEnabled = value; }
	constexpr void setChromaticAberrationEnabled(bool value) { chromaticAberrationEnabled = value; }
	constexpr void setDitherEnabled(bool value) { ditherEnabled = value; }
	constexpr void setOcclusionCullingEnabled(bool value) { occlusionCullingEnabled = value; }
	constexpr void setSceneUpdateEnabled(bool value) { sceneUpdateEnabled = value; }
	void setFSREnabled(bool value);

	virtual void setMSAASampleCount(uint32_t value) { msaaSampleCount = value; }

#ifdef GGREDUCED
	void Set3DResolution( float width, float height , bool resizebuffers = true);
	float GetWidth3D() const { return width3D; }
	float GetHeight3D() const { return height3D; }
	void SetFSRScale( float scale );
	float GetFSRScale() const { return fsrUpScale; }
#endif

	void PreUpdate() override;
	void StorePreviousLeft() override;
	void StorePreviousRight() override;
	void Update(float dt) override;
	void Render( int mode ) const override;
	void Compose(wiGraphics::CommandList cmd) const override;
#ifdef GGREDUCED
	void ComposeSimple(wiGraphics::CommandList cmd) const;
	void ComposeSimple2D(wiGraphics::CommandList cmd) const;
#endif
};

