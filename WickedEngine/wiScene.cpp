#include "wiScene.h"
#include "wiMath.h"
#include "wiTextureHelper.h"
#include "wiResourceManager.h"
#include "wiPhysicsEngine.h"
#include "wiArchive.h"
#include "wiRenderer.h"
#include "wiJobSystem.h"
#include "wiSpinLock.h"
#include "wiHelper.h"
#include "wiRenderer.h"
#include "wiBackLog.h"

#include <functional>
#include <unordered_map>

#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
#include "optick.h"
#endif
#endif

#ifdef SHADERCOMPILER
extern bool g_bNoTerrainRender;
extern float fWickedCallShadowFarPlane;
extern float fWickedMaxCenterTest;
extern bool g_bDelayedShadows;
extern uint32_t iCulledPointShadows;
extern uint32_t iCulledSpotShadows;
extern uint32_t iCulledAnimations;
extern bool bEnable30FpsAnimations;
extern bool bEnableTerrainChunkCulling;
extern bool bEnablePointShadowCulling;
extern bool bEnableDelayPointShadow;
extern float pointShadowScaler;
extern bool bEnableSpotShadowCulling;
extern bool bEnableObjectCulling;
extern bool bEnableAnimationCulling;
extern bool bRaycastLowestLOD;
#else
#ifdef GGREDUCED
//PE: Sorry LMFIX need Wicked function.
#include "timeapi.h"
extern float fWickedMaxCenterTest;
extern float fWickedCallShadowFarPlane;
extern bool g_bNoTerrainRender;
extern bool g_bDelayedShadows;
extern uint32_t iCulledPointShadows;
extern uint32_t iCulledAnimations;
extern uint32_t iCulledSpotShadows;
extern bool bEnableTerrainChunkCulling;
extern bool bEnablePointShadowCulling;
extern bool bEnableDelayPointShadow;
extern float pointShadowScaler;
extern bool bEnableSpotShadowCulling;
extern bool bEnableObjectCulling;
extern bool bEnableAnimationCulling;
extern bool bEnable30FpsAnimations;
extern bool bRaycastLowestLOD;
#endif
#endif

//PE: https://github.com/turanszkij/WickedEngine/commit/4d736898771269baf908b8dbe2ca083505cfef01
//PE: multithreaded hierarchy update system
#ifdef GGREDUCED
struct TerrainChunkOcclusion
{
	AABB aabb = {};
	bool bChunkVisible = false;
	uint32_t history = 1;
	uint32_t writeQuery = 0;
};
namespace GGTerrain {
	extern "C" TerrainChunkOcclusion* GetChunkVisibleMem(int lod, int idx);
	extern "C" uint32_t GetChunkLodStart(void);

}
#define MTHREAD_HIERARCHY
#endif

using namespace wiECS;
using namespace wiGraphics;

namespace wiScene
{

	XMFLOAT3 TransformComponent::GetPosition() const
	{
		return *((XMFLOAT3*)&world._41);
	}
	XMFLOAT4 TransformComponent::GetRotation() const
	{
		XMFLOAT4 rotation;
		XMStoreFloat4(&rotation, GetRotationV());
		return rotation;
	}
	XMFLOAT3 TransformComponent::GetScale() const
	{
		XMFLOAT3 scale;
		XMStoreFloat3(&scale, GetScaleV());
		return scale;
	}
	XMVECTOR TransformComponent::GetPositionV() const
	{
		return XMLoadFloat3((XMFLOAT3*)&world._41);
	}
	XMVECTOR TransformComponent::GetRotationV() const
	{
		XMVECTOR S, R, T;
		XMMatrixDecompose(&S, &R, &T, XMLoadFloat4x4(&world));
		return R;
	}
	XMVECTOR TransformComponent::GetScaleV() const
	{
		XMVECTOR S, R, T;
		XMMatrixDecompose(&S, &R, &T, XMLoadFloat4x4(&world));
		return S;
	}
	XMMATRIX TransformComponent::GetLocalMatrix() const
	{
		XMVECTOR S_local = XMLoadFloat3(&scale_local);
		XMVECTOR R_local = XMLoadFloat4(&rotation_local);
		XMVECTOR T_local = XMLoadFloat3(&translation_local);
		return
			XMMatrixScalingFromVector(S_local) *
			XMMatrixRotationQuaternion(R_local) *
			XMMatrixTranslationFromVector(T_local);
	}
	void TransformComponent::UpdateTransform()
	{
		if (IsDirty())
		{
			SetDirty(false);

			XMStoreFloat4x4(&world, GetLocalMatrix());
		}
	}
	void TransformComponent::UpdateTransform_Parented(const TransformComponent& parent)
	{
		XMMATRIX W = GetLocalMatrix();
		XMMATRIX W_parent = XMLoadFloat4x4(&parent.world);
		W = W * W_parent;

		XMStoreFloat4x4(&world, W);
	}
	void TransformComponent::ApplyTransform()
	{
		SetDirty();

		XMVECTOR S, R, T;
		XMMatrixDecompose(&S, &R, &T, XMLoadFloat4x4(&world));
		XMStoreFloat3(&scale_local, S);
		XMStoreFloat4(&rotation_local, R);
		XMStoreFloat3(&translation_local, T);
	}
	void TransformComponent::ClearTransform()
	{
		SetDirty();
		scale_local = XMFLOAT3(1, 1, 1);
		rotation_local = XMFLOAT4(0, 0, 0, 1);
		translation_local = XMFLOAT3(0, 0, 0);
	}
	void TransformComponent::Translate(const XMFLOAT3& value)
	{
		SetDirty();
		translation_local.x += value.x;
		translation_local.y += value.y;
		translation_local.z += value.z;
	}
	void TransformComponent::Translate(const XMVECTOR& value)
	{
		XMFLOAT3 translation;
		XMStoreFloat3(&translation, value);
		Translate(translation);
	}
	void TransformComponent::RotateRollPitchYaw(const XMFLOAT3& value)
	{
		SetDirty();

		// This needs to be handled a bit differently
		XMVECTOR quat = XMLoadFloat4(&rotation_local);
		XMVECTOR x = XMQuaternionRotationRollPitchYaw(value.x, 0, 0);
		XMVECTOR y = XMQuaternionRotationRollPitchYaw(0, value.y, 0);
		XMVECTOR z = XMQuaternionRotationRollPitchYaw(0, 0, value.z);

		quat = XMQuaternionMultiply(x, quat);
		quat = XMQuaternionMultiply(quat, y);
		quat = XMQuaternionMultiply(z, quat);
		quat = XMQuaternionNormalize(quat);

		XMStoreFloat4(&rotation_local, quat);
	}
	void TransformComponent::Rotate(const XMFLOAT4& quaternion)
	{
		SetDirty();

		XMVECTOR result = XMQuaternionMultiply(XMLoadFloat4(&rotation_local), XMLoadFloat4(&quaternion));
		result = XMQuaternionNormalize(result);
		XMStoreFloat4(&rotation_local, result);
	}
	void TransformComponent::Rotate(const XMVECTOR& quaternion)
	{
		XMFLOAT4 rotation;
		XMStoreFloat4(&rotation, quaternion);
		Rotate(rotation);
	}
	void TransformComponent::Scale(const XMFLOAT3& value)
	{
		SetDirty();
		scale_local.x *= value.x;
		scale_local.y *= value.y;
		scale_local.z *= value.z;
	}
	void TransformComponent::Scale(const XMVECTOR& value)
	{
		XMFLOAT3 scale;
		XMStoreFloat3(&scale, value);
		Scale(scale);
	}
	void TransformComponent::MatrixTransform(const XMFLOAT4X4& matrix)
	{
		MatrixTransform(XMLoadFloat4x4(&matrix));
	}
	void TransformComponent::MatrixTransform(const XMMATRIX& matrix)
	{
		SetDirty();

		XMVECTOR S;
		XMVECTOR R;
		XMVECTOR T;
		XMMatrixDecompose(&S, &R, &T, GetLocalMatrix() * matrix);

		XMStoreFloat3(&scale_local, S);
		XMStoreFloat4(&rotation_local, R);
		XMStoreFloat3(&translation_local, T);
	}
	void TransformComponent::Lerp(const TransformComponent& a, const TransformComponent& b, float t)
	{
		SetDirty();

		XMVECTOR aS, aR, aT;
		XMMatrixDecompose(&aS, &aR, &aT, XMLoadFloat4x4(&a.world));

		XMVECTOR bS, bR, bT;
		XMMatrixDecompose(&bS, &bR, &bT, XMLoadFloat4x4(&b.world));

		XMVECTOR S = XMVectorLerp(aS, bS, t);
		XMVECTOR R = XMQuaternionSlerp(aR, bR, t);
		XMVECTOR T = XMVectorLerp(aT, bT, t);

		XMStoreFloat3(&scale_local, S);
		XMStoreFloat4(&rotation_local, R);
		XMStoreFloat3(&translation_local, T);
	}
	void TransformComponent::CatmullRom(const TransformComponent& a, const TransformComponent& b, const TransformComponent& c, const TransformComponent& d, float t)
	{
		SetDirty();

		XMVECTOR aS, aR, aT;
		XMMatrixDecompose(&aS, &aR, &aT, XMLoadFloat4x4(&a.world));

		XMVECTOR bS, bR, bT;
		XMMatrixDecompose(&bS, &bR, &bT, XMLoadFloat4x4(&b.world));

		XMVECTOR cS, cR, cT;
		XMMatrixDecompose(&cS, &cR, &cT, XMLoadFloat4x4(&c.world));

		XMVECTOR dS, dR, dT;
		XMMatrixDecompose(&dS, &dR, &dT, XMLoadFloat4x4(&d.world));

		XMVECTOR T = XMVectorCatmullRom(aT, bT, cT, dT, t);

		XMVECTOR setupA;
		XMVECTOR setupB;
		XMVECTOR setupC;

		aR = XMQuaternionNormalize(aR);
		bR = XMQuaternionNormalize(bR);
		cR = XMQuaternionNormalize(cR);
		dR = XMQuaternionNormalize(dR);

		XMQuaternionSquadSetup(&setupA, &setupB, &setupC, aR, bR, cR, dR);
		XMVECTOR R = XMQuaternionSquad(bR, setupA, setupB, setupC, t);

		XMVECTOR S = XMVectorCatmullRom(aS, bS, cS, dS, t);

		XMStoreFloat3(&translation_local, T);
		XMStoreFloat4(&rotation_local, R);
		XMStoreFloat3(&scale_local, S);
	}

