#pragma once
#include "CommonInclude.h"
#include "wiEnums.h"
#include "wiGraphicsDevice.h"
#include "wiScene.h"
#include "wiECS.h"
#include "wiIntersect.h"
#include "wiCanvas.h"
#include "shaders/ShaderInterop_Renderer.h"

#include <memory>

struct RAY;
struct wiResource;

namespace wiRenderer
{
	extern wiGraphics::GPUBuffer constantBuffers[CBTYPE_COUNT];

	inline uint32_t CombineStencilrefs(STENCILREF engineStencilRef, uint8_t userStencilRef)
	{
		return (userStencilRef << 4) | static_cast<uint8_t>(engineStencilRef);
	}

	inline XMUINT3 GetEntityCullingTileCount(XMUINT2 internalResolution)
	{
		return XMUINT3(
			(internalResolution.x + TILED_CULLING_BLOCKSIZE - 1) / TILED_CULLING_BLOCKSIZE,
			(internalResolution.y + TILED_CULLING_BLOCKSIZE - 1) / TILED_CULLING_BLOCKSIZE,
			1
		);
	}

	const wiGraphics::Sampler* GetSampler(int slot);
	const wiGraphics::Shader* GetShader(SHADERTYPE id);
	const wiGraphics::InputLayout* GetInputLayout(ILTYPES id);
	const wiGraphics::RasterizerState* GetRasterizerState(RSTYPES id);
	const wiGraphics::DepthStencilState* GetDepthStencilState(DSSTYPES id);
	const wiGraphics::BlendState* GetBlendState(BSTYPES id);
	const wiGraphics::GPUBuffer* GetConstantBuffer(CBTYPES id);
	const wiGraphics::Texture* GetTexture(TEXTYPES id);

	void ModifyObjectSampler(const wiGraphics::SamplerDesc& desc);


	void Initialize();

	// Clears the scene and the associated renderer resources
	void ClearWorld(wiScene::Scene& scene);

	// Set the main graphics device globally:
	void SetDevice(std::shared_ptr<wiGraphics::GraphicsDevice> newDevice);
	// Retrieve the main graphics device:
	wiGraphics::GraphicsDevice* GetDevice();

	// Returns the shader binary directory
	const std::string& GetShaderPath();
	// Sets the shader binary directory
	void SetShaderPath(const std::string& path);
	// Returns the shader source directory
	const std::string& GetShaderSourcePath();
	// Sets the shader source directory
	void SetShaderSourcePath(const std::string& path);
	// Reload shaders
	void ReloadShaders();
	// Returns how many shaders are embedded (if wiShaderDump.h is used)
	//	wiShaderDump.h can be generated by OfflineShaderCompiler.exe using shaderdump argument
	size_t GetShaderDumpCount();

	bool LoadShader(
		wiGraphics::SHADERSTAGE stage,
		wiGraphics::Shader& shader,
		const std::string& filename,
		wiGraphics::SHADERMODEL minshadermodel = wiGraphics::SHADERMODEL_5_0
	);


	struct Visibility
	{
		// User fills these:
		uint32_t layerMask = ~0u;
		const wiScene::Scene* scene = nullptr;
		const wiScene::CameraComponent* camera = nullptr;
		enum FLAGS
		{
			EMPTY = 0,
			ALLOW_OBJECTS = 1 << 0,
			ALLOW_LIGHTS = 1 << 1,
			ALLOW_DECALS = 1 << 2,
			ALLOW_ENVPROBES = 1 << 3,
			ALLOW_EMITTERS = 1 << 4,
			ALLOW_HAIRS = 1 << 5,
			ALLOW_REQUEST_REFLECTION = 1 << 6,

			ALLOW_EVERYTHING = ~0u
		};
		uint32_t flags = EMPTY;

		// wiRenderer::UpdateVisibility() fills these:
		Frustum frustum;
		std::vector<uint32_t> visibleObjects;
		std::vector<uint32_t> visibleDecals;
		std::vector<uint32_t> visibleEnvProbes;
		std::vector<uint32_t> visibleEmitters;
		std::vector<uint32_t> visibleHairs;

