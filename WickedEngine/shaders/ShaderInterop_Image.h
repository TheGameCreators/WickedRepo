#ifndef WI_SHADERINTEROP_IMAGE_H
#define WI_SHADERINTEROP_IMAGE_H
#include "ShaderInterop.h"

CBUFFER(ImageCB, CBSLOT_IMAGE)
{
	//float4	xCorners[4]; //PE: Fix AMD issue - cant index dynamically (Black Screen).
	float4 corners0;
	float4 corners1;
	float4 corners2;
	float4 corners3;
	float4	xTexMulAdd;
	float4	xTexMulAdd2;
	float4	xColor;
};

struct PushConstantsImage
{
	int texture_base_index;
	int texture_mask_index;
	int texture_background_index;
	int sampler_index;
};


#endif // WI_SHADERINTEROP_IMAGE_H