	void MaterialComponent::WriteShaderMaterial(ShaderMaterial* dest) const
	{
		dest->baseColor = baseColor;
		dest->specularColor = specularColor;
		dest->emissiveColor = emissiveColor;
		dest->texMulAdd = texMulAdd;
		dest->roughness = roughness;
		dest->reflectance = reflectance;
		dest->metalness = metalness;
		dest->refraction = refraction;
		dest->normalMapStrength = (textures[NORMALMAP].resource == nullptr ? 0 : normalMapStrength);
		dest->parallaxOcclusionMapping = parallaxOcclusionMapping;
		dest->displacementMapping = displacementMapping;

		dest->customShaderParam1 = customShaderParam1;
		dest->customShaderParam2 = customShaderParam2;
		dest->customShaderParam3 = customShaderParam3;
		dest->customShaderParam4 = customShaderParam4;
		dest->customShaderParam5 = customShaderParam5;
		dest->customShaderParam6 = customShaderParam6;
		dest->customShaderParam7 = customShaderParam7;

		//PE: https://github.com/turanszkij/WickedEngine/commit/fcc3c3b3b0723966d9441d8c2de9b3924eb11565
		/* 
		dest->subsurfaceScattering = subsurfaceScattering;
		dest->subsurfaceScattering.x *= dest->subsurfaceScattering.w;
		dest->subsurfaceScattering.y *= dest->subsurfaceScattering.w;
		dest->subsurfaceScattering.z *= dest->subsurfaceScattering.w;
		dest->subsurfaceScattering_inv.x = 1.0f / ((1 + dest->subsurfaceScattering.x) * (1 + dest->subsurfaceScattering.x));
		dest->subsurfaceScattering_inv.y = 1.0f / ((1 + dest->subsurfaceScattering.y) * (1 + dest->subsurfaceScattering.y));
		dest->subsurfaceScattering_inv.z = 1.0f / ((1 + dest->subsurfaceScattering.z) * (1 + dest->subsurfaceScattering.z));
		dest->subsurfaceScattering_inv.w = 1.0f / ((1 + dest->subsurfaceScattering.w) * (1 + dest->subsurfaceScattering.w));
		*/

		XMFLOAT4 sss = subsurfaceScattering;
		sss.x *= sss.w;
		sss.y *= sss.w;
		sss.z *= sss.w;
		XMFLOAT4 sss_inv = XMFLOAT4(
			sss_inv.x = 1.0f / ((1 + sss.x) * (1 + sss.x)),
			sss_inv.y = 1.0f / ((1 + sss.y) * (1 + sss.y)),
			sss_inv.z = 1.0f / ((1 + sss.z) * (1 + sss.z)),
			sss_inv.w = 1.0f / ((1 + sss.w) * (1 + sss.w))
		);
		dest->subsurfaceScattering = sss;
		dest->subsurfaceScattering_inv = sss_inv;


		dest->uvset_baseColorMap = textures[BASECOLORMAP].GetUVSet();
		dest->uvset_surfaceMap = textures[SURFACEMAP].GetUVSet();
		dest->uvset_normalMap = textures[NORMALMAP].GetUVSet();
		dest->uvset_displacementMap = textures[DISPLACEMENTMAP].GetUVSet();
		dest->uvset_emissiveMap = textures[EMISSIVEMAP].GetUVSet();
		dest->uvset_occlusionMap = textures[OCCLUSIONMAP].GetUVSet();
		dest->uvset_transmissionMap = textures[TRANSMISSIONMAP].GetUVSet();
		dest->uvset_sheenColorMap = textures[SHEENCOLORMAP].GetUVSet();
		dest->uvset_sheenRoughnessMap = textures[SHEENROUGHNESSMAP].GetUVSet();
		dest->uvset_clearcoatMap = textures[CLEARCOATMAP].GetUVSet();
		dest->uvset_clearcoatRoughnessMap = textures[CLEARCOATROUGHNESSMAP].GetUVSet();
		dest->uvset_clearcoatNormalMap = textures[CLEARCOATNORMALMAP].GetUVSet();
		dest->uvset_specularMap = textures[SPECULARMAP].GetUVSet();
		dest->sheenColor = sheenColor;
		dest->sheenRoughness = sheenRoughness;
		dest->clearcoat = clearcoat;
		dest->clearcoatRoughness = clearcoatRoughness;
		dest->alphaTest = 1 - alphaRef + 1.0f / 256.0f; // 256 so that it is just about smaller than 1 unorm unit (1.0/255.0)
		dest->layerMask = layerMask;
		dest->transmission = transmission;
		dest->options = 0;
		if (IsUsingVertexColors())
		{
			dest->options |= SHADERMATERIAL_OPTION_BIT_USE_VERTEXCOLORS;
		}
		if (IsUsingSpecularGlossinessWorkflow())
		{
			dest->options |= SHADERMATERIAL_OPTION_BIT_SPECULARGLOSSINESS_WORKFLOW;
		}
		if (IsOcclusionEnabled_Primary())
		{
			dest->options |= SHADERMATERIAL_OPTION_BIT_OCCLUSION_PRIMARY;
		}
		if (IsOcclusionEnabled_Secondary())
		{
			dest->options |= SHADERMATERIAL_OPTION_BIT_OCCLUSION_SECONDARY;
		}
		if (IsUsingWind())
		{
			dest->options |= SHADERMATERIAL_OPTION_BIT_USE_WIND;
		}
		if (IsReceiveShadow())
		{
			dest->options |= SHADERMATERIAL_OPTION_BIT_RECEIVE_SHADOW;
		}
		if (IsCastingShadow())
		{
			dest->options |= SHADERMATERIAL_OPTION_BIT_CAST_SHADOW;
		}

		GraphicsDevice* device = wiRenderer::GetDevice();
		dest->texture_basecolormap_index = device->GetDescriptorIndex(textures[BASECOLORMAP].GetGPUResource(), SRV);
		dest->texture_surfacemap_index = device->GetDescriptorIndex(textures[SURFACEMAP].GetGPUResource(), SRV);
		dest->texture_emissivemap_index = device->GetDescriptorIndex(textures[EMISSIVEMAP].GetGPUResource(), SRV);
		dest->texture_normalmap_index = device->GetDescriptorIndex(textures[NORMALMAP].GetGPUResource(), SRV);
		dest->texture_displacementmap_index = device->GetDescriptorIndex(textures[DISPLACEMENTMAP].GetGPUResource(), SRV);
		dest->texture_occlusionmap_index = device->GetDescriptorIndex(textures[OCCLUSIONMAP].GetGPUResource(), SRV);
		dest->texture_transmissionmap_index = device->GetDescriptorIndex(textures[TRANSMISSIONMAP].GetGPUResource(), SRV);
		dest->texture_sheencolormap_index = device->GetDescriptorIndex(textures[SHEENCOLORMAP].GetGPUResource(), SRV);
		dest->texture_sheenroughnessmap_index = device->GetDescriptorIndex(textures[SHEENROUGHNESSMAP].GetGPUResource(), SRV);
		dest->texture_clearcoatmap_index = device->GetDescriptorIndex(textures[CLEARCOATMAP].GetGPUResource(), SRV);
		dest->texture_clearcoatroughnessmap_index = device->GetDescriptorIndex(textures[CLEARCOATROUGHNESSMAP].GetGPUResource(), SRV);
		dest->texture_clearcoatnormalmap_index = device->GetDescriptorIndex(textures[CLEARCOATNORMALMAP].GetGPUResource(), SRV);
		dest->texture_specularmap_index = device->GetDescriptorIndex(textures[SPECULARMAP].GetGPUResource(), SRV);

		dest->baseColorAtlasMulAdd = XMFLOAT4(0, 0, 0, 0);
		dest->surfaceMapAtlasMulAdd = XMFLOAT4(0, 0, 0, 0);
		dest->emissiveMapAtlasMulAdd = XMFLOAT4(0, 0, 0, 0);
		dest->normalMapAtlasMulAdd = XMFLOAT4(0, 0, 0, 0);
	}
	void MaterialComponent::WriteTextures(const wiGraphics::GPUResource** dest, int count) const
	{
		count = std::min(count, (int)TEXTURESLOT_COUNT);
		for (int i = 0; i < count; ++i)
		{
			dest[i] = textures[i].GetGPUResource();
		}
	}
	uint32_t MaterialComponent::GetRenderTypes() const
	{
		if (IsCustomShader() && customShaderID < (int)wiRenderer::GetCustomShaders().size())
		{
			auto& customShader = wiRenderer::GetCustomShaders()[customShaderID];
			return customShader.renderTypeFlags;
		}
		if (shaderType == SHADERTYPE_WATER)
		{
			return RENDERTYPE_TRANSPARENT | RENDERTYPE_WATER;
		}
		if (transmission > 0)
		{
			return RENDERTYPE_TRANSPARENT;
		}
		if (userBlendMode == BLENDMODE_OPAQUE)
		{
			return RENDERTYPE_OPAQUE;
		}
		return RENDERTYPE_TRANSPARENT;
	}
	void MaterialComponent::CreateRenderData() 
	{
		for (auto& x : textures)
		{
#ifdef GGREDUCED
			if (!x.name.empty() && !x.resource)
#else
			if (!x.name.empty())
#endif
			{
				x.resource = wiResourceManager::Load(x.name, wiResourceManager::IMPORT_RETAIN_FILEDATA);
			}
		}

		ShaderMaterial shadermat;
		WriteShaderMaterial(&shadermat);

		SubresourceData data;
		data.pSysMem = &shadermat;

		GraphicsDevice* device = wiRenderer::GetDevice();
		GPUBufferDesc desc;
		desc.Usage = USAGE_DEFAULT;
		desc.BindFlags = BIND_CONSTANT_BUFFER;
		if (device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_BINDLESS_DESCRIPTORS))
		{
			desc.BindFlags |= BIND_SHADER_RESOURCE;
			desc.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		}
		desc.ByteWidth = sizeof(MaterialCB);
		device->CreateBuffer(&desc, &data, &constantBuffer);
	}
	uint32_t MaterialComponent::GetStencilRef() const
	{
		return wiRenderer::CombineStencilrefs(engineStencilRef, userStencilRef);
	}

	void MeshComponent::CreateRenderData()
	{
		GraphicsDevice* device = wiRenderer::GetDevice();

		// Create index buffer GPU data:
		{
			GPUBufferDesc bd;
			bd.Usage = USAGE_IMMUTABLE;
			bd.CPUAccessFlags = 0;
			bd.BindFlags = BIND_INDEX_BUFFER | BIND_SHADER_RESOURCE;
			bd.MiscFlags = 0;
			if (device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_PIPELINE) || device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_INLINE))
			{
				bd.MiscFlags |= RESOURCE_MISC_RAY_TRACING;
			}

			SubresourceData initData;

			if (GetIndexFormat() == INDEXFORMAT_32BIT)
			{
				bd.StructureByteStride = sizeof(uint32_t);
				bd.Format = FORMAT_R32_UINT;
				bd.ByteWidth = uint32_t(sizeof(uint32_t) * indices.size());

				// Use indices directly since vector is in correct format
				static_assert(std::is_same<decltype(indices)::value_type, uint32_t>::value, "indices not in INDEXFORMAT_32BIT");
				initData.pSysMem = indices.data();

				device->CreateBuffer(&bd, &initData, &indexBuffer);
				device->SetName(&indexBuffer, "indexBuffer_32bit");
			}
			else
			{
				bd.StructureByteStride = sizeof(uint16_t);
				bd.Format = FORMAT_R16_UINT;
				bd.ByteWidth = uint32_t(sizeof(uint16_t) * indices.size());

				std::vector<uint16_t> gpuIndexData(indices.size());
				std::copy(indices.begin(), indices.end(), gpuIndexData.begin());
				initData.pSysMem = gpuIndexData.data();

				device->CreateBuffer(&bd, &initData, &indexBuffer);
				device->SetName(&indexBuffer, "indexBuffer_16bit");
			}
		}


		XMFLOAT3 _min = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
		XMFLOAT3 _max = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

		// vertexBuffer - POSITION + NORMAL + WIND:
		{
		    if (!targets.empty())
		    {
				vertex_positions_morphed.resize(vertex_positions.size());
				dirty_morph = true;
		    }

			std::vector<Vertex_POS> vertices(vertex_positions.size());
			for (size_t i = 0; i < vertices.size(); ++i)
			{
				const XMFLOAT3& pos = vertex_positions[i];
			    XMFLOAT3 nor = vertex_normals.empty() ? XMFLOAT3(1, 1, 1) : vertex_normals[i];
			    XMStoreFloat3(&nor, XMVector3Normalize(XMLoadFloat3(&nor)));
				const uint8_t wind = vertex_windweights.empty() ? 0xFF : vertex_windweights[i];
				vertices[i].FromFULL(pos, nor, wind);

				_min = wiMath::Min(_min, pos);
				_max = wiMath::Max(_max, pos);
			}

			GPUBufferDesc bd;
			bd.Usage = USAGE_DEFAULT;
			bd.CPUAccessFlags = 0;
			bd.BindFlags = BIND_VERTEX_BUFFER | BIND_SHADER_RESOURCE;
			bd.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			if (device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_PIPELINE) || device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_INLINE))
			{
				bd.MiscFlags |= RESOURCE_MISC_RAY_TRACING;
			}
			bd.ByteWidth = (uint32_t)(sizeof(Vertex_POS) * vertices.size());

			SubresourceData InitData;
			InitData.pSysMem = vertices.data();
			device->CreateBuffer(&bd, &InitData, &vertexBuffer_POS);
			device->SetName(&vertexBuffer_POS, "vertexBuffer_POS");
		}

		// vertexBuffer - TANGENTS
		if(!vertex_uvset_0.empty())
		{
			if (vertex_tangents.empty())
			{
				// Generate tangents if not found:
				vertex_tangents.resize(vertex_positions.size());

				for (size_t i = 0; i < indices.size(); i += 3)
				{
					const uint32_t i0 = indices[i + 0];
					const uint32_t i1 = indices[i + 1];
					const uint32_t i2 = indices[i + 2];

					const XMFLOAT3 v0 = vertex_positions[i0];
					const XMFLOAT3 v1 = vertex_positions[i1];
					const XMFLOAT3 v2 = vertex_positions[i2];

					const XMFLOAT2 u0 = vertex_uvset_0[i0];
					const XMFLOAT2 u1 = vertex_uvset_0[i1];
					const XMFLOAT2 u2 = vertex_uvset_0[i2];

					const XMFLOAT3 n0 = vertex_normals[i0];
					const XMFLOAT3 n1 = vertex_normals[i1];
					const XMFLOAT3 n2 = vertex_normals[i2];

					const XMVECTOR nor0 = XMLoadFloat3(&n0);
					const XMVECTOR nor1 = XMLoadFloat3(&n1);
					const XMVECTOR nor2 = XMLoadFloat3(&n2);

					const XMVECTOR facenormal = XMVector3Normalize(nor0 + nor1 + nor2);

					const float x1 = v1.x - v0.x;
					const float x2 = v2.x - v0.x;
					const float y1 = v1.y - v0.y;
					const float y2 = v2.y - v0.y;
					const float z1 = v1.z - v0.z;
					const float z2 = v2.z - v0.z;

					const float s1 = u1.x - u0.x;
					const float s2 = u2.x - u0.x;
					const float t1 = u1.y - u0.y;
					const float t2 = u2.y - u0.y;

					const float r = 1.0f / (s1 * t2 - s2 * t1);
					const XMVECTOR sdir = XMVectorSet((t2 * x1 - t1 * x2) * r, (t2 * y1 - t1 * y2) * r,
						(t2 * z1 - t1 * z2) * r, 0);
					const XMVECTOR tdir = XMVectorSet((s1 * x2 - s2 * x1) * r, (s1 * y2 - s2 * y1) * r,
						(s1 * z2 - s2 * z1) * r, 0);

					XMVECTOR tangent;
					tangent = XMVector3Normalize(sdir - facenormal * XMVector3Dot(facenormal, sdir));
					float sign = XMVectorGetX(XMVector3Dot(XMVector3Cross(tangent, facenormal), tdir)) < 0.0f ? -1.0f : 1.0f;

					XMFLOAT3 t;
					XMStoreFloat3(&t, tangent);

					vertex_tangents[i0].x += t.x;
					vertex_tangents[i0].y += t.y;
					vertex_tangents[i0].z += t.z;
					vertex_tangents[i0].w = sign;

					vertex_tangents[i1].x += t.x;
					vertex_tangents[i1].y += t.y;
					vertex_tangents[i1].z += t.z;
					vertex_tangents[i1].w = sign;

					vertex_tangents[i2].x += t.x;
					vertex_tangents[i2].y += t.y;
					vertex_tangents[i2].z += t.z;
					vertex_tangents[i2].w = sign;
				}

			}

			std::vector<Vertex_TAN> vertices(vertex_tangents.size());
			for (size_t i = 0; i < vertex_tangents.size(); ++i)
			{
				vertices[i].FromFULL(vertex_tangents[i]);
			}

			GPUBufferDesc bd;
			bd.Usage = USAGE_DEFAULT;
			bd.CPUAccessFlags = 0;
			bd.BindFlags = BIND_VERTEX_BUFFER | BIND_SHADER_RESOURCE;
			bd.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			bd.StructureByteStride = sizeof(Vertex_TAN);
			bd.ByteWidth = (uint32_t)(bd.StructureByteStride * vertices.size());

			SubresourceData InitData;
			InitData.pSysMem = vertices.data();
			device->CreateBuffer(&bd, &InitData, &vertexBuffer_TAN);
			device->SetName(&vertexBuffer_TAN, "vertexBuffer_TAN");
		}

		aabb = AABB(_min, _max);

		// skinning buffers:
		if (!vertex_boneindices.empty())
		{
			std::vector<Vertex_BON> vertices(vertex_boneindices.size());
			for (size_t i = 0; i < vertices.size(); ++i)
			{
				XMFLOAT4& wei = vertex_boneweights[i];
				// normalize bone weights
				float len = wei.x + wei.y + wei.z + wei.w;
				if (len > 0)
				{
					wei.x /= len;
					wei.y /= len;
					wei.z /= len;
					wei.w /= len;
				}
				vertices[i].FromFULL(vertex_boneindices[i], wei);
			}

			GPUBufferDesc bd;
			bd.Usage = USAGE_IMMUTABLE;
			bd.BindFlags = BIND_SHADER_RESOURCE;
			bd.CPUAccessFlags = 0;
			bd.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			bd.ByteWidth = (uint32_t)(sizeof(Vertex_BON) * vertices.size());

			SubresourceData InitData;
			InitData.pSysMem = vertices.data();
			device->CreateBuffer(&bd, &InitData, &vertexBuffer_BON);

			bd.Usage = USAGE_DEFAULT;
			bd.BindFlags = BIND_VERTEX_BUFFER | BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
			bd.CPUAccessFlags = 0;
			bd.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

			if (!vertex_tangents.empty())
			{
				bd.ByteWidth = (uint32_t)(sizeof(Vertex_TAN) * vertex_tangents.size());
				device->CreateBuffer(&bd, nullptr, &streamoutBuffer_TAN);
				device->SetName(&streamoutBuffer_TAN, "streamoutBuffer_TAN");
			}

			bd.ByteWidth = (uint32_t)(sizeof(Vertex_POS) * vertex_positions.size());
			if (device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_PIPELINE) || device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_INLINE))
			{
				bd.MiscFlags |= RESOURCE_MISC_RAY_TRACING;
		}
			device->CreateBuffer(&bd, nullptr, &streamoutBuffer_POS);
			device->SetName(&streamoutBuffer_POS, "streamoutBuffer_POS");
		}

		// vertexBuffer - UV SET 0
		if(!vertex_uvset_0.empty())
		{
			std::vector<Vertex_TEX> vertices(vertex_uvset_0.size());
			for (size_t i = 0; i < vertices.size(); ++i)
			{
				vertices[i].FromFULL(vertex_uvset_0[i]);
			}

			GPUBufferDesc bd;
			bd.Usage = USAGE_IMMUTABLE;
			bd.CPUAccessFlags = 0;
			bd.BindFlags = BIND_VERTEX_BUFFER | BIND_SHADER_RESOURCE;
			bd.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			bd.StructureByteStride = sizeof(Vertex_TEX);
			bd.ByteWidth = (uint32_t)(bd.StructureByteStride * vertices.size());

			SubresourceData InitData;
			InitData.pSysMem = vertices.data();
			device->CreateBuffer(&bd, &InitData, &vertexBuffer_UV0);
			device->SetName(&vertexBuffer_UV0, "vertexBuffer_UV0");
		}

		// vertexBuffer - UV SET 1
		if (!vertex_uvset_1.empty())
		{
			std::vector<Vertex_TEX> vertices(vertex_uvset_1.size());
			for (size_t i = 0; i < vertices.size(); ++i)
			{
				vertices[i].FromFULL(vertex_uvset_1[i]);
			}

			GPUBufferDesc bd;
			bd.Usage = USAGE_IMMUTABLE;
			bd.CPUAccessFlags = 0;
			bd.BindFlags = BIND_VERTEX_BUFFER | BIND_SHADER_RESOURCE;
			bd.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			bd.StructureByteStride = sizeof(Vertex_TEX);
			bd.ByteWidth = (uint32_t)(bd.StructureByteStride * vertices.size());

			SubresourceData InitData;
			InitData.pSysMem = vertices.data();
			device->CreateBuffer(&bd, &InitData, &vertexBuffer_UV1);
			device->SetName(&vertexBuffer_UV1, "vertexBuffer_UV1");
		}

		// vertexBuffer - COLORS
		if (!vertex_colors.empty())
		{
			GPUBufferDesc bd;
			bd.Usage = USAGE_IMMUTABLE;
			bd.CPUAccessFlags = 0;
			bd.BindFlags = BIND_VERTEX_BUFFER | BIND_SHADER_RESOURCE;
			bd.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			bd.StructureByteStride = sizeof(Vertex_COL);
			bd.ByteWidth = (uint32_t)(bd.StructureByteStride * vertex_colors.size());

			SubresourceData InitData;
			InitData.pSysMem = vertex_colors.data();
			device->CreateBuffer(&bd, &InitData, &vertexBuffer_COL);
			device->SetName(&vertexBuffer_COL, "vertexBuffer_COL");
		}

		// vertexBuffer - ATLAS
		if (!vertex_atlas.empty())
		{
			std::vector<Vertex_TEX> vertices(vertex_atlas.size());
			for (size_t i = 0; i < vertices.size(); ++i)
			{
				vertices[i].FromFULL(vertex_atlas[i]);
			}

			GPUBufferDesc bd;
			bd.Usage = USAGE_IMMUTABLE;
			bd.CPUAccessFlags = 0;
			bd.BindFlags = BIND_VERTEX_BUFFER | BIND_SHADER_RESOURCE;
			bd.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			bd.StructureByteStride = sizeof(Vertex_TEX);
			bd.ByteWidth = (uint32_t)(bd.StructureByteStride * vertices.size());

			SubresourceData InitData;
			InitData.pSysMem = vertices.data();
			device->CreateBuffer(&bd, &InitData, &vertexBuffer_ATL);
			device->SetName(&vertexBuffer_ATL, "vertexBuffer_ATL");
		}

		// vertexBuffer - SUBSETS
		{
			vertex_subsets.resize(vertex_positions.size());

			uint32_t subsetCounter = 0;
			for (auto& subset : subsets)
			{
				for (uint32_t i = 0; i < subset.indexCount; ++i)
				{
					uint32_t index = indices[subset.indexOffset + i];
					vertex_subsets[index] = subsetCounter;
				}
				subsetCounter++;
			}

			GPUBufferDesc bd;
			bd.Usage = USAGE_IMMUTABLE;
			bd.CPUAccessFlags = 0;
			bd.BindFlags = BIND_VERTEX_BUFFER | BIND_SHADER_RESOURCE;
			bd.MiscFlags = 0;
			bd.StructureByteStride = sizeof(uint8_t);
			bd.ByteWidth = (uint32_t)(bd.StructureByteStride * vertex_subsets.size());
			bd.Format = FORMAT_R8_UINT;

			SubresourceData InitData;
			InitData.pSysMem = vertex_subsets.data();
			device->CreateBuffer(&bd, &InitData, &vertexBuffer_SUB);
			device->SetName(&vertexBuffer_SUB, "vertexBuffer_SUB");// vertexBuffer_SUB;
		}

		// vertexBuffer_PRE will be created on demand later!
		vertexBuffer_PRE = GPUBuffer();


		if (device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_PIPELINE) || device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_INLINE))
		{
			BLAS_state = BLAS_STATE_NEEDS_REBUILD;

			RaytracingAccelerationStructureDesc desc;
			desc.type = RaytracingAccelerationStructureDesc::BOTTOMLEVEL;

			if (streamoutBuffer_POS.IsValid())
			{
				desc._flags |= RaytracingAccelerationStructureDesc::FLAG_ALLOW_UPDATE;
				desc._flags |= RaytracingAccelerationStructureDesc::FLAG_PREFER_FAST_BUILD;
			}
			else
			{
				desc._flags |= RaytracingAccelerationStructureDesc::FLAG_PREFER_FAST_TRACE;
			}

			for (auto& subset : subsets)
			{
				desc.bottomlevel.geometries.emplace_back();
				auto& geometry = desc.bottomlevel.geometries.back();
				geometry.type = RaytracingAccelerationStructureDesc::BottomLevel::Geometry::TRIANGLES;
				geometry.triangles.vertexBuffer = streamoutBuffer_POS.IsValid() ? streamoutBuffer_POS : vertexBuffer_POS;
				geometry.triangles.indexBuffer = indexBuffer;
				geometry.triangles.indexFormat = GetIndexFormat();
				geometry.triangles.indexCount = subset.indexCount;
				geometry.triangles.indexOffset = subset.indexOffset;
				geometry.triangles.vertexCount = (uint32_t)vertex_positions.size();
				geometry.triangles.vertexFormat = FORMAT_R32G32B32_FLOAT;
				geometry.triangles.vertexStride = sizeof(MeshComponent::Vertex_POS);
			}

			bool success = device->CreateRaytracingAccelerationStructure(&desc, &BLAS);
			assert(success);
			device->SetName(&BLAS, "BLAS");
		}

		if(device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_BINDLESS_DESCRIPTORS))
		{
			dirty_bindless = true;

			GPUBufferDesc desc;
			desc.BindFlags = BIND_SHADER_RESOURCE;
			desc.MiscFlags = RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			desc.ByteWidth = sizeof(ShaderMesh);
			bool success = device->CreateBuffer(&desc, nullptr, &descriptor);
			assert(success);

			desc.BindFlags = BIND_SHADER_RESOURCE;
			desc.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(ShaderMeshSubset);
			desc.ByteWidth = desc.StructureByteStride * (uint32_t)subsets.size();
			success = device->CreateBuffer(&desc, nullptr, &subsetBuffer);
			assert(success);
		}
	}
	void MeshComponent::WriteShaderMesh(ShaderMesh* dest) const
	{
		GraphicsDevice* device = wiRenderer::GetDevice();
		dest->ib = device->GetDescriptorIndex(&indexBuffer, SRV);
		if (streamoutBuffer_POS.IsValid())
		{
			dest->vb_pos_nor_wind = device->GetDescriptorIndex(&streamoutBuffer_POS, SRV);
		}
		else
		{
			dest->vb_pos_nor_wind = device->GetDescriptorIndex(&vertexBuffer_POS, SRV);
		}
		if (streamoutBuffer_TAN.IsValid())
		{
			dest->vb_tan = device->GetDescriptorIndex(&streamoutBuffer_TAN, SRV);
		}
		else
		{
			dest->vb_tan = device->GetDescriptorIndex(&vertexBuffer_TAN, SRV);
		}
		dest->vb_col = device->GetDescriptorIndex(&vertexBuffer_COL, SRV);
		dest->vb_uv0 = device->GetDescriptorIndex(&vertexBuffer_UV0, SRV);
		dest->vb_uv1 = device->GetDescriptorIndex(&vertexBuffer_UV1, SRV);
		dest->vb_atl = device->GetDescriptorIndex(&vertexBuffer_ATL, SRV);
		dest->vb_pre = device->GetDescriptorIndex(&vertexBuffer_PRE, SRV);
		dest->blendmaterial1 = terrain_material1_index;
		dest->blendmaterial2 = terrain_material2_index;
		dest->blendmaterial3 = terrain_material3_index;
		dest->subsetbuffer = device->GetDescriptorIndex(&subsetBuffer, SRV);
	}
	void MeshComponent::ComputeNormals(COMPUTE_NORMALS compute)
	{
		// Start recalculating normals:

		if(compute != COMPUTE_NORMALS_SMOOTH_FAST)
		{
			// Compute hard surface normals:

			// Right now they are always computed even before smooth setting

			std::vector<uint32_t> newIndexBuffer;
			std::vector<XMFLOAT3> newPositionsBuffer;
			std::vector<XMFLOAT3> newNormalsBuffer;
			std::vector<XMFLOAT2> newUV0Buffer;
			std::vector<XMFLOAT2> newUV1Buffer;
			std::vector<XMFLOAT2> newAtlasBuffer;
			std::vector<XMUINT4> newBoneIndicesBuffer;
			std::vector<XMFLOAT4> newBoneWeightsBuffer;
			std::vector<uint32_t> newColorsBuffer;

			for (size_t face = 0; face < indices.size() / 3; face++)
			{
				uint32_t i0 = indices[face * 3 + 0];
				uint32_t i1 = indices[face * 3 + 1];
				uint32_t i2 = indices[face * 3 + 2];

				XMFLOAT3& p0 = vertex_positions[i0];
				XMFLOAT3& p1 = vertex_positions[i1];
				XMFLOAT3& p2 = vertex_positions[i2];

				XMVECTOR U = XMLoadFloat3(&p2) - XMLoadFloat3(&p0);
				XMVECTOR V = XMLoadFloat3(&p1) - XMLoadFloat3(&p0);

				XMVECTOR N = XMVector3Cross(U, V);
				N = XMVector3Normalize(N);

				XMFLOAT3 normal;
				XMStoreFloat3(&normal, N);

				newPositionsBuffer.push_back(p0);
				newPositionsBuffer.push_back(p1);
				newPositionsBuffer.push_back(p2);

				newNormalsBuffer.push_back(normal);
				newNormalsBuffer.push_back(normal);
				newNormalsBuffer.push_back(normal);

				if (!vertex_uvset_0.empty())
				{
					newUV0Buffer.push_back(vertex_uvset_0[i0]);
					newUV0Buffer.push_back(vertex_uvset_0[i1]);
					newUV0Buffer.push_back(vertex_uvset_0[i2]);
				}

				if (!vertex_uvset_1.empty())
				{
					newUV1Buffer.push_back(vertex_uvset_1[i0]);
					newUV1Buffer.push_back(vertex_uvset_1[i1]);
					newUV1Buffer.push_back(vertex_uvset_1[i2]);
				}

				if (!vertex_atlas.empty())
				{
					newAtlasBuffer.push_back(vertex_atlas[i0]);
					newAtlasBuffer.push_back(vertex_atlas[i1]);
					newAtlasBuffer.push_back(vertex_atlas[i2]);
				}

				if (!vertex_boneindices.empty())
				{
					newBoneIndicesBuffer.push_back(vertex_boneindices[i0]);
					newBoneIndicesBuffer.push_back(vertex_boneindices[i1]);
					newBoneIndicesBuffer.push_back(vertex_boneindices[i2]);
				}

				if (!vertex_boneweights.empty())
				{
					newBoneWeightsBuffer.push_back(vertex_boneweights[i0]);
					newBoneWeightsBuffer.push_back(vertex_boneweights[i1]);
					newBoneWeightsBuffer.push_back(vertex_boneweights[i2]);
				}

				if (!vertex_colors.empty())
				{
					newColorsBuffer.push_back(vertex_colors[i0]);
					newColorsBuffer.push_back(vertex_colors[i1]);
					newColorsBuffer.push_back(vertex_colors[i2]);
				}

				newIndexBuffer.push_back(static_cast<uint32_t>(newIndexBuffer.size()));
				newIndexBuffer.push_back(static_cast<uint32_t>(newIndexBuffer.size()));
				newIndexBuffer.push_back(static_cast<uint32_t>(newIndexBuffer.size()));
			}

			// For hard surface normals, we created a new mesh in the previous loop through faces, so swap data:
			vertex_positions = newPositionsBuffer;
			vertex_normals = newNormalsBuffer;
			vertex_uvset_0 = newUV0Buffer;
			vertex_uvset_1 = newUV1Buffer;
			vertex_atlas = newAtlasBuffer;
			vertex_colors = newColorsBuffer;
			if (!vertex_boneindices.empty())
			{
				vertex_boneindices = newBoneIndicesBuffer;
			}
			if (!vertex_boneweights.empty())
			{
				vertex_boneweights = newBoneWeightsBuffer;
			}
			indices = newIndexBuffer;
		}

		switch (compute)
		{
		case wiScene::MeshComponent::COMPUTE_NORMALS_HARD: 
		break;

		case wiScene::MeshComponent::COMPUTE_NORMALS_SMOOTH:
		{
			// Compute smooth surface normals:

			// 1.) Zero normals, they will be averaged later
			for (size_t i = 0; i < vertex_normals.size(); i++)
			{
				vertex_normals[i] = XMFLOAT3(0, 0, 0);
			}

			// 2.) Find identical vertices by POSITION, accumulate face normals
			for (size_t i = 0; i < vertex_positions.size(); i++)
			{
				XMFLOAT3& v_search_pos = vertex_positions[i];

				for (size_t ind = 0; ind < indices.size() / 3; ++ind)
				{
					uint32_t i0 = indices[ind * 3 + 0];
					uint32_t i1 = indices[ind * 3 + 1];
					uint32_t i2 = indices[ind * 3 + 2];

					XMFLOAT3& v0 = vertex_positions[i0];
					XMFLOAT3& v1 = vertex_positions[i1];
					XMFLOAT3& v2 = vertex_positions[i2];

					bool match_pos0 =
						fabs(v_search_pos.x - v0.x) < FLT_EPSILON &&
						fabs(v_search_pos.y - v0.y) < FLT_EPSILON &&
						fabs(v_search_pos.z - v0.z) < FLT_EPSILON;

					bool match_pos1 =
						fabs(v_search_pos.x - v1.x) < FLT_EPSILON &&
						fabs(v_search_pos.y - v1.y) < FLT_EPSILON &&
						fabs(v_search_pos.z - v1.z) < FLT_EPSILON;

					bool match_pos2 =
						fabs(v_search_pos.x - v2.x) < FLT_EPSILON &&
						fabs(v_search_pos.y - v2.y) < FLT_EPSILON &&
						fabs(v_search_pos.z - v2.z) < FLT_EPSILON;

					if (match_pos0 || match_pos1 || match_pos2)
					{
						XMVECTOR U = XMLoadFloat3(&v2) - XMLoadFloat3(&v0);
						XMVECTOR V = XMLoadFloat3(&v1) - XMLoadFloat3(&v0);

						XMVECTOR N = XMVector3Cross(U, V);
						N = XMVector3Normalize(N);

						XMFLOAT3 normal;
						XMStoreFloat3(&normal, N);

						vertex_normals[i].x += normal.x;
						vertex_normals[i].y += normal.y;
						vertex_normals[i].z += normal.z;
					}

				}
			}

			// 3.) Find duplicated vertices by POSITION and UV0 and UV1 and ATLAS and SUBSET and remove them:
			for (auto& subset : subsets)
			{
				for (uint32_t i = 0; i < subset.indexCount - 1; i++)
				{
					uint32_t ind0 = indices[subset.indexOffset + (uint32_t)i];
					const XMFLOAT3& p0 = vertex_positions[ind0];
					const XMFLOAT2& u00 = vertex_uvset_0.empty() ? XMFLOAT2(0, 0) : vertex_uvset_0[ind0];
					const XMFLOAT2& u10 = vertex_uvset_1.empty() ? XMFLOAT2(0, 0) : vertex_uvset_1[ind0];
					const XMFLOAT2& at0 = vertex_atlas.empty() ? XMFLOAT2(0, 0) : vertex_atlas[ind0];

					for (uint32_t j = i + 1; j < subset.indexCount; j++)
					{
						uint32_t ind1 = indices[subset.indexOffset + (uint32_t)j];

						if (ind1 == ind0)
						{
							continue;
						}

						const XMFLOAT3& p1 = vertex_positions[ind1];
						const XMFLOAT2& u01 = vertex_uvset_0.empty() ? XMFLOAT2(0, 0) : vertex_uvset_0[ind1];
						const XMFLOAT2& u11 = vertex_uvset_1.empty() ? XMFLOAT2(0, 0) : vertex_uvset_1[ind1];
						const XMFLOAT2& at1 = vertex_atlas.empty() ? XMFLOAT2(0, 0) : vertex_atlas[ind1];

						const bool duplicated_pos =
							fabs(p0.x - p1.x) < FLT_EPSILON &&
							fabs(p0.y - p1.y) < FLT_EPSILON &&
							fabs(p0.z - p1.z) < FLT_EPSILON;

						const bool duplicated_uv0 =
							fabs(u00.x - u01.x) < FLT_EPSILON &&
							fabs(u00.y - u01.y) < FLT_EPSILON;

						const bool duplicated_uv1 =
							fabs(u10.x - u11.x) < FLT_EPSILON &&
							fabs(u10.y - u11.y) < FLT_EPSILON;

						const bool duplicated_atl =
							fabs(at0.x - at1.x) < FLT_EPSILON &&
							fabs(at0.y - at1.y) < FLT_EPSILON;

						if (duplicated_pos && duplicated_uv0 && duplicated_uv1 && duplicated_atl)
						{
							// Erase vertices[ind1] because it is a duplicate:
							if (ind1 < vertex_positions.size())
							{
								vertex_positions.erase(vertex_positions.begin() + ind1);
							}
							if (ind1 < vertex_normals.size())
							{
								vertex_normals.erase(vertex_normals.begin() + ind1);
							}
							if (ind1 < vertex_uvset_0.size())
							{
								vertex_uvset_0.erase(vertex_uvset_0.begin() + ind1);
							}
							if (ind1 < vertex_uvset_1.size())
							{
								vertex_uvset_1.erase(vertex_uvset_1.begin() + ind1);
							}
							if (ind1 < vertex_atlas.size())
							{
								vertex_atlas.erase(vertex_atlas.begin() + ind1);
							}
							if (ind1 < vertex_boneindices.size())
							{
								vertex_boneindices.erase(vertex_boneindices.begin() + ind1);
							}
							if (ind1 < vertex_boneweights.size())
							{
								vertex_boneweights.erase(vertex_boneweights.begin() + ind1);
							}

							// The vertices[ind1] was removed, so each index after that needs to be updated:
							for (auto& index : indices)
							{
								if (index > ind1 && index > 0)
								{
									index--;
								}
								else if (index == ind1)
								{
									index = ind0;
								}
							}

						}

					}
				}

			}

		}
		break;

		case wiScene::MeshComponent::COMPUTE_NORMALS_SMOOTH_FAST:
		{
			for (size_t i = 0; i < vertex_normals.size(); i++)
			{
				vertex_normals[i] = XMFLOAT3(0, 0, 0);
			}
			for (size_t i = 0; i < indices.size() / 3; ++i)
			{
				uint32_t index1 = indices[i * 3];
				uint32_t index2 = indices[i * 3 + 1];
				uint32_t index3 = indices[i * 3 + 2];

				XMVECTOR side1 = XMLoadFloat3(&vertex_positions[index1]) - XMLoadFloat3(&vertex_positions[index3]);
				XMVECTOR side2 = XMLoadFloat3(&vertex_positions[index1]) - XMLoadFloat3(&vertex_positions[index2]);
				XMVECTOR N = XMVector3Normalize(XMVector3Cross(side1, side2));
				XMFLOAT3 normal;
				XMStoreFloat3(&normal, N);

				vertex_normals[index1].x += normal.x;
				vertex_normals[index1].y += normal.y;
				vertex_normals[index1].z += normal.z;

				vertex_normals[index2].x += normal.x;
				vertex_normals[index2].y += normal.y;
				vertex_normals[index2].z += normal.z;

				vertex_normals[index3].x += normal.x;
				vertex_normals[index3].y += normal.y;
				vertex_normals[index3].z += normal.z;
			}
		}
		break;

		}

		vertex_tangents.clear(); // <- will be recomputed

		CreateRenderData(); // <- normals will be normalized here!
	}
	void MeshComponent::FlipCulling()
	{
		for (size_t face = 0; face < indices.size() / 3; face++)
		{
			uint32_t i0 = indices[face * 3 + 0];
			uint32_t i1 = indices[face * 3 + 1];
			uint32_t i2 = indices[face * 3 + 2];

			indices[face * 3 + 0] = i0;
			indices[face * 3 + 1] = i2;
			indices[face * 3 + 2] = i1;
		}

		CreateRenderData();
	}
	void MeshComponent::FlipNormals()
	{
		for (auto& normal : vertex_normals)
		{
			normal.x *= -1;
			normal.y *= -1;
			normal.z *= -1;
		}

		CreateRenderData();
	}
	void MeshComponent::Recenter()
	{
		XMFLOAT3 center = aabb.getCenter();

		for (auto& pos : vertex_positions)
		{
			pos.x -= center.x;
			pos.y -= center.y;
			pos.z -= center.z;
		}

		CreateRenderData();
	}
	void MeshComponent::RecenterToBottom()
	{
		XMFLOAT3 center = aabb.getCenter();
		center.y -= aabb.getHalfWidth().y;

		for (auto& pos : vertex_positions)
		{
			pos.x -= center.x;
			pos.y -= center.y;
			pos.z -= center.z;
		}

		CreateRenderData();
	}
	SPHERE MeshComponent::GetBoundingSphere() const
	{
		XMFLOAT3 halfwidth = aabb.getHalfWidth();

		SPHERE sphere;
		sphere.center = aabb.getCenter();
		sphere.radius = std::max(halfwidth.x, std::max(halfwidth.y, halfwidth.z));

		return sphere;
	}

	void ObjectComponent::ClearLightmap()
	{
		lightmap = Texture();
		lightmap_rect = {};
		lightmapWidth = 0;
		lightmapHeight = 0;
		lightmapIterationCount = 0; 
		lightmapTextureData.clear();
		SetLightmapRenderRequest(false);
	}

