#pragma once

// The "loadouts" DevBench tool (rules 31 and 64): read the bar's live state so tests can check what a real press did.
namespace DevBenchTool
{
	void Init(bool a_lastAttempt = false);
}
