#include "InputHook.h"

#include "Bar.h"
#include "SelfCheck.h"
#include "utils/Logger.h"

#include <format>
#include <mutex>
#include <vector>

namespace InputHook
{
	namespace
	{
		// BSInputDeviceManager::PollInputDevices -> DispatchInputEvent (SE 67315 / AE 68617 + 0x7B), as Back Pocket
		// for Controller hooks it.
		inline constexpr REL::RelocationID kPollInputDevicesID{ 67315, 68617 };
		inline constexpr std::ptrdiff_t kPollInputDevicesOffset = 0x7B;

		// XInput masks as Skyrim's ButtonEvent carries them, and DirectInput scan codes.
		inline constexpr std::uint32_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008;
		inline constexpr std::uint32_t kPadA = 0x1000, kPadB = 0x2000;
		inline constexpr std::uint32_t kKeyEsc = 1, kKeyEnter = 28, kKeyE = 18, kKeyUp = 200, kKeyDown = 208, kKeyLeft = 203, kKeyRight = 205;

		// Menus that sit ON TOP of the inventory and take its input.
		inline constexpr const char* kCoveringMenus[] = { "MessageBoxMenu", "Book Menu", "Console", "ContainerMenu", "Journal Menu" };

		enum class Key { none, up, down, left, right, accept, back };

		Key Classify(RE::INPUT_DEVICE a_device, std::uint32_t a_code)
		{
			if (a_device == RE::INPUT_DEVICE::kGamepad) {
				switch (a_code) {
				case kPadUp: return Key::up;
				case kPadDown: return Key::down;
				case kPadLeft: return Key::left;
				case kPadRight: return Key::right;
				case kPadA: return Key::accept;
				case kPadB: return Key::back;
				default: return Key::none;
				}
			}
			if (a_device == RE::INPUT_DEVICE::kKeyboard) {
				switch (a_code) {
				case kKeyUp: return Key::up;
				case kKeyDown: return Key::down;
				case kKeyLeft: return Key::left;
				case kKeyRight: return Key::right;
				case kKeyEnter:
				case kKeyE: return Key::accept;
				case kKeyEsc: return Key::back;
				default: return Key::none;
				}
			}
			return Key::none;
		}

		struct Held
		{
			RE::INPUT_DEVICE device;
			std::uint32_t code;
		};
		std::vector<Held> g_swallowing;   // presses the bar took on their way down; their repeats and release are the bar's too

		bool Swallowing(RE::INPUT_DEVICE a_device, std::uint32_t a_code, bool a_release)
		{
			for (auto it = g_swallowing.begin(); it != g_swallowing.end(); ++it) {
				if (it->device == a_device && it->code == a_code) {
					if (a_release) { g_swallowing.erase(it); }
					return true;
				}
			}
			return false;
		}

		RE::GFxMovieView* InventoryInFront()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui || !ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) { return nullptr; }
			for (const char* name : kCoveringMenus) {
				if (ui->IsMenuOpen(name)) { return nullptr; }
			}
			auto menu = ui->GetMenu<RE::InventoryMenu>();
			return menu ? menu->uiMovie.get() : nullptr;
		}

		// True when the event is swallowed. Main thread, inside the game's input dispatch.
		bool Filter(RE::InputEvent* a_event, RE::GFxMovieView* a_movie)
		{
			auto* button = a_event->AsButtonEvent();
			if (!button) { return false; }
			const auto device = button->GetDevice();
			const auto code = button->GetIDCode();
			const Key key = Classify(device, code);
			if (key == Key::none) { return false; }

			if (!button->IsDown()) {   // a repeat or the release of a press the bar took stays with the bar
				return Swallowing(device, code, !button->IsPressed());
			}

			if (!bar::Focused()) {
				if (key != Key::up) { return false; }
				const int row = bar::ListIndex(a_movie);
				if (row > 0 || row == -2) { return false; }   // SkyUI's list takes Up unless it is on its first row (or empty)
				bar::Focus(a_movie, true);
				g_swallowing.push_back({ device, code });
				bar::Decide(std::format("Up on row {} - focus to the bar", row));
				return true;
			}

			switch (key) {
			case Key::left: bar::Move(a_movie, -1); break;
			case Key::right: bar::Move(a_movie, 1); break;
			case Key::accept: bar::Choose(a_movie); break;
			case Key::down:
			case Key::back:
				bar::Focus(a_movie, false);
				bar::Decide("focus back to the item list");
				break;
			default: break;   // Up on the bar: nothing above it
			}
			g_swallowing.push_back({ device, code });
			return true;
		}

		struct PollInputDevicesHook
		{
			static inline REL::Relocation<void(RE::BSTEventSource<RE::InputEvent*>*, RE::InputEvent**)> func;

			static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent** a_events)
			{
				if (auto* movie = InventoryInFront()) {
					bar::Ensure(movie);
					bar::ServiceInspect(movie);
					if (a_events) {
						for (RE::InputEvent** link = a_events; *link;) {
							RE::InputEvent* ev = *link;
							if (Filter(ev, movie)) {
								*link = ev->next;
								continue;
							}
							link = &ev->next;
						}
					}
				} else {
					auto* ui = RE::UI::GetSingleton();
					if (!ui || !ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) {
						bar::Closed();
						g_swallowing.clear();
					}
				}
				func(a_dispatcher, a_events);
			}
		};
	}

	bool Install()
	{
		const std::uintptr_t site = kPollInputDevicesID.address() + kPollInputDevicesOffset;
		if (!REL::make_pattern<"E8">().match(site)) {
			logger::error("PollInputDevices site 0x{:X} (ID {}+0x{:X}) is not a call instruction; the bar is NOT installed", site,
						  kPollInputDevicesID.id(), kPollInputDevicesOffset);
			SelfCheck::Set("Input hook", false, std::format("site 0x{:X} (ID {}+0x{:X}) is not a call", site, kPollInputDevicesID.id(), kPollInputDevicesOffset));
			return false;
		}
		SKSE::AllocTrampoline(14);
		PollInputDevicesHook::func = SKSE::GetTrampoline().write_call<5>(site, PollInputDevicesHook::thunk);
		logger::info("PollInputDevices hook installed at 0x{:X} (ID {}+0x{:X})", site, kPollInputDevicesID.id(), kPollInputDevicesOffset);
		SelfCheck::Set("Input hook", true, std::format("installed at 0x{:X} (ID {}+0x{:X})", site, kPollInputDevicesID.id(), kPollInputDevicesOffset));
		return true;
	}
}