		struct VisibleLight
		{
			uint16_t index;
			uint16_t distance;
			bool operator<(const VisibleLight& other) {
				return uint32_t(index | (uint32_t(distance) << 16)) < uint32_t(other.index | (uint32_t(other.distance) << 16));
			}
		};
		std::vector<VisibleLight> visibleLights;

		std::atomic<uint32_t> object_counter;
		std::atomic<uint32_t> light_counter;
		std::atomic<uint32_t> decal_counter;

		wiSpinLock locker;
		bool planar_reflection_visible = false;
		float closestRefPlane = FLT_MAX;
		XMFLOAT4 reflectionPlane = XMFLOAT4(0, 1, 0, 0);
		std::atomic_bool volumetriclight_request{ false };

		void Clear()
		{
			visibleObjects.clear();
			visibleLights.clear();
			visibleDecals.clear();
			visibleEnvProbes.clear();
			visibleEmitters.clear();
			visibleHairs.clear();

			object_counter.store(0);
			light_counter.store(0);
			decal_counter.store(0);

			closestRefPlane = FLT_MAX;
			planar_reflection_visible = false;
			volumetriclight_request.store(false);
		}

		bool IsRequestedPlanarReflections() const
		{
			return planar_reflection_visible;
		}
		bool IsRequestedVolumetricLights() const
		{
			return volumetriclight_request.load();
		}
	};

	// Performs frustum culling.
	void UpdateVisibility(Visibility& vis, float maxApparentSize = 0);
	// Prepares the scene for rendering
	void UpdatePerFrameData(
		wiScene::Scene& scene,
		const Visibility& vis,
		FrameCB& frameCB,
		XMUINT2 internalResolution,
		const wiCanvas& canvas,
		float dt
	);
	// Updates the GPU state according to the previously called UpdatePerFrameData()
	void UpdateRenderData(
		const Visibility& vis,
		const FrameCB& frameCB,
		wiGraphics::CommandList cmd
	);

	void UpdateRaytracingAccelerationStructures(const wiScene::Scene& scene, wiGraphics::CommandList cmd);

	// Binds all common constant buffers and samplers that may be used in all shaders
	void BindCommonResources(wiGraphics::CommandList cmd);
	// Updates the per camera constant buffer need to call for each different camera that is used when calling DrawScene() and the like
	//	camera_previous : camera from previous frame, used for reprojection effects.
	//	camera_reflection : camera that renders planar reflection
	void UpdateCameraCB(
		const wiScene::CameraComponent& camera,
		const wiScene::CameraComponent& camera_previous,
		const wiScene::CameraComponent& camera_reflection,
		wiGraphics::CommandList cmd
	);


	enum DRAWSCENE_FLAGS
	{
		DRAWSCENE_OPAQUE = 1 << 0,
		DRAWSCENE_TRANSPARENT = 1 << 1,
		DRAWSCENE_OCCLUSIONCULLING = 1 << 2,
		DRAWSCENE_TESSELLATION = 1 << 3,
		DRAWSCENE_HAIRPARTICLE = 1 << 4,
		DRAWSCENE_OCEAN = 1 << 5,
	};

	// Draw the world from a camera. You must call UpdateCameraCB() at least once in this frame prior to this
	void DrawScene(
		const Visibility& vis,
		RENDERPASS renderPass,
		wiGraphics::CommandList cmd,
		uint32_t flags = DRAWSCENE_OPAQUE
	);

	// Render mip levels for textures that reqested it:
	void ProcessDeferredMipGenRequests(wiGraphics::CommandList cmd);

