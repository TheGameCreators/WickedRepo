#pragma once
#include "wiGraphicsDevice.h"
#include "wiCanvas.h"

namespace wiProfiler
{
	typedef size_t range_id;

	// Begin collecting profiling data for the current frame
	void BeginFrame();

	// Finalize collecting profiling data for the current frame
	void EndFrame(wiGraphics::CommandList cmd);

	// Start a CPU profiling range
	range_id BeginRangeCPU(const char* name);

	// Start a GPU profiling range
	range_id BeginRangeGPU(const char* name, wiGraphics::CommandList cmd);

	// End a profiling range
	void EndRange(range_id id);

	// Renders a basic text of the Profiling results to the (x,y) screen coordinate
	void DrawData(const wiCanvas& canvas, float x, float y, wiGraphics::CommandList cmd);

#ifdef GGREDUCED
	std::string GetProfilerData(void);
	std::string GetProfilerDataFilter(char* filter);

	void CountDrawCalls(void);
	void CountDrawCallsShadows(void);
	void CountDrawCallsShadowsCube(void);
	void CountDrawCallsTransparent(void);

	int GetDrawCalls(void);
	int GetDrawCallsShadows(void);
	int GetDrawCallsShadowsCube(void);
	int GetDrawCallsTransparent(void);

	void CountPolygons(int iPoly);
	void CountPolygonsShadows(int iPoly);
	void CountPolygonsTransparent(int iPoly);
	
	int GetPolygons(void);
	int GetPolygonsShadows(void);
	int GetPolygonsTransparent(void);

	int GetFrustumCulled(void);
	void SetFrustumCulled(int iFrustum);

	void ResetPeek (void);

#endif

	// Enable/disable profiling
	void SetEnabled(bool value);

	bool IsEnabled();
};

