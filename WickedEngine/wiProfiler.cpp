#include "wiProfiler.h"
#include "wiGraphicsDevice.h"
#include "wiRenderer.h"
#include "wiFont.h"
#include "wiImage.h"
#include "wiTimer.h"
#include "wiTextureHelper.h"
#include "wiHelper.h"

#include <string>
#include <unordered_map>
#include <stack>
#include <mutex>
#include <atomic>
#include <sstream>

using namespace wiGraphics;

namespace wiProfiler
{
	bool ENABLED = false;
	bool initialized = false;
#ifdef GGREDUCED
	int iDrawCalls = 0, iOldDrawCalls = 0;
	int iDrawCallsShadows = 0, iOldDrawCallsShadows = 0;
	int iDrawCallsTransparent = 0, iOldDrawCallsTransparent = 0;

	int iPolygonsDrawn = 0, iOldPolygonsDrawn = 0;
	int iPolygonsDrawnShadows = 0, iOldPolygonsDrawnShadows = 0;
	int iPolygonsDrawnTransparent = 0, iOldPolygonsDrawnTransparent = 0;
#endif
	std::mutex lock;
	range_id cpu_frame;
	range_id gpu_frame;
	GPUQueryHeap queryHeap[wiGraphics::GraphicsDevice::GetBufferCount() + 1];
	std::vector<uint64_t> queryResults;
	std::atomic<uint32_t> nextQuery{ 0 };
	uint32_t writtenQueries[arraysize(queryHeap)] = {};
	int queryheap_idx = 0;

	struct Range
	{
		bool in_use = false;
		std::string name;
		float times[20] = {};
		int avg_counter = 0;
		float time = 0;
		CommandList cmd = COMMANDLIST_COUNT;

		wiTimer cpuBegin, cpuEnd;

		int gpuBegin[arraysize(queryHeap)];
		int gpuEnd[arraysize(queryHeap)];

		bool IsCPURange() const { return cmd == COMMANDLIST_COUNT; }
	};
	std::unordered_map<size_t, Range> ranges;
	std::vector<range_id> rangeOrder[COMMANDLIST_COUNT + 1];

	void BeginFrame()
	{
		if (!ENABLED)
			return;

		if (!initialized)
		{
			initialized = true;

			ranges.reserve(100);
			
			GPUQueryHeapDesc desc;
			desc.type = GPU_QUERY_TYPE_TIMESTAMP;
			desc.queryCount = 1024;
			for (int i = 0; i < arraysize(queryHeap); ++i)
			{
				bool success = wiRenderer::GetDevice()->CreateQueryHeap(&desc, &queryHeap[i]);
				assert(success);
			}

			queryResults.resize(desc.queryCount);
		}

		cpu_frame = BeginRangeCPU("CPU Frame");

		CommandList cmd = wiRenderer::GetDevice()->BeginCommandList();
		gpu_frame = BeginRangeGPU("GPU Frame", cmd);

#ifdef GGREDUCED
		iOldDrawCalls = iDrawCalls; 
		iOldDrawCallsShadows = iDrawCallsShadows;
		iOldDrawCallsTransparent = iDrawCallsTransparent;
		iDrawCalls = 0;
		iDrawCallsShadows = 0;
		iDrawCallsTransparent = 0;

		iOldPolygonsDrawn = iPolygonsDrawn;
		iOldPolygonsDrawnShadows = iPolygonsDrawnShadows;
		iOldPolygonsDrawnTransparent = iPolygonsDrawnTransparent;
		iPolygonsDrawn = 0;
		iPolygonsDrawnShadows = 0;
		iPolygonsDrawnTransparent = 0;
#endif
	}
	void EndFrame(CommandList cmd)
	{
		if (!ENABLED || !initialized)
			return;

		GraphicsDevice* device = wiRenderer::GetDevice();

		// note: read the GPU Frame end range manually because it will be on a separate command list than start point: 
		auto& gpu_range = ranges[gpu_frame];
		gpu_range.gpuEnd[queryheap_idx] = nextQuery.fetch_add(1);
		device->QueryEnd(&queryHeap[queryheap_idx], gpu_range.gpuEnd[queryheap_idx], cmd);

		EndRange(cpu_frame);

		double gpu_frequency = (double)device->GetTimestampFrequency() / 1000.0;

		device->QueryResolve(&queryHeap[queryheap_idx], 0, nextQuery.load(), cmd);

		writtenQueries[queryheap_idx] = nextQuery.load();
		nextQuery.store(0);
		queryheap_idx = (queryheap_idx + 1) % arraysize(queryHeap);
		if (writtenQueries[queryheap_idx] > 0)
		{
			wiRenderer::GetDevice()->QueryRead(&queryHeap[queryheap_idx], 0, writtenQueries[queryheap_idx], queryResults.data());
		}

		for (auto& x : ranges)
		{
			auto& range = x.second;

			range.time = 0;
			if (range.IsCPURange())
			{
				range.time = (float)abs(range.cpuEnd.elapsed() - range.cpuBegin.elapsed());
			}
			else
			{
				int begin_query = range.gpuBegin[queryheap_idx];
				int end_query = range.gpuEnd[queryheap_idx];
				if (begin_query >= 0 && end_query >= 0)
				{
					uint64_t begin_result = queryResults[begin_query];
					uint64_t end_result = queryResults[end_query];
					range.time = (float)abs((double)(end_result - begin_result) / gpu_frequency);
				}
				range.gpuBegin[queryheap_idx] = -1;
				range.gpuEnd[queryheap_idx] = -1;
			}
			range.times[range.avg_counter++ % arraysize(range.times)] = range.time;

			if (range.avg_counter > arraysize(range.times))
			{
				float avg_time = 0;
				for (int i = 0; i < arraysize(range.times); ++i)
				{
					avg_time += range.times[i];
				}
				range.time = avg_time / arraysize(range.times);
			}

			range.in_use = false;
		}
	}

