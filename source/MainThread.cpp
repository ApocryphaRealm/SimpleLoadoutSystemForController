#include "MainThread.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace mainthread
{
	namespace
	{
		std::mutex g_lock;
		std::condition_variable g_cv;
		std::function<std::string()> g_job;
		std::string g_result;
		bool g_done = false;
		std::mutex g_callers;   // one question at a time
	}

	std::string Run(std::function<std::string()> a_job, int a_timeoutMs)
	{
		std::scoped_lock one(g_callers);
		std::unique_lock l(g_lock);
		g_job = std::move(a_job);
		g_done = false;
		if (!g_cv.wait_for(l, std::chrono::milliseconds(a_timeoutMs), [] { return g_done; })) {
			g_job = nullptr;
			return {};
		}
		return std::move(g_result);
	}

	void Service()
	{
		std::function<std::string()> job;
		{
			std::scoped_lock l(g_lock);
			if (!g_job) { return; }
			job = std::exchange(g_job, nullptr);
		}
		std::string result = job();
		{
			std::scoped_lock l(g_lock);
			g_result = std::move(result);
			g_done = true;
		}
		g_cv.notify_all();
	}
}
