#include "globals.hlsli"
#include "ShaderInterop_Postprocess.h"

#define UNDERWATERWAVE
#define PP_RADIUS .03

#ifdef BINDLESS
PUSHCONSTANT(push, PushConstantsTonemap);
Texture2D<float4> bindless_textures[] : register(t0, space1);
Texture3D<float4> bindless_textures3D[] : register(t0, space2);
RWTexture2D<float4> bindless_rwtextures[] : register(u0, space3);
#else
TEXTURE2D(input, float4, TEXSLOT_ONDEMAND0);
TEXTURE2D(input_luminance, float, TEXSLOT_ONDEMAND1);
TEXTURE2D(input_distortion, float4, TEXSLOT_ONDEMAND2);
TEXTURE3D(colorgrade_lookuptable, float4, TEXSLOT_ONDEMAND3);

RWTEXTURE2D(output, unorm float4, 0);
#endif // BINDLESS


// https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl
// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
static const float3x3 ACESInputMat =
{
	{0.59719, 0.35458, 0.04823},
	{0.07600, 0.90834, 0.01566},
	{0.02840, 0.13383, 0.83777}
};

// ODT_SAT => XYZ => D60_2_D65 => sRGB
static const float3x3 ACESOutputMat =
{
	{ 1.60475, -0.53108, -0.07367},
	{-0.10208,  1.10813, -0.00605},
	{-0.00327, -0.07276,  1.07602}
};

float3 RRTAndODTFit(float3 v)
{
	
	// original
	float3 a = v * (v + 0.0245786f) - 0.000090537f;
	float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
	return a / b;
	
	/*
	// modified
	float3 a = v * (v + 0.0245786f) - 0.000090537f;
	float3 b = v * (0.983729f * v + 0.8329510f) + 0.238081f;
	return a / b;
	*/
	/*
	// Uncharted 2 tonemapping
	float3 a = v * (0.22*v + 0.03);
	float3 b = v * (0.22*v + 0.3) + 0.165;
	a = a / b;
	//return a / 0.9878; // white point = 100
	//return a / 0.9758; // white point = 50
	return a / 0.9409; // white point = 20
	*/
}

float3 ACESFitted(float3 color)
{
	//color = mul(ACESInputMat, color);

	// Apply RRT and ODT
	color = RRTAndODTFit(color);

	//color = mul(ACESOutputMat, color);

	// Clamp to [0, 1]
	color = saturate(color);

	return color;
}

float distanceRayPoint(float3 ro, float3 rd, float3 p, out float h)
{
    h = dot(p - ro, rd);
    return length(p - ro - rd * h);
}

float3 hashSnow(const in float3 p)
{
    return frac(float3(
        sin(dot(p, float3(127.1, 311.7, 758.5453123))),
        cos(dot(p.zyx, float3(127.1, 311.7, 758.5453123))),
        sin(dot(p.yxz, float3(127.1, 311.7, 758.5453123)))) * 43758.5453123);
}

float3 renderSnow2(in float3 ro, in float3 rd, float depth)
{
    const float step_size = 3;
    const float fragDepth = depth;
    float3 ros = ro / step_size;
    float3 offset, id, mm;
    float3 pos = floor(ros);
    const float3 ri = 1. / rd;
    const float3 rs = sign(rd);
	float3 dis = (pos - ros + .5 + rs * .5) * ri;
    
    float distance_from_cam = 0.1;
    float d = 0.0f;
    float3 col = float4(0, 0, 0, 0);
    float3 sum = float4(0, 0, 0, 0);
    const float edge = 0.0525 * g_xFrame_WindWaveSize;

    for (int i = 0; i < g_xFrame_Voxel_Steps; i++)
    {
        id = hashSnow(pos);
        offset = clamp(id + .4 * cos(id + (id.x) * (g_xFrame_Time * 2.0 * g_xFrame_WindSpeed)) * g_xFrame_WindRandomness, PP_RADIUS, 1.0 - PP_RADIUS);
        d = distanceRayPoint(ros, rd, pos + offset, distance_from_cam);
        
        if (distance_from_cam > 0.3 && (distance_from_cam / 7500) < fragDepth)
        {
            //if (d < edge * 0.5) d = 1; //PE: Boubble for underwater.
            col.rgb = smoothstep(edge, -edge, d);
            float distFade = (1.0 - (0.025 * distance_from_cam)); //PE: Dade by distance
            distFade += sin(id + (id.y) * (g_xFrame_Time * 4.0 * g_xFrame_WindSpeed)) * 0.5; //PE: Sparkling
            distFade *= saturate(distance_from_cam * 0.5); //PE: Fade out close to camera.
            sum += (col * distFade);
        }

        mm = (step(dis, dis.yxy) * step(dis, dis.zzx));
        dis += mm * rs * ri;
        pos += mm * rs;
    }
    return sum;
}

float3 screenBlending(float3 s, float3 d)
{
    return s + d - s * d;
}
// From https://www.shadertoy.com/view/lsSXzD, modified
float3 GetCameraRayDir(float2 vWindow, float3 vCameraDir, float fov)
{
    float3 vForward = normalize(vCameraDir);
    float3 vRight = normalize(cross(float3(0.0, 1.0, 0.0), vForward));
    float3 vUp = normalize(cross(vForward, vRight));
    float3 vDir = normalize(vWindow.x * vRight + vWindow.y * vUp + vForward * fov);
    return vDir;
}