	// Compute essential atmospheric scattering textures for skybox, fog and clouds
	void RenderAtmosphericScatteringTextures(wiGraphics::CommandList cmd);
	// Update atmospheric scattering primarily for environment probes.
	void RefreshAtmosphericScatteringTextures(wiGraphics::CommandList cmd);
	// Draw skydome centered to camera.
	void DrawSky(const wiScene::Scene& scene, wiGraphics::CommandList cmd);
	// Draw sky velocity buffer
	void DrawSkyVelocity(wiGraphics::CommandList cmd);
	// Draw shadow maps for each visible light that has associated shadow maps
	void DrawSun(wiGraphics::CommandList cmd);
	// Draw shadow maps for each visible light that has associated shadow maps
	void DrawShadowmaps(
		const Visibility& vis,
		wiGraphics::CommandList cmd
	);
	// Draw debug world. You must also enable what parts to draw, eg. SetToDrawGridHelper, etc, see implementation for details what can be enabled.
	void DrawDebugWorld(
		const wiScene::Scene& scene,
		const wiScene::CameraComponent& camera,
		const wiCanvas& canvas,
		wiGraphics::CommandList cmd
	);
	// Draw Soft offscreen particles.
	void DrawSoftParticles(
		const Visibility& vis,
		const wiGraphics::Texture& lineardepth,
		bool distortion, 
		wiGraphics::CommandList cmd
	);
	// Draw simple light visualizer geometries
	void DrawLightVisualizers(
		const Visibility& vis,
		wiGraphics::CommandList cmd
	);
	// Draw volumetric light scattering effects
	void DrawVolumeLights(
		const Visibility& vis,
		const wiGraphics::Texture& depthbuffer,
		wiGraphics::CommandList cmd
	);
	// Draw Lens Flares for lights that have them enabled
	void DrawLensFlares(
		const Visibility& vis,
		const wiGraphics::Texture& depthbuffer,
		wiGraphics::CommandList cmd,
		const wiGraphics::Texture* texture_directional_occlusion = nullptr
	);
	// Call once per frame to re-render out of date environment probes
	void RefreshEnvProbes(const Visibility& vis, wiGraphics::CommandList cmd);
	// Call once per frame to re-render out of date impostors
	void RefreshImpostors(const wiScene::Scene& scene, wiGraphics::CommandList cmd);
	// Call once per frame to repack out of date decals in the atlas
	void RefreshDecalAtlas(const wiScene::Scene& scene, wiGraphics::CommandList cmd);
	// Call once per frame to repack out of date lightmaps in the atlas
	void RefreshLightmapAtlas(const wiScene::Scene& scene, wiGraphics::CommandList cmd);
	// Voxelize the scene into a voxel grid 3D texture
	void VoxelRadiance(const Visibility& vis, wiGraphics::CommandList cmd);
	// Run a compute shader that will resolve a MSAA depth buffer to a single-sample texture
	void ResolveMSAADepthBuffer(const wiGraphics::Texture& dst, const wiGraphics::Texture& src, wiGraphics::CommandList cmd);
	void DownsampleDepthBuffer(const wiGraphics::Texture& src, wiGraphics::CommandList cmd);

	struct TiledLightResources
	{
		wiGraphics::GPUBuffer tileFrustums; // entity culling frustums
		wiGraphics::GPUBuffer entityTiles_Opaque; // culled entity indices (for opaque pass)
		wiGraphics::GPUBuffer entityTiles_Transparent; // culled entity indices (for transparent pass)
	};
	void CreateTiledLightResources(TiledLightResources& res, XMUINT2 resolution);
	// Compute light grid tiles
	void ComputeTiledLightCulling(
		const TiledLightResources& res,
		const wiGraphics::Texture& depthbuffer,
		const wiGraphics::Texture& debugUAV,
		wiGraphics::CommandList cmd
	);

	struct LuminanceResources
	{
		wiGraphics::Texture reductiontex;
		wiGraphics::Texture luminance;
	};
	void CreateLuminanceResources(LuminanceResources& res, XMUINT2 resolution);
	// Compute the luminance for the source image and return the texture containing the luminance value in pixel [0,0]
	const wiGraphics::Texture* ComputeLuminance(
		const LuminanceResources& res,
		const wiGraphics::Texture& sourceImage,
		wiGraphics::CommandList cmd,
		float adaption_rate = 1
	);

	void ComputeShadingRateClassification(
		const wiGraphics::Texture gbuffer[GBUFFER_COUNT],
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::Texture& output,
		const wiGraphics::Texture& debugUAV,
		wiGraphics::CommandList cmd
	);

