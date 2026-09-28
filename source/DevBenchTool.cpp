#include "DevBenchTool.h"

#include "Bar.h"
#include "DevBench/DevBenchAPI.h"
#include "SelfCheck.h"
#include "utils/Logger.h"

#include <format>
#include <string>

// "loadouts": observe the bar. Presses are made through the player's own input path (keyboard or the virtual
// controller), never through this tool, so a proof is a proof of what a player does (the owner, 2026-09-18).
namespace DevBenchTool
{
	namespace
	{
		std::string Escape(const std::string& a_s)
		{
			std::string out;
			for (char c : a_s) {
				if (c == '"' || c == '\\') { out += '\\'; }
				out += c;
			}
			return out;
		}

		// "path":"..." from the args (no JSON parser dependency).
		std::string PathArg(const char* a_json)
		{
			const std::string j = a_json ? a_json : "";
			const auto key = j.find("\"path\"");
			if (key == std::string::npos) { return {}; }
			const auto open = j.find('"', j.find(':', key) + 1);
			const auto close = j.find('"', open + 1);
			return open == std::string::npos || close == std::string::npos ? std::string{} : j.substr(open + 1, close - open - 1);
		}

		void Tool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			if (const std::string path = PathArg(a_argsJson); !path.empty()) {
				const std::string r = bar::Inspect(path, 2000);
				a_write(a_sink, r.empty() ? R"({"ok":false,"error":"no answer within 2 s - is the inventory open?"})" : (R"({"ok":true,"op":"inspect",)" + r.substr(1)).c_str());
				return;
			}
			const auto s = bar::GetSnapshot();
			const std::string json = std::format(
				R"({{"ok":true,"op":"state","inventoryOpen":{},"built":{},"focused":{},"cursor":{},"active":{},"count":{},"listIndex":{},"builtCount":{},"lastDecision":"{}"}})",
				s.inventoryOpen, s.built, s.focused, s.cursor, s.active, s.count, s.listIndex, s.builtCount, Escape(s.lastDecision));
			a_write(a_sink, json.c_str());
		}
	}

	void Init(bool a_lastAttempt)
	{
		static bool registered = false;
		if (registered) { return; }
		auto* devBench = DevBenchAPI::GetDevBenchInterface001();
		if (!devBench) {
			if (a_lastAttempt) {
				logger::info("DevBench not detected; skipping the \"loadouts\" tool");
				SelfCheck::Set("DevBench tool", true, "DevBench not installed - no tool (not needed to play)");
			}
			return;
		}
		constexpr const char* descriptor =
			"{\"description\":\"Simple Loadout System for Controller' bar: whether the inventory is open and the bar drawn, focus, the highlighted "
			"button (cursor), the active loadout (-1 none), SkyUI's item-list row, and the last input decision.\","
			"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\"}}},\"readOnly\":true}";
		if (devBench->RegisterTool("loadouts", descriptor, &Tool, nullptr)) {
			registered = true;
			logger::info("Registered \"loadouts\" with DevBench (build {})", devBench->GetBuildNumber());
			SelfCheck::Set("DevBench tool", true, "\"loadouts\" registered");
		}
	}
}
