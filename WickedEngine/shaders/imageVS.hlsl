#include "globals.hlsli"
#include "imageHF.hlsli"

VertextoPixel main(uint vI : SV_VERTEXID)
{
	VertextoPixel Out;

	// This vertex shader generates a trianglestrip like this:
	//	1--2
	//	  /
	//	 /
	//	3--4

	//Out.pos = xCorners[vI]; //PE: Fix AMD issue - cant index dynamically (Black Screen).
	switch (vI)
	{
	default:
	case 0:
		Out.pos = corners0;
		break;
	case 1:
		Out.pos = corners1;
		break;
	case 2:
		Out.pos = corners2;
		break;
	case 3:
		Out.pos = corners3;
		break;
	}

	Out.uv_screen = Out.pos;

	// Set up inverse bilinear interpolation
	//Out.q = Out.pos.xy - xCorners[0].xy;
	//Out.b1 = xCorners[1].xy - xCorners[0].xy;
	//Out.b2 = xCorners[2].xy - xCorners[0].xy;
	//Out.b3 = xCorners[0].xy - xCorners[1].xy - xCorners[2].xy + xCorners[3].xy;

	//PE: Fix AMD issue - cant index dynamically (Black Screen).
	Out.q = Out.pos.xy - corners0.xy;
	Out.b1 = corners1.xy - corners0.xy;
	Out.b2 = corners2.xy - corners0.xy;
	Out.b3 = corners0.xy - corners1.xy - corners2.xy + corners3.xy;

	return Out;
}

