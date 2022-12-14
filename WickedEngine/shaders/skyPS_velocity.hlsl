#include "objectHF.hlsli"
#include "skyHF.hlsli"

float4 main(float4 pos : SV_POSITION, float2 clipspace : TEXCOORD) : SV_TARGET
{
	// use Z=1 to use near plane instead of far plane for accuracy
	float4 unprojected = mul(g_xCamera_InvVP, float4(clipspace, 1.0f, 1.0f));
	unprojected.xyz /= unprojected.w;

	float4 pos2DPrev = mul(g_xCamera_PrevVP, float4(unprojected.xyz, 1));
	float2 velocity = ((pos2DPrev.xy / pos2DPrev.w - g_xFrame_TemporalAAJitterPrev) - (clipspace - g_xFrame_TemporalAAJitter)) * float2(0.5f, -0.5f);

	return float4(velocity, 0, 0);
}