	void Postprocess_Blur_Gaussian(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& temp,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		int mip_src = -1,
		int mip_dst = -1,
		bool wide = false
	);
	void Postprocess_Blur_Bilateral(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::Texture& temp,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float depth_threshold = 1.0f,
		int mip_src = -1,
		int mip_dst = -1,
		bool wide = false
	);
	struct SSAOResources
	{
		wiGraphics::Texture temp;
	};
	void CreateSSAOResources(SSAOResources& res, XMUINT2 resolution);
	void Postprocess_SSAO(
		const SSAOResources& res,
		const wiGraphics::Texture& depthbuffer,
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float range = 1.0f,
		uint32_t samplecount = 16,
		float power = 2.0f
	);
	void Postprocess_HBAO(
		const SSAOResources& res,
		const wiScene::CameraComponent& camera,
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float power = 2.0f
		);
	struct MSAOResources
	{
		wiGraphics::Texture texture_lineardepth_downsize1;
		wiGraphics::Texture texture_lineardepth_tiled1;
		wiGraphics::Texture texture_lineardepth_downsize2;
		wiGraphics::Texture texture_lineardepth_tiled2;
		wiGraphics::Texture texture_lineardepth_downsize3;
		wiGraphics::Texture texture_lineardepth_tiled3;
		wiGraphics::Texture texture_lineardepth_downsize4;
		wiGraphics::Texture texture_lineardepth_tiled4;
		wiGraphics::Texture texture_ao_merged1;
		wiGraphics::Texture texture_ao_hq1;
		wiGraphics::Texture texture_ao_smooth1;
		wiGraphics::Texture texture_ao_merged2;
		wiGraphics::Texture texture_ao_hq2;
		wiGraphics::Texture texture_ao_smooth2;
		wiGraphics::Texture texture_ao_merged3;
		wiGraphics::Texture texture_ao_hq3;
		wiGraphics::Texture texture_ao_smooth3;
		wiGraphics::Texture texture_ao_merged4;
		wiGraphics::Texture texture_ao_hq4;
	};
	void CreateMSAOResources(MSAOResources& res, XMUINT2 resolution);
	void Postprocess_MSAO(
		const MSAOResources& res,
		const wiScene::CameraComponent& camera,
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float power = 2.0f
		);
	struct RTAOResources
	{
		wiGraphics::Texture normals;

		mutable int frame = 0;
		wiGraphics::GPUBuffer tiles;
		wiGraphics::GPUBuffer metadata;
		wiGraphics::Texture scratch[2];
		wiGraphics::Texture moments[2];
	};
	void CreateRTAOResources(RTAOResources& res, XMUINT2 resolution);
	void Postprocess_RTAO(
		const RTAOResources& res,
		const wiScene::Scene& scene,
		const wiGraphics::Texture& depthbuffer,
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::Texture& depth_history,
		const wiGraphics::Texture gbuffer[GBUFFER_COUNT],
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float range = 1.0f,
		float power = 2.0f
	);
	struct RTReflectionResources
	{
		wiGraphics::Texture temporal[2];
		wiGraphics::Texture rayLengths;
	};
	void CreateRTReflectionResources(RTReflectionResources& res, XMUINT2 resolution);
	void Postprocess_RTReflection(
		const RTReflectionResources& res,
		const wiScene::Scene& scene,
		const wiGraphics::Texture& depthbuffer,
		const wiGraphics::Texture& depth_history,
		const wiGraphics::Texture gbuffer[GBUFFER_COUNT],
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float range = 1000.0f
	);
	struct SSRResources
	{
		wiGraphics::Texture texture_raytrace;
		wiGraphics::Texture rayLengths;
		wiGraphics::Texture texture_temporal[2];
	};
	void CreateSSRResources(SSRResources& res, XMUINT2 resolution);
	void Postprocess_SSR(
		const SSRResources& res,
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& depthbuffer,
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::Texture& depth_history,
		const wiGraphics::Texture gbuffer[GBUFFER_COUNT],
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd
	);
	struct RTShadowResources
	{
		wiGraphics::Texture temp;
		wiGraphics::Texture temporal[2];
		wiGraphics::Texture normals;

