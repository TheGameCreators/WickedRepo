#pragma once
#include "CommonInclude.h"
#include "wiGraphicsDevice.h"
#include "wiIntersect.h"
#include "shaders/ShaderInterop_EmittedParticle.h"
#include "wiEnums.h"
#include "wiScene_Decl.h"
#include "wiECS.h"

#include <memory>

class wiArchive;

namespace wiScene
{

class wiEmittedParticle
{
public:

	// This is serialized, order of enums shouldn't change!
	enum PARTICLESHADERTYPE
	{
		SOFT,
		SOFT_DISTORTION,
		SIMPLE,
		SOFT_LIGHTING,
		PARTICLESHADERTYPE_COUNT,
		ENUM_FORCE_UINT32 = 0xFFFFFFFF,
	};

private:
	ParticleCounters statistics = {};
	wiGraphics::GPUBuffer statisticsReadbackBuffer[wiGraphics::GraphicsDevice::GetBufferCount() + 3];

	wiGraphics::GPUBuffer particleBuffer;
	wiGraphics::GPUBuffer aliveList[2];
	wiGraphics::GPUBuffer deadList;
	wiGraphics::GPUBuffer distanceBuffer; // for sorting
	wiGraphics::GPUBuffer sphPartitionCellIndices; // for SPH
	wiGraphics::GPUBuffer sphPartitionCellOffsets; // for SPH
	wiGraphics::GPUBuffer densityBuffer; // for SPH
	wiGraphics::GPUBuffer counterBuffer;
	wiGraphics::GPUBuffer indirectBuffers; // kickoffUpdate, simulation, draw
	wiGraphics::GPUBuffer constantBuffer;
	void CreateSelfBuffers();

	float emit = 0.0f;
	int burst = 0;
	bool buffersUpToDate = false;
	uint32_t MAX_PARTICLES = 1000;

public:
	void UpdateCPU(const TransformComponent& transform, float dt);
	void Burst(int num);
	void Restart();

	// Must have a transform and material component, but mesh is optional
	void UpdateGPU(const TransformComponent& transform, const MaterialComponent& material, const MeshComponent* mesh, wiGraphics::CommandList cmd) const;
	void Draw(const CameraComponent& camera, const MaterialComponent& material, wiGraphics::CommandList cmd) const;

	ParticleCounters GetStatistics() { return statistics; }

	enum FLAGS
	{
		FLAG_EMPTY = 0,
		FLAG_DEBUG = 1 << 0,
		FLAG_PAUSED = 1 << 1,
		FLAG_SORTING = 1 << 2,
		FLAG_DEPTHCOLLISION = 1 << 3,
		FLAG_SPH_FLUIDSIMULATION = 1 << 4,
		FLAG_HAS_VOLUME = 1 << 5,
		FLAG_FRAME_BLENDING = 1 << 6,
	};
	uint32_t _flags = FLAG_EMPTY;

	PARTICLESHADERTYPE shaderType = SOFT;

	wiECS::Entity meshID = wiECS::INVALID_ENTITY;

	float FIXED_TIMESTEP = -1.0f; // -1 : variable timestep; >=0 : fixed timestep

	float size = 1.0f;
	float random_factor = 1.0f;
	float normal_factor = 1.0f;
	float count = 0.0f;
	float life = 1.0f;
	float random_life = 1.0f;
	float scaleX = 1.0f;
	float scaleY = 1.0f;
	float rotation = 0.0f;
	float motionBlurAmount = 0.0f;
	float mass = 1.0f;
	float random_color = 0;

	XMFLOAT3 velocity = {}; // starting velocity of all new particles
	XMFLOAT3 gravity = {}; // constant gravity force
	float drag = 1.0f; // constant drag (per frame velocity multiplier, reducing it will make particles slow down over time)

	float SPH_h = 1.0f;		// smoothing radius
	float SPH_K = 250.0f;	// pressure constant
	float SPH_p0 = 1.0f;	// reference density
	float SPH_e = 0.018f;	// viscosity constant

	// Sprite sheet properties:
	uint32_t framesX = 1;
	uint32_t framesY = 1;
	uint32_t frameCount = 1;
	uint32_t frameStart = 0;
	float frameRate = 0; // frames per second

//#ifdef GGREDUCED
	float restitution = 0.70f; // 0.98f; // if the particles have collision enabled, then after collision this is a multiplier for their bouncing velocities
	float fadein_time = 0.1f;
	float burst_amount = 0;
	float burst_split = 0;
	float burst_delay = 0;
	float normal_factor_x = 0.0f;
	float normal_factor_y = 0.0f;
	float normal_factor_z = 0.0f;

