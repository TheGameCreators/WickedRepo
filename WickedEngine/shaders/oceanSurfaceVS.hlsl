#include "globals.hlsli"
#include "oceanSurfaceHF.hlsli"

TEXTURE2D(texture_displacementmap, float4, TEXSLOT_ONDEMAND0);

static const float3 QUAD[] = {
	float3(0, 0, 0),
	float3(0, 1, 0),
	float3(1, 0, 0),
	float3(1, 0, 0),
	float3(1, 1, 0),
	float3(0, 1, 0),
};

#define infinite g_xCamera_ZFarP
float3 intersectPlaneClampInfinite(in float3 rayOrigin, in float3 rayDirection, float planeHeight)
{
	float dist = (planeHeight - rayOrigin.y) / rayDirection.y;
	if (dist < 0.0)
		return rayOrigin + rayDirection * dist;
	else
		return float3(rayOrigin.x, planeHeight, rayOrigin.z) - normalize(float3(rayDirection.x, 0, rayDirection.z)) * infinite;
}

PSIn main(uint fakeIndex : SV_VERTEXID)
{
	PSIn Out;

	// fake instancing of quads:
	uint vertexID = fakeIndex % 6;
	uint instanceID = fakeIndex / 6;

	// Retrieve grid dimensions and 1/gridDimensions:
	float2 dim = xOceanScreenSpaceParams.xy;
	float2 invdim = xOceanScreenSpaceParams.zw;
	uint dimX = (uint) dim.x;

	// Assemble screen space grid:
	Out.pos = float4(QUAD[vertexID], 1);
	Out.pos.xy *= invdim;
	Out.pos.xy += float2(instanceID % dimX, instanceID / dimX) * invdim;
	float2 nextPos = Out.pos.xy + invdim;
	Out.pos.xy = Out.pos.xy * 2 - 1;
	Out.pos.xy *= max(1, xOceanSurfaceDisplacementTolerance); // extrude screen space grid to tolerate displacement

	nextPos.xy = nextPos.xy * 2 - 1;
	nextPos.xy *= max(1, xOceanSurfaceDisplacementTolerance);

	// Perform ray tracing of screen grid and plane surface to unproject to world space:
	float3 o = g_xCamera_CamPos;
	float4 r = mul(g_xCamera_InvP, float4(Out.pos.xy, 1, 1)); // use Z=1 to get a vector to the near plane rather than the far plane (might be infinite)
	r.xyz /= r.w;
	float3x3 InvView3 = (float3x3) g_xCamera_InvV;
	float3 d = mul( InvView3, r.xyz );
	d = -normalize( d );
	float3 worldPos = intersectPlaneClampInfinite(o, d, xOceanWaterHeight);

	r = mul(g_xCamera_InvP, float4(nextPos.xy, 1, 1));
	r.xyz /= r.w;
	d = mul( InvView3, r.xyz );
	d = -normalize( d );
	float3 nextWorldPos = intersectPlaneClampInfinite(o, d, xOceanWaterHeight);

	// Displace surface:
	float2 uv = worldPos.xz * xOceanPatchSizeRecip * 0.0254;
	float2 nextUV = nextWorldPos.xz * xOceanPatchSizeRecip * 0.0254;
	float3 displacement = texture_displacementmap.SampleGrad(sampler_linear_wrap, uv + xOceanMapHalfTexel, nextUV.x - uv.x, nextUV.y - uv.y).xzy * 39.37;
	displacement *= 1 - saturate(distance(g_xCamera_CamPos, worldPos) * 0.0025f * 0.0254);
	/*
	float4 screenPos = mul(g_xCamera_VP, float4(worldPos, 1));
	screenPos.xy /= screenPos.w;
	float linearDepth = length( g_xCamera_CamPos - worldPos );
	screenPos.xy = screenPos.xy * float2(0.5, -0.5) + 0.5;
	float sampled_lineardepth = texture_lineardepth.SampleLevel(sampler_point_clamp, screenPos.xy, 0) * g_xCamera_ZFarP;

	float displacementScale = (sampled_lineardepth - linearDepth) * 0.0254 / 10;
	displacementScale = saturate( displacementScale );
	*/
	worldPos += displacement;// * displacementScale;

	// Reproject displaced surface and output:
	Out.pos = mul(g_xCamera_VP, float4(worldPos, 1));
	Out.pos3D = worldPos;
	Out.uv = uv;
	Out.ReflectionMapSamplingPos = mul(g_xCamera_ReflVP, float4(worldPos, 1));

	return Out;
}
