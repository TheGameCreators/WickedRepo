#include "globals.hlsli"
#include "ShaderInterop_EmittedParticle.h"
#include "emittedparticleHF.hlsli"

#define PIHALF 1.5707963265f
static const float3 BILLBOARD[] = {
	float3(-1, -1, 0),	// 0
	float3(1, -1, 0),	// 1
	float3(-1, 1, 0),	// 2
	float3(1, 1, 0),	// 4
};

STRUCTUREDBUFFER(particleBuffer, Particle, TEXSLOT_ONDEMAND21);
STRUCTUREDBUFFER(aliveList, uint, TEXSLOT_ONDEMAND22);

VertextoPixel main(uint vertexID : SV_VERTEXID, uint instanceID : SV_INSTANCEID)
{
	VertextoPixel Out;

	// load particle data:
	Particle particle = particleBuffer[aliveList[instanceID]];

	// calculate render properties from life:
	float lifeLerp = 1 - particle.life / particle.maxLife;
	float size = lerp(particle.sizeBeginEnd.x, particle.sizeBeginEnd.y, lifeLerp);
	float opacity = saturate(lerp(1, 0, lifeLerp) * xEmitterOpacity);
	float rotation = lifeLerp * particle.rotationalVelocity;

	// expand the point into a billboard in view space:
	float3 quadPos = BILLBOARD[vertexID];
	quadPos.x = particle.color_mirror & 0x10000000 ? -quadPos.x : quadPos.x;
	quadPos.y = particle.color_mirror & 0x20000000 ? -quadPos.y : quadPos.y;
	float2 uv = quadPos.xy * float2(0.5f, -0.5f) + 0.5f;
	float2 uv2 = uv;

	// Sprite sheet UV transform:
	//PE: Changed to always move flibook forward.
	const float spriteframe = xEmitterFrameRate == 0 ? 
		lerp(xEmitterFrameStart, xEmitterFrameCount, lifeLerp) : 
		((xEmitterFrameStart + (particle.maxLife - particle.life) * xEmitterFrameRate) % xEmitterFrameCount);
	const uint currentFrame = floor(spriteframe);
	const uint nextFrame = ceil(spriteframe);
	const float frameBlend = frac(spriteframe);
	uint2 offset = uint2(currentFrame % xEmitterFramesXY.x, currentFrame / xEmitterFramesXY.x);
	uv.xy += offset;
	uv.xy *= xEmitterTexMul;
	uint2 offset2 = uint2(nextFrame % xEmitterFramesXY.x, nextFrame / xEmitterFramesXY.x);
	uv2.xy += offset2;
	uv2.xy *= xEmitterTexMul;

	//PE: Rotate to fit direction if top up view.
    float3 velocity = mul((float3x3) g_xCamera_View, particle.velocity);
    
    if (particle.velocity.x != 0 || particle.velocity.z != 0)
    {
        rotation += (tan(normalize(velocity)) * xParticleStartRotation);
    }
	
	// rotate the billboard:
	float2x2 rot = float2x2(
		cos(rotation), -sin(rotation),
		sin(rotation), cos(rotation)
		);
	quadPos.xy = mul(quadPos.xy, rot);

	// scale the billboard:
	quadPos *= size;

	// scale the billboard along view space motion vector:

	quadPos += dot(quadPos, velocity) * velocity * xParticleMotionBlurAmount;


	// copy to output:
	Out.pos = float4(particle.position, 1);
	Out.pos = mul(g_xCamera_View, Out.pos);
	Out.pos.xyz += quadPos.xyz;
	Out.P = mul(g_xCamera_InvV, float4(Out.pos.xyz, 1)).xyz;
	Out.pos = mul(g_xCamera_Proj, Out.pos);

	Out.tex = float4(uv, uv2);
	Out.size = size;

	
	//PE: Fade in particle.
	float normalizedTime = clamp(lifeLerp / xEmitterFadeinTime, 0, 1);
	opacity = saturate(lerp(0, opacity, normalizedTime));

	//PE: Endcolor
    uint red = particle.color_mirror   & 0x000000FF;
    uint green = (particle.color_mirror & 0x0000FF00) >> 8;
    uint blue = (particle.color_mirror & 0x00FF0000) >> 16;
	
    uint endcolor_red = 255;
    uint endcolor_green = 255;
    uint endcolor_blue = 255;

    red = lerp(red, xParticleEndColorRed, lifeLerp);
    green = lerp(green, xParticleEndColorGreen, lifeLerp);
    blue = lerp(blue, xParticleEndColorBlue, lifeLerp);

	//Out.color = (particle.color_mirror & 0x00FFFFFF) | (uint(opacity * 255.0f) << 24);
    Out.color = (red + (green << 8) + (blue << 16)) | (uint(opacity * 255.0f) << 24);

	Out.unrotated_uv = quadPos.xy * float2(1, -1) / size * 0.5f + 0.5f;
	Out.frameBlend = frameBlend;

	return Out;
}