	float burst_factor_x = 0.0f;
	float burst_factor_y = 0.0f;
	float burst_factor_z = 0.0f;

	float burst_delay_timer = 0;
	XMFLOAT3 startpos = {};

	float normal_random = 1;
	float rotation_random = 0;
	float size_random = 0;
	float scaling_random = 1;

	float spawn_random = 0;
	float spawn_pause = 0;
	float spawn_pause_random = 0;

	uint endcolor_red = 255;
	uint endcolor_green = 255;
	uint endcolor_blue = 255;

	bool bFindFloor = false;
	float burst_factor_speed = 1.0f;
	float start_rotation = 0.0f;
	bool bFollowCamera = false;

	float random_position = 0.0f;
	float random_position_scale = 1.0f;
	uint32_t total_emit_count = 0;

	bool bTriggerOutDoor = true;
	bool bTriggerInDoor = false;
	bool bTriggerUnderWater = false;

	float randemit = 0;
	uint32_t randpause = 0;
	float distance_sort_bias = 0;
	float wpe_filler_1 = 0;
	float wpe_filler_2 = 0;
	float wpe_filler_3 = 0;

	DWORD64 emittimer = 0;
	inline void SetTimer(DWORD64 t) { emittimer = t; }
	inline DWORD64 GetTimer() const { return emittimer; }

	bool bVisible = true;
	inline bool IsVisible() const { return bVisible; }
	inline void SetVisible(bool value) { bVisible = value; }

	bool bActive = true;
	inline bool IsActive() const { return bActive; }
	inline void SetActive(bool value) { bActive = value; }

	bool bStatActive = false;
	inline bool IsStatActive() const { return bStatActive; }
	inline void SetStatActive(bool value) { bStatActive = value; }

	//#endif


	void SetMaxParticleCount(uint32_t value);
	uint32_t GetMaxParticleCount() const { return MAX_PARTICLES; }
	uint32_t GetMemorySizeInBytes() const;

	// Non-serialized attributes:
	XMFLOAT3 center;
	uint32_t statisticsReadBackIndex = 0;
	uint32_t layerMask = ~0u;

	inline bool IsDebug() const { return _flags & FLAG_DEBUG; }
	inline bool IsPaused() const { return _flags & FLAG_PAUSED; }
	inline bool IsSorted() const { return _flags & FLAG_SORTING; }
	inline bool IsDepthCollisionEnabled() const { return _flags & FLAG_DEPTHCOLLISION; }
	inline bool IsSPHEnabled() const { return _flags & FLAG_SPH_FLUIDSIMULATION; }
	inline bool IsVolumeEnabled() const { return _flags & FLAG_HAS_VOLUME; }
	inline bool IsFrameBlendingEnabled() const { return _flags & FLAG_FRAME_BLENDING; }

	inline void SetDebug(bool value) { if (value) { _flags |= FLAG_DEBUG; } else { _flags &= ~FLAG_DEBUG; } }
	inline void SetPaused(bool value) { if (value) { _flags |= FLAG_PAUSED; } else { _flags &= ~FLAG_PAUSED; } }
	inline void SetSorted(bool value) { if (value) { _flags |= FLAG_SORTING; } else { _flags &= ~FLAG_SORTING; } }
	inline void SetDepthCollisionEnabled(bool value) { if (value) { _flags |= FLAG_DEPTHCOLLISION; } else { _flags &= ~FLAG_DEPTHCOLLISION; } }
	inline void SetSPHEnabled(bool value) { if (value) { _flags |= FLAG_SPH_FLUIDSIMULATION; } else { _flags &= ~FLAG_SPH_FLUIDSIMULATION; } }
	inline void SetVolumeEnabled(bool value) { if (value) { _flags |= FLAG_HAS_VOLUME; } else { _flags &= ~FLAG_HAS_VOLUME; } }
	inline void SetFrameBlendingEnabled(bool value) { if (value) { _flags |= FLAG_FRAME_BLENDING; } else { _flags &= ~FLAG_FRAME_BLENDING; } }

	void Serialize(wiArchive& archive, wiECS::EntitySerializer& seri);

	static void Initialize();
};

}

