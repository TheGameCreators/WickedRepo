#define OBJECTSHADER_LAYOUT_COMMON
#include "objectHF.hlsli"

[earlydepthstencil]
float4 main(PixelInput input) : SV_TARGET
{
	float4 color;
	[branch]
	if (GetMaterial().uvset_baseColorMap >= 0 && (g_xFrame_Options & OPTION_BIT_DISABLE_ALBEDO_MAPS) == 0)
	{
		const float2 UV_baseColorMap = GetMaterial().uvset_baseColorMap == 0 ? input.uvsets.xy : input.uvsets.zw;
		color = texture_basecolormap.Sample(sampler_objectshader, UV_baseColorMap);
		color.rgb = DEGAMMA(color.rgb);
		color.rgb = max(color.r, max(color.g, color.b));
	}
	else
	{
		color = 1;
	}
	color *= input.color;

	float4 emissiveColor = GetMaterial().emissiveColor;
	[branch]
	if (emissiveColor.a > 0 && GetMaterial().uvset_emissiveMap >= 0)
	{
		const float2 UV_emissiveMap = GetMaterial().uvset_emissiveMap == 0 ? input.uvsets.xy : input.uvsets.zw;
		float4 emissiveMap = texture_emissivemap.Sample(sampler_objectshader, UV_emissiveMap);
		emissiveMap.rgb = DEGAMMA(emissiveMap.rgb);
		emissiveColor *= emissiveMap;
	}
	color.rgb += emissiveColor.rgb * emissiveColor.a;

	float time = g_xFrame_Time;
	float2 uv = input.pos.xy * g_xFrame_InternalResolution_rcp;
	uv.y *= g_xFrame_InternalResolution.y * g_xFrame_InternalResolution_rcp.x;

	// wave:
	//color.a *= sin(input.pos3D.y * 30 + time * 10) * 0.5 + 0.5;
	color.a *= sin(input.pos3D.y * 6 + time * 8) * 0.5 + 0.5;

	// rim:
	color *= lerp(0.3, 6, pow(1 - saturate(dot(normalize(input.nor), normalize(g_xCamera_CamPos - input.pos3D))), 2));

	// keep some base color
	color.a += 0.2;
	color.a = saturate(color.a);

	// grain:
	float noise = 1;
	noise *= texture_random64x64.SampleLevel(sampler_linear_wrap, uv * 4 + time, 0).r * 2 - 1;
	noise *= texture_random64x64.SampleLevel(sampler_linear_wrap, uv * float2(-2, 3) + time, 0).g * 2 - 1;
	noise = noise * 0.5 + 0.5;

	color *= noise;

	return color;
}