		mutable int frame = 0;
		wiGraphics::GPUBuffer tiles;
		wiGraphics::GPUBuffer metadata;
		wiGraphics::Texture scratch[4][2];
		wiGraphics::Texture moments[4][2];
		wiGraphics::Texture denoised;
	};
	void CreateRTShadowResources(RTShadowResources& res, XMUINT2 resolution);
	void Postprocess_RTShadow(
		const RTShadowResources& res,
		const wiScene::Scene& scene,
		const wiGraphics::Texture& depthbuffer,
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::Texture& depth_history,
		const wiGraphics::GPUBuffer& entityTiles_Opaque,
		const wiGraphics::Texture gbuffer[GBUFFER_COUNT],
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd
	);
	struct ScreenSpaceShadowResources
	{
		int placeholder;
	};
	void CreateScreenSpaceShadowResources(ScreenSpaceShadowResources& res, XMUINT2 resolution);
	void Postprocess_ScreenSpaceShadow(
		const ScreenSpaceShadowResources& res,
		const wiGraphics::Texture& depthbuffer,
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::GPUBuffer& entityTiles_Opaque,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float range = 1,
		uint32_t samplecount = 16
	);
	void Postprocess_LightShafts(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		const XMFLOAT2& center
	);
	struct DepthOfFieldResources
	{
		wiGraphics::Texture texture_tilemax_horizontal;
		wiGraphics::Texture texture_tilemin_horizontal;
		wiGraphics::Texture texture_tilemax;
		wiGraphics::Texture texture_tilemin;
		wiGraphics::Texture texture_neighborhoodmax;
		wiGraphics::Texture texture_presort;
		wiGraphics::Texture texture_prefilter;
		wiGraphics::Texture texture_main;
		wiGraphics::Texture texture_postfilter;
		wiGraphics::Texture texture_alpha1;
		wiGraphics::Texture texture_alpha2;
		wiGraphics::GPUBuffer buffer_tile_statistics;
		wiGraphics::GPUBuffer buffer_tiles_earlyexit;
		wiGraphics::GPUBuffer buffer_tiles_cheap;
		wiGraphics::GPUBuffer buffer_tiles_expensive;
	};
	void CreateDepthOfFieldResources(DepthOfFieldResources& res, XMUINT2 resolution);
	void Postprocess_DepthOfField(
		const DepthOfFieldResources& res,
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& output,
		const wiGraphics::Texture& lineardepth,
		wiGraphics::CommandList cmd,
		float coc_scale = 10,
		float max_coc = 18
	);
	void Postprocess_Outline(
		const wiGraphics::Texture& input,
		wiGraphics::CommandList cmd,
		float threshold = 0.1f,
		float thickness = 1.0f,
		const XMFLOAT4& color = XMFLOAT4(0, 0, 0, 1)
	);
	struct MotionBlurResources
	{
		wiGraphics::Texture texture_tilemin_horizontal;
		wiGraphics::Texture texture_tilemax_horizontal;
		wiGraphics::Texture texture_tilemax;
		wiGraphics::Texture texture_tilemin;
		wiGraphics::Texture texture_neighborhoodmax;
		wiGraphics::GPUBuffer buffer_tile_statistics;
		wiGraphics::GPUBuffer buffer_tiles_earlyexit;
		wiGraphics::GPUBuffer buffer_tiles_cheap;
		wiGraphics::GPUBuffer buffer_tiles_expensive;
	};
	void CreateMotionBlurResources(MotionBlurResources& res, XMUINT2 resolution);
	void Postprocess_MotionBlur(
		const MotionBlurResources& res,
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::Texture gbuffer[GBUFFER_COUNT],
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float strength = 100.0f
	);
	struct BloomResources
	{
		wiGraphics::Texture texture_bloom;
		wiGraphics::Texture texture_temp;
	};
	void CreateBloomResources(BloomResources& res, XMUINT2 resolution);
	void Postprocess_Bloom(
		const BloomResources& res,
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float threshold = 1.0f,
		float strength = 1.0f
	);
	struct VolumetricCloudResources
	{
		wiGraphics::Texture texture_cloudRender;
		wiGraphics::Texture texture_cloudDepth;
		wiGraphics::Texture texture_reproject[2];
		wiGraphics::Texture texture_reproject_depth[2];
		wiGraphics::Texture texture_temporal[2];
		wiGraphics::Texture texture_cloudMask;
	};
	void CreateVolumetricCloudResources(VolumetricCloudResources& res, XMUINT2 resolution);
	void Postprocess_VolumetricClouds(
		const VolumetricCloudResources& res,
		const wiGraphics::Texture& depthbuffer,
		wiGraphics::CommandList cmd
	);
	#ifdef GGREDUCED
	void Postprocess_Rain(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		const wiGraphics::Texture& raintexture,
		const wiGraphics::Texture& rainnormal,
		float opacity,
		float rainScaleX,
		float rainScaleY,
		float rainOffsetX,
		float rainOffsetY,
		float rainRefreactionScale
	);
	void Postprocess_Snow(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float opacity,
		float layers,
		float depth,
		float windiness,
		float speed,
		float offset
	);

