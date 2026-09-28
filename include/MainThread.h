#pragma once

#include <functional>
#include <string>

// Answer a question on the game's main thread: DevBench calls arrive on its own thread, and inventories and movies may
// only be read on the main one. The input dispatch hook runs Service() every frame.
namespace mainthread
{
	// Empty when nothing answered within a_timeoutMs (no game frame ran - loading, or the game minimised).
	std::string Run(std::function<std::string()> a_job, int a_timeoutMs);
	void Service();
}