	range_id BeginRangeCPU(const char* name)
	{
		if (!ENABLED || !initialized)
			return 0;

		range_id id = wiHelper::string_hash(name);

		lock.lock();

		// If one range name is hit multiple times, differentiate between them!
		size_t differentiator = 0;
		while (ranges[id].in_use)
		{
			wiHelper::hash_combine(id, differentiator++);
		}
		ranges[id].in_use = true;
		if ( ranges[id].name.length() == 0 ) rangeOrder[0].push_back( id );
		ranges[id].name = name;

		ranges[id].cpuBegin.record();

		lock.unlock();

		return id;
	}
	range_id BeginRangeGPU(const char* name, CommandList cmd)
	{
		if (!ENABLED || !initialized)
			return 0;

		range_id id = wiHelper::string_hash(name);

		lock.lock();

		// If one range name is hit multiple times, differentiate between them!
		size_t differentiator = 0;
		while (ranges[id].in_use)
		{
			wiHelper::hash_combine(id, differentiator++);
		}
		ranges[id].in_use = true;
		if ( ranges[id].name.length() == 0 ) rangeOrder[cmd+1].push_back( id );
		ranges[id].name = name;

		ranges[id].cmd = cmd;

		ranges[id].gpuBegin[queryheap_idx] = nextQuery.fetch_add(1);
		wiRenderer::GetDevice()->QueryEnd(&queryHeap[queryheap_idx], ranges[id].gpuBegin[queryheap_idx], cmd);

		lock.unlock();

		return id;
	}
	void EndRange(range_id id)
	{
		if (!ENABLED || !initialized)
			return;

		lock.lock();

		auto it = ranges.find(id);
		if (it != ranges.end())
		{
			if (it->second.IsCPURange())
			{
				it->second.cpuEnd.record();
			}
			else
			{
				ranges[id].gpuEnd[queryheap_idx] = nextQuery.fetch_add(1);
				wiRenderer::GetDevice()->QueryEnd(&queryHeap[queryheap_idx], it->second.gpuEnd[queryheap_idx], it->second.cmd);
			}
		}
		else
		{
			assert(0);
		}

		lock.unlock();
	}

#ifdef GGREDUCED
	void CountDrawCalls(void) { iDrawCalls++; }
	void CountDrawCallsShadows(void) { iDrawCallsShadows++; }
	void CountDrawCallsTransparent(void) { iDrawCallsTransparent++; }

	int GetDrawCalls(void) { return(iOldDrawCalls); }
	int GetDrawCallsShadows(void) { return(iOldDrawCallsShadows); };
	int GetDrawCallsTransparent(void) { return(iOldDrawCallsTransparent); };

	void CountPolygons(int iPoly) { iPolygonsDrawn += iPoly; }
	void CountPolygonsShadows(int iPoly) { iPolygonsDrawnShadows += iPoly; }
	void CountPolygonsTransparent(int iPoly) { iPolygonsDrawnTransparent += iPoly; }

	int GetPolygons(void) { return iOldPolygonsDrawn; }
	int GetPolygonsShadows(void) { return iOldPolygonsDrawnShadows; }
	int GetPolygonsTransparent(void) { return iOldPolygonsDrawnTransparent; }

