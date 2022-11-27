#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"

// This will cut out bright parts (>1) and also downsample 4x

TEXTURE2D(input, float4, TEXSLOT_ONDEMAND0);

RWTEXTURE2D(output, float4, 0);

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const float2 uv = DTid.xy;

	float3 color = 0;

	color += min( input.SampleLevel(sampler_linear_clamp, (uv + float2(0.25, 0.25)) * xPPResolution_rcp, 0).rgb, 30 );
	color += min( input.SampleLevel(sampler_linear_clamp, (uv + float2(0.75, 0.25)) * xPPResolution_rcp, 0).rgb, 30 );
	color += min( input.SampleLevel(sampler_linear_clamp, (uv + float2(0.25, 0.75)) * xPPResolution_rcp, 0).rgb, 30 );
	color += min( input.SampleLevel(sampler_linear_clamp, (uv + float2(0.75, 0.75)) * xPPResolution_rcp, 0).rgb, 30 );

	color /= 4.0f;

	const float bloomThreshold = xPPParams0.x;

	//color = min(color, 10);
	color = max(color - bloomThreshold, 0);

	output[DTid.xy] = float4(color, 1);
}
