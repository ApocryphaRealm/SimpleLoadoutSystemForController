#include "DevBenchTool.h"
#include "InputHook.h"
#include "Loadouts.h"
#include "SelfCheck.h"
#include "Settings.h"

#include "utils/Logger.h"
#include "utils/AddressLibraryGuard.h"   // uses logger, so after Logger.h
#include "utils/Strings.h"

namespace
{
	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) { return; }
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostPostLoad:
			DevBenchTool::Init();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			strings::Configure("SimpleLoadoutSystemForController");   // Interface\Translations\SimpleLoadoutSystemForController_<language>.txt
			loadouts::Init();
			DevBenchTool::Init(/* a_lastAttempt = */ true);
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	// Workaround for static initialization order bug of CommonLibSSE-NG.
	REL::Module::reset();

	const SKSE::PluginDeclaration* plugin = SKSE::PluginDeclaration::GetSingleton();
	if (!logger::init(plugin->GetName())) {
		return false;
	}
	logger::info("Loading {} {}...", plugin->GetName(), plugin->GetVersion());

	// Address Library pre-check before SKSE::Init, which opens the Address Library itself (logic library 6026).
	if (!AddressLibraryGuard::Guard("Simple Loadout System for Controller")) {
		SelfCheck::Set("Address Library", false, "missing or unreadable - the mod is inert");
		return true;
	}
	SelfCheck::Set("Address Library", true, "found");

	SKSE::Init(a_skse);

	settings::Init(std::string(plugin->GetName()) + ".ini");
	logger::set_level(static_cast<spdlog::level::level_enum>(settings::Get().logLevel), static_cast<spdlog::level::level_enum>(settings::Get().logLevel));
	SelfCheck::Set("INI", true, settings::Get().iniFound ? settings::IniPath() + " read" : settings::IniPath() + " not found - defaults");

	InputHook::Install();
	loadouts::RegisterSerialization();

	if (!SKSE::GetMessagingInterface()->RegisterListener("SKSE", OnSKSEMessage)) {
		logger::error("Could not register the SKSE message listener; the DevBench tool will not appear");
	}
	logger::info("Successfully loaded!");
	return true;
}
