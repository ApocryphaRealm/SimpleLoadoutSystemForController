#include "Settings.h"

#include "utils/Logger.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <format>

namespace settings
{
	namespace
	{
		Values g_values;
		std::string g_path;
	}

	void Init(const std::string& a_iniName)
	{
		g_path = "Data/SKSE/Plugins/" + a_iniName;
		const std::string full = std::filesystem::absolute(g_path).string();
		g_values.iniFound = std::filesystem::exists(full);

		g_values.count = std::clamp(static_cast<int>(GetPrivateProfileIntA("General", "iLoadoutCount", 5, full.c_str())), 1, kMaxLoadouts);
		g_values.names.clear();
		for (int i = 1; i <= kMaxLoadouts; ++i) {
			char buf[128]{};
			GetPrivateProfileStringA("General", std::format("sLoadoutName{}", i).c_str(), "", buf, sizeof(buf), full.c_str());
			std::string name = buf;
			if (name.empty()) { name = std::format("Loadout {}", i); }
			g_values.names.push_back(std::move(name));
		}
		g_values.logLevel = std::clamp(static_cast<int>(GetPrivateProfileIntA("Debug", "uLogLevel", 2, full.c_str())), 0, 6);

		logger::info("settings: {} ({}), {} loadouts, log level {}", g_path, g_values.iniFound ? "found" : "not found - defaults",
					 g_values.count, g_values.logLevel);
	}

	const Values& Get() { return g_values; }

	std::string IniPath() { return g_path; }
}
