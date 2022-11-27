#pragma once
#include "CommonInclude.h"

#include <chrono>

struct wiTimer
{
	std::chrono::high_resolution_clock::time_point timestamp;

	// cannot include function bodies here as we get "already defined" linker errors

	wiTimer();

	// Record a reference timestamp
	void record();

	// Elapsed time in seconds since the wiTimer creation or last call to record()
	double elapsed_seconds();

	// Elapsed time in milliseconds since the wiTimer creation or last call to record()
	double elapsed_milliseconds();
	
	// Elapsed time in milliseconds since the wiTimer creation or last call to record()
	double elapsed();
};