	#endif
	void Postprocess_FXAA(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd
	);
	void Postprocess_TemporalAA(
		const wiGraphics::Texture& input_current,
		const wiGraphics::Texture& input_history,
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::Texture& depth_history,
		const wiGraphics::Texture gbuffer[GBUFFER_COUNT],
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd
	);
	void Postprocess_DepthPyramid(
		const wiGraphics::Texture& depthbuffer,
		const wiGraphics::Texture& lineardepth,
		wiGraphics::CommandList cmd
	);
	void Postprocess_Sharpen(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float amount = 1.0f
	);
	void Postprocess_Tonemap(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float exposure,
		bool dither,
		const wiGraphics::Texture* texture_colorgradinglut = nullptr,
		const wiGraphics::Texture* texture_distortion = nullptr,
		const wiGraphics::Texture* texture_luminance = nullptr,
		float eyeadaptionkey = 0.115f
	);
	void Postprocess_FSR(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& temp,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float sharpness = 1.0f
	);
	void Postprocess_Chromatic_Aberration(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		float amount = 1.0f
	);
	void Postprocess_Upsample_Bilateral(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& lineardepth,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd,
		bool is_pixelshader = false,
		float threshold = 1.0f
	);
	void Postprocess_Downsample4x(
		const wiGraphics::Texture& input,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd
	);
	void Postprocess_NormalsFromDepth(
		const wiGraphics::Texture& depthbuffer,
		const wiGraphics::Texture& output,
		wiGraphics::CommandList cmd
	);

	// Render the scene with ray tracing. You provide the ray buffer, where each ray maps to one pixel of the result testure
	void RayTraceScene(
		const wiScene::Scene& scene,
		const wiGraphics::Texture& output,
		int accumulation_sample,
		wiGraphics::CommandList cmd,
		const wiGraphics::Texture* output_albedo = nullptr,
		const wiGraphics::Texture* output_normal = nullptr
	);
	// Render the scene BVH with ray tracing to the screen
	void RayTraceSceneBVH(const wiScene::Scene& scene, wiGraphics::CommandList cmd);

	// Render occluders against a depth buffer
	void OcclusionCulling_Render(const wiScene::CameraComponent& camera_previous, const Visibility& vis, wiGraphics::CommandList cmd);


	enum MIPGENFILTER
	{
		MIPGENFILTER_POINT,
		MIPGENFILTER_LINEAR,
		MIPGENFILTER_GAUSSIAN,
	};
	struct MIPGEN_OPTIONS
	{
		int arrayIndex = -1;
		const wiGraphics::Texture* gaussian_temp = nullptr;
		bool preserve_coverage = false;
		bool wide_gauss = false;
	};
	void GenerateMipChain(const wiGraphics::Texture& texture, MIPGENFILTER filter, wiGraphics::CommandList cmd, const MIPGEN_OPTIONS& options = {});

	enum BORDEREXPANDSTYLE
	{
		BORDEREXPAND_DISABLE,
		BORDEREXPAND_WRAP,
		BORDEREXPAND_CLAMP,
	};
	// GGREDUCED
	void CopyTexture2DArea(
		const wiGraphics::Texture& dst, int DstMIP, int DstX, int DstY,
		const wiGraphics::Texture& src, int SrcX, int SrcY, int SrcMIP,
		wiGraphics::CommandList cmd,
		BORDEREXPANDSTYLE borderExpand = BORDEREXPAND_DISABLE
	);
	// Performs copy operation even between different texture formats
	//	NOTE: DstMIP can be specified as -1 to use main subresource, otherwise the subresource (>=0) must have been generated explicitly!
	//	Can also expand border region according to desired sampler func
	void CopyTexture2D(
		const wiGraphics::Texture& dst, int DstMIP, int DstX, int DstY,
		const wiGraphics::Texture& src, int SrcMIP, 
		wiGraphics::CommandList cmd,
		BORDEREXPANDSTYLE borderExpand = BORDEREXPAND_DISABLE
	);