#if __has_include("OpenImageDenoise/oidn.hpp")
#define OPEN_IMAGE_DENOISE
#include "OpenImageDenoise/oidn.hpp"
#pragma comment(lib,"OpenImageDenoise.lib")
#pragma comment(lib,"tbb.lib")
// Also provide OpenImageDenoise.dll and tbb.dll near the exe!
#endif
	void ObjectComponent::SaveLightmap()
	{
		if (lightmap.IsValid())
		{
			bool success = wiHelper::saveTextureToMemory(lightmap, lightmapTextureData);
			assert(success);

#ifdef OPEN_IMAGE_DENOISE
			if (success)
			{
				std::vector<uint8_t> texturedata_dst(lightmapTextureData.size());

				size_t width = (size_t)lightmapWidth;
				size_t height = (size_t)lightmapHeight;
				{
					// https://github.com/OpenImageDenoise/oidn#c11-api-example

					// Create an Intel Open Image Denoise device
					static oidn::DeviceRef device = oidn::newDevice();
					static bool init = false;
					if (!init)
					{
						device.commit();
						init = true;
		}

					// Create a denoising filter
					oidn::FilterRef filter = device.newFilter("RTLightmap");
					filter.setImage("color", lightmapTextureData.data(), oidn::Format::Float3, width, height, 0, sizeof(XMFLOAT4));
					filter.setImage("output", texturedata_dst.data(), oidn::Format::Float3, width, height, 0, sizeof(XMFLOAT4));
					filter.commit();

					// Filter the image
					filter.execute();

					// Check for errors
					const char* errorMessage;
					auto error = device.getError(errorMessage);
					if (error != oidn::Error::None && error != oidn::Error::Cancelled)
					{
						wiBackLog::post((std::string("[OpenImageDenoise error] ") + errorMessage).c_str());
	}
				}

				GraphicsDevice* device = wiRenderer::GetDevice();

				SubresourceData initdata;
				initdata.pSysMem = texturedata_dst.data();
				initdata.SysMemPitch = uint32_t(sizeof(XMFLOAT4) * width);
				device->CreateTexture(&lightmap.desc, &initdata, &lightmap);

				lightmapTextureData = std::move(texturedata_dst);
			}
			lightmap_rect = {}; // repack into global atlas
#endif // OPEN_IMAGE_DENOISE

		}
	}
	FORMAT ObjectComponent::GetLightmapFormat()
	{
		uint32_t stride = (uint32_t)lightmapTextureData.size() / lightmapWidth / lightmapHeight;

		switch (stride)
		{
		case 4: return FORMAT_R8G8B8A8_UNORM;
		case 8: return FORMAT_R16G16B16A16_FLOAT;
		case 16: return FORMAT_R32G32B32A32_FLOAT;
		}

		return FORMAT_UNKNOWN;
	}

	void ArmatureComponent::CreateRenderData()
	{
		GraphicsDevice* device = wiRenderer::GetDevice();

		GPUBufferDesc bd;
		bd.Usage = USAGE_DYNAMIC;
		bd.CPUAccessFlags = CPU_ACCESS_WRITE;

		bd.ByteWidth = sizeof(ArmatureComponent::ShaderBoneType) * (uint32_t)boneCollection.size();
		bd.BindFlags = BIND_SHADER_RESOURCE;
		bd.MiscFlags = RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = sizeof(ArmatureComponent::ShaderBoneType);

		device->CreateBuffer(&bd, nullptr, &boneBuffer);
	}

	void SoftBodyPhysicsComponent::CreateFromMesh(const MeshComponent& mesh)
	{
		vertex_positions_simulation.resize(mesh.vertex_positions.size());
		vertex_tangents_tmp.resize(mesh.vertex_tangents.size());
		vertex_tangents_simulation.resize(mesh.vertex_tangents.size());

		XMMATRIX W = XMLoadFloat4x4(&worldMatrix);
		XMFLOAT3 _min = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
		XMFLOAT3 _max = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		for (size_t i = 0; i < vertex_positions_simulation.size(); ++i)
		{
			XMFLOAT3 pos = mesh.vertex_positions[i];
			XMStoreFloat3(&pos, XMVector3Transform(XMLoadFloat3(&pos), W));
			XMFLOAT3 nor = mesh.vertex_normals.empty() ? XMFLOAT3(1, 1, 1) : mesh.vertex_normals[i];
			XMStoreFloat3(&nor, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&nor), W)));
			const uint8_t wind = mesh.vertex_windweights.empty() ? 0xFF : mesh.vertex_windweights[i];
			vertex_positions_simulation[i].FromFULL(pos, nor, wind);
			_min = wiMath::Min(_min, pos);
			_max = wiMath::Max(_max, pos);
		}
		aabb = AABB(_min, _max);

		if(physicsToGraphicsVertexMapping.empty())
		{
			// Create a mapping that maps unique vertex positions to all vertex indices that share that. Unique vertex positions will make up the physics mesh:
			std::unordered_map<size_t, uint32_t> uniquePositions;
			graphicsToPhysicsVertexMapping.resize(mesh.vertex_positions.size());
			physicsToGraphicsVertexMapping.clear();
			weights.clear();

			for (size_t i = 0; i < mesh.vertex_positions.size(); ++i)
			{
				const XMFLOAT3& position = mesh.vertex_positions[i];

				size_t hashes[] = {
					std::hash<float>{}(position.x),
					std::hash<float>{}(position.y),
					std::hash<float>{}(position.z),
				};
				size_t vertexHash = (((hashes[0] ^ (hashes[1] << 1) >> 1) ^ (hashes[2] << 1)) >> 1);

				if (uniquePositions.count(vertexHash) == 0)
				{
					uniquePositions[vertexHash] = (uint32_t)physicsToGraphicsVertexMapping.size();
					physicsToGraphicsVertexMapping.push_back((uint32_t)i);
				}
				graphicsToPhysicsVertexMapping[i] = uniquePositions[vertexHash];
			}

			weights.resize(physicsToGraphicsVertexMapping.size());
			std::fill(weights.begin(), weights.end(), 1.0f);
		}
	}
	
	void CameraComponent::CreatePerspective(float newWidth, float newHeight, float newNear, float newFar, float newFOV)
	{
		zNearP = newNear;
		zFarP = newFar;
		width = newWidth;
		height = newHeight;
		fov = newFOV;

		SetCustomProjectionEnabled(false);

		UpdateCamera();
	}
	void CameraComponent::UpdateCamera()
	{
		if (!IsCustomProjectionEnabled())
		{
			float fzFarPCannotBeZero = zFarP;
			float fzNearPPCannotBeZero = zNearP;
			if (fzFarPCannotBeZero > 0.0f && fzNearPPCannotBeZero > 0)
			{
				XMStoreFloat4x4(&Projection, XMMatrixPerspectiveFovLH(fov, width / height, fzFarPCannotBeZero, fzNearPPCannotBeZero)); // reverse zbuffer!
				Projection.m[2][0] = jitter.x;
				Projection.m[2][1] = jitter.y;
			}
			#ifdef GGREDUCED
				// can use infinite far plane when using reversed float depth buffer
				//#define INFINITE_FAR_PLANE
				#ifdef INFINITE_FAR_PLANE
						zFarP = 1000000;
						Projection.m[2][2] = 0;
						Projection.m[3][2] = zNearP;
				#endif
			#endif
		}

		XMVECTOR _Eye = XMLoadFloat3(&Eye);
		XMVECTOR _At = XMLoadFloat3(&At);
		XMVECTOR _Up = XMLoadFloat3(&Up);
		XMMATRIX _V = XMMatrixLookToLH(_Eye, _At, _Up);
		XMStoreFloat4x4(&View, _V);

		#ifdef GGREDUCED
		XMMATRIX _InvV = XMMatrixInverse(nullptr, _V);
		XMStoreFloat4x4(&InvView, _InvV);
		XMMATRIX _P = XMLoadFloat4x4(&Projection);
		#ifdef INFINITE_FAR_PLANE
			XMFLOAT4X4 InvP;
			memset( &InvP, 0, sizeof(XMFLOAT4X4) );
			InvP.m[0][0] = zNearP / Projection.m[0][0];
			InvP.m[1][1] = zNearP / Projection.m[1][1];
			InvP.m[3][0] = -Projection.m[2][0] / Projection.m[0][0];
			InvP.m[3][1] = -Projection.m[2][1] / Projection.m[1][1];
			InvP.m[3][2] = zNearP;
			InvP.m[2][3] = 1;
			InvP.m[3][3] = 0.000005f; // should be 0 but don't want infinities
			XMMATRIX _InvP = XMLoadFloat4x4(&InvP);
		#else
			XMMATRIX _InvP = XMMatrixInverse(nullptr, _P);
		#endif
		XMStoreFloat4x4(&InvProjection, _InvP);
		XMMATRIX _VP = XMMatrixMultiply(_V, _P);
		XMStoreFloat4x4(&VP, _VP);
		XMMATRIX _InvVP = XMMatrixMultiply(_InvP, _InvV);
		XMStoreFloat4x4(&InvVP, _InvVP);
		#else
		XMMATRIX _P = XMLoadFloat4x4(&Projection);
		XMMATRIX _InvP = XMMatrixInverse(nullptr, _P);
		XMStoreFloat4x4(&InvProjection, _InvP);
		XMMATRIX _VP = XMMatrixMultiply(_V, _P);
		XMStoreFloat4x4(&View, _V);
		XMStoreFloat4x4(&VP, _VP);
		XMStoreFloat4x4(&InvView, XMMatrixInverse(nullptr, _V));
		XMStoreFloat4x4(&InvVP, XMMatrixInverse(nullptr, _VP));
		XMStoreFloat4x4(&Projection, _P);
		XMStoreFloat4x4(&InvProjection, XMMatrixInverse(nullptr, _P));
		#endif
		frustum.Create(_VP);
	}
	void CameraComponent::TransformCamera(const TransformComponent& transform)
	{
		XMVECTOR S, R, T;
		XMMatrixDecompose(&S, &R, &T, XMLoadFloat4x4(&transform.world));

		XMVECTOR _Eye = T;
		XMVECTOR _At = XMVectorSet(0, 0, 1, 0);
		XMVECTOR _Up = XMVectorSet(0, 1, 0, 0);

		XMMATRIX _Rot = XMMatrixRotationQuaternion(R);
		_At = XMVector3TransformNormal(_At, _Rot);
		_Up = XMVector3TransformNormal(_Up, _Rot);
		XMStoreFloat3x3(&rotationMatrix, _Rot);

		XMMATRIX _V = XMMatrixLookToLH(_Eye, _At, _Up);
		XMStoreFloat4x4(&View, _V);

		XMStoreFloat3(&Eye, _Eye);
		XMStoreFloat3(&At, _At);
		XMStoreFloat3(&Up, _Up);
	}
	void CameraComponent::Reflect(const XMFLOAT4& plane)
	{
		XMVECTOR _Eye = XMLoadFloat3(&Eye);
		XMVECTOR _At = XMLoadFloat3(&At);
		XMVECTOR _Up = XMLoadFloat3(&Up);
		XMMATRIX _Ref = XMMatrixReflect(XMLoadFloat4(&plane));

		// reverse clipping if behind clip plane ("if underwater")
		clipPlane = plane;
		float d = XMVectorGetX(XMPlaneDotCoord(XMLoadFloat4(&clipPlane), _Eye));
		if (d < 0)
		{
			clipPlane.x *= -1;
			clipPlane.y *= -1;
			clipPlane.z *= -1;
			clipPlane.w *= -1;
		}

		_Eye = XMVector3Transform(_Eye, _Ref);
		_At = XMVector3TransformNormal(_At, _Ref);
		_Up = XMVector3TransformNormal(_Up, _Ref);

		XMStoreFloat3(&Eye, _Eye);
		XMStoreFloat3(&At, _At);
		XMStoreFloat3(&Up, _Up);

		UpdateCamera();
	}


	void Scene::UpdateSceneTransform(float dt)
	{
		this->dt = dt;
		wiJobSystem::context ctx;
		RunPreviousFrameTransformUpdateSystem(ctx);
		//RunAnimationUpdateSystem(ctx);
		RunTransformUpdateSystem(ctx);
		wiJobSystem::Wait(ctx); // dependencies
	}

	void Scene::Update(float dt)
	{
		this->dt = dt;

		GraphicsDevice* device = wiRenderer::GetDevice();

		if (device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_PIPELINE) || device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_INLINE))
		{
			TLAS_instances.resize(objects.GetCount() * device->GetTopLevelAccelerationStructureInstanceSize());
		}

		// Occlusion culling read:
		if(!wiRenderer::GetFreezeCullingCameraEnabled())
		{
			if (!queryHeap[0].IsValid())
			{
				GPUQueryHeapDesc desc;
				desc.type = GPU_QUERY_TYPE_OCCLUSION_BINARY;
				desc.queryCount = 5120; // 4096;
				for (int i = 0; i < arraysize(queryHeap); ++i)
				{
					bool success = wiRenderer::GetDevice()->CreateQueryHeap(&desc, &queryHeap[i]);
					assert(success);
				}
				queryResults.resize(desc.queryCount);
			}

			// Previously allocated and written query count (newest one) is saved:
			writtenQueries[queryheap_idx] = std::min(queryAllocator.load(), queryHeap[queryheap_idx].desc.queryCount);
			queryAllocator.store(0);

			// Advance to next query heap to use (this will be the oldest one that was written)
			queryheap_idx = (queryheap_idx + 1) % arraysize(queryHeap);

			// Read back data from the oldest query heap:
			if (writtenQueries[queryheap_idx] > 0)
			{
				device->QueryRead(
					&queryHeap[queryheap_idx],
					0,
					writtenQueries[queryheap_idx],
					queryResults.data()
				);
			}
#ifdef GGREDUCED
			TerrainChunkOcclusion* pTCO;
			uint32_t lodstart = GGTerrain::GetChunkLodStart();
			if (bEnableTerrainChunkCulling)
			{
				for (int lod = lodstart; lod < 9; lod++)
				{
					for (int i = 0; i < 64; i++)
					{
						pTCO = GGTerrain::GetChunkVisibleMem(lod, i);
						if (pTCO && pTCO->bChunkVisible)
						{
							//Update last status.
							pTCO->history <<= 1;
							if (pTCO->writeQuery > 0)
								pTCO->history |= queryResults[pTCO->writeQuery];
							else
								pTCO->history = 1;
						}
					}
				}
			}

			if (bEnableSpotShadowCulling || bEnablePointShadowCulling)
			{
				for (int i = 0; i < lights.GetCount(); i++)
				{
					LightComponent& light = lights[i];
					bool shadow = light.IsCastingShadow() && !light.IsStatic();

					if (shadow)
					{
						switch (light.GetType())
						{
						case LightComponent::DIRECTIONAL:
							break;
						case LightComponent::SPOT:
							if (bEnableSpotShadowCulling)
							{
								light.history <<= 1;
								if (light.writeQuery > 0)
									light.history |= queryResults[light.writeQuery];
								else
									light.history = 1;
							}
							break;
						default:
							if (bEnablePointShadowCulling)
							{
								light.history <<= 1;
								if (light.writeQuery > 0)
									light.history |= queryResults[light.writeQuery];
								else
									light.history = 1;
								if (light.history == 0)
								{
									//PE: Add extra frames before removing shadow rendering to let fit 1/2 frames behind.
									if(light.delayed_shadow > 0)
										light.delayed_shadow--;
								}
								else
								{
									light.delayed_shadow = 10;
								}
							}
							break;
						}
					}
				}
			}
#endif
		}

		wiJobSystem::context ctx;

		RunPreviousFrameTransformUpdateSystem(ctx);

		RunAnimationUpdateSystem(ctx);

		RunTransformUpdateSystem(ctx);

		wiJobSystem::Wait(ctx); // dependencies

#ifdef MTHREAD_HIERARCHY
		RunHierarchyUpdateSystem(ctx);
		RunMeshUpdateSystem(ctx);
		RunMaterialUpdateSystem(ctx);
		wiJobSystem::Wait(ctx); // dependencies
#else
		RunHierarchyUpdateSystem(ctx);
#endif

		RunSpringUpdateSystem(ctx);
		RunInverseKinematicsUpdateSystem(ctx);
		RunArmatureUpdateSystem(ctx);

#ifndef MTHREAD_HIERARCHY
		RunMeshUpdateSystem(ctx);

		RunMaterialUpdateSystem(ctx);
#endif

		RunImpostorUpdateSystem(ctx);
		RunWeatherUpdateSystem(ctx);

#ifndef GGREDUCED
		wiPhysicsEngine::RunPhysicsUpdateSystem(ctx, *this, dt); // this syncs dependencies internally