	//We need the data returned here.
	std::string GetProfilerData(void)
	{
		if (!ENABLED || !initialized)
			return "Profiler not initialized.";

		std::stringstream ss("");
		ss.precision(2);
		//ss << "Profiler:" << std::endl << "----------------------------" << std::endl; //PE: Make more room.

		// Print CPU ranges:
		for (auto& index : rangeOrder[0])
		{
			Range range = ranges[index];
			if (range.IsCPURange())
			{
				ss << range.name << ": " << std::fixed << range.time << " ms" << std::endl;
			}
		}
		ss << std::endl;

		// Print GPU ranges:
		float shadowTerrainTotal = 0;
		float shadowTreesTotal = 0;
		//float shadowPointTotal = 0;
		for( int i = 0; i < COMMANDLIST_COUNT; i++ )
		{
			for (auto& index : rangeOrder[i+1])
			{
				Range range = ranges[index];
				if (!range.IsCPURange())
				{
					if ( range.name.compare( 0, strlen("Shadow Rendering - Terrain"), "Shadow Rendering - Terrain" ) == 0 )
					{
						shadowTerrainTotal += range.time;
					}
					else if ( range.name.compare( 0, strlen("Shadow Rendering - Tree"), "Shadow Rendering - Tree" ) == 0 )
					{
						shadowTreesTotal += range.time;
					}
					//else if (range.name.compare(0, strlen("Shadow Rendering - Point"), "Shadow Rendering - Point") == 0)
					//{
					//	shadowPointTotal += range.time;
					//}
					else
					{
						if ( shadowTerrainTotal > 0 )
						{
							ss << "Shadow Rendering - Terrain" << ": " << std::fixed << shadowTerrainTotal << " ms" << std::endl;
							shadowTerrainTotal = 0;
						}
						if ( shadowTreesTotal > 0 )
						{
							ss << "Shadow Rendering - Trees" << ": " << std::fixed << shadowTreesTotal << " ms" << std::endl;
							shadowTreesTotal = 0;
						}
						//if (shadowPointTotal > 0)
						//{
						//	ss << "Shadow Rendering - Point" << ": " << std::fixed << shadowPointTotal << " ms" << std::endl;
						//	shadowPointTotal = 0;
						//}

						ss << range.name << ": " << std::fixed << range.time << " ms" << std::endl;
					}
				}
			}
		}
		return ss.str();
	}
#endif
	struct Hits
	{
		uint32_t num_hits = 0;
		float total_time = 0;
	};
	std::unordered_map<std::string, Hits> time_cache_cpu;
	std::unordered_map<std::string, Hits> time_cache_gpu;
	void DrawData(const wiCanvas& canvas, float x, float y, CommandList cmd)
	{
#ifdef GGREDUCED
		//PE: Never draw profiler data in GG.
		return;
#endif
		if (!ENABLED || !initialized)
			return;

		wiImage::SetCanvas(canvas, cmd);
		wiFont::SetCanvas(canvas, cmd);

		std::stringstream ss("");
		ss.precision(2);
		ss << "Frame Profiler Ranges:" << std::endl << "----------------------------" << std::endl;


		for (auto& x : ranges)
		{
			if (x.second.IsCPURange())
			{
				time_cache_cpu[x.second.name].num_hits++;
				time_cache_cpu[x.second.name].total_time += x.second.time;
			}
			else
			{
				time_cache_gpu[x.second.name].num_hits++;
				time_cache_gpu[x.second.name].total_time += x.second.time;
		}
		}

		// Print CPU ranges:
		for (auto& x : time_cache_cpu)
		{
			ss << x.first << " (" << x.second.num_hits << "x)" << ": " << std::fixed << x.second.total_time << " ms" << std::endl;
			x.second.num_hits = 0;
			x.second.total_time = 0;
		}
		ss << std::endl;

		// Print GPU ranges:
		for (auto& x : time_cache_gpu)
		{
			ss << x.first << " (" << x.second.num_hits << "x)" << ": " << std::fixed << x.second.total_time << " ms" << std::endl;
			x.second.num_hits = 0;
			x.second.total_time = 0;
			}

		wiFontParams params = wiFontParams(x, y, WIFONTSIZE_DEFAULT - 4, WIFALIGN_LEFT, WIFALIGN_TOP, wiColor(255, 255, 255, 255), wiColor(0, 0, 0, 255));

		wiImageParams fx;
		fx.pos.x = (float)params.posX;
		fx.pos.y = (float)params.posY;
		fx.siz.x = (float)wiFont::textWidth(ss.str(), params);
		fx.siz.y = (float)wiFont::textHeight(ss.str(), params);
		fx.color = wiColor(20, 20, 20, 230);
		wiImage::Draw(wiTextureHelper::getWhite(), fx, cmd);

		wiFont::Draw(ss.str(), params, cmd);
	}

	void SetEnabled(bool value)
	{
		if (value != ENABLED)
		{
			initialized = false;
			ranges.clear();
			for( int i = 0; i < COMMANDLIST_COUNT+1; i++ ) rangeOrder[i].clear();
			ENABLED = value;
		}
	}

	bool IsEnabled()
	{
		return ENABLED;
	}

}
