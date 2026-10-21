#pragma once

#include <string>
#include <vector>

// SimpleLoadoutSystemForController.ini - the only settings surface (the owner, 2026-09-27: no in-game menu for it). Read once at load.
namespace settings
{
	inline constexpr int kMaxLoadouts = 10;

	struct Values
	{
		int count = 10;                       // [General] iLoadoutCount, 1-10
		std::vector<std::string> names;       // [General] sLoadoutName1..10, "Loadout N" when empty
		bool allowSwitchInCombat = false;     // [General] bAllowSwitchInCombat, 0 = no switching in combat (the shipped INI says 0)
		int logLevel = 2;                     // [Debug] uLogLevel (spdlog levels, 2 = info)
		bool iniFound = false;
	};

	void Init(const std::string& a_iniName);
	const Values& Get();
	std::string IniPath();
}