[numthreads(POSTPROCESS_BLOCKSIZE, POSTPROCESS_BLOCKSIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
#ifdef BINDLESS
	const float2 uv = (DTid.xy + 0.5f) * push.xPPResolution_rcp;
	float exposure = push.exposure;
	const bool is_dither = push.dither != 0;
	const bool is_colorgrading = push.texture_colorgrade_lookuptable >= 0;
	const bool is_eyeadaption = push.texture_input_luminance >= 0;
	const bool is_distortion = push.texture_input_distortion >= 0;

	float2 distortion = 0;
	[branch]
	if (is_distortion)
	{
		distortion = bindless_textures[push.texture_input_distortion].SampleLevel(sampler_linear_clamp, uv, 0).rg;
	}

	float4 hdr = bindless_textures[push.texture_input].SampleLevel(sampler_linear_clamp, uv + distortion, 0);

	[branch]
	if (is_eyeadaption)
	{
		exposure *= push.eyeadaptionkey / max(bindless_textures[push.texture_input_luminance][uint2(0, 0)].r, 0.0001);
	}
    const float center_depth = 0.003;

#else
	
	float2 uv = (DTid.xy + 0.5f) * xPPResolution_rcp;
	float exposure = tonemap_exposure;
	const bool is_dither = tonemap_dither != 0;
	const bool is_colorgrading = tonemap_colorgrading != 0;
	const bool is_eyeadaption = tonemap_eyeadaption != 0;
	const bool is_distortion = tonemap_distortion != 0;
	
#endif

	#ifdef UNDERWATERWAVE
    if (( g_xFrame_Options & OPTION_BIT_WATER_ENABLED ) && g_xCamera_CamPos.y < g_xFrame_WaterHeight)
	{
		//PE: Wave underwater.
		float freq = 350.0;
		float freqY = 110.0;
		float freqX = 50.0;
		float amplitudeX = 0.00069;
		float amplitudeY = 0.000359;
		float speed = 4.0;
		float waveX = amplitudeX * sin(uv.y * freqY + g_xFrame_Time * speed);
		float waveY = amplitudeY * cos(uv.x * freqX + g_xFrame_Time * speed);
		uv.x += waveX;
		uv.y += waveY;
		uv = saturate(uv);
	}
	#endif
	
	float2 distortion = 0;
	[branch]
	if (is_distortion)
	{
		distortion = input_distortion.SampleLevel(sampler_linear_clamp, uv, 0).rg;
	}

	float4 hdr = input.SampleLevel(sampler_linear_clamp, uv + distortion, 0);

	//PE: Postprocessing effects.
	if ((g_xFrame_Options & OPTION_BIT_SNOW_ENABLED) && g_xCamera_CamPos.y > g_xFrame_WaterHeight)
	{
		const float3 camPos = float3(g_xCamera_CamPos.x, -g_xCamera_CamPos.y, g_xCamera_CamPos.z);
		const float3 camDir = g_xCamera_At;
		const float SnowAlpha = g_xFrame_PP_Alpha;
		const float windForce = 2.0f * g_xFrame_WindSpeed;
		const float3 windDirection = float3(g_xFrame_WindDirection.x * 25.0f, g_xFrame_WindDirection.y * 25.0f, g_xFrame_WindDirection.z * 25.0f) * windForce;
     
		// camera
		float2 p = -1.0 + (2.0 * uv);
		p.x *= ((float) xPPResolution.x / (float) xPPResolution.y);
        
		float angleY = atan2(camDir.x, camDir.z); //radians
		const float angleX = (atan(-camDir.y));
		const float center_depth = texture_lineardepth[DTid.xy];
		const float depth = saturate(center_depth * 2.0f);

		const float3 pos = ((camPos + (windDirection * g_xFrame_Time * 0.1)) * 0.1);
		const float3 ta = float3(cos(angleY - 0.3), sin(angleX), sin(angleY - 0.3));
		const float3 ro = float3(pos.x + ta.x, (pos.y + ta.y), pos.z + ta.z);
		const float fov = 1.75;
		const float3 rd = GetCameraRayDir(p, float3(camDir.x, -camDir.y, camDir.z), fov);
        
		float3 snow = renderSnow2(ro, rd, depth.r);
		hdr.rgb = screenBlending(hdr.rgb, saturate(snow.rgb * SnowAlpha));
	}
    
	[branch]
        if (is_eyeadaption)
        {
            exposure *= tonemap_eyeadaptionkey / max(input_luminance[uint2(0, 0)].r, 0.0001);
        }

	hdr.rgb *= exposure;

	float4 ldr = float4(ACESFitted(hdr.rgb), hdr.a);

#if 0 // DEBUG luminance
	if(DTid.x<800)
		ldr = average_luminance;
#endif

	ldr = saturate(ldr);
	
	ldr.rgb = GAMMA(ldr.rgb);

	float luminance = dot(ldr.rgb, float3(0.299, 0.587, 0.114)); //PE: Luminance Weights.
	ldr.rgb = lerp(luminance, ldr.rgb, g_xFrame_DeSaturate);
	
	if (is_colorgrading)
	{
#ifdef BINDLESS
		ldr.rgb = bindless_textures3D[push.texture_colorgrade_lookuptable].SampleLevel(sampler_linear_clamp, ldr.rgb, 0).rgb;
#else
		ldr.rgb = colorgrade_lookuptable.SampleLevel(sampler_linear_clamp, ldr.rgb, 0).rgb;
#endif // BINDLESS
	}

	if (is_dither)
	{
		// dithering before outputting to SDR will reduce color banding:
		ldr.rgb += (dither((float2)DTid.xy) - 0.5f) / 64.0f;
	}

#ifdef BINDLESS
	bindless_rwtextures[push.texture_output][DTid.xy] = ldr;
#else
	output[DTid.xy] = ldr;
#endif // BINDLESS
}
