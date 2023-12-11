#define OBJECTSHADER_LAYOUT_POS_TEX
#define OBJECTSHADER_USE_COLOR
#include "objectHF.hlsli"

[earlydepthstencil]
float4 main(PixelInput input) : SV_TARGET
{
	float4 color = 0;
	[branch]
	if (GetMaterial().uvset_baseColorMap >= 0)
	{
		const float2 UV_baseColorMap = GetMaterial().uvset_baseColorMap == 0 ? input.uvsets.xy : input.uvsets.zw;
		color = texture_basecolormap.Sample(sampler_objectshader, UV_baseColorMap);
	}
	else
	{
		color = 1;
	}
	color *= input.color;

	// early clip should speed up performance when using transparent shadows
	clip(color.a - GetMaterial().alphaTest);

	// replace 'stain glass transparent shadow effect' with 'basic greyscale occlusion' (like it used to be a while ago)
	//float opacity = color.a; color.rgb *= 1 - opacity;
	float opacity = 1 - color.a;
	color.rgb = float3(opacity, opacity, opacity);

	// returns secondary depth in alpha after working out color RGB
	color.a = input.pos.z; // secondary depth
	return color;

	//#ifdef GGREDUCED no support for transmissions - transparent shadows now work properly :)
	/*
	float4 color;
	[branch]
	if (GetMaterial().uvset_baseColorMap >= 0)
	{
		const float2 UV_baseColorMap = GetMaterial().uvset_baseColorMap == 0 ? input.uvsets.xy : input.uvsets.zw;
		color = texture_basecolormap.Sample(sampler_objectshader, UV_baseColorMap);
		color.rgb = DEGAMMA(color.rgb);
	}
	else
	{
		color = 1;
	}
	color *= input.color;
	float opacity = color.a;
	[branch]
	if (GetMaterial().transmission > 0)
	{
		float transmission = GetMaterial().transmission;
		[branch]
		if (GetMaterial().uvset_transmissionMap >= 0)
		{
			const float2 UV_transmissionMap = GetMaterial().uvset_transmissionMap == 0 ? input.uvsets.xy : input.uvsets.zw;
			float transmissionMap = texture_transmissionmap.Sample(sampler_objectshader, UV_transmissionMap).r;
			transmission *= transmissionMap;
		}
		opacity *= 1 - transmission;
	}
	color.rgb *= 1 - opacity; // if fully opaque, then black (not let through any light)
	color.a = input.pos.z; // secondary depth
	*/
	//#endif
}