#endif

		wiJobSystem::Wait(ctx); // dependencies

		RunObjectUpdateSystem(ctx);
		RunCameraUpdateSystem(ctx);
		RunDecalUpdateSystem(ctx);
		RunProbeUpdateSystem(ctx);
		RunForceUpdateSystem(ctx);
		RunLightUpdateSystem(ctx);
		//RunParticleUpdateSystem(ctx); //PE: Moved down. GGREDUCED
		RunSoundUpdateSystem(ctx);

		//wiJobSystem::Wait(ctx); // dependencies

		//PE: Getting debuglayer error here:
		//D3D11 CORRUPTION: ID3D11DeviceContext::Map:
		//Two threads were found to be executing functions associated with the same Device[Context] at the same time.
		//	This will cause corruption of memory.Appropriate thread synchronization needs to occur external to the Direct3D API(or through the ID3D10Multithread interface).
		//	23540 and 920 are the implicated thread ids.[MISCELLANEOUS CORRUPTION #28: CORRUPTED_MULTITHREADING]

		//PE: Moved to mainthread only.
		RunParticleUpdateSystem(ctx); // GGREDUCED

		wiJobSystem::Wait(ctx); // dependencies

		// Merge parallel bounds computation (depends on object update system):
		bounds = AABB();
		for (auto& group_bound : parallel_bounds)
		{
			bounds = AABB::Merge(bounds, group_bound);
		}

		if (device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_PIPELINE) || device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_RAYTRACING_INLINE))
		{
			// Recreate top level acceleration structure if the object count changed:
			if (objects.GetCount() > 0 && objects.GetCount() != TLAS.desc.toplevel.count)
			{
				RaytracingAccelerationStructureDesc desc;
				desc._flags = RaytracingAccelerationStructureDesc::FLAG_PREFER_FAST_BUILD;
				desc.type = RaytracingAccelerationStructureDesc::TOPLEVEL;
				desc.toplevel.count = (uint32_t)objects.GetCount();
				GPUBufferDesc bufdesc;
				bufdesc.MiscFlags |= RESOURCE_MISC_RAY_TRACING;
				bufdesc.ByteWidth = desc.toplevel.count * (uint32_t)device->GetTopLevelAccelerationStructureInstanceSize();
				bool success = device->CreateBuffer(&bufdesc, nullptr, &desc.toplevel.instanceBuffer);
				assert(success);
				device->SetName(&desc.toplevel.instanceBuffer, "TLAS.instanceBuffer");
				success = device->CreateRaytracingAccelerationStructure(&desc, &TLAS);
				assert(success);
				device->SetName(&TLAS, "TLAS");
			}
		}

		if (lightmap_refresh_needed.load())
		{
			SetAccelerationStructureUpdateRequested(true);
	}
		if (lightmap_repack_needed.load())
		{
			std::vector<wiRectPacker::bin> bins;
			if (wiRectPacker::pack(lightmap_rects.data(), (int)lightmap_rect_allocator.load(), 16384, bins))
			{
				assert(bins.size() == 1 && "The regions won't fit into the texture!");

				TextureDesc desc;
				desc.Width = (uint32_t)bins[0].size.w;
				desc.Height = (uint32_t)bins[0].size.h;
				desc.MipLevels = 1;
				desc.ArraySize = 1;
				desc.Format = FORMAT_R11G11B10_FLOAT;
				desc.SampleCount = 1;
				desc.Usage = USAGE_DEFAULT;
				desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
				desc.CPUAccessFlags = 0;
				desc.MiscFlags = 0;

				device->CreateTexture(&desc, nullptr, &lightmap);
				device->SetName(&lightmap, "Scene::lightmap");
			}
			else
			{
				wiBackLog::post("Global Lightmap atlas packing failed!");
			}
		}
		if (!lightmap.IsValid())
		{
			// In case no lightmaps, still create a dummy texture
			TextureDesc desc;
			desc.Width = 1;
			desc.Height = 1;
			desc.Format = FORMAT_R11G11B10_FLOAT;
			desc.BindFlags = BIND_SHADER_RESOURCE;
			device->CreateTexture(&desc, nullptr, &lightmap);
		}

		// Update atlas texture if it is invalidated:
		if (decal_repack_needed)
		{
			std::vector<wiRectPacker::rect_xywh*> out_rects(packedDecals.size());
			int i = 0;
			for (auto& it : packedDecals)
			{
				out_rects[i] = &it.second;
				i++;
			}

			std::vector<wiRectPacker::bin> bins;
			if (wiRectPacker::pack(out_rects.data(), (int)packedDecals.size(), 16384, bins))
			{
				assert(bins.size() == 1 && "The regions won't fit into the texture!");

				TextureDesc desc;
				desc.Width = (uint32_t)bins[0].size.w;
				desc.Height = (uint32_t)bins[0].size.h;
				desc.MipLevels = 0;
				desc.ArraySize = 1;
				desc.Format = FORMAT_R8G8B8A8_UNORM;
				desc.SampleCount = 1;
				desc.Usage = USAGE_DEFAULT;
				desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
				desc.CPUAccessFlags = 0;
				desc.MiscFlags = 0;

				device->CreateTexture(&desc, nullptr, &decalAtlas);
				device->SetName(&decalAtlas, "Scene::decalAtlas");

				for (uint32_t i = 0; i < decalAtlas.GetDesc().MipLevels; ++i)
				{
					int subresource_index;
					subresource_index = device->CreateSubresource(&decalAtlas, UAV, 0, 1, i, 1);
					assert(subresource_index == i);
				}
			}
			else
			{
				wiBackLog::post("Decal atlas packing failed!");
			}
		}

		// Update water ripples:
		for (size_t i = 0; i < waterRipples.size(); ++i)
		{
			auto& ripple = waterRipples[i];
			ripple.Update(dt * 60);

			// Remove inactive ripples:
			if (ripple.params.opacity <= 0 + FLT_EPSILON || ripple.params.fade >= 1 - FLT_EPSILON)
			{
				ripple = waterRipples.back();
				waterRipples.pop_back();
				i--;
			}
		}

	}
	void Scene::Clear()
	{
		names.Clear();
		layers.Clear();
		transforms.Clear();
		prev_transforms.Clear();
		hierarchy.Clear();
		materials.Clear();
		meshes.Clear();
		impostors.Clear();
		objects.Clear();
		aabb_objects.Clear();
		rigidbodies.Clear();
#ifndef GGREDUCED //PE: Remove all physics checks, we dont use it.
		softbodies.Clear();
#endif
		armatures.Clear();
		lights.Clear();
		aabb_lights.Clear();
		cameras.Clear();
		probes.Clear();
		aabb_probes.Clear();
		forces.Clear();
		decals.Clear();
		aabb_decals.Clear();
		animations.Clear();
		animation_datas.Clear();
		emitters.Clear();
		hairs.Clear();
		weathers.Clear();
		sounds.Clear();
		inverse_kinematics.Clear();
		springs.Clear();

		TLAS = RaytracingAccelerationStructure();
		BVH.Clear();
		packedDecals.clear();
		waterRipples.clear();
	}
	void Scene::Merge(Scene& other)
	{
		names.Merge(other.names);
		layers.Merge(other.layers);
		transforms.Merge(other.transforms);
		prev_transforms.Merge(other.prev_transforms);
		hierarchy.Merge(other.hierarchy);
		materials.Merge(other.materials);
		meshes.Merge(other.meshes);
		impostors.Merge(other.impostors);
		objects.Merge(other.objects);
		aabb_objects.Merge(other.aabb_objects);
		rigidbodies.Merge(other.rigidbodies);
#ifndef GGREDUCED //PE: Remove all physics checks, we dont use it.
		softbodies.Merge(other.softbodies);
#endif
		armatures.Merge(other.armatures);
		lights.Merge(other.lights);
		aabb_lights.Merge(other.aabb_lights);
		cameras.Merge(other.cameras);
		probes.Merge(other.probes);
		aabb_probes.Merge(other.aabb_probes);
		forces.Merge(other.forces);
		decals.Merge(other.decals);
		aabb_decals.Merge(other.aabb_decals);
		animations.Merge(other.animations);
		animation_datas.Merge(other.animation_datas);
		emitters.Merge(other.emitters);
		hairs.Merge(other.hairs);
		weathers.Merge(other.weathers);
		sounds.Merge(other.sounds);
		inverse_kinematics.Merge(other.inverse_kinematics);
		springs.Merge(other.springs);

		bounds = AABB::Merge(bounds, other.bounds);
	}

	void Scene::Entity_Remove(Entity entity)
	{
		Component_Detach(entity); // special case, this will also remove entity from hierarchy but also do more!

		names.Remove(entity);
		layers.Remove(entity);
		transforms.Remove(entity);
		prev_transforms.Remove(entity);
		materials.Remove(entity);
		meshes.Remove(entity);
		impostors.Remove(entity);
		objects.Remove(entity);
		aabb_objects.Remove(entity);
		rigidbodies.Remove(entity);
#ifndef GGREDUCED //PE: Remove all physics checks, we dont use it.
		softbodies.Remove(entity);
#endif
		armatures.Remove(entity);
		lights.Remove(entity);
		aabb_lights.Remove(entity);
		cameras.Remove(entity);
		probes.Remove(entity);
		aabb_probes.Remove(entity);
		forces.Remove(entity);
		decals.Remove(entity);
		aabb_decals.Remove(entity);
		animations.Remove(entity);
		animation_datas.Remove(entity);
		emitters.Remove(entity);
		hairs.Remove(entity);
		weathers.Remove(entity);
		sounds.Remove(entity);
		inverse_kinematics.Remove(entity);
		springs.Remove(entity);
	}
	Entity Scene::Entity_FindByName(const std::string& name)
	{
		for (size_t i = 0; i < names.GetCount(); ++i)
		{
			if (names[i] == name)
			{
				return names.GetEntity(i);
			}
		}
		return INVALID_ENTITY;
	}
	Entity Scene::Entity_Duplicate(Entity entity)
	{
		wiArchive archive;

		// First write the root entity to staging area:
		archive.SetReadModeAndResetPos(false);
		Entity_Serialize(archive, entity);

		// Then deserialize root:
		archive.SetReadModeAndResetPos(true);
		Entity root = Entity_Serialize(archive);

		return root;
	}
	Entity Scene::Entity_CreateMaterial(
		const std::string& name
	)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		materials.Create(entity);

		return entity;
	}
	Entity Scene::Entity_CreateObject(
		const std::string& name
	)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		layers.Create(entity);

		transforms.Create(entity);

		prev_transforms.Create(entity);

		aabb_objects.Create(entity);

		objects.Create(entity);

		return entity;
	}
	Entity Scene::Entity_CreateMesh(
		const std::string& name
	)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		meshes.Create(entity);

		return entity;
	}
	Entity Scene::Entity_CreateLight(
		const std::string& name,
		const XMFLOAT3& position,
		const XMFLOAT3& color,
		float energy,
		float range,
		LightComponent::LightType type)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		layers.Create(entity);

		TransformComponent& transform = transforms.Create(entity);
		transform.Translate(position);
		transform.UpdateTransform();

		aabb_lights.Create(entity).createFromHalfWidth(position, XMFLOAT3(range, range, range));

		LightComponent& light = lights.Create(entity);
		light.energy = energy;
		light.range_local = range;
		light.fov = XM_PIDIV4;
		light.color = color;
		light.SetType(type);

		return entity;
	}
	Entity Scene::Entity_CreateForce(
		const std::string& name,
		const XMFLOAT3& position
	)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		layers.Create(entity);

		TransformComponent& transform = transforms.Create(entity);
		transform.Translate(position);
		transform.UpdateTransform();

		ForceFieldComponent& force = forces.Create(entity);
		force.gravity = 0;
		force.range_local = 0;
		force.type = ENTITY_TYPE_FORCEFIELD_POINT;

		return entity;
	}
	Entity Scene::Entity_CreateEnvironmentProbe(
		const std::string& name,
		const XMFLOAT3& position
	)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		layers.Create(entity);

		TransformComponent& transform = transforms.Create(entity);
		transform.Translate(position);
		transform.UpdateTransform();

		aabb_probes.Create(entity);

		probes.Create(entity);

		return entity;
	}
	Entity Scene::Entity_CreateDecal(
		const std::string& name,
		const std::string& textureName,
		const std::string& normalMapName
	)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		layers.Create(entity);

		transforms.Create(entity);

		aabb_decals.Create(entity);

		decals.Create(entity);

		MaterialComponent& material = materials.Create(entity);
		material.textures[MaterialComponent::BASECOLORMAP].name = textureName;
		material.textures[MaterialComponent::NORMALMAP].name = normalMapName;
		material.CreateRenderData();

		return entity;
	}
	Entity Scene::Entity_CreateCamera(
		const std::string& name,
		float width, float height, float nearPlane, float farPlane, float fov
	)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		layers.Create(entity);

		transforms.Create(entity);

		CameraComponent& camera = cameras.Create(entity);
		camera.CreatePerspective(width, height, nearPlane, farPlane, fov);

		return entity;
	}
	Entity Scene::Entity_CreateEmitter(
		const std::string& name,
		const XMFLOAT3& position
	)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		emitters.Create(entity).count = 10;

		TransformComponent& transform = transforms.Create(entity);
		transform.Translate(position);
		transform.UpdateTransform();

		materials.Create(entity).userBlendMode = BLENDMODE_ALPHA;

		return entity;
	}
	Entity Scene::Entity_CreateHair(
		const std::string& name,
		const XMFLOAT3& position
	)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		hairs.Create(entity);

		TransformComponent& transform = transforms.Create(entity);
		transform.Translate(position);
		transform.UpdateTransform();

		materials.Create(entity);

		return entity;
	}
	Entity Scene::Entity_CreateSound(
		const std::string& name,
		const std::string& filename,
		const XMFLOAT3& position
	)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		SoundComponent& sound = sounds.Create(entity);
		sound.filename = filename;
		sound.soundResource = wiResourceManager::Load(filename, wiResourceManager::IMPORT_RETAIN_FILEDATA);
		if (sound.soundResource)
		{
			wiAudio::CreateSoundInstance(&sound.soundResource->sound, &sound.soundinstance);
			TransformComponent& transform = transforms.Create(entity);
			transform.Translate(position);
			transform.UpdateTransform();
			return entity;
		}
		Entity_Remove(entity);
		return 0;
	}

	Entity Scene::Entity_CreateSound_GG(
		const std::string& name,
		const std::string& filename,
		const XMFLOAT3& position,
		const std::string& realname,
		std::vector<uint8_t>& data
		)
	{
		Entity entity = CreateEntity();

		names.Create(entity) = name;

		//MODE_ALLOW_RETAIN_FILEDATA
		uint32_t oldmode = wiResourceManager::GetMode();
		wiResourceManager::SetMode(wiResourceManager::MODE_ALLOW_RETAIN_FILEDATA);

		SoundComponent& sound = sounds.Create(entity);
		sound.filename = filename;
		if (data.size() > 0)
		{
			sound.soundResource = wiResourceManager::Load(realname, wiResourceManager::IMPORT_RETAIN_FILEDATA, data.data(), data.size());
		}
		else
		{
			std::vector<uint8_t> newdata;
			if (wiHelper::FileRead(filename, newdata))
			{
				sound.soundResource = wiResourceManager::Load(realname, wiResourceManager::IMPORT_RETAIN_FILEDATA, newdata.data(), newdata.size());
				data.clear();
			}
		}

		wiResourceManager::SetMode( (wiResourceManager::MODE) oldmode);

		// sound.soundResource = wiResourceManager::Load(filename, wiResourceManager::IMPORT_RETAIN_FILEDATA);

		if (sound.soundResource)
		{
			wiAudio::CreateSoundInstance(&sound.soundResource->sound, &sound.soundinstance);
			TransformComponent& transform = transforms.Create(entity);
			transform.Translate(position);
			transform.UpdateTransform();
			return entity;
		}
		Entity_Remove(entity);
		return 0;
	}

	// PE: New taken from latest wicked repo: https://github.com/turanszkij/WickedEngine/blob/5b02548268ccae82ecebc041e0d13636faeba118/WickedEngine/wiScene.cpp#L2263
	// PE: Thanks Lee, will try it on non animated object using this one :)

#ifdef MTHREAD_HIERARCHY
	void Scene::Component_Attach(Entity entity, Entity parent, bool child_already_in_local_space)
	{
		assert(entity != parent);

		if (hierarchy.Contains(entity))
		{
			Component_Detach(entity);
		}

		HierarchyComponent& parentcomponent = hierarchy.Create(entity);
		parentcomponent.parentID = parent;

		TransformComponent* transform_parent = transforms.GetComponent(parent);
		if (transform_parent == nullptr)
		{
			transform_parent = &transforms.Create(parent);
		}

		TransformComponent* transform_child = transforms.GetComponent(entity);
		if (transform_child == nullptr)
		{
			transform_child = &transforms.Create(entity);
			transform_parent = transforms.GetComponent(parent); // after transforms.Create(), transform_parent pointer could have become invalidated!
		}
		if (!child_already_in_local_space)
		{
			XMMATRIX B = XMMatrixInverse(nullptr, XMLoadFloat4x4(&transform_parent->world));
			transform_child->MatrixTransform(B);
			transform_child->UpdateTransform();
		}
		transform_child->UpdateTransform_Parented(*transform_parent);

		LayerComponent* layer_parent = layers.GetComponent(parent);
		if (layer_parent == nullptr)
		{
			layer_parent = &layers.Create(parent);
		}
		LayerComponent* layer_child = layers.GetComponent(entity);
		if (layer_child == nullptr)
		{
			layer_child = &layers.Create(entity);
		}
	}
#endif
	// LB: This is the old Component_Attach which has built in sorting and needed for the current MAX code
	// still recommend switching to the separate sort function, but need to account for animating/etc objects
#ifndef MTHREAD_HIERARCHY
	void Scene::Component_Attach(Entity entity, Entity parent, bool child_already_in_local_space)
	{
		assert(entity != parent);

		if (hierarchy.Contains(entity))
		{
			Component_Detach(entity);
		}

		// Add a new hierarchy node to the end of container:
		hierarchy.Create(entity).parentID = parent;

		// Detect breaks in the tree and fix them:
		//	when children are before parents, we move the parents before the children while keeping ordering of other components intact
		if (hierarchy.GetCount() > 1)
		{
			for (size_t i = hierarchy.GetCount() - 1; i > 0; --i)
			{
				Entity parent_candidate_entity = hierarchy.GetEntity(i);
				for (size_t j = 0; j < i; ++j)
				{
					const HierarchyComponent& child_candidate = hierarchy[j];

					if (child_candidate.parentID == parent_candidate_entity)
					{
						hierarchy.MoveItem(i, j);
						++i; // next outer iteration will check the same index again as parent candidate, however things were moved upwards, so it will be a different entity!
						break;
					}
				}
			}
		}

		// Re-query parent after potential MoveItem(), because it invalidates references:
		HierarchyComponent& parentcomponent = *hierarchy.GetComponent(entity);

		TransformComponent* transform_parent = transforms.GetComponent(parent);
		if (transform_parent == nullptr)
		{
			transform_parent = &transforms.Create(parent);
		}

		TransformComponent* transform_child = transforms.GetComponent(entity);
		if (transform_child == nullptr)
		{
			transform_child = &transforms.Create(entity);
			transform_parent = transforms.GetComponent(parent); // after transforms.Create(), transform_parent pointer could have become invalidated!
		}
		if (!child_already_in_local_space)
		{
			XMMATRIX B = XMMatrixInverse(nullptr, XMLoadFloat4x4(&transform_parent->world));
			transform_child->MatrixTransform(B);
			transform_child->UpdateTransform();
		}
		transform_child->UpdateTransform_Parented(*transform_parent);

		LayerComponent* layer_parent = layers.GetComponent(parent);
		if (layer_parent == nullptr)
		{
			layer_parent = &layers.Create(parent);
		}
		LayerComponent* layer_child = layers.GetComponent(entity);
		if (layer_child == nullptr)
		{
			layer_child = &layers.Create(entity);
		}
		layer_child->propagationMask = layer_parent->GetLayerMask();
	}
#endif

	void Scene::Component_Detach(Entity entity)
	{
		const HierarchyComponent* parent = hierarchy.GetComponent(entity);

		if (parent != nullptr)
		{
			TransformComponent* transform = transforms.GetComponent(entity);
			if (transform != nullptr)
			{
				transform->ApplyTransform();
			}

			LayerComponent* layer = layers.GetComponent(entity);
			if (layer != nullptr)
			{
				layer->propagationMask = ~0;
			}
#ifdef MTHREAD_HIERARCHY
			hierarchy.Remove(entity);
#else
			hierarchy.Remove_KeepSorted(entity);
#endif
		}
	}
	void Scene::Component_DetachChildren(Entity parent)
	{
		for (size_t i = 0; i < hierarchy.GetCount(); )
		{
			if (hierarchy[i].parentID == parent)
			{
				Entity entity = hierarchy.GetEntity(i);
				Component_Detach(entity);
			}
			else
			{
				++i;
			}
		}
	}


	const uint32_t small_subtask_groupsize = 64;

	void Scene::RunPreviousFrameTransformUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		wiJobSystem::Dispatch(ctx, (uint32_t)prev_transforms.GetCount(), small_subtask_groupsize, [&](wiJobArgs args)
		{
			PreviousFrameTransformComponent& prev_transform = prev_transforms[args.jobIndex];
			Entity entity = prev_transforms.GetEntity(args.jobIndex);
			const TransformComponent& transform = *transforms.GetComponent(entity);

			prev_transform.world_prev = transform.world;
		});
	}
	void Scene::RunAnimationUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
