#define DISABLE_DECALS
#define DISABLE_ENVMAPS
#define DISABLE_TRANSPARENT_SHADOWMAP
#include "globals.hlsli"
#include "oceanSurfaceHF.hlsli"
#include "objectHF.hlsli"

TEXTURE2D(texture_gradientmap, float4, TEXSLOT_ONDEMAND1);

[earlydepthstencil]
float4 main(PSIn input) : SV_TARGET
{
	float3 V = g_xCamera_CamPos - input.pos3D;
	float dist = length(V);
	V /= dist;
	float emissive = 0;

	float gradient_fade = saturate(dist * 0.001 * 0.0254);
	float2 gradientNear = texture_gradientmap.Sample(sampler_aniso_wrap, input.uv).xy;
	float2 gradientFar = texture_gradientmap.Sample(sampler_aniso_wrap, input.uv * 0.125).xy;
	float2 gradient = lerp(gradientNear, gradientFar, gradient_fade);
	//float2 gradient = lerp( gradientNear, gradientFar, 0.5);

	Surface surface;
	surface.init();
	surface.albedo = xOceanWaterColor.rgb;
	surface.f0 = 0.02;
	surface.roughness = 0.1;
	surface.P = input.pos3D;
	surface.N = normalize(float3(gradient.x, xOceanTexelLength * 2, gradient.y));
	surface.V = V;
	surface.update();

	Lighting lighting;
	float3 ambient = GetAmbient(surface.N) * 0.5;
	lighting.create(0, 0, ambient, 0);

	surface.pixel = input.pos.xy;
	float depth = input.pos.z;

	float3 envAmbient = 0;
	TiledLighting(surface, lighting, envAmbient);
	lighting.indirect.diffuse += envAmbient;

	float2 ScreenCoord = surface.pixel * g_xFrame_InternalResolution_rcp;

	//REFLECTION
	float2 reflectUV = input.ReflectionMapSamplingPos.xy / input.ReflectionMapSamplingPos.w;
	reflectUV = reflectUV * float2(0.5, -0.5) + 0.5f;
	float3 reflection = texture_reflection.SampleLevel(sampler_linear_mirror, reflectUV + surface.N.xz * 0.04f, 0).rgb;
	reflection *= 0.5;
	
	// WATER REFRACTION 
	float lineardepth = input.pos.w;
	float sampled_lineardepth = texture_lineardepth.SampleLevel(sampler_linear_clamp, ScreenCoord.xy + surface.N.xz * 0.04f, 0) * g_xCamera_ZFarP;
	float depth_difference = (sampled_lineardepth - lineardepth) * 0.0254;
	depth_difference = max( 0, depth_difference );
	float3 refraction = texture_refraction.SampleLevel(sampler_linear_mirror, ScreenCoord.xy + surface.N.xz * 0.04f * saturate(0.5 * depth_difference), 0).rgb;

	// recalculate depth for sampled point
	float sampled_depth = texture_lineardepth.SampleLevel(sampler_linear_clamp, ScreenCoord.xy + surface.N.xz * 0.04f * saturate(0.5 * depth_difference), 0);
	sampled_lineardepth = sampled_depth * g_xCamera_ZFarP;
	depth_difference = (sampled_lineardepth - lineardepth) * 0.0254;
	depth_difference = max( 0, depth_difference );

	//float fresnel = 1 - saturate( dot( V, surface.N ) );
	float3 fresnel = surface.F * 0.5;
		
	if ( g_xCamera_CamPos.y < xOceanWaterHeight ) depth_difference = lineardepth * 0.0254;

	// apply water fog
	float3 fogColor = xOceanWaterColor.rgb;
	float fogMin = xOceanFogMin * 0.0254; // meters
	float fogMax = xOceanFogMax * 0.0254; // meters
	float fogScale = -4.0 / (fogMax - fogMin);
	float fogMinAmount = saturate( 1 - xOceanFogMinAmount );

	float3 color;
	if ( g_xCamera_CamPos.y >= xOceanWaterHeight )
	{
		float diff = depth_difference / (dist * 0.0254);
		diff *= (g_xCamera_CamPos.y - xOceanWaterHeight);
		depth_difference += (diff * 0.0254);

		float fade = max( 0, depth_difference - fogMin );
		fade = exp( fade * fogScale );
		fade = min( fogMinAmount, fade );

		fogColor *= (lighting.direct.diffuse + lighting.indirect.diffuse);
		refraction = lerp( fogColor, refraction, fade );

		color = lerp( refraction, reflection, fresnel );
		color += lighting.direct.specular;

		//#ifdef GGREDUCED
		//LB: Restore fog control for interior scenes - arg duplicated all this, should really use common parts of ApplyFog
		const float3 PassedInFogColor = GetFogColor();
		const float PassedInFogOpacity = clamp(GetFogOpacity(), 0.0, 1.0);
		//#endif

		// atmospheric fog
		float3 fogColor2;
		if (g_xFrame_Options & OPTION_BIT_REALISTIC_SKY)
		{
			float3 horizonDir = -V;
			float invLen = rsqrt( horizonDir.x*horizonDir.x + horizonDir.z*horizonDir.z );
			invLen *= 0.995;
			horizonDir.x *= invLen;
			horizonDir.z *= invLen;
			fogColor2 = GetDynamicSkyColor( float3(horizonDir.x, 0.1, horizonDir.z), false, false, false, true );
		}
		else
		{
			float3 viewDir = -V;
			viewDir.y = abs( viewDir.y );
			//PE: interior scenes , skybox looks strange, so disable for now.
			//fogColor2 = texture_globalenvmap.SampleLevel(sampler_linear_clamp, viewDir, 8).rgb;
			fogColor2 = color.rgb;
			//#ifdef GGREDUCED
			//PE: Remove env map by opacity.
			//fogColor2 = lerp(color.rgb, fogColor2, PassedInFogOpacity);
			//#endif
		}

		//#ifdef GGREDUCED
		fogColor2 = lerp(fogColor2, PassedInFogColor, PassedInFogOpacity);
		//#endif

		float fogAmount = max( 0.0, lineardepth - g_xFrame_Fog.x );
		fogAmount = exp( fogAmount * 4.0 / (g_xFrame_Fog.x - g_xFrame_Fog.y) );
		
		color = lerp( fogColor2, color, fogAmount );
	}
	else
	{
		float fade = max( 0, depth_difference - fogMin );
		fade = exp( fade * fogScale );
		fade = min( fogMinAmount, fade );

		// under water
		color = refraction;//lerp( refraction, reflection, fresnel );
		//color += lighting.direct.specular;

		//float fogFade = (xOceanWaterHeight - g_xCamera_CamPos.y) * 0.01;
		//fogFade = 1.0 - saturate( fogFade );
		color = lerp( fogColor, color, fade );
	}

	return float4( color, 1 );
}

