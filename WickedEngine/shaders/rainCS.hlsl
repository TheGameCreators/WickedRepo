#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"


TEXTURE2D(input, float4, TEXSLOT_ONDEMAND0);
TEXTURE2D(raintexture, float4, TEXSLOT_ONDEMAND1);
TEXTURE2D(rainnormal, float4, TEXSLOT_ONDEMAND2);

struct RainTex { SamplerState smpl; Texture2D tex; };

RWTEXTURE2D(output, unorm float4, 0);

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	float2 uv = (DTid.xy + 0.5f) * xPPResolution_rcp;
	float RainRefractionScale = xPPParams1.y;
	float RainOpacity = xPPParams0.x;
	float RainScaleX = xPPParams0.y;
	float RainScaleY = xPPParams0.z;

	float RainOffsetX = xPPParams1.x;
	float RainOffsetY = xPPParams0.w;

	float2 uv2 = uv;

	RainTex maintex = { sampler_linear_clamp, input };
	RainTex raintex = { sampler_linear_wrap, raintexture };
	RainTex normaltex = { sampler_linear_wrap, rainnormal };

	uv2 = frac(uv2 + float2(RainOffsetX, RainOffsetY));
	uv2.x *= RainScaleX;
	uv2.y *= RainScaleY;

	float4 NormalTexture = normaltex.tex.SampleLevel(sampler_linear_wrap, uv2, 0.0);
	float4 Normalmap = NormalTexture * 2.0 - 1.0;
	float2 displaceUV = Normalmap * RainRefractionScale;


	//PE: Mix in original pixel so we dont have huge differernces.
	float4 MainTextureOrg = maintex.tex.SampleLevel(sampler_linear_clamp, uv, 0.0);
	uv = frac(uv + displaceUV);
	float4 MainTexture = maintex.tex.SampleLevel(sampler_linear_clamp, uv, 0.0);

	float4 RainTexture = raintex.tex.SampleLevel(sampler_linear_wrap, uv2, 0.0);
	MainTexture = lerp(MainTexture, MainTextureOrg, 0.5 - (RainTexture.x * 0.5) );

	//Mix in a little refraction to the actual rain drop.
	RainTexture = lerp(MainTexture, RainTexture, 0.75);

	float4 FinalMap = lerp(MainTexture, RainTexture, clamp(RainTexture.x + RainOpacity,0.0,1.0) );

	output[DTid.xy] = FinalMap;
}