OPTICK_EVENT();
#endif
		iCulledAnimations = 0;
		static uint32_t iAnimFrames = 0;
		iAnimFrames++;
		// extra loop to process primary animations, before allowing secondary to 'steal' the timer value to sync child animations to primary ones
		for (int handleprimaryandsecondary = 0; handleprimaryandsecondary < 2; handleprimaryandsecondary++)
		{
#endif
			for (size_t i = 0; i < animations.GetCount(); ++i)
			{
				AnimationComponent& animation = animations[i];

#ifdef GGREDUCED
				bool bCulled = false;
			        bool bResetKeys = false;
			        if (animation.updateonce) bResetKeys = true;
				//PE: Also respect animation.updateonce
				if (bEnable30FpsAnimations && animation.updateonce == false)
				{
					if ((iAnimFrames + i) % 2 == 0)
					{
						//PE: still add to timer so same speed.
						iCulledAnimations++;
						bCulled = true;
					}
				}
				//if (!bCulled)
				//{
					if (handleprimaryandsecondary == 0 && animation.useprimaryanimtimer != 0) continue;
					if (handleprimaryandsecondary == 1 && animation.useprimaryanimtimer == 0) continue;
					if (handleprimaryandsecondary == 1 && animation.useprimaryanimtimer != 0)
					{
						// this results in getting some old data, not the one written into animations[i]!!
						int iThisAnimIndexHereAndNow = -1;
						for (size_t ii = 0; ii < animations.GetCount(); ++ii)
						{
							if (animation.useprimaryanimtimer == animations[ii].primaryanimid)
							{
								iThisAnimIndexHereAndNow = ii;
								break;
							}
						}
						if (iThisAnimIndexHereAndNow != -1)
						{
							// this actually does not work, somehow reading older values than are generated inside this function, impressive!
							animation.timer = animations[iThisAnimIndexHereAndNow].timer;
							animation.speed = animations[iThisAnimIndexHereAndNow].speed;
							animation.amount = animations[iThisAnimIndexHereAndNow].amount;
						}
					}
					else
					{
						if ((!animation.IsPlaying() && animation.timer == 0.0f) && animation.updateonce == false) continue;
					}

				//}
#else
				if (!animation.IsPlaying() && animation.timer == 0.0f)
				{
					continue;
				}
#endif

#ifdef GGREDUCED
				if (!bCulled && animation.updateonce == false)
				{
					//objects.GetComponent()
					ObjectComponent* object = objects.GetComponent(animation.objectIndex);
					if (bEnableAnimationCulling)
					{
						if (object && ((object->IsOccluded() && bEnableObjectCulling) || object->IsCulled()))
						{
							iCulledAnimations++;
							bCulled = true;
						}
					}
				}

				animation.updateonce = false;

				if (!bCulled)
#endif
				{
					for (const AnimationComponent::AnimationChannel& channel : animation.channels)
					{
						assert(channel.samplerIndex < (int)animation.samplers.size());

						AnimationComponent::AnimationSampler& sampler = animation.samplers[channel.samplerIndex];
						if (sampler.data == INVALID_ENTITY)
						{
							// backwards-compatibility mode
							sampler.data = CreateEntity();
							animation_datas.Create(sampler.data) = sampler.backwards_compatibility_data;
							sampler.backwards_compatibility_data.keyframe_times.clear();
							sampler.backwards_compatibility_data.keyframe_data.clear();
						}
				                //const AnimationDataComponent* animationdata = animation_datas.GetComponent(sampler.data);
				                AnimationDataComponent* animationdata = animation_datas.GetComponent(sampler.data);
						if (animationdata == nullptr)
						{
							continue;
						}

						int keyLeft = 0;
						int keyRight = 0;
				                if(bResetKeys)
					            animationdata->prevKeyRight = 0;

						if (animationdata->keyframe_times.back() < animation.timer)
						{
							// Rightmost keyframe is already outside animation, so just snap to last keyframe:
							keyLeft = keyRight = (int)animationdata->keyframe_times.size() - 1;
					                animationdata->prevKeyRight = 0;
						}
						else
						{
							float fAnimTimeEnd = animation.timer;

#ifdef GGREDUCED
							if (animation.IsLooped())
							{
								// this endures that in a loop mode, the last rightmost frame is locked, even when the animation.timer goes beyond to do the first frame interp
								if (animation.timer > animation.end) fAnimTimeEnd = animation.end;
							}
#endif
					                 keyRight = animationdata->prevKeyRight;
					                 if (animationdata->keyframe_times[keyRight] >= fAnimTimeEnd)
						               keyRight = 0;

							// Search for the right keyframe (greater/equal to anim time):
							while (animationdata->keyframe_times[keyRight++] < fAnimTimeEnd) {}
							keyRight--;

							// Left keyframe is just near right:
							keyLeft = std::max(0, keyRight - 1);
					animationdata->prevKeyRight = keyLeft;

						}

#ifdef GGREDUCED
						// new dsadsa mode which can replace any frame with a different animation frame (used to wipe out Bip01 rotation when turning)
						// but can be expanded to apply a second stream of animation so an object can play twi distinct animation tracks on different body parts
						if (channel.iUsePreFrame >= 10000)
						{
							if (channel.path == AnimationComponent::AnimationChannel::Path::ROTATION)
							{
								int iNewAnimFrame = channel.iUsePreFrame - 10000;
								keyLeft = iNewAnimFrame;
								keyRight = iNewAnimFrame;
							}
						}
#endif

						float left = animationdata->keyframe_times[keyLeft];

						TransformComponent transform;

						TransformComponent* target_transform = nullptr;
						MeshComponent* target_mesh = nullptr;

						if (channel.path == AnimationComponent::AnimationChannel::Path::WEIGHTS)
						{
							ObjectComponent* object = objects.GetComponent(channel.target);
							//assert(object != nullptr); GGREDUCED stops debug!
							if (object == nullptr)
								continue;
							target_mesh = meshes.GetComponent(object->meshID);
							//assert(target_mesh != nullptr); GGREDUCED stops debug!
							if (target_mesh == nullptr)
								continue;
							animation.morph_weights_temp.resize(target_mesh->targets.size());
						}
						else
						{
							target_transform = transforms.GetComponent(channel.target);
							//assert(target_transform != nullptr); GGREDUCED stops debug!
							if (target_transform == nullptr)
								continue;
							transform = *target_transform;
						}

						switch (sampler.mode)
						{
						default:
						case AnimationComponent::AnimationSampler::Mode::STEP:
						{
							// Nearest neighbor method (snap to left):
							switch (channel.path)
							{
							default:
							case AnimationComponent::AnimationChannel::Path::TRANSLATION:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * 3);
								transform.translation_local = ((const XMFLOAT3*)animationdata->keyframe_data.data())[keyLeft];
							}
							break;
							case AnimationComponent::AnimationChannel::Path::ROTATION:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * 4);
								transform.rotation_local = ((const XMFLOAT4*)animationdata->keyframe_data.data())[keyLeft];
							}
							break;
							case AnimationComponent::AnimationChannel::Path::SCALE:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * 3);
								transform.scale_local = ((const XMFLOAT3*)animationdata->keyframe_data.data())[keyLeft];
							}
							break;
							case AnimationComponent::AnimationChannel::Path::WEIGHTS:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * animation.morph_weights_temp.size());
								for (size_t j = 0; j < animation.morph_weights_temp.size(); ++j)
								{
									animation.morph_weights_temp[j] = animationdata->keyframe_data[keyLeft * animation.morph_weights_temp.size() + j];
								}
							}
							break;
							}
						}
						break;
						case AnimationComponent::AnimationSampler::Mode::LINEAR:
						{
							// Linear interpolation method:
							float t;
							if (keyLeft == keyRight)
							{
								t = 0;
							}
							else
							{
								// animation playing normally
								float right = animationdata->keyframe_times[keyRight];
								t = (animation.timer - left) / (right - left);
							}

							switch (channel.path)
							{
							default:
							case AnimationComponent::AnimationChannel::Path::TRANSLATION:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * 3);
								const XMFLOAT3* data = (const XMFLOAT3*)animationdata->keyframe_data.data();
								XMVECTOR vLeft = XMLoadFloat3(&data[keyLeft]);
								XMVECTOR vRight = XMLoadFloat3(&data[keyRight]);
								XMVECTOR vAnim = XMVectorLerp(vLeft, vRight, t);
								XMStoreFloat3(&transform.translation_local, vAnim);
							}
							break;
							case AnimationComponent::AnimationChannel::Path::ROTATION:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * 4);
								const XMFLOAT4* data = (const XMFLOAT4*)animationdata->keyframe_data.data();
								XMVECTOR vLeft = XMLoadFloat4(&data[keyLeft]);
								XMVECTOR vRight = XMLoadFloat4(&data[keyRight]);
								XMVECTOR vAnim = XMQuaternionSlerp(vLeft, vRight, t);
								vAnim = XMQuaternionNormalize(vAnim);
								XMStoreFloat4(&transform.rotation_local, vAnim);
							}
							break;
							case AnimationComponent::AnimationChannel::Path::SCALE:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * 3);
								const XMFLOAT3* data = (const XMFLOAT3*)animationdata->keyframe_data.data();
								XMVECTOR vLeft = XMLoadFloat3(&data[keyLeft]);
								XMVECTOR vRight = XMLoadFloat3(&data[keyRight]);
								XMVECTOR vAnim = XMVectorLerp(vLeft, vRight, t);
								XMStoreFloat3(&transform.scale_local, vAnim);
							}
							break;
							case AnimationComponent::AnimationChannel::Path::WEIGHTS:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * animation.morph_weights_temp.size());
								for (size_t j = 0; j < animation.morph_weights_temp.size(); ++j)
								{
									float vLeft = animationdata->keyframe_data[keyLeft * animation.morph_weights_temp.size() + j];
									float vRight = animationdata->keyframe_data[keyLeft * animation.morph_weights_temp.size() + j];
									float vAnim = wiMath::Lerp(vLeft, vRight, t);
									animation.morph_weights_temp[j] = vAnim;
								}
							}
							break;
							}
						}
						break;
						case AnimationComponent::AnimationSampler::Mode::CUBICSPLINE:
						{
							// Cubic Spline interpolation method:
							float t;
							if (keyLeft == keyRight)
							{
								t = 0;
							}
							else
							{
								float right = animationdata->keyframe_times[keyRight];
								t = (animation.timer - left) / (right - left);
							}

							const float t2 = t * t;
							const float t3 = t2 * t;

							switch (channel.path)
							{
							default:
							case AnimationComponent::AnimationChannel::Path::TRANSLATION:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * 3 * 3);
								const XMFLOAT3* data = (const XMFLOAT3*)animationdata->keyframe_data.data();
								XMVECTOR vLeft = XMLoadFloat3(&data[keyLeft * 3 + 1]);
								XMVECTOR vLeftTanOut = dt * XMLoadFloat3(&data[keyLeft * 3 + 2]);
								XMVECTOR vRightTanIn = dt * XMLoadFloat3(&data[keyRight * 3 + 0]);
								XMVECTOR vRight = XMLoadFloat3(&data[keyRight * 3 + 1]);
								XMVECTOR vAnim = (2 * t3 - 3 * t2 + 1) * vLeft + (t3 - 2 * t2 + t) * vLeftTanOut + (-2 * t3 + 3 * t2) * vRight + (t3 - t2) * vRightTanIn;
								XMStoreFloat3(&transform.translation_local, vAnim);
							}
							break;
							case AnimationComponent::AnimationChannel::Path::ROTATION:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * 4 * 3);
								const XMFLOAT4* data = (const XMFLOAT4*)animationdata->keyframe_data.data();
								XMVECTOR vLeft = XMLoadFloat4(&data[keyLeft * 3 + 1]);
								XMVECTOR vLeftTanOut = dt * XMLoadFloat4(&data[keyLeft * 3 + 2]);
								XMVECTOR vRightTanIn = dt * XMLoadFloat4(&data[keyRight * 3 + 0]);
								XMVECTOR vRight = XMLoadFloat4(&data[keyRight * 3 + 1]);
								XMVECTOR vAnim = (2 * t3 - 3 * t2 + 1) * vLeft + (t3 - 2 * t2 + t) * vLeftTanOut + (-2 * t3 + 3 * t2) * vRight + (t3 - t2) * vRightTanIn;
								vAnim = XMQuaternionNormalize(vAnim);
								XMStoreFloat4(&transform.rotation_local, vAnim);
							}
							break;
							case AnimationComponent::AnimationChannel::Path::SCALE:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * 3 * 3);
								const XMFLOAT3* data = (const XMFLOAT3*)animationdata->keyframe_data.data();
								XMVECTOR vLeft = XMLoadFloat3(&data[keyLeft * 3 + 1]);
								XMVECTOR vLeftTanOut = dt * XMLoadFloat3(&data[keyLeft * 3 + 2]);
								XMVECTOR vRightTanIn = dt * XMLoadFloat3(&data[keyRight * 3 + 0]);
								XMVECTOR vRight = XMLoadFloat3(&data[keyRight * 3 + 1]);
								XMVECTOR vAnim = (2 * t3 - 3 * t2 + 1) * vLeft + (t3 - 2 * t2 + t) * vLeftTanOut + (-2 * t3 + 3 * t2) * vRight + (t3 - t2) * vRightTanIn;
								XMStoreFloat3(&transform.scale_local, vAnim);
							}
							break;
							case AnimationComponent::AnimationChannel::Path::WEIGHTS:
							{
								assert(animationdata->keyframe_data.size() == animationdata->keyframe_times.size() * animation.morph_weights_temp.size() * 3);
								for (size_t j = 0; j < animation.morph_weights_temp.size(); ++j)
								{
									float vLeft = animationdata->keyframe_data[(keyLeft * animation.morph_weights_temp.size() + j) * 3 + 1];
									float vLeftTanOut = animationdata->keyframe_data[(keyLeft * animation.morph_weights_temp.size() + j) * 3 + 2];
									float vRightTanIn = animationdata->keyframe_data[(keyLeft * animation.morph_weights_temp.size() + j) * 3 + 0];
									float vRight = animationdata->keyframe_data[(keyLeft * animation.morph_weights_temp.size() + j) * 3 + 1];
									float vAnim = (2 * t3 - 3 * t2 + 1) * vLeft + (t3 - 2 * t2 + t) * vLeftTanOut + (-2 * t3 + 3 * t2) * vRight + (t3 - t2) * vRightTanIn;
									animation.morph_weights_temp[j] = vAnim;
								}
							}
							break;
							}
						}
						break;
						}

						if (target_transform != nullptr)
						{
							target_transform->SetDirty();

#ifdef GGREDUCED
							// additional functionality to impose other animation frames to the current frame
							float fAmountCurveProportionalToSpeed = 0.0002f;
							if (animation.speed < 0.5f) fAmountCurveProportionalToSpeed = 0.0001f;
							if (animation.speed > 1.5f) fAmountCurveProportionalToSpeed = 0.0005f;
							animation.amount = wiMath::Lerp(animation.amount, 1, fAmountCurveProportionalToSpeed);
							float t = animation.amount;
							// and adjust the rotation of frames for post-animation features such as head turning
							if (channel.iUsePreFrame == 1)
							{
								// merge preframe with current frame
								switch (channel.path)
								{
								case AnimationComponent::AnimationChannel::Path::TRANSLATION:
								{
									XMVECTOR preframeT = channel.vPreFrameTranslation;
									XMVECTOR currentT = XMLoadFloat3(&transform.translation_local);
									XMVECTOR resultT = XMVectorAdd(preframeT, currentT); //?hmm
									XMStoreFloat3(&transform.translation_local, resultT);
									break;
								}
								case AnimationComponent::AnimationChannel::Path::ROTATION:
								{
									XMVECTOR preframeR = channel.qPreFrameRotation;
									XMVECTOR currentR = XMLoadFloat4(&transform.rotation_local);
									XMVECTOR resultR = XMQuaternionMultiply(preframeR, currentR);
									XMStoreFloat4(&transform.rotation_local, resultR);
									break;
								}
								case AnimationComponent::AnimationChannel::Path::SCALE:
								{
									XMVECTOR preframeS = channel.vPreFrameScale;
									XMVECTOR currentS = XMLoadFloat3(&transform.scale_local);
									XMVECTOR resultS = XMVectorMultiply(preframeS, currentS);
									XMStoreFloat3(&transform.scale_local, resultS);
									break;
								}
								}
							}
							if (channel.iUsePreFrame == 2)
							{
								// entirely replace frame with preframe
								switch (channel.path)
								{
								case AnimationComponent::AnimationChannel::Path::TRANSLATION:
								{
									XMVECTOR preframeT = channel.vPreFrameTranslation;
									XMStoreFloat3(&transform.translation_local, preframeT);
									break;
								}
								case AnimationComponent::AnimationChannel::Path::ROTATION:
								{
									XMVECTOR preframeR = channel.qPreFrameRotation;
									XMStoreFloat4(&transform.rotation_local, preframeR);
									break;
								}
								case AnimationComponent::AnimationChannel::Path::SCALE:
								{
									XMVECTOR preframeS = channel.vPreFrameScale;
									XMStoreFloat3(&transform.scale_local, preframeS);
									break;
								}
								}

								// use fSmoothAmount to introduce this frame smoothly into current frame
								t = channel.fSmoothAmount;
							}
							if (channel.iUsePreFrame == 3)
							{
								if (channel.path == AnimationComponent::AnimationChannel::Path::TRANSLATION)
								{
									float fRetainYPos = transform.translation_local.y;
									XMVECTOR preframeT = channel.vPreFrameTranslation;
									XMStoreFloat3(&transform.translation_local, preframeT);
									transform.translation_local.y = fRetainYPos;
								}
								if (channel.path == AnimationComponent::AnimationChannel::Path::ROTATION)
								{
									XMVECTOR preframeT = channel.qPreFrameRotation;
									XMStoreFloat4(&transform.rotation_local, preframeT);
								}
							}
#else
							const float t = animation.amount;
#endif
							const XMVECTOR aS = XMLoadFloat3(&target_transform->scale_local);
							const XMVECTOR aR = XMLoadFloat4(&target_transform->rotation_local);
							const XMVECTOR aT = XMLoadFloat3(&target_transform->translation_local);

							const XMVECTOR bS = XMLoadFloat3(&transform.scale_local);
							const XMVECTOR bR = XMLoadFloat4(&transform.rotation_local);
							const XMVECTOR bT = XMLoadFloat3(&transform.translation_local);

							const XMVECTOR S = XMVectorLerp(aS, bS, t);
							const XMVECTOR R = XMQuaternionSlerp(aR, bR, t);
							const XMVECTOR T = XMVectorLerp(aT, bT, t);

							XMStoreFloat3(&target_transform->scale_local, S);
							XMStoreFloat4(&target_transform->rotation_local, R);
							XMStoreFloat3(&target_transform->translation_local, T);
						}

						if (target_mesh != nullptr)
						{
							const float t = animation.amount;

							for (size_t j = 0; j < target_mesh->targets.size(); ++j)
							{
								target_mesh->targets[j].weight = wiMath::Lerp(target_mesh->targets[j].weight, animation.morph_weights_temp[j], t);
							}

							target_mesh->dirty_morph = true;
						}

					}
				}
				if (animation.IsPlaying())
				{
					animation.timer += dt * animation.speed;
				}

#ifdef GGREDUCED
				//stopped this for now, seemed it was the append anim chopping off a frame as gabys loops are great
				// extend animation.end so that it accounts for the extra 'step to interpolate back to the start frame :)
				//if (animation.IsLooped())
				//{
				//	float fRealEndOfAnimationTime = animation.end + fAnimLoopEndTimeStep;
				//	if (animation.timer > fRealEndOfAnimationTime)
				//	{
				//		float fLengthOfAnimInTime = fRealEndOfAnimationTime - animation.start;
				//		animation.timer -= fLengthOfAnimInTime;
				//	}
				//}
				if (animation.IsLooped())
				{
					if (animation.timer > animation.end)
					{
						float fLengthOfAnimInTime = animation.end - animation.start;
						animation.timer -= fLengthOfAnimInTime;
					}
				}
#else
				if (animation.IsLooped() && animation.timer > animation.end)
				{
					animation.timer = animation.start;
				}
#endif
			}
#ifdef GGREDUCED
			// end extra loop (see above)
		}
#endif
	}
	void Scene::RunTransformUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		wiJobSystem::Dispatch(ctx, (uint32_t)transforms.GetCount(), small_subtask_groupsize, [&](wiJobArgs args) {

			TransformComponent& transform = transforms[args.jobIndex];
			transform.UpdateTransform();
		});
	}
#ifdef MTHREAD_HIERARCHY
	void Scene::RunHierarchyUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		wiJobSystem::Dispatch(ctx, (uint32_t)hierarchy.GetCount(), small_subtask_groupsize, [&](wiJobArgs args) {

			HierarchyComponent& hier = hierarchy[args.jobIndex];
			Entity entity = hierarchy.GetEntity(args.jobIndex);

			TransformComponent* transform_child = transforms.GetComponent(entity);
			XMMATRIX worldmatrix;
			if (transform_child != nullptr)
			{
				worldmatrix = transform_child->GetLocalMatrix();
			}

			LayerComponent* layer_child = layers.GetComponent(entity);
			if (layer_child != nullptr)
			{
				layer_child->propagationMask = ~0u; // clear propagation mask to full
			}

			if (transform_child == nullptr && layer_child == nullptr)
				return;

			Entity parentID = hier.parentID;
			while (parentID != INVALID_ENTITY)
			{
				TransformComponent* transform_parent = transforms.GetComponent(parentID);
				if (transform_child != nullptr && transform_parent != nullptr)
				{
					worldmatrix *= transform_parent->GetLocalMatrix();
				}

				LayerComponent* layer_parent = layers.GetComponent(parentID);
				if (layer_child != nullptr && layer_parent != nullptr)
				{
					layer_child->propagationMask &= layer_parent->layerMask;
				}

				const HierarchyComponent* hier_recursive = hierarchy.GetComponent(parentID);
				if (hier_recursive != nullptr)
				{
					parentID = hier_recursive->parentID;
				}
				else
				{
					parentID = INVALID_ENTITY;
				}
			}

			if (transform_child != nullptr)
			{
				XMStoreFloat4x4(&transform_child->world, worldmatrix);
			}

		});
	}
#endif
#ifndef MTHREAD_HIERARCHY
	void Scene::RunHierarchyUpdateSystem(wiJobSystem::context& ctx)
	{
		// This needs serialized execution because there are dependencies enforced by component order!

		for (size_t i = 0; i < hierarchy.GetCount(); ++i)
		{
			const HierarchyComponent& parentcomponent = hierarchy[i];
			Entity entity = hierarchy.GetEntity(i);

			TransformComponent* transform_child = transforms.GetComponent(entity);
			TransformComponent* transform_parent = transforms.GetComponent(parentcomponent.parentID);
			if (transform_child != nullptr && transform_parent != nullptr)
			{
				transform_child->UpdateTransform_Parented(*transform_parent);
			}


			LayerComponent* layer_child = layers.GetComponent(entity);
			LayerComponent* layer_parent = layers.GetComponent(parentcomponent.parentID);
			if (layer_child != nullptr && layer_parent != nullptr)
			{
				layer_child->propagationMask = layer_parent->GetLayerMask();
			}

		}
	}
