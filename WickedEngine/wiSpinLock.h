#pragma once
#include <atomic>
#include <emmintrin.h> // _mm_pause()

class wiSpinLock
{
private:
	std::atomic_flag lck = ATOMIC_FLAG_INIT;
public:
	//PE: https://github.com/turanszkij/WickedEngine/commit/54d11f1e9138813b7815e9d523f2e7b8fac523a2
	inline void lock()
	{
		while (!try_lock())
		{
			_mm_pause(); // SMT thread swap can occur here
		}
	}
	inline bool try_lock()
	{
		return !lck.test_and_set(std::memory_order_acquire);
	}

	inline void unlock()
	{
		lck.clear(std::memory_order_release);
	}
	/* old
	void lock()
	{
		while (!try_lock()){}
	}
	bool try_lock()
	{
		return !lck.test_and_set(std::memory_order_acquire);
	}

	void unlock()
	{
		lck.clear(std::memory_order_release);
	}
	*/
};
