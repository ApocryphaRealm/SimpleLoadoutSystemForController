#pragma once

// Self-check report (the owner, 2026-09-19: "have it log its findings. In a text file"). One named check per install
// point - the Address Library guard, the INI, the input hook, the DevBench tool, the bar - each OK or FAIL with a
// detail, written to Documents\My Games\Skyrim Special Edition\SKSE\SimpleLoadoutSystemForController-selfcheck.txt and rewritten as
// the state changes.

#include <string>

namespace SelfCheck
{
	void Set(const std::string& a_name, bool a_ok, const std::string& a_detail);
	void Write();
}
