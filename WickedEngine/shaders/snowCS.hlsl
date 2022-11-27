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
	float snowOpacity = xPPParams0.x;
	float snowLayers = xPPParams0.y;
	float snowDepth = xPPParams0.z;
	float snowWind = xPPParams0.w;
	float snowSpeed = 0.15; //xPPParams1.x;
	float snowOffset = xPPParams1.y;

	RainTex maintex = { sampler_linear_clamp, input };
	float4 MainTextureOrg = maintex.tex.SampleLevel(sampler_linear_clamp, uv, 0.0);

	float4 result = 0;
	float3x3 snowmatrix = { 13.323122, 23.5112, 21.71123,
							21.1212,   28.7312, 11.9312,
							21.8112,   14.7212, 61.3934 };

	float time = snowOffset;
	float sparkle = sin(time*.01); //snow sparkle-twinkle

	for (float i = 0.; i < snowLayers; i++)
	{
		float2 q = uv * (1.0 + i * snowDepth);
		q += float2(q.y* snowWind *(frac(i*7.2) - 0.5), -snowSpeed * time / (1.0 + i * snowDepth *.03));

		float3 n = float3(floor(q), i);
		float3 m = floor(n) / 1e5 + frac(n);
		float3 matmult = mul(snowmatrix, m);
		float3 mp = (31415.9 + m) / frac(matmult);
		float3 r = frac(mp);
		float2 s = abs(frac(q) - .5 + .9*r.xy - .5) + .01*abs(2 * frac(10 * q.yx) - 1.0);
		float d = .6* (s.x + s.y) + max(s.x, s.y) - .01;
		float edge = .005 + .05* min(.5* abs(i - 5 - sparkle), 1);

		result += smoothstep(edge, -edge, d) * r.x / (1.0 + .02*i*snowDepth);
	}


	output[DTid.xy] = (result * snowOpacity) + MainTextureOrg;

	// add snow to scene simple additive blend
	//return (result * snowOpacity) + MainTextureOrg;
	//alternative blending - fades snow into lighter areas of the screen more naturally
	//return max(result*1.5*snowOpacity, cTextureScreen);

}
