#include "SelfCheck.h"

#include "Bar.h"
#include "utils/Logger.h"

#include <chrono>
#include <format>
#include <fstream>
#include <mutex>
#include <vector>

namespace SelfCheck
{
	namespace
	{
		struct Check
		{
			std::string name;
			bool ok;
			std::string detail;
		};
		std::mutex g_lock;
		std::vector<Check> g_checks;
	}

	void Set(const std::string& a_name, bool a_ok, const std::string& a_detail)
	{
		{
			std::scoped_lock l(g_lock);
			bool found = false;
			for (auto& c : g_checks) {
				if (c.name == a_name) {
					c.ok = a_ok;
					c.detail = a_detail;
					found = true;
					break;
				}
			}
			if (!found) { g_checks.push_back({ a_name, a_ok, a_detail }); }
		}
		if (a_ok) { logger::debug("self-check {}: OK - {}", a_name, a_detail); } else { logger::warn("self-check {}: FAIL - {}", a_name, a_detail); }
		Write();
	}

	void Write()
	{
		const auto dir = SKSE::log::log_directory();
		if (!dir) { return; }
		const auto path = *dir / "SimpleLoadoutSystemForController-selfcheck.txt";
		std::vector<Check> checks;
		{
			std::scoped_lock l(g_lock);
			checks = g_checks;
		}
		std::ofstream out(path, std::ios::trunc);
		if (!out) { return; }
		const auto ver = SKSE::PluginDeclaration::GetSingleton()->GetVersion().string(".");
		const auto rt = REL::Module::get().version().string("-");
		out << "Simple Loadout System for Controller " << ver << " - self-check\n";
		out << "Runtime " << rt << "   written "
			<< std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::zoned_time{ std::chrono::current_zone(), std::chrono::system_clock::now() }) << "\n\n";
		std::size_t failed = 0;
		for (const auto& c : checks) {
			out << (c.ok ? "  OK    " : "  FAIL  ") << c.name << " - " << c.detail << "\n";
			if (!c.ok) { ++failed; }
		}
		out << "\n" << (failed == 0 ? "Everything came up." : std::format("{} check(s) failed - see above.", failed)) << "\n";
	}
}