#endif

	void Scene::RunSpringUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		static float time = 0;
		time += dt;
		const XMVECTOR windDir = XMLoadFloat3(&weather.windDirection);
		const XMVECTOR gravity = XMVectorSet(0, -9.8f, 0, 0);

		for (size_t i = 0; i < springs.GetCount(); ++i)
		{
			SpringComponent& spring = springs[i];
			if (spring.IsDisabled())
			{
				continue;
			}
			Entity entity = springs.GetEntity(i);
			TransformComponent* transform = transforms.GetComponent(entity);
			if (transform == nullptr)
			{
				assert(0);
				continue;
			}

			if (spring.IsResetting())
			{
				spring.Reset(false);
				spring.center_of_mass = transform->GetPosition();
				spring.velocity = XMFLOAT3(0, 0, 0);
			}

			const HierarchyComponent* hier = hierarchy.GetComponent(entity);
			TransformComponent* parent_transform = hier == nullptr ? nullptr : transforms.GetComponent(hier->parentID);
			if (parent_transform != nullptr)
			{
				// Spring hierarchy resolve depends on spring component order!
				//	It works best when parent spring is located before child spring!
				//	It will work the other way, but results will be less convincing
				transform->UpdateTransform_Parented(*parent_transform);
			}

			const XMVECTOR position_current = transform->GetPositionV();
			XMVECTOR position_prev = XMLoadFloat3(&spring.center_of_mass);
			XMVECTOR force = (position_current - position_prev) * spring.stiffness;

			if (spring.wind_affection > 0)
			{
				force += std::sin(time * weather.windSpeed + XMVectorGetX(XMVector3Dot(position_current, windDir))) * windDir * spring.wind_affection;
			}
			if (spring.IsGravityEnabled())
			{
				force += gravity;
			}

			XMVECTOR velocity = XMLoadFloat3(&spring.velocity);
			velocity += force * dt;
			XMVECTOR position_target = position_prev + velocity * dt;

			if (parent_transform != nullptr)
			{
				const XMVECTOR position_parent = parent_transform->GetPositionV();
				const XMVECTOR parent_to_child = position_current - position_parent;
				const XMVECTOR parent_to_target = position_target - position_parent;

				if (!spring.IsStretchEnabled())
				{
					// Limit offset to keep distance from parent:
					const XMVECTOR len = XMVector3Length(parent_to_child);
					position_target = position_parent + XMVector3Normalize(parent_to_target) * len;
				}

				// Parent rotation to point to new child position:
				const XMVECTOR dir_parent_to_child = XMVector3Normalize(parent_to_child);
				const XMVECTOR dir_parent_to_target = XMVector3Normalize(parent_to_target);
				const XMVECTOR axis = XMVector3Normalize(XMVector3Cross(dir_parent_to_child, dir_parent_to_target));
				const float angle = XMScalarACos(XMVectorGetX(XMVector3Dot(dir_parent_to_child, dir_parent_to_target))); // don't use std::acos!
				const XMVECTOR Q = XMQuaternionNormalize(XMQuaternionRotationNormal(axis, angle));
				TransformComponent saved_parent = *parent_transform;
				saved_parent.ApplyTransform();
				saved_parent.Rotate(Q);
				saved_parent.UpdateTransform();
				std::swap(saved_parent.world, parent_transform->world); // only store temporary result, not modifying actual local space!
			}

			XMStoreFloat3(&spring.center_of_mass, position_target);
			velocity *= spring.damping;
			XMStoreFloat3(&spring.velocity, velocity);
			*((XMFLOAT3*)&transform->world._41) = spring.center_of_mass;
		}
	}
	void Scene::RunInverseKinematicsUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		bool recompute_hierarchy = false;
		for (size_t i = 0; i < inverse_kinematics.GetCount(); ++i)
		{
			const InverseKinematicsComponent& ik = inverse_kinematics[i];
			if (ik.IsDisabled())
			{
				continue;
			}
			Entity entity = inverse_kinematics.GetEntity(i);
			TransformComponent* transform = transforms.GetComponent(entity);
			TransformComponent* target = transforms.GetComponent(ik.target);
			const HierarchyComponent* hier = hierarchy.GetComponent(entity);
			if (transform == nullptr || target == nullptr || hier == nullptr)
			{
				continue;
			}

			const XMVECTOR target_pos = target->GetPositionV();
			for (uint32_t iteration = 0; iteration < ik.iteration_count; ++iteration)
			{
				TransformComponent* stack[32] = {};
				Entity parent_entity = hier->parentID;
				TransformComponent* child_transform = transform;
				for (uint32_t chain = 0; chain < std::min(ik.chain_length, (uint32_t)arraysize(stack)); ++chain)
				{
					recompute_hierarchy = true; // any IK will trigger a full transform hierarchy recompute step at the end(**)

					// stack stores all traversed chain links so far:
					stack[chain] = child_transform;

					// Compute required parent rotation that moves ik transform closer to target transform:
					TransformComponent* parent_transform = transforms.GetComponent(parent_entity);
					const XMVECTOR parent_pos = parent_transform->GetPositionV();
					const XMVECTOR dir_parent_to_ik = XMVector3Normalize(transform->GetPositionV() - parent_pos);
					const XMVECTOR dir_parent_to_target = XMVector3Normalize(target_pos - parent_pos);
					const XMVECTOR axis = XMVector3Normalize(XMVector3Cross(dir_parent_to_ik, dir_parent_to_target));
					const float angle = XMScalarACos(XMVectorGetX(XMVector3Dot(dir_parent_to_ik, dir_parent_to_target)));
					const XMVECTOR Q = XMQuaternionNormalize(XMQuaternionRotationNormal(axis, angle));

					// parent to world space:
					parent_transform->ApplyTransform();
					// rotate parent:
					parent_transform->Rotate(Q);
					parent_transform->UpdateTransform();
					// parent back to local space (if parent has parent):
					const HierarchyComponent* hier_parent = hierarchy.GetComponent(parent_entity);
					if (hier_parent != nullptr)
					{
						Entity parent_of_parent_entity = hier_parent->parentID;
						const TransformComponent* transform_parent_of_parent = transforms.GetComponent(parent_of_parent_entity);
						XMMATRIX parent_of_parent_inverse = XMMatrixInverse(nullptr, XMLoadFloat4x4(&transform_parent_of_parent->world));
						parent_transform->MatrixTransform(parent_of_parent_inverse);
						// Do not call UpdateTransform() here, to keep parent world matrix in world space!
					}

					// update chain from parent to children:
					const TransformComponent* recurse_parent = parent_transform;
					for (int recurse_chain = (int)chain; recurse_chain >= 0; --recurse_chain)
					{
						stack[recurse_chain]->UpdateTransform_Parented(*recurse_parent);
						recurse_parent = stack[recurse_chain];
					}

					if (hier_parent == nullptr)
					{
						// chain root reached, exit
						break;
					}

					// move up in the chain by one:
					child_transform = parent_transform;
					parent_entity = hier_parent->parentID;
					assert(chain < (uint32_t)arraysize(stack) - 1); // if this is encountered, just extend stack array size

				}
			}
		}

		if (recompute_hierarchy)
		{
			// (**)If there was IK, we need to recompute transform hierarchy. This is only necessary for transforms that have parent
			//	transforms that are IK. Because the IK chain is computed from child to parent upwards, IK that have child would not update
			//	its transform properly in some cases (such as if animation writes to that child)
			for (size_t i = 0; i < hierarchy.GetCount(); ++i)
			{
				const HierarchyComponent& parentcomponent = hierarchy[i];
				Entity entity = hierarchy.GetEntity(i);

				TransformComponent* transform_child = transforms.GetComponent(entity);
				TransformComponent* transform_parent = transforms.GetComponent(parentcomponent.parentID);
				if (transform_child != nullptr && transform_parent != nullptr)
				{
					transform_child->UpdateTransform_Parented(*transform_parent);
				}
			}
		}
	}
	void Scene::RunArmatureUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		wiJobSystem::Dispatch(ctx, (uint32_t)armatures.GetCount(), 1, [&](wiJobArgs args) {

			ArmatureComponent& armature = armatures[args.jobIndex];
			Entity entity = armatures.GetEntity(args.jobIndex);
			const TransformComponent& transform = *transforms.GetComponent(entity);

			// The transform world matrices are in world space, but skinning needs them in armature-local space, 
			//	so that the skin is reusable for instanced meshes.
			//	We remove the armature's world matrix from the bone world matrix to obtain the bone local transform
			//	These local bone matrices will only be used for skinning, the actual transform components for the bones
			//	remain unchanged.
			//
			//	This is useful for an other thing too:
			//	If a whole transform tree is transformed by some parent (even gltf import does that to convert from RH to LH space)
			//	then the inverseBindMatrices are not reflected in that because they are not contained in the hierarchy system. 
			//	But this will correct them too.
			XMMATRIX R = XMMatrixInverse(nullptr, XMLoadFloat4x4(&transform.world));

			if (armature.boneData.size() != armature.boneCollection.size())
			{
				armature.boneData.resize(armature.boneCollection.size());
			}

			XMFLOAT3 _min = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
			XMFLOAT3 _max = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

			int boneIndex = 0;
			for (Entity boneEntity : armature.boneCollection)
			{
				const TransformComponent& bone = *transforms.GetComponent(boneEntity);

				XMMATRIX B = XMLoadFloat4x4(&armature.inverseBindMatrices[boneIndex]);
				XMMATRIX W = XMLoadFloat4x4(&bone.world);
				XMMATRIX M = B * W * R;

				armature.boneData[boneIndex++].Store(M);

				const float bone_radius = 1;
				XMFLOAT3 bonepos = bone.GetPosition();
				AABB boneAABB;
				boneAABB.createFromHalfWidth(bonepos, XMFLOAT3(bone_radius, bone_radius, bone_radius));
				_min = wiMath::Min(_min, boneAABB._min);
				_max = wiMath::Max(_max, boneAABB._max);
			}

			armature.aabb = AABB(_min, _max);

			if (!armature.boneBuffer.IsValid())
			{
				armature.CreateRenderData();
			}
		});
	}
	void Scene::RunMeshUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		wiJobSystem::Dispatch(ctx, (uint32_t)meshes.GetCount(), small_subtask_groupsize, [&](wiJobArgs args) {

			Entity entity = meshes.GetEntity(args.jobIndex);
			MeshComponent& mesh = meshes[args.jobIndex];
			GraphicsDevice* device = wiRenderer::GetDevice();

			if (mesh.IsSkinned() && armatures.Contains(mesh.armatureID))
			{
#ifndef GGREDUCED //PE: Remove all physics checks, we dont use it.
				const SoftBodyPhysicsComponent* softbody = softbodies.GetComponent(entity);
				if (softbody == nullptr || softbody->vertex_positions_simulation.empty())
				{
#endif
					if (!mesh.vertexBuffer_PRE.IsValid())
					{
						device->CreateBuffer(&mesh.streamoutBuffer_POS.GetDesc(), nullptr, &mesh.vertexBuffer_PRE);
					}
#ifndef GGREDUCED //PE: Remove all physics checks, we dont use it.
				}
#endif
			}

			if (mesh.streamoutBuffer_POS.IsValid() && mesh.vertexBuffer_PRE.IsValid())
			{
				mesh.dirty_bindless = true;
				std::swap(mesh.streamoutBuffer_POS, mesh.vertexBuffer_PRE);
			}

			uint32_t subsetIndex = 0;
			for (auto& subset : mesh.subsets)
			{
				const MaterialComponent* material = materials.GetComponent(subset.materialID);
				if (material != nullptr)
				{
					subset.materialIndex = (uint32_t)materials.GetIndex(subset.materialID);
					if (mesh.BLAS.IsValid())
					{
						auto& geometry = mesh.BLAS.desc.bottomlevel.geometries[subsetIndex];
						uint32_t flags = geometry._flags;
						if (material->IsAlphaTestEnabled() || (material->GetRenderTypes() & RENDERTYPE_TRANSPARENT) || !material->IsCastingShadow())
						{
							geometry._flags &= ~RaytracingAccelerationStructureDesc::BottomLevel::Geometry::FLAG_OPAQUE;
						}
						else
						{
							geometry._flags = RaytracingAccelerationStructureDesc::BottomLevel::Geometry::FLAG_OPAQUE;
						}
						if (flags != geometry._flags)
						{
							mesh.BLAS_state = MeshComponent::BLAS_STATE_NEEDS_REBUILD;
						}
						if (mesh.streamoutBuffer_POS.IsValid())
						{
							mesh.BLAS_state = MeshComponent::BLAS_STATE_NEEDS_REBUILD;
							geometry.triangles.vertexBuffer = mesh.streamoutBuffer_POS;
						}
					}
				}
				else
				{
					subset.materialIndex = 0;
				}
				subsetIndex++;
			}

					if (mesh.BLAS.IsValid())
					{
				if (mesh.dirty_morph)
						{
					mesh.BLAS_state = MeshComponent::BLAS_STATE_NEEDS_REBUILD;
						}
			}

			if (device->CheckCapability(GRAPHICSDEVICE_CAPABILITY_BINDLESS_DESCRIPTORS))
						{
				if (mesh.terrain_material1 != INVALID_ENTITY)
				{
					const MaterialComponent* mat = materials.GetComponent(mesh.terrain_material1);
					if (mat != nullptr)
					{
						int index = device->GetDescriptorIndex(&mat->constantBuffer, SRV);
						if (mesh.terrain_material1_index != index)
						{
							mesh.dirty_bindless = true;
							mesh.terrain_material1_index = index;
						}
					}
				}
				if (mesh.terrain_material2 != INVALID_ENTITY)
						{
					const MaterialComponent* mat = materials.GetComponent(mesh.terrain_material2);
					if (mat != nullptr)
					{
						int index = device->GetDescriptorIndex(&mat->constantBuffer, SRV);
						if (mesh.terrain_material2_index != index)
						{
							mesh.dirty_bindless = true;
							mesh.terrain_material2_index = index;
						}
					}
				}
				if (mesh.terrain_material3 != INVALID_ENTITY)
				{
					const MaterialComponent* mat = materials.GetComponent(mesh.terrain_material3);
					if (mat != nullptr)
					{
						int index = device->GetDescriptorIndex(&mat->constantBuffer, SRV);
						if (mesh.terrain_material3_index != index)
						{
							mesh.dirty_bindless = true;
							mesh.terrain_material3_index = index;
			}
					}
				}
			}

			// Update morph targets if needed:
			if (mesh.dirty_morph && !mesh.targets.empty())
			{
			    XMFLOAT3 _min = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
			    XMFLOAT3 _max = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

			    for (size_t i = 0; i < mesh.vertex_positions.size(); ++i)
			    {
					XMFLOAT3 pos = mesh.vertex_positions[i];
					XMFLOAT3 nor = mesh.vertex_normals.empty() ? XMFLOAT3(1, 1, 1) : mesh.vertex_normals[i];
					const uint8_t wind = mesh.vertex_windweights.empty() ? 0xFF : mesh.vertex_windweights[i];

					for (const MeshComponent::MeshMorphTarget& target : mesh.targets)
					{
						pos.x += target.weight * target.vertex_positions[i].x;
						pos.y += target.weight * target.vertex_positions[i].y;
						pos.z += target.weight * target.vertex_positions[i].z;

						if (!target.vertex_normals.empty())
						{
							nor.x += target.weight * target.vertex_normals[i].x;
							nor.y += target.weight * target.vertex_normals[i].y;
							nor.z += target.weight * target.vertex_normals[i].z;
						}
					}

					XMStoreFloat3(&nor, XMVector3Normalize(XMLoadFloat3(&nor)));
					mesh.vertex_positions_morphed[i].FromFULL(pos, nor, wind);

					_min = wiMath::Min(_min, pos);
					_max = wiMath::Max(_max, pos);
			    }

			    mesh.aabb = AABB(_min, _max);
			}

		});
	}
	void Scene::RunMaterialUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		wiJobSystem::Dispatch(ctx, (uint32_t)materials.GetCount(), small_subtask_groupsize, [&](wiJobArgs args) {

			MaterialComponent& material = materials[args.jobIndex];
			Entity entity = materials.GetEntity(args.jobIndex);
			const LayerComponent* layer = layers.GetComponent(entity);
			if (layer != nullptr)
			{
				material.layerMask = layer->layerMask;
			}

			if (!material.constantBuffer.IsValid())
			{
				material.CreateRenderData();
			}

			material.texAnimElapsedTime += dt * material.texAnimFrameRate;
			if (material.texAnimElapsedTime >= 1.0f)
			{
				material.texMulAdd.z = fmodf(material.texMulAdd.z + material.texAnimDirection.x, 1);
				material.texMulAdd.w = fmodf(material.texMulAdd.w + material.texAnimDirection.y, 1);
				material.texAnimElapsedTime = 0.0f;

				material.SetDirty(); // will trigger constant buffer update later on
			}

			material.engineStencilRef = STENCILREF_DEFAULT;
			if (material.IsCustomShader())
			{
				material.engineStencilRef = STENCILREF_CUSTOMSHADER;
			}

			if (material.IsDirty())
			{
				material.SetDirty(false);
				material.dirty_buffer = true;
			}

		});
	}
	void Scene::RunImpostorUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		if (impostors.GetCount() > 0 && !impostorArray.IsValid())
		{
			GraphicsDevice* device = wiRenderer::GetDevice();

			TextureDesc desc;
			desc.Width = impostorTextureDim;
			desc.Height = impostorTextureDim;

			desc.BindFlags = BIND_DEPTH_STENCIL;
			desc.ArraySize = 1;
			desc.Format = FORMAT_D16_UNORM;
			desc.layout = IMAGE_LAYOUT_DEPTHSTENCIL;
			device->CreateTexture(&desc, nullptr, &impostorDepthStencil);
			device->SetName(&impostorDepthStencil, "impostorDepthStencil");

			desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
			desc.ArraySize = maxImpostorCount * impostorCaptureAngles * 3;
			desc.Format = FORMAT_R8G8B8A8_UNORM;
			desc.layout = IMAGE_LAYOUT_SHADER_RESOURCE;

			device->CreateTexture(&desc, nullptr, &impostorArray);
			device->SetName(&impostorArray, "impostorArray");

			renderpasses_impostor.resize(desc.ArraySize);

			for (uint32_t i = 0; i < desc.ArraySize; ++i)
			{
				int subresource_index;
				subresource_index = device->CreateSubresource(&impostorArray, RTV, i, 1, 0, 1);
				assert(subresource_index == i);

				RenderPassDesc renderpassdesc;
				renderpassdesc.attachments.push_back(
					RenderPassAttachment::RenderTarget(
						&impostorArray,
						RenderPassAttachment::LOADOP_CLEAR
					)
				);
				renderpassdesc.attachments.back().subresource = subresource_index;

				renderpassdesc.attachments.push_back(
					RenderPassAttachment::DepthStencil(
						&impostorDepthStencil,
						RenderPassAttachment::LOADOP_CLEAR,
						RenderPassAttachment::STOREOP_DONTCARE
					)
				);

				device->CreateRenderPass(&renderpassdesc, &renderpasses_impostor[subresource_index]);
			}
		}

		wiJobSystem::Dispatch(ctx, (uint32_t)impostors.GetCount(), 1, [&](wiJobArgs args) {

			ImpostorComponent& impostor = impostors[args.jobIndex];
			impostor.aabb = AABB();
			impostor.instanceMatrices.clear();

			if (impostor.IsDirty())
			{
				impostor.SetDirty(false);
				impostor.render_dirty = true;
			}
		});
	}
	void Scene::RunObjectUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		assert(objects.GetCount() == aabb_objects.GetCount());

		lightmap_rects.resize(objects.GetCount());
		lightmap_rect_allocator.store(0);

		parallel_bounds.clear();
		parallel_bounds.resize((size_t)wiJobSystem::DispatchGroupCount((uint32_t)objects.GetCount(), small_subtask_groupsize));
		
		wiJobSystem::Dispatch(ctx, (uint32_t)objects.GetCount(), small_subtask_groupsize, [&](wiJobArgs args) {

			ObjectComponent& object = objects[args.jobIndex];
			AABB& aabb = aabb_objects[args.jobIndex];

#ifdef GGREDUCED
			//PE: LOD
			//object.SetRenderLOD(false);
			//if (object.IsLOD())
			//{
			//	if (object.GetCameraDistance() > object.GetLodDistance())
			//	{
			//		object.SetRenderLOD(true);
			//	}
			//}
#endif
			// Update occlusion culling status:
			if (!wiRenderer::GetFreezeCullingCameraEnabled())
			{
#ifdef GGREDUCED
				if (bEnableObjectCulling)
				{
					object.occlusionHistory <<= 1; // advance history by 1 frame
					int query_id = object.occlusionQueries[queryheap_idx];
					if (query_id >= 0 && (int)writtenQueries[queryheap_idx] > query_id)
					{
						uint64_t visible = queryResults[query_id];
						if (visible)
						{
							object.occlusionHistory |= 1; // visible
						}
					}
					else
					{
						object.occlusionHistory |= 1; // visible
					}
					object.occlusionQueries[queryheap_idx] = -1; // invalidate query
				}
				else
				{
					object.occlusionHistory |= 1;
				}
#endif
			}

			aabb = AABB();
			object.rendertypeMask = 0;
			object.SetDynamic(false);
			object.SetImpostorPlacement(false);
			object.SetRequestPlanarReflection(false);

			if (object.meshID != INVALID_ENTITY)
			{
				object.mesh_index = (uint32_t)meshes.GetIndex(object.meshID);

				Entity entity = objects.GetEntity(args.jobIndex);
				//const MeshComponent* mesh = meshes.GetComponent(object.meshID);
				// These will only be valid for a single frame:
				object.transform_index = (int)transforms.GetIndex(entity);
				object.prev_transform_index = (int)prev_transforms.GetIndex(entity);

				const TransformComponent& transform = transforms[object.transform_index];

				if (object.mesh_index >= 0)
				{
					//const MeshComponent& mesh = meshes[object.mesh_index];

					MeshComponent* mesh = &meshes[object.mesh_index]; //PE: NEWLOD
					XMMATRIX W = XMLoadFloat4x4(&transform.world);
					aabb = mesh->aabb.transform(W);

					// This is instance bounding box matrix:
					XMFLOAT4X4 meshMatrix;
					XMStoreFloat4x4(&meshMatrix, mesh->aabb.getAsBoxMatrix() * W);

					// We need sometimes the center of the instance bounding box, not the transform position (which can be outside the bounding box)
					object.center = *((XMFLOAT3*)&meshMatrix._41);

					if (mesh->IsSkinned() || mesh->IsDynamic())
					{
						object.SetDynamic(true);
						const ArmatureComponent* armature = armatures.GetComponent(mesh->armatureID);
						if (armature != nullptr)
						{
							aabb = AABB::Merge(aabb, armature->aabb);
						}
					}

					//PE: NEWLOD select lod object.
					if (mesh->lodlevels > 0)
					{
						extern float fLODMultiplier;
						float length_between_lod = 200.0f * fLODMultiplier;
						float dist = object.GetCameraDistance();
						int active_lod = object.forcelod;
						if (object.forcelod == 0)
						{
							if (mesh->subsets.size() > 1 && mesh->lodlevels >= 1 && dist > (length_between_lod * 2))
								active_lod = 1;
							if (mesh->subsets.size() > 2 && mesh->lodlevels >= 2 && dist > (length_between_lod * 3))
								active_lod = 2;
							if (mesh->subsets.size() > 3 && mesh->lodlevels >= 3 && dist > (length_between_lod * 4))
								active_lod = 3;
						}
						object.activelod = active_lod;

						int count = 0;
						if (mesh->activelodlevels)
						{
							for (auto& subset : mesh->subsets)
							{
								subset.active = true;
							}
							mesh->activelodlevels = false;
						}
						else
						{
							for (auto& subset : mesh->subsets)
							{
								if (count++ == active_lod)
									subset.active = true;
								else
									subset.active = false;
							}
						}
					}
					auto& subset = mesh->subsets[0]; //PE: Always use subset[0] for material.
					//for (auto& subset : mesh->subsets)
					{
						const MaterialComponent* material = materials.GetComponent(subset.materialID);

						if (material != nullptr)
						{
							object.rendertypeMask |= material->GetRenderTypes();

							if (material->HasPlanarReflection())
							{
								object.SetRequestPlanarReflection(true);
							}
						}
					}

					ImpostorComponent* impostor = impostors.GetComponent(object.meshID);
					if (impostor != nullptr)
					{
						object.SetImpostorPlacement(true);
						object.impostorSwapDistance = impostor->swapInDistance;
						object.impostorFadeThresholdRadius = aabb.getRadius();

						impostor->aabb = AABB::Merge(impostor->aabb, aabb);
						impostor->color = object.color;
						impostor->fadeThresholdRadius = object.impostorFadeThresholdRadius;

						const SPHERE boundingsphere = mesh->GetBoundingSphere();

						locker.lock();
						impostor->instanceMatrices.emplace_back();
						XMStoreFloat4x4(&impostor->instanceMatrices.back(),
							XMMatrixScaling(boundingsphere.radius, boundingsphere.radius, boundingsphere.radius) *
							XMMatrixTranslation(boundingsphere.center.x, boundingsphere.center.y, boundingsphere.center.z) *
							W
						);
						locker.unlock();
					}

#ifndef GGREDUCED //PE: Remove all physics checks, we dont use it.
					SoftBodyPhysicsComponent* softbody = softbodies.GetComponent(object.meshID);
					if (softbody != nullptr)
					{
						// this will be registered as soft body in the next physics update
						softbody->_flags |= SoftBodyPhysicsComponent::SAFE_TO_REGISTER;

						// soft body manipulated with the object matrix
						softbody->worldMatrix = transform.world;

						if (softbody->graphicsToPhysicsVertexMapping.empty())
						{
							softbody->CreateFromMesh(*mesh);
						}

						// simulation aabb will be used for soft bodies
						aabb = softbody->aabb;

						// soft bodies have no transform, their vertices are simulated in world space
						object.transform_index = -1;
						object.prev_transform_index = -1;
					}
#endif
					if (TLAS.IsValid())
					{
						GraphicsDevice* device = wiRenderer::GetDevice();
						RaytracingAccelerationStructureDesc::TopLevel::Instance instance = {};
						const XMFLOAT4X4& worldMatrix = object.transform_index >= 0 ? transforms[object.transform_index].world : IDENTITYMATRIX;
						instance = {};
						instance.transform = XMFLOAT3X4(
							worldMatrix._11, worldMatrix._21, worldMatrix._31, worldMatrix._41,
							worldMatrix._12, worldMatrix._22, worldMatrix._32, worldMatrix._42,
							worldMatrix._13, worldMatrix._23, worldMatrix._33, worldMatrix._43
						);
						instance.InstanceID = (uint32_t)device->GetDescriptorIndex(&mesh->descriptor, SRV);
						instance.InstanceMask = 1;
						instance.bottomlevel = mesh->BLAS;

						if (XMVectorGetX(XMMatrixDeterminant(W)) > 0)
						{
							// There is a mismatch between object space winding and BLAS winding:
							//	https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_raytracing_instance_flags
							instance.Flags = RaytracingAccelerationStructureDesc::TopLevel::Instance::FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
						}

						void* dest = (void*)((size_t)TLAS_instances.data() + (size_t)args.jobIndex * device->GetTopLevelAccelerationStructureInstanceSize());
						device->WriteTopLevelAccelerationStructureInstance(&instance, dest);
					}

					// lightmap things:
					if (dt > 0)
					{
						if (object.IsLightmapRenderRequested() && dt > 0)
						{
							if (!object.lightmap.IsValid())
							{
								{
									// Unfortunately, fp128 format only correctly downloads from GPU if it is pow2 size:
									object.lightmapWidth = wiMath::GetNextPowerOfTwo(object.lightmapWidth + 1) / 2;
									object.lightmapHeight = wiMath::GetNextPowerOfTwo(object.lightmapHeight + 1) / 2;
								}

								TextureDesc desc;
								desc.Width = object.lightmapWidth;
								desc.Height = object.lightmapHeight;
								desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
								// Note: we need the full precision format to achieve correct accumulative blending! 
								//	But the global atlas will have less precision for good bandwidth for sampling
								desc.Format = FORMAT_R32G32B32A32_FLOAT;

								GraphicsDevice* device = wiRenderer::GetDevice();
								device->CreateTexture(&desc, nullptr, &object.lightmap);
								device->SetName(&object.lightmap, "object.lightmap");

								RenderPassDesc renderpassdesc;

								renderpassdesc.attachments.push_back(RenderPassAttachment::RenderTarget(&object.lightmap, RenderPassAttachment::LOADOP_CLEAR));

								device->CreateRenderPass(&renderpassdesc, &object.renderpass_lightmap_clear);

								renderpassdesc.attachments.back().loadop = RenderPassAttachment::LOADOP_LOAD;
								device->CreateRenderPass(&renderpassdesc, &object.renderpass_lightmap_accumulate);
							}
							lightmap_refresh_needed.store(true);
						}

						if (!object.lightmapTextureData.empty() && !object.lightmap.IsValid())
						{
							// Create a GPU-side per object lighmap if there is none yet, so that copying into atlas can be done efficiently:
							wiTextureHelper::CreateTexture(object.lightmap, object.lightmapTextureData.data(), object.lightmapWidth, object.lightmapHeight, object.GetLightmapFormat());
						}

						if (object.lightmap.IsValid())
						{
							if (object.lightmap_rect.w == 0)
							{
								// we need to pack this lightmap texture into the atlas
								object.lightmap_rect = wiRectPacker::rect_xywh(0, 0, object.lightmap.GetDesc().Width + atlasClampBorder * 2, object.lightmap.GetDesc().Height + atlasClampBorder * 2);
								lightmap_repack_needed.store(true); // will need to repack all in this case!
							}
							// lightmap rects' state is always updated, in case one needs repacking
							uint32_t alloc = lightmap_rect_allocator.fetch_add(1);
							lightmap_rects[alloc] = &object.lightmap_rect;
						}
					}
				}

				const LayerComponent* layer = layers.GetComponent(entity);
				if (layer == nullptr)
				{
					aabb.layerMask = ~0;
				}
				else
				{
					aabb.layerMask = layer->GetLayerMask();
				}

				// parallel bounds computation using shared memory:
				AABB* shared_bounds = (AABB*)args.sharedmemory;
				if (args.isFirstJobInGroup)
				{
					*shared_bounds = aabb_objects[args.jobIndex];
				}
				else
				{
					*shared_bounds = AABB::Merge(*shared_bounds, aabb_objects[args.jobIndex]);
				}
				if (args.isLastJobInGroup)
				{
					parallel_bounds[args.groupID] = *shared_bounds;
				}
			}

		}, sizeof(AABB));
	}
	void Scene::RunCameraUpdateSystem(wiJobSystem::context& ctx)
	{
		wiJobSystem::Dispatch(ctx, (uint32_t)cameras.GetCount(), small_subtask_groupsize, [&](wiJobArgs args) {

			CameraComponent& camera = cameras[args.jobIndex];
			Entity entity = cameras.GetEntity(args.jobIndex);
			const TransformComponent* transform = transforms.GetComponent(entity);
			if (transform != nullptr)
			{
				camera.TransformCamera(*transform);
			}
			camera.UpdateCamera();
		});
	}
	void Scene::RunDecalUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		assert(decals.GetCount() == aabb_decals.GetCount());

		for (size_t i = 0; i < decals.GetCount(); ++i)
		{
			DecalComponent& decal = decals[i];
			Entity entity = decals.GetEntity(i);
			const TransformComponent& transform = *transforms.GetComponent(entity);
			decal.world = transform.world;

			XMMATRIX W = XMLoadFloat4x4(&decal.world);
			XMVECTOR front = XMVectorSet(0, 0, 1, 0);
			front = XMVector3TransformNormal(front, W);
			XMStoreFloat3(&decal.front, front);

			XMVECTOR S, R, T;
			XMMatrixDecompose(&S, &R, &T, W);
			XMStoreFloat3(&decal.position, T);
			XMFLOAT3 scale;
			XMStoreFloat3(&scale, S);
			decal.range = std::max(scale.x, std::max(scale.y, scale.z)) * 2;

			AABB& aabb = aabb_decals[i];
			aabb.createFromHalfWidth(XMFLOAT3(0, 0, 0), XMFLOAT3(1, 1, 1));
			aabb = aabb.transform(transform.world);

			const LayerComponent* layer = layers.GetComponent(entity);
			if (layer == nullptr)
			{
				aabb.layerMask = ~0;
			}
			else
			{
				aabb.layerMask = layer->GetLayerMask();
			}

			const MaterialComponent& material = *materials.GetComponent(entity);
			decal.color = material.baseColor;
			decal.emissive = material.GetEmissiveStrength();
			decal.texture = material.textures[MaterialComponent::BASECOLORMAP].resource;
			decal.normal = material.textures[MaterialComponent::NORMALMAP].resource;

			// atlas part is not thread safe:
			if (decal.texture != nullptr && decal.texture->texture.IsValid())
			{
				if (packedDecals.find(decal.texture) == packedDecals.end())
				{
					// we need to pack this decal texture into the atlas
					wiRectPacker::rect_xywh newRect = wiRectPacker::rect_xywh(0, 0, decal.texture->texture.desc.Width + atlasClampBorder * 2, decal.texture->texture.desc.Height + atlasClampBorder * 2);
					packedDecals[decal.texture] = newRect;
					decal_repack_needed = true;
	}
			}
		}
	}
	void Scene::RunProbeUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		assert(probes.GetCount() == aabb_probes.GetCount());

