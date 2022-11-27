#define OBJECTSHADER_USE_NORMAL
#define OBJECTSHADER_USE_RENDERTARGETARRAYINDEX
#include "objectHF.hlsli"
#include "lightingHF.hlsli"
#include "skyHF.hlsli"

float4 main(PixelInput input) : SV_TARGET
{
	float3 normal = normalize(input.nor);
	float4 color = float4(GetDynamicSkyColor(normal, false), 1); // disable sun as its light is applied to objects separately
	return float4(color.rgb,1);
}
