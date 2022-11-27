#include "objectHF.hlsli"
#include "skyHF.hlsli"

float4 main(float4 pos : SV_POSITION, float2 clipspace : TEXCOORD) : SV_TARGET
{
	// use Z=1 to use near plane instead of far plane for accuracy
	//float4 unprojected = mul(g_xCamera_InvVP, float4(clipspace, 1.0f, 1.0f));
	//unprojected.xyz /= unprojected.w;
	//const float3 V = normalize(unprojected.xyz - g_xCamera_CamPos);

	//PE: Found this under dynamic (// avoid CameraPos since it introduces floating point inaccuracies) works no flicker.
	float4 unprojected = mul(g_xCamera_InvP, float4(clipspace, 1.0f, 1.0f));
	unprojected.xyz /= unprojected.w;
	float3x3 InvView3 = (float3x3) g_xCamera_InvV;
	float3 V = mul(InvView3, unprojected.xyz);
	V = normalize(V);


	float4 color = float4(DEGAMMA_SKY(texture_globalenvmap.SampleLevel(sampler_linear_clamp, V, 0).rgb), 1);
	CalculateClouds(color.rgb, V, false);

	//#ifdef GGREDUCED
	//LB: Restore fog control for interior scenes - arg duplicated all this, should really use common parts of ApplyFog
	const float4 PassedInFogColor = float4(GetFogColor().xyz, 1);
	const float PassedInFogOpacity = clamp(GetFogOpacity(), 0.0, 1.0);
	color = lerp(color, PassedInFogColor, PassedInFogOpacity);
	//#endif

	//PE: Not used ?
	//float4 pos2DPrev = mul(g_xCamera_PrevVP, float4(unprojected.xyz, 1));
	//float2 velocity = ((pos2DPrev.xy / pos2DPrev.w - g_xFrame_TemporalAAJitterPrev) - (clipspace - g_xFrame_TemporalAAJitter)) * float2(0.5f, -0.5f);

	return color;
}