#ifdef GGREDUCED
		if (!envmapArray.IsValid() || envmapNewRes != envmapRes) // even when zero probes, this will be created, since sometimes only the sky will be rendered into it
		{
			envmapRes = envmapNewRes;
#else
		if (!envmapArray.IsValid()) // even when zero probes, this will be created, since sometimes only the sky will be rendered into it
		{
#endif
			GraphicsDevice* device = wiRenderer::GetDevice();

			TextureDesc desc;
			desc.ArraySize = 6;
			desc.BindFlags = BIND_DEPTH_STENCIL;
			desc.CPUAccessFlags = 0;
			desc.Format = FORMAT_D16_UNORM;
			desc.Height = envmapRes;
			desc.Width = envmapRes;
			desc.MipLevels = 1;
			desc.MiscFlags = RESOURCE_MISC_TEXTURECUBE;
			desc.Usage = USAGE_DEFAULT;
			desc.layout = IMAGE_LAYOUT_DEPTHSTENCIL;

			device->CreateTexture(&desc, nullptr, &envrenderingDepthBuffer);
			device->SetName(&envrenderingDepthBuffer, "envrenderingDepthBuffer");

			desc.ArraySize = envmapCount * 6;
			desc.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET | BIND_UNORDERED_ACCESS;
			desc.CPUAccessFlags = 0;
			desc.Format = FORMAT_R11G11B10_FLOAT;
			desc.Height = envmapRes;
			desc.Width = envmapRes;
			desc.MipLevels = envmapMIPs;
			desc.MiscFlags = RESOURCE_MISC_TEXTURECUBE;
			desc.Usage = USAGE_DEFAULT;
			desc.layout = IMAGE_LAYOUT_SHADER_RESOURCE;

			device->CreateTexture(&desc, nullptr, &envmapArray);
			device->SetName(&envmapArray, "envmapArray");

			renderpasses_envmap.resize(envmapCount);

			for (uint32_t i = 0; i < envmapCount; ++i)
			{
				int subresource_index;
				subresource_index = device->CreateSubresource(&envmapArray, RTV, i * 6, 6, 0, 1);
				assert(subresource_index == i);

				RenderPassDesc renderpassdesc;
				renderpassdesc.attachments.push_back(
					RenderPassAttachment::RenderTarget(&envmapArray,
						RenderPassAttachment::LOADOP_DONTCARE
					)
				);
				renderpassdesc.attachments.back().subresource = subresource_index;

				renderpassdesc.attachments.push_back(
					RenderPassAttachment::DepthStencil(
						&envrenderingDepthBuffer,
						RenderPassAttachment::LOADOP_CLEAR,
						RenderPassAttachment::STOREOP_DONTCARE
					)
				);

				device->CreateRenderPass(&renderpassdesc, &renderpasses_envmap[subresource_index]);
			}
			for (uint32_t i = 0; i < envmapArray.desc.MipLevels; ++i)
			{
				int subresource_index;
				subresource_index = device->CreateSubresource(&envmapArray, SRV, 0, desc.ArraySize, i, 1);
				assert(subresource_index == i);
				subresource_index = device->CreateSubresource(&envmapArray, UAV, 0, desc.ArraySize, i, 1);
				assert(subresource_index == i);
			}

			// debug probe views, individual cubes:
			for (uint32_t i = 0; i < envmapCount; ++i)
			{
				int subresource_index;
				subresource_index = device->CreateSubresource(&envmapArray, SRV, i * 6, 6, 0, -1);
				assert(subresource_index == envmapArray.desc.MipLevels + i);
			}
		}

		// reconstruct envmap array status:
		bool envmapTaken[envmapCount] = {};
		for (size_t i = 0; i < probes.GetCount(); ++i)
		{
			EnvironmentProbeComponent& probe = probes[i];
			if (probe.textureIndex >= 0 && probe.textureIndex < envmapCount)
			{
				envmapTaken[probe.textureIndex] = true;
			}
			else
			{
				probe.textureIndex = -1;
			}
		}

		for (size_t probeIndex = 0; probeIndex < probes.GetCount(); ++probeIndex)
		{
			EnvironmentProbeComponent& probe = probes[probeIndex];
			Entity entity = probes.GetEntity(probeIndex);
			const TransformComponent& transform = *transforms.GetComponent(entity);

			probe.position = transform.GetPosition();

			XMMATRIX W = XMLoadFloat4x4(&transform.world);
			XMStoreFloat4x4(&probe.inverseMatrix, XMMatrixInverse(nullptr, W));

			XMVECTOR S, R, T;
			XMMatrixDecompose(&S, &R, &T, W);
			XMFLOAT3 scale;
			XMStoreFloat3(&scale, S);
			probe.range = std::max(scale.x, std::max(scale.y, scale.z)) * 2;

			AABB& aabb = aabb_probes[probeIndex];
			aabb.createFromHalfWidth(XMFLOAT3(0, 0, 0), XMFLOAT3(1, 1, 1));
			aabb = aabb.transform(transform.world);

			const LayerComponent* layer = layers.GetComponent(entity);
			if (layer == nullptr)
			{
				aabb.layerMask = ~0;
	}
			else
			{
				aabb.layerMask = layer->GetLayerMask();
			}

			if (probe.IsDirty() || probe.IsRealTime())
			{
				probe.SetDirty(false);
				probe.render_dirty = true;
			}

			if (probe.render_dirty && probe.textureIndex < 0)
			{
				// need to take a free envmap texture slot:
				bool found = false;
				for (int i = 0; i < arraysize(envmapTaken); ++i)
				{
					if (envmapTaken[i] == false)
					{
						envmapTaken[i] = true;
						probe.textureIndex = i;
						found = true;
						break;
					}
				}
			}
		}
	}
	void Scene::RunForceUpdateSystem(wiJobSystem::context& ctx)
	{
		wiJobSystem::Dispatch(ctx, (uint32_t)forces.GetCount(), small_subtask_groupsize, [&](wiJobArgs args) {

			ForceFieldComponent& force = forces[args.jobIndex];
			Entity entity = forces.GetEntity(args.jobIndex);
			const TransformComponent& transform = *transforms.GetComponent(entity);

			XMMATRIX W = XMLoadFloat4x4(&transform.world);
			XMVECTOR S, R, T;
			XMMatrixDecompose(&S, &R, &T, W);

			XMStoreFloat3(&force.position, T);
			XMStoreFloat3(&force.direction, XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, -1, 0, 0), W)));

			force.range_global = force.range_local * std::max(XMVectorGetX(S), std::max(XMVectorGetY(S), XMVectorGetZ(S)));
		});
	}
	static uint32_t iLightcounter = 0;
	void Scene::RunLightUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		assert(lights.GetCount() == aabb_lights.GetCount());
		iLightcounter++;

		wiJobSystem::Dispatch(ctx, (uint32_t)lights.GetCount(), small_subtask_groupsize, [&](wiJobArgs args) {

			LightComponent& light = lights[args.jobIndex];
			//light.bNotRenderedInThisframe = false;
			//if (light.type == LightComponent::POINT)
			//{
			//	bool shadow = light.IsCastingShadow() && !light.IsStatic();
			//	if (shadow)
			//	{
			//		if ((iLightcounter + args.jobIndex) % 10 != 0)
			//		{
			//			light.bNotRenderedInThisframe = true;
			//			//return;
			//		}
			//	}
			//}

			Entity entity = lights.GetEntity(args.jobIndex);
			const TransformComponent& transform = *transforms.GetComponent(entity);
			AABB& aabb = aabb_lights[args.jobIndex];

			const LayerComponent* layer = layers.GetComponent(entity);
			if (layer == nullptr)
			{
				aabb.layerMask = ~0;
			}
			else
			{
				aabb.layerMask = layer->GetLayerMask();
			}

			XMMATRIX W = XMLoadFloat4x4(&transform.world);
			XMVECTOR S, R, T;
			XMMatrixDecompose(&S, &R, &T, W);

			XMStoreFloat3(&light.position, T);
			XMStoreFloat4(&light.rotation, R);
			XMStoreFloat3(&light.scale, S);
			XMStoreFloat3(&light.direction, XMVector3TransformNormal(XMVectorSet(0, 1, 0, 0), W));

			light.range_global = light.range_local * std::max(XMVectorGetX(S), std::max(XMVectorGetY(S), XMVectorGetZ(S)));

			switch (light.type)
			{
			default:
			case LightComponent::DIRECTIONAL:
				aabb.createFromHalfWidth(XMFLOAT3(0, 0, 0), XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX));
				locker.lock();
				if (args.jobIndex < weather.most_important_light_index)
				{
					weather.most_important_light_index = args.jobIndex;
					weather.sunColor = light.color;
					weather.sunDirection = light.direction;
					weather.sunEnergy = light.energy;
				}
				locker.unlock();
				break;
			case LightComponent::SPOT:
				aabb.createFromHalfWidth(light.position, XMFLOAT3(light.GetRange(), light.GetRange(), light.GetRange()));
				break;
			case LightComponent::POINT:
				aabb.createFromHalfWidth(light.position, XMFLOAT3(light.GetRange(), light.GetRange(), light.GetRange()));
				break;
			}

		});
	}
	void Scene::RunParticleUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		//PE: Try running this on main thread only. to prevent CORRUPTED_MULTITHREADING
		for (uint32_t i = 0; i < emitters.GetCount(); i++)
		{
			wiEmittedParticle& emitter = emitters[i];
			Entity entity = emitters.GetEntity(i);

			const LayerComponent* layer = layers.GetComponent(entity);
			if (layer != nullptr)
			{
				emitter.layerMask = layer->GetLayerMask();
			}

			const TransformComponent& transform = *transforms.GetComponent(entity);
			emitter.UpdateCPU(transform, dt);
		}