	void DrawWaterRipples(const Visibility& vis, wiGraphics::CommandList cmd);



	// Set any param to -1 if don't want to modify
	void SetShadowProps2D(int resolution, int count);
	void SetShadowPropsSpot2D(int resolution, int count);
	// Set any param to -1 if don't want to modify
	void SetShadowPropsCube(int resolution, int count);

	// Returns the resolution that is used for all spotlight and directional light shadow maps
	int GetShadowRes2D();
	// Returns the resolution that is used for all pointlight and area light shadow maps
	int GetShadowResCube();



	void SetTransparentShadowsEnabled(float value);
	float GetTransparentShadowsEnabled();
	void SetGamma(float value);
	float GetGamma();
	void SetWireRender(bool value);
	bool IsWireRender();
	void SetToDrawDebugBoneLines(bool param);
	bool GetToDrawDebugBoneLines();
	void SetToDrawDebugPartitionTree(bool param);
	bool GetToDrawDebugPartitionTree();
	bool GetToDrawDebugEnvProbes();
	void SetToDrawDebugEnvProbes(bool value);
	void SetToDrawDebugEmitters(bool param);
	bool GetToDrawDebugEmitters();
	void SetToDrawDebugForceFields(bool param);
	bool GetToDrawDebugForceFields();
	void SetToDrawDebugCameras(bool param);
	bool GetToDrawDebugCameras();
	bool GetToDrawGridHelper();
	void SetToDrawGridHelper(bool value);
	bool GetToDrawVoxelHelper();
	void SetToDrawVoxelHelper(bool value);
	void SetDebugLightCulling(bool enabled);
	bool GetDebugLightCulling();
	void SetAdvancedLightCulling(bool enabled);
	bool GetAdvancedLightCulling();
	void SetVariableRateShadingClassification(bool enabled);
	bool GetVariableRateShadingClassification();
	void SetVariableRateShadingClassificationDebug(bool enabled);
	bool GetVariableRateShadingClassificationDebug();
	void SetOcclusionCullingEnabled(bool enabled);
	bool GetOcclusionCullingEnabled();
	void SetLDSSkinningEnabled(bool enabled);
	bool GetLDSSkinningEnabled();
	void SetTemporalAAEnabled(bool enabled);
	bool GetTemporalAAEnabled();
	void SetTemporalAADebugEnabled(bool enabled);
	bool GetTemporalAADebugEnabled();
	void SetFreezeCullingCameraEnabled(bool enabled);
	bool GetFreezeCullingCameraEnabled();
	void SetVoxelRadianceEnabled(bool enabled);
	bool GetVoxelRadianceEnabled();
	void SetVoxelRadianceSecondaryBounceEnabled(bool enabled);
	bool GetVoxelRadianceSecondaryBounceEnabled();
	void SetVoxelRadianceReflectionsEnabled(bool enabled);
	bool GetVoxelRadianceReflectionsEnabled();
	void SetVoxelRadianceVoxelSize(float value);
	float GetVoxelRadianceVoxelSize();
	void SetVoxelRadianceMaxDistance(float value);
	float GetVoxelRadianceMaxDistance();
	int GetVoxelRadianceResolution();
	void SetVoxelRadianceNumCones(int value);
	int GetVoxelRadianceNumCones();
	float GetVoxelRadianceRayStepSize();
	void SetVoxelRadianceRayStepSize(float value);
	void SetGameSpeed(float value);
	float GetGameSpeed();
	void SetRaytraceBounceCount(uint32_t bounces);
	uint32_t GetRaytraceBounceCount();
	void SetRaytraceDebugBVHVisualizerEnabled(bool value);
	bool GetRaytraceDebugBVHVisualizerEnabled();
	void SetRaytracedShadowsEnabled(bool value);
	bool GetRaytracedShadowsEnabled();
	void SetTessellationEnabled(bool value);
	bool GetTessellationEnabled();
	void SetDisableAlbedoMaps(bool value);
	bool IsDisableAlbedoMaps();
	void SetScreenSpaceShadowsEnabled(bool value);
	bool GetScreenSpaceShadowsEnabled();