/*
		wiJobSystem::Dispatch(ctx, (uint32_t)emitters.GetCount(), small_subtask_groupsize, [&](wiJobArgs args) {

			wiEmittedParticle& emitter = emitters[args.jobIndex];
			Entity entity = emitters.GetEntity(args.jobIndex);

			const LayerComponent* layer = layers.GetComponent(entity);
			if (layer != nullptr)
			{
				emitter.layerMask = layer->GetLayerMask();
			}

			const TransformComponent& transform = *transforms.GetComponent(entity);
			emitter.UpdateCPU(transform, dt);
		});
*/

		wiJobSystem::Dispatch(ctx, (uint32_t)hairs.GetCount(), small_subtask_groupsize, [&](wiJobArgs args) {

			wiHairParticle& hair = hairs[args.jobIndex];
			Entity entity = hairs.GetEntity(args.jobIndex);

			const LayerComponent* layer = layers.GetComponent(entity);
			if (layer != nullptr)
			{
				hair.layerMask = layer->GetLayerMask();
			}

			if (hair.meshID != INVALID_ENTITY)
			{
				const MeshComponent* mesh = meshes.GetComponent(hair.meshID);

				if (mesh != nullptr)
				{
					const TransformComponent& transform = *transforms.GetComponent(entity);

					hair.UpdateCPU(transform, *mesh, dt);
				}
			}

		});
	}
	void Scene::RunWeatherUpdateSystem(wiJobSystem::context& ctx)
	{
#ifdef GGREDUCED
#ifdef OPTICK_ENABLE
		OPTICK_EVENT();
#endif
#endif
		if (weathers.GetCount() > 0)
		{
			weather = weathers[0];
			weather.most_important_light_index = ~0;

			if (weather.IsOceanEnabled() && !ocean.IsValid())
			{
				OceanRegenerate();
		}
	}
	}
	void Scene::RunSoundUpdateSystem(wiJobSystem::context& ctx)
	{
		const CameraComponent& camera = GetCamera();
		wiAudio::SoundInstance3D instance3D;
		instance3D.listenerPos = camera.Eye;
		instance3D.listenerUp = camera.Up;
		instance3D.listenerFront = camera.At;

		for (size_t i = 0; i < sounds.GetCount(); ++i)
		{
			SoundComponent& sound = sounds[i];
			const bool bIsplaying = sound.IsPlaying();
			if (!sound.IsDisable3D() && bIsplaying)
			{
				Entity entity = sounds.GetEntity(i);
				const TransformComponent* transform = transforms.GetComponent(entity);
				if (transform != nullptr)
				{
					instance3D.emitterPos = transform->GetPosition();
					wiAudio::Update3D(&sound.soundinstance, instance3D, sound.CurveDistanceScaler);
				}
			}
			if (bIsplaying)
			{
				wiAudio::Play(&sound.soundinstance);
			}
			else
			{
				wiAudio::Stop(&sound.soundinstance);
			}
			wiAudio::SetVolume(sound.volume, &sound.soundinstance);
		}
	}

	void Scene::PutWaterRipple(const std::string& image, const XMFLOAT3& pos)
	{
		wiSprite img(image);
		img.params.enableExtractNormalMap();
		img.params.blendFlag = BLENDMODE_ADDITIVE;
		img.anim.fad = 0.01f;
		img.anim.scaleX = 0.2f;
		img.anim.scaleY = 0.2f;
		img.params.pos = pos;
		img.params.rotation = (wiRandom::getRandom(0, 1000) * 0.001f) * 2 * 3.1415f;
		img.params.siz = XMFLOAT2(1, 1);
		img.params.quality = QUALITY_ANISOTROPIC;
		img.params.pivot = XMFLOAT2(0.5f, 0.5f);
		waterRipples.push_back(img);
	}

	XMVECTOR SkinVertex(const MeshComponent& mesh, const ArmatureComponent& armature, uint32_t index, XMVECTOR* N)
	{
		XMVECTOR P;
		if (mesh.vertex_positions_morphed.empty())
		{
		    P = XMLoadFloat3(&mesh.vertex_positions[index]);
		}
		else
		{
		    P = mesh.vertex_positions_morphed[index].LoadPOS();
		}
#ifdef GGREDUCED
		if (index >= mesh.vertex_boneindices.size()) return(P);
		if (index >= mesh.vertex_boneweights.size()) return(P);
#endif
		const XMUINT4& ind = mesh.vertex_boneindices[index];
		const XMFLOAT4& wei = mesh.vertex_boneweights[index];

		const XMMATRIX M[] = {
			armature.boneData[ind.x].Load(),
			armature.boneData[ind.y].Load(),
			armature.boneData[ind.z].Load(),
			armature.boneData[ind.w].Load(),
		};

		XMVECTOR skinned;
		skinned =  XMVector3Transform(P, M[0]) * wei.x;
		skinned += XMVector3Transform(P, M[1]) * wei.y;
		skinned += XMVector3Transform(P, M[2]) * wei.z;
		skinned += XMVector3Transform(P, M[3]) * wei.w;
		P = skinned;

		if (N != nullptr)
		{
			*N = XMLoadFloat3(&mesh.vertex_normals[index]);
			skinned =  XMVector3TransformNormal(*N, M[0]) * wei.x;
			skinned += XMVector3TransformNormal(*N, M[1]) * wei.y;
			skinned += XMVector3TransformNormal(*N, M[2]) * wei.z;
			skinned += XMVector3TransformNormal(*N, M[3]) * wei.w;
			*N = XMVector3Normalize(skinned);
		}

		return P;
	}




	Entity LoadModel(const std::string& fileName, const XMMATRIX& transformMatrix, bool attached)
	{
		Scene scene;
		Entity root = LoadModel(scene, fileName, transformMatrix, attached);
		GetScene().Merge(scene);
		return root;
	}

	Entity LoadModel(Scene& scene, const std::string& fileName, const XMMATRIX& transformMatrix, bool attached)
	{
		wiArchive archive(fileName, true);
		if (archive.IsOpen())
		{
			// Serialize it from file:
			scene.Serialize(archive);

			// First, create new root:
			Entity root = CreateEntity();
			scene.transforms.Create(root);
			scene.layers.Create(root).layerMask = ~0;

			{
				// Apply the optional transformation matrix to the new scene:

				// Parent all unparented transforms to new root entity
				for (size_t i = 0; i < scene.transforms.GetCount() - 1; ++i) // GetCount() - 1 because the last added was the "root"
				{
					Entity entity = scene.transforms.GetEntity(i);
					if (!scene.hierarchy.Contains(entity))
					{
						scene.Component_Attach(entity, root);
					}
				}

				// The root component is transformed, scene is updated:
				scene.transforms.GetComponent(root)->MatrixTransform(transformMatrix);
				scene.Update(0);
			}

			if (!attached)
			{
				// In this case, we don't care about the root anymore, so delete it. This will simplify overall hierarchy
				scene.Component_DetachChildren(root);
				scene.Entity_Remove(root);
				root = INVALID_ENTITY;
			}

			return root;
		}

		return INVALID_ENTITY;
	}

	PickResult Pick(const RAY& ray, uint32_t renderTypeMask, uint32_t layerMask, const Scene& scene)
	{
		PickResult result;

		if (scene.objects.GetCount() > 0)
		{
			const XMVECTOR rayOrigin = XMLoadFloat3(&ray.origin);
			const XMVECTOR rayDirection = XMVector3Normalize(XMLoadFloat3(&ray.direction));

			/* Attempted to sort to speed things up
//#ifdef GGREDUCED
			// ray pick check is slow, checks ALL scene objects with no consideration of nearness to ray start
			struct sNearItem
			{
				int iIndex;
				float fDistance;
			};
			std::vector<sNearItem> nearestFirstList;
			nearestFirstList.clear();
			XMVECTOR origRayDir = XMLoadFloat3(&ray.direction);
			XMVECTOR vRayLength = XMVector3Length(origRayDir);
			for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
			{
				const AABB& aabb = scene.aabb_objects[i];
				if (ray.intersects(aabb))
				{
					// add AABB if within distance of ray length
					const XMVECTOR aabbCenter = XMLoadFloat3(&aabb.getCenter());
					const XMVECTOR distVector = aabbCenter - rayOrigin;
					XMVECTOR vDistance = XMVector3Length(distVector);
					if (*vDistance.m128_f32 < *vRayLength.m128_f32)
					{
						sNearItem item;
						item.iIndex = i;
						item.fDistance = *vDistance.m128_f32;
						nearestFirstList.push_back(item);
					}
				}
			}
			// bubble sort all AABB boxes the ray slices through within the range of the ray
			for (size_t j = 0; j < nearestFirstList.size(); ++j)
			{
				for (size_t k = 0; k < nearestFirstList.size(); ++k)
				{
					if (nearestFirstList[j].iIndex != nearestFirstList[k].iIndex )
					{
						if (nearestFirstList[j].fDistance > nearestFirstList[k].fDistance)
						{
							sNearItem storej = nearestFirstList[j];
							nearestFirstList[j] = nearestFirstList[k];
							nearestFirstList[k] = storej;
						}
					}
				}
			}
//#else
*/
			for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
			{
//#endif
/*
//#ifdef GGREDUCED
			for (size_t j = 0; j < nearestFirstList.size(); ++j)
			{
				int i = nearestFirstList[j].iIndex;
				const AABB& aabb = scene.aabb_objects[i];
//#else
*/
				const AABB& aabb = scene.aabb_objects[i];
				if (!ray.intersects(aabb))
				{
					continue;
				}
//#endif

				const ObjectComponent& object = scene.objects[i];
				if (object.meshID == INVALID_ENTITY)
				{
					continue;
				}
				if (!(renderTypeMask & object.GetRenderTypes()))
				{
					continue;
				}

#ifdef GGREDUCED
				if (!object.IsRenderable())
				{
					//PE: Do not Pick from hidden objects.
					continue;
				}
#endif
				Entity entity = scene.aabb_objects.GetEntity(i);
				const LayerComponent* layer = scene.layers.GetComponent(entity);
				if (layer != nullptr && !(layer->GetLayerMask() & layerMask))
				{
					continue;
				}

				const MeshComponent& mesh = *scene.meshes.GetComponent(object.meshID);
#ifndef GGREDUCED //PE: Remove all physics checks, we dont use it.
				const SoftBodyPhysicsComponent* softbody = scene.softbodies.GetComponent(object.meshID);
				const bool softbody_active = softbody != nullptr && !softbody->vertex_positions_simulation.empty();
#else
				const bool softbody_active = false;
#endif

				const XMMATRIX objectMat = object.transform_index >= 0 ? XMLoadFloat4x4(&scene.transforms[object.transform_index].world) : XMMatrixIdentity();
				const XMMATRIX objectMat_Inverse = XMMatrixInverse(nullptr, objectMat);

				const XMVECTOR rayOrigin_local = XMVector3Transform(rayOrigin, objectMat_Inverse);
				const XMVECTOR rayDirection_local = XMVector3Normalize(XMVector3TransformNormal(rayDirection, objectMat_Inverse));

				const ArmatureComponent* armature = mesh.IsSkinned() ? scene.armatures.GetComponent(mesh.armatureID) : nullptr;

				int subsetCounter = 0;
				int count = 0;
				int target_lod = 0;

				if (bRaycastLowestLOD)
					target_lod = mesh.lodlevels;

				for (auto& subset : mesh.subsets)
				{
					if (count++ != target_lod) //PE: Always uselowest lod.
						continue;

					//if (!subset.active)
					//	continue;

					for (size_t i = 0; i < subset.indexCount; i += 3)
					{
						const uint32_t i0 = mesh.indices[subset.indexOffset + i + 0];
						const uint32_t i1 = mesh.indices[subset.indexOffset + i + 1];
						const uint32_t i2 = mesh.indices[subset.indexOffset + i + 2];

						XMVECTOR p0;
						XMVECTOR p1;
						XMVECTOR p2;

						//if (softbody_active)
						//{
						//	p0 = softbody->vertex_positions_simulation[i0].LoadPOS();
						//	p1 = softbody->vertex_positions_simulation[i1].LoadPOS();
						//	p2 = softbody->vertex_positions_simulation[i2].LoadPOS();
						//}
						//else
						{
							if (armature == nullptr)
							{
								if (mesh.vertex_positions_morphed.empty())
							    {
									p0 = XMLoadFloat3(&mesh.vertex_positions[i0]);
									p1 = XMLoadFloat3(&mesh.vertex_positions[i1]);
									p2 = XMLoadFloat3(&mesh.vertex_positions[i2]);
								}
								else
								{
								    p0 = mesh.vertex_positions_morphed[i0].LoadPOS();
								    p1 = mesh.vertex_positions_morphed[i1].LoadPOS();
								    p2 = mesh.vertex_positions_morphed[i2].LoadPOS();
								}
							}
							else
							{
								p0 = SkinVertex(mesh, *armature, i0);
								p1 = SkinVertex(mesh, *armature, i1);
								p2 = SkinVertex(mesh, *armature, i2);
							}
						}

						float distance;
						XMFLOAT2 bary;
						if (wiMath::RayTriangleIntersects(rayOrigin_local, rayDirection_local, p0, p1, p2, distance, bary))
						{
							const XMVECTOR pos = XMVector3Transform(XMVectorAdd(rayOrigin_local, rayDirection_local*distance), objectMat);
							distance = wiMath::Distance(pos, rayOrigin);

							if (distance < result.distance)
							{
								const XMVECTOR nor = XMVector3Normalize(XMVector3TransformNormal(XMVector3Cross(XMVectorSubtract(p2, p1), XMVectorSubtract(p1, p0)), objectMat));

								#ifdef GGREDUCED
								// ignore world normals facing away, means ceilings no longer interfere with ray cast
								// LB: OR the object is double sided and can be seen from behind, thus pickable
								if (XMVectorGetX(XMVector3Dot(nor, rayDirection)) <= 0.0f || mesh.IsDoubleSided() == true)
								{ 
								#endif

								result.entity = entity;
								XMStoreFloat3(&result.position, pos);
								XMStoreFloat3(&result.normal, nor);
								result.distance = distance;
								result.subsetIndex = subsetCounter;
								result.vertexID0 = (int)i0;
								result.vertexID1 = (int)i1;
								result.vertexID2 = (int)i2;
								result.bary = bary;

								#ifdef GGREDUCED
								}
								#endif
							}
						}
					}
					subsetCounter++;
				}

			}
		}

		// Construct a matrix that will orient to position (P) according to surface normal (N):
		XMVECTOR N = XMLoadFloat3(&result.normal);
		XMVECTOR P = XMLoadFloat3(&result.position);
		XMVECTOR E = XMLoadFloat3(&ray.origin);
		XMVECTOR T = XMVector3Normalize(XMVector3Cross(N, P - E));
		XMVECTOR B = XMVector3Normalize(XMVector3Cross(T, N));
		XMMATRIX M = { T, N, B, P };
		XMStoreFloat4x4(&result.orientation, M);

		return result;
	}

	SceneIntersectSphereResult SceneIntersectSphere(const SPHERE& sphere, uint32_t renderTypeMask, uint32_t layerMask, const Scene& scene)
	{
		SceneIntersectSphereResult result;
		XMVECTOR Center = XMLoadFloat3(&sphere.center);
		XMVECTOR Radius = XMVectorReplicate(sphere.radius);
		XMVECTOR RadiusSq = XMVectorMultiply(Radius, Radius);

		if (scene.objects.GetCount() > 0)
		{

			for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
			{
				const AABB& aabb = scene.aabb_objects[i];
				if (!sphere.intersects(aabb))
				{
					continue;
				}

				const ObjectComponent& object = scene.objects[i];
				if (object.meshID == INVALID_ENTITY)
				{
					continue;
				}
				if (!(renderTypeMask & object.GetRenderTypes()))
				{
					continue;
				}

				Entity entity = scene.aabb_objects.GetEntity(i);
				const LayerComponent* layer = scene.layers.GetComponent(entity);
				if (layer != nullptr && !(layer->GetLayerMask() & layerMask))
				{
					continue;
				}

				const MeshComponent& mesh = *scene.meshes.GetComponent(object.meshID);
#ifndef GGREDUCED //PE: Remove all physics checks, we dont use it.
				const SoftBodyPhysicsComponent* softbody = scene.softbodies.GetComponent(object.meshID);
				const bool softbody_active = softbody != nullptr && !softbody->vertex_positions_simulation.empty();
#else
				const bool softbody_active = false;
#endif

				const XMMATRIX objectMat = object.transform_index >= 0 ? XMLoadFloat4x4(&scene.transforms[object.transform_index].world) : XMMatrixIdentity();

				const ArmatureComponent* armature = mesh.IsSkinned() ? scene.armatures.GetComponent(mesh.armatureID) : nullptr;

				int subsetCounter = 0;
				int count = 0;
				int target_lod = 0;

				if (bRaycastLowestLOD)
					target_lod = mesh.lodlevels;

				for (auto& subset : mesh.subsets)
				{
					if (count++ != target_lod) //PE: Always uselowest lod.
						continue;

					//if (!subset.active)
					//	continue;

					for (size_t i = 0; i < subset.indexCount; i += 3)
					{
						const uint32_t i0 = mesh.indices[subset.indexOffset + i + 0];
						const uint32_t i1 = mesh.indices[subset.indexOffset + i + 1];
						const uint32_t i2 = mesh.indices[subset.indexOffset + i + 2];

						XMVECTOR p0;
						XMVECTOR p1;
						XMVECTOR p2;

						//if (softbody_active)
						//{
						//	p0 = softbody->vertex_positions_simulation[i0].LoadPOS();
						//	p1 = softbody->vertex_positions_simulation[i1].LoadPOS();
						//	p2 = softbody->vertex_positions_simulation[i2].LoadPOS();
						//}
						//else
						{
							if (armature == nullptr)
							{
								p0 = XMLoadFloat3(&mesh.vertex_positions[i0]);
								p1 = XMLoadFloat3(&mesh.vertex_positions[i1]);
								p2 = XMLoadFloat3(&mesh.vertex_positions[i2]);
							}
							else
							{
								p0 = SkinVertex(mesh, *armature, i0);
								p1 = SkinVertex(mesh, *armature, i1);
								p2 = SkinVertex(mesh, *armature, i2);
							}
						}

						p0 = XMVector3Transform(p0, objectMat);
						p1 = XMVector3Transform(p1, objectMat);
						p2 = XMVector3Transform(p2, objectMat);

						XMFLOAT3 min, max;
						XMStoreFloat3(&min, XMVectorMin(p0, XMVectorMin(p1, p2)));
						XMStoreFloat3(&max, XMVectorMax(p0, XMVectorMax(p1, p2)));
						AABB aabb_triangle(min, max);
						if (sphere.intersects(aabb_triangle) == AABB::OUTSIDE)
						{
							continue;
						}

						// Compute the plane of the triangle (has to be normalized).
						XMVECTOR N = XMVector3Normalize(XMVector3Cross(XMVectorSubtract(p1, p0), XMVectorSubtract(p2, p0)));

						// Assert that the triangle is not degenerate.
						assert(!XMVector3Equal(N, XMVectorZero()));

						// Find the nearest feature on the triangle to the sphere.
						XMVECTOR Dist = XMVector3Dot(XMVectorSubtract(Center, p0), N);

						if (!mesh.IsDoubleSided() && XMVectorGetX(Dist) > 0)
						{
							continue; // pass through back faces
						}

						// If the center of the sphere is farther from the plane of the triangle than
						// the radius of the sphere, then there cannot be an intersection.
						XMVECTOR NoIntersection = XMVectorLess(Dist, XMVectorNegate(Radius));
						NoIntersection = XMVectorOrInt(NoIntersection, XMVectorGreater(Dist, Radius));

						// Project the center of the sphere onto the plane of the triangle.
						XMVECTOR Point0 = XMVectorNegativeMultiplySubtract(N, Dist, Center);

						// Is it inside all the edges? If so we intersect because the distance 
						// to the plane is less than the radius.
						//XMVECTOR Intersection = DirectX::Internal::PointOnPlaneInsideTriangle(Point0, p0, p1, p2);

						// Compute the cross products of the vector from the base of each edge to 
						// the point with each edge vector.
						XMVECTOR C0 = XMVector3Cross(XMVectorSubtract(Point0, p0), XMVectorSubtract(p1, p0));
						XMVECTOR C1 = XMVector3Cross(XMVectorSubtract(Point0, p1), XMVectorSubtract(p2, p1));
						XMVECTOR C2 = XMVector3Cross(XMVectorSubtract(Point0, p2), XMVectorSubtract(p0, p2));

						// If the cross product points in the same direction as the normal the the
						// point is inside the edge (it is zero if is on the edge).
						XMVECTOR Zero = XMVectorZero();
						XMVECTOR Inside0 = XMVectorLessOrEqual(XMVector3Dot(C0, N), Zero);
						XMVECTOR Inside1 = XMVectorLessOrEqual(XMVector3Dot(C1, N), Zero);
						XMVECTOR Inside2 = XMVectorLessOrEqual(XMVector3Dot(C2, N), Zero);

						// If the point inside all of the edges it is inside.
						XMVECTOR Intersection = XMVectorAndInt(XMVectorAndInt(Inside0, Inside1), Inside2);

						bool inside = XMVector4EqualInt(XMVectorAndCInt(Intersection, NoIntersection), XMVectorTrueInt());

						// Find the nearest point on each edge.

						// Edge 0,1
						XMVECTOR Point1 = DirectX::Internal::PointOnLineSegmentNearestPoint(p0, p1, Center);

						// If the distance to the center of the sphere to the point is less than 
						// the radius of the sphere then it must intersect.
						Intersection = XMVectorOrInt(Intersection, XMVectorLessOrEqual(XMVector3LengthSq(XMVectorSubtract(Center, Point1)), RadiusSq));

						// Edge 1,2
						XMVECTOR Point2 = DirectX::Internal::PointOnLineSegmentNearestPoint(p1, p2, Center);

						// If the distance to the center of the sphere to the point is less than 
						// the radius of the sphere then it must intersect.
						Intersection = XMVectorOrInt(Intersection, XMVectorLessOrEqual(XMVector3LengthSq(XMVectorSubtract(Center, Point2)), RadiusSq));

						// Edge 2,0
						XMVECTOR Point3 = DirectX::Internal::PointOnLineSegmentNearestPoint(p2, p0, Center);

						// If the distance to the center of the sphere to the point is less than 
						// the radius of the sphere then it must intersect.
						Intersection = XMVectorOrInt(Intersection, XMVectorLessOrEqual(XMVector3LengthSq(XMVectorSubtract(Center, Point3)), RadiusSq));

						bool intersects = XMVector4EqualInt(XMVectorAndCInt(Intersection, NoIntersection), XMVectorTrueInt());

						if (intersects)
						{
							XMVECTOR bestPoint = Point0;
							if (!inside)
							{
								// If the sphere center's projection on the triangle plane is not within the triangle,
								//	determine the closest point on triangle to the sphere center
								float bestDist = XMVectorGetX(XMVector3LengthSq(Point1 - Center));
								bestPoint = Point1;

								float d = XMVectorGetX(XMVector3LengthSq(Point2 - Center));
								if (d < bestDist)
								{
									bestDist = d;
									bestPoint = Point2;
								}
								d = XMVectorGetX(XMVector3LengthSq(Point3 - Center));
								if (d < bestDist)
								{
									bestDist = d;
									bestPoint = Point3;
								}
							}
							XMVECTOR intersectionVec = Center - bestPoint;
							XMVECTOR intersectionVecLen = XMVector3Length(intersectionVec);

							result.entity = entity;
							result.depth = sphere.radius - XMVectorGetX(intersectionVecLen);
							XMStoreFloat3(&result.position, bestPoint);
							XMStoreFloat3(&result.normal, intersectionVec / intersectionVecLen);
							return result;
						}
					}
					subsetCounter++;
				}

			}
		}

		return result;
	}
	SceneIntersectSphereResult SceneIntersectCapsule(const CAPSULE& capsule, uint32_t renderTypeMask, uint32_t layerMask, const Scene& scene)
	{
		SceneIntersectSphereResult result;
		XMVECTOR Base = XMLoadFloat3(&capsule.base);
		XMVECTOR Tip = XMLoadFloat3(&capsule.tip);
		XMVECTOR Radius = XMVectorReplicate(capsule.radius);
		XMVECTOR LineEndOffset = XMVector3Normalize(Tip - Base) * Radius;
		XMVECTOR A = Base + LineEndOffset;
		XMVECTOR B = Tip - LineEndOffset;
		XMVECTOR RadiusSq = XMVectorMultiply(Radius, Radius);
		AABB capsule_aabb = capsule.getAABB();

		if (scene.objects.GetCount() > 0)
		{

			for (size_t i = 0; i < scene.aabb_objects.GetCount(); ++i)
			{
				const AABB& aabb = scene.aabb_objects[i];
				if (capsule_aabb.intersects(aabb) == AABB::INTERSECTION_TYPE::OUTSIDE)
				{
					continue;
				}

				const ObjectComponent& object = scene.objects[i];
				if (object.meshID == INVALID_ENTITY)
				{
					continue;
				}
				if (!(renderTypeMask & object.GetRenderTypes()))
				{
					continue;
				}

				Entity entity = scene.aabb_objects.GetEntity(i);
				const LayerComponent* layer = scene.layers.GetComponent(entity);
				if (layer != nullptr && !(layer->GetLayerMask() & layerMask))
				{
					continue;
				}

				const MeshComponent& mesh = *scene.meshes.GetComponent(object.meshID);
#ifndef GGREDUCED //PE: Remove all physics checks, we dont use it.
				const SoftBodyPhysicsComponent* softbody = scene.softbodies.GetComponent(object.meshID);
				const bool softbody_active = softbody != nullptr && !softbody->vertex_positions_simulation.empty();
#else
				const bool softbody_active = false;
#endif

				const XMMATRIX objectMat = object.transform_index >= 0 ? XMLoadFloat4x4(&scene.transforms[object.transform_index].world) : XMMatrixIdentity();

				const ArmatureComponent* armature = mesh.IsSkinned() ? scene.armatures.GetComponent(mesh.armatureID) : nullptr;

				int subsetCounter = 0;
				int count = 0;

				int target_lod = 0;

				if(bRaycastLowestLOD)
					target_lod = mesh.lodlevels;

				for (auto& subset : mesh.subsets)
				{
					if (count++ != target_lod) //PE: Always uselowest lod.
						continue;

					//if (!subset.active)
					//	continue;

					for (size_t i = 0; i < subset.indexCount; i += 3)
					{
						const uint32_t i0 = mesh.indices[subset.indexOffset + i + 0];
						const uint32_t i1 = mesh.indices[subset.indexOffset + i + 1];
						const uint32_t i2 = mesh.indices[subset.indexOffset + i + 2];

						XMVECTOR p0;
						XMVECTOR p1;
						XMVECTOR p2;

						//if (softbody_active)
						//{
						//	p0 = softbody->vertex_positions_simulation[i0].LoadPOS();
						//	p1 = softbody->vertex_positions_simulation[i1].LoadPOS();
						//	p2 = softbody->vertex_positions_simulation[i2].LoadPOS();
						//}
						//else
						{
							if (armature == nullptr)
							{
								p0 = XMLoadFloat3(&mesh.vertex_positions[i0]);
								p1 = XMLoadFloat3(&mesh.vertex_positions[i1]);
								p2 = XMLoadFloat3(&mesh.vertex_positions[i2]);
							}
							else
							{
								p0 = SkinVertex(mesh, *armature, i0);
								p1 = SkinVertex(mesh, *armature, i1);
								p2 = SkinVertex(mesh, *armature, i2);
							}
						}
						
						p0 = XMVector3Transform(p0, objectMat);
						p1 = XMVector3Transform(p1, objectMat);
						p2 = XMVector3Transform(p2, objectMat);

						XMFLOAT3 min, max;
						XMStoreFloat3(&min, XMVectorMin(p0, XMVectorMin(p1, p2)));
						XMStoreFloat3(&max, XMVectorMax(p0, XMVectorMax(p1, p2)));
						AABB aabb_triangle(min, max);
						if (capsule_aabb.intersects(aabb_triangle) == AABB::OUTSIDE)
						{
							continue;
						}

						// Compute the plane of the triangle (has to be normalized).
						XMVECTOR N = XMVector3Normalize(XMVector3Cross(XMVectorSubtract(p1, p0), XMVectorSubtract(p2, p0)));
						
						XMVECTOR ReferencePoint;
						XMVECTOR d = XMVector3Normalize(B - A);
						if (abs(XMVectorGetX(XMVector3Dot(N, d))) < FLT_EPSILON)
						{
							// Capsule line cannot be intersected with triangle plane (they are parallel)
							//	In this case, just take a point from triangle
							ReferencePoint = p0;
						}
						else
						{
							// Intersect capsule line with triangle plane:
							XMVECTOR t = XMVector3Dot(N, (Base - p0) / XMVectorAbs(XMVector3Dot(N, d)));
							XMVECTOR LinePlaneIntersection = Base + d * t;

							// Compute the cross products of the vector from the base of each edge to 
							// the point with each edge vector.
							XMVECTOR C0 = XMVector3Cross(XMVectorSubtract(LinePlaneIntersection, p0), XMVectorSubtract(p1, p0));
							XMVECTOR C1 = XMVector3Cross(XMVectorSubtract(LinePlaneIntersection, p1), XMVectorSubtract(p2, p1));
							XMVECTOR C2 = XMVector3Cross(XMVectorSubtract(LinePlaneIntersection, p2), XMVectorSubtract(p0, p2));

							// If the cross product points in the same direction as the normal the the
							// point is inside the edge (it is zero if is on the edge).
							XMVECTOR Zero = XMVectorZero();
							XMVECTOR Inside0 = XMVectorLessOrEqual(XMVector3Dot(C0, N), Zero);
							XMVECTOR Inside1 = XMVectorLessOrEqual(XMVector3Dot(C1, N), Zero);
							XMVECTOR Inside2 = XMVectorLessOrEqual(XMVector3Dot(C2, N), Zero);

							// If the point inside all of the edges it is inside.
							XMVECTOR Intersection = XMVectorAndInt(XMVectorAndInt(Inside0, Inside1), Inside2);

							bool inside = XMVectorGetIntX(Intersection) != 0;

							if (inside)
							{
								ReferencePoint = LinePlaneIntersection;
							}
							else
							{
								// Find the nearest point on each edge.

								// Edge 0,1
								XMVECTOR Point1 = wiMath::ClosestPointOnLineSegment(p0, p1, LinePlaneIntersection);

								// Edge 1,2
								XMVECTOR Point2 = wiMath::ClosestPointOnLineSegment(p1, p2, LinePlaneIntersection);

								// Edge 2,0
								XMVECTOR Point3 = wiMath::ClosestPointOnLineSegment(p2, p0, LinePlaneIntersection);

								ReferencePoint = Point1;
								float bestDist = XMVectorGetX(XMVector3LengthSq(Point1 - LinePlaneIntersection));
								float d = abs(XMVectorGetX(XMVector3LengthSq(Point2 - LinePlaneIntersection)));
								if (d < bestDist)
								{
									bestDist = d;
									ReferencePoint = Point2;
								}
								d = abs(XMVectorGetX(XMVector3LengthSq(Point3 - LinePlaneIntersection)));
								if (d < bestDist)
								{
									bestDist = d;
									ReferencePoint = Point3;
								}
							}


						}

						// Place a sphere on closest point on line segment to intersection:
						XMVECTOR Center = wiMath::ClosestPointOnLineSegment(A, B, ReferencePoint);

						// Assert that the triangle is not degenerate.
						assert(!XMVector3Equal(N, XMVectorZero()));

						// Find the nearest feature on the triangle to the sphere.
						XMVECTOR Dist = XMVector3Dot(XMVectorSubtract(Center, p0), N);

						if (!mesh.IsDoubleSided() && XMVectorGetX(Dist) > 0)
						{
							continue; // pass through back faces
						}

						// If the center of the sphere is farther from the plane of the triangle than
						// the radius of the sphere, then there cannot be an intersection.
						XMVECTOR NoIntersection = XMVectorLess(Dist, XMVectorNegate(Radius));
						NoIntersection = XMVectorOrInt(NoIntersection, XMVectorGreater(Dist, Radius));

						// Project the center of the sphere onto the plane of the triangle.
						XMVECTOR Point0 = XMVectorNegativeMultiplySubtract(N, Dist, Center);

						// Is it inside all the edges? If so we intersect because the distance 
						// to the plane is less than the radius.
						//XMVECTOR Intersection = DirectX::Internal::PointOnPlaneInsideTriangle(Point0, p0, p1, p2);

						// Compute the cross products of the vector from the base of each edge to 
						// the point with each edge vector.
						XMVECTOR C0 = XMVector3Cross(XMVectorSubtract(Point0, p0), XMVectorSubtract(p1, p0));
						XMVECTOR C1 = XMVector3Cross(XMVectorSubtract(Point0, p1), XMVectorSubtract(p2, p1));
						XMVECTOR C2 = XMVector3Cross(XMVectorSubtract(Point0, p2), XMVectorSubtract(p0, p2));

						// If the cross product points in the same direction as the normal the the
						// point is inside the edge (it is zero if is on the edge).
						XMVECTOR Zero = XMVectorZero();
						XMVECTOR Inside0 = XMVectorLessOrEqual(XMVector3Dot(C0, N), Zero);
						XMVECTOR Inside1 = XMVectorLessOrEqual(XMVector3Dot(C1, N), Zero);
						XMVECTOR Inside2 = XMVectorLessOrEqual(XMVector3Dot(C2, N), Zero);

						// If the point inside all of the edges it is inside.
						XMVECTOR Intersection = XMVectorAndInt(XMVectorAndInt(Inside0, Inside1), Inside2);

						bool inside = XMVector4EqualInt(XMVectorAndCInt(Intersection, NoIntersection), XMVectorTrueInt());

						// Find the nearest point on each edge.

						// Edge 0,1
						XMVECTOR Point1 = wiMath::ClosestPointOnLineSegment(p0, p1, Center);

						// If the distance to the center of the sphere to the point is less than 
						// the radius of the sphere then it must intersect.
						Intersection = XMVectorOrInt(Intersection, XMVectorLessOrEqual(XMVector3LengthSq(XMVectorSubtract(Center, Point1)), RadiusSq));

						// Edge 1,2
						XMVECTOR Point2 = wiMath::ClosestPointOnLineSegment(p1, p2, Center);

						// If the distance to the center of the sphere to the point is less than 
						// the radius of the sphere then it must intersect.
						Intersection = XMVectorOrInt(Intersection, XMVectorLessOrEqual(XMVector3LengthSq(XMVectorSubtract(Center, Point2)), RadiusSq));

						// Edge 2,0
						XMVECTOR Point3 = wiMath::ClosestPointOnLineSegment(p2, p0, Center);

						// If the distance to the center of the sphere to the point is less than 
						// the radius of the sphere then it must intersect.
						Intersection = XMVectorOrInt(Intersection, XMVectorLessOrEqual(XMVector3LengthSq(XMVectorSubtract(Center, Point3)), RadiusSq));

						bool intersects = XMVector4EqualInt(XMVectorAndCInt(Intersection, NoIntersection), XMVectorTrueInt());

						if (intersects)
						{
							XMVECTOR bestPoint = Point0;
							if (!inside)
							{
								// If the sphere center's projection on the triangle plane is not within the triangle,
								//	determine the closest point on triangle to the sphere center
								float bestDist = XMVectorGetX(XMVector3LengthSq(Point1 - Center));
								bestPoint = Point1;

								float d = XMVectorGetX(XMVector3LengthSq(Point2 - Center));
								if (d < bestDist)
								{
									bestDist = d;
									bestPoint = Point2;
								}
								d = XMVectorGetX(XMVector3LengthSq(Point3 - Center));
								if (d < bestDist)
								{
									bestDist = d;
									bestPoint = Point3;
								}
							}
							XMVECTOR intersectionVec = Center - bestPoint;
							XMVECTOR intersectionVecLen = XMVector3Length(intersectionVec);

							result.entity = entity;
							result.depth = capsule.radius - XMVectorGetX(intersectionVecLen);
							XMStoreFloat3(&result.position, bestPoint);
							XMStoreFloat3(&result.normal, intersectionVec / intersectionVecLen);
							return result;
						}
					}
					subsetCounter++;
				}

			}
		}

		return result;
	}

}