	// Gets pick ray according to the current screen resolution and pointer coordinates. Can be used as input into RayIntersectWorld()
	RAY GetPickRay(long cursorX, long cursorY, const wiCanvas& canvas, const wiScene::CameraComponent& camera = wiScene::GetCamera());


	// Add box to render in next frame. It will be rendered in DrawDebugWorld()
	void DrawBox(const XMFLOAT4X4& boxMatrix, const XMFLOAT4& color = XMFLOAT4(1,1,1,1));
	// Add sphere to render in next frame. It will be rendered in DrawDebugWorld()
	void DrawSphere(const SPHERE& sphere, const XMFLOAT4& color = XMFLOAT4(1, 1, 1, 1));
	// Add capsule to render in next frame. It will be rendered in DrawDebugWorld()
	void DrawCapsule(const CAPSULE& capsule, const XMFLOAT4& color = XMFLOAT4(1, 1, 1, 1));

	struct RenderableLine
	{
		XMFLOAT3 start = XMFLOAT3(0, 0, 0);
		XMFLOAT3 end = XMFLOAT3(0, 0, 0);
		XMFLOAT4 color_start = XMFLOAT4(1, 1, 1, 1);
		XMFLOAT4 color_end = XMFLOAT4(1, 1, 1, 1);
	};
	// Add line to render in the next frame. It will be rendered in DrawDebugWorld()
	void DrawLine(const RenderableLine& line);

	struct RenderableLine2D
	{
		XMFLOAT2 start = XMFLOAT2(0, 0);
		XMFLOAT2 end = XMFLOAT2(0, 0);
		XMFLOAT4 color_start = XMFLOAT4(1, 1, 1, 1);
		XMFLOAT4 color_end = XMFLOAT4(1, 1, 1, 1);
	};
	// Add 2D line to render in the next frame. It will be rendered in DrawDebugWorld() in screen space
	void DrawLine(const RenderableLine2D& line);

	struct RenderablePoint
	{
		XMFLOAT3 position = XMFLOAT3(0, 0, 0);
		float size = 1.0f;
		XMFLOAT4 color = XMFLOAT4(1, 1, 1, 1);
	};
	void DrawPoint(const RenderablePoint& point);

	struct RenderableTriangle
	{
		XMFLOAT3 positionA = XMFLOAT3(0, 0, 0);
		XMFLOAT4 colorA = XMFLOAT4(1, 1, 1, 1);
		XMFLOAT3 positionB = XMFLOAT3(0, 0, 0);
		XMFLOAT4 colorB = XMFLOAT4(1, 1, 1, 1);
		XMFLOAT3 positionC = XMFLOAT3(0, 0, 0);
		XMFLOAT4 colorC = XMFLOAT4(1, 1, 1, 1);
	};
	void DrawTriangle(const RenderableTriangle& triangle, bool wireframe = false);

	struct PaintRadius
	{
		wiECS::Entity objectEntity = wiECS::INVALID_ENTITY;
		int subset = -1;
		uint32_t uvset = 0;
		float radius = 0;
		XMUINT2 center;
		XMUINT2 dimensions;
	};
	void DrawPaintRadius(const PaintRadius& paintrad);

	// Add a texture that should be mipmapped whenever it is feasible to do so
	void AddDeferredMIPGen(std::shared_ptr<wiResource> res, bool preserve_coverage = false);

	struct CustomShader
	{
		std::string name;
		uint32_t renderTypeFlags = RENDERTYPE_OPAQUE;
		wiGraphics::PipelineState pso[RENDERPASS_COUNT] = {};
	};
	// Registers a custom shader that can be set to materials. 
	//	Returns the ID of the custom shader that can be used with MaterialComponent::SetCustomShaderID()
	int RegisterCustomShader(const CustomShader& customShader);
	const std::vector<CustomShader>& GetCustomShaders();

};
