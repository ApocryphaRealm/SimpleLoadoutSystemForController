#include "InputHook.h"

#include "Bar.h"
#include "MainThread.h"
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
		inline constexpr std::uint32_t kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000;
		inline constexpr std::uint32_t kKeyEsc = 1, kKeyEnter = 28, kKeyE = 18, kKeyUp = 200, kKeyDown = 208, kKeyLeft = 203, kKeyRight = 205;
		// 1.0.4: F2 renames the highlighted loadout, a right-click on its button too; the wheel over the bar scrolls it.
		inline constexpr std::uint32_t kKeyF2 = 60, kKeyBackspace = 14, kKeyShiftL = 42, kKeyShiftR = 54;
		inline constexpr std::uint32_t kMouseLeft = 0, kMouseRight = 1, kMouseWheelUp = 8, kMouseWheelDown = 9;

		// Menus that sit ON TOP of the inventory and take its input.
		inline constexpr const char* kCoveringMenus[] = { "MessageBoxMenu", "Book Menu", "Console", "ContainerMenu", "Journal Menu" };

		enum class Key { none, up, down, left, right, accept, back, rename };

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
				case kPadY: return Key::rename;
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
				case kKeyF2: return Key::rename;
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


		// A letter, digit or mark for a DirectInput scan code (the rename box is typed in with the real keyboard too).
		char CharOf(std::uint32_t a_code)
		{
			static constexpr const char* kRow1 = "1234567890-";     // scan codes 2-12
			static constexpr const char* kRow2 = "qwertyuiop";      // 16-25
			static constexpr const char* kRow3 = "asdfghjkl";       // 30-38
			static constexpr const char* kRow4 = "zxcvbnm,.";       // 44-52
			if (a_code >= 2 && a_code <= 12) { return kRow1[a_code - 2]; }
			if (a_code >= 16 && a_code <= 25) { return kRow2[a_code - 16]; }
			if (a_code >= 30 && a_code <= 38) { return kRow3[a_code - 30]; }
			if (a_code >= 44 && a_code <= 52) { return kRow4[a_code - 44]; }
			if (a_code == 57) { return ' '; }
			if (a_code == 40) { return '\''; }
			return 0;
		}

		void EnsureOutermost();   // below: puts the rename swallow ahead of every other plugin's input hook

		// While the rename keyboard is up every button press belongs to it: nothing reaches the inventory.
		bool FilterRename(RE::ButtonEvent* a_button, RE::GFxMovieView* a_movie)
		{
			const auto device = a_button->GetDevice();
			const auto code = a_button->GetIDCode();
			if (device == RE::INPUT_DEVICE::kKeyboard && (code == kKeyShiftL || code == kKeyShiftR)) {
				bar::RenameShift(a_button->IsPressed());
				return true;
			}
			if (!a_button->IsDown()) { return true; }
			const Key key = Classify(device, code);
			if (device == RE::INPUT_DEVICE::kGamepad) {
				switch (code) {
				case kPadX: bar::RenameStep(a_movie, bar::RenameKey::erase); break;
				case kPadY: bar::RenameStep(a_movie, bar::RenameKey::save); break;
				default: break;
				}
			}
			switch (key) {
			case Key::up: bar::RenameStep(a_movie, bar::RenameKey::up); break;
			case Key::down: bar::RenameStep(a_movie, bar::RenameKey::down); break;
			case Key::left: bar::RenameStep(a_movie, bar::RenameKey::left); break;
			case Key::right: bar::RenameStep(a_movie, bar::RenameKey::right); break;
			case Key::back: bar::RenameStep(a_movie, bar::RenameKey::cancel); break;
			case Key::accept:
				if (device == RE::INPUT_DEVICE::kGamepad) {
					bar::RenameStep(a_movie, bar::RenameKey::accept);
				} else if (code == kKeyEnter) {
					bar::RenameStep(a_movie, bar::RenameKey::save);
				} else if (const char c = CharOf(code)) {
					bar::RenameChar(a_movie, c);   // E on the keyboard types an e
				}
				break;
			default:
				if (device == RE::INPUT_DEVICE::kKeyboard) {
					if (code == kKeyBackspace) {
						bar::RenameChar(a_movie, '\b');
					} else if (const char c = CharOf(code)) {
						bar::RenameChar(a_movie, c);
					}
				}
				break;
			}
			return true;
		}

		// The mouse over the bar: the wheel scrolls the strip, a left click picks a button, a right click renames it.
		bool FilterMouse(RE::ButtonEvent* a_button, RE::GFxMovieView* a_movie)
		{
			const auto code = a_button->GetIDCode();
			if (code == kMouseWheelUp || code == kMouseWheelDown) {
				if (a_button->Value() == 0.0f) { return false; }
				int hit;
				const bool over = bar::Hover(a_movie, hit);
				static int told = 0;
				if (told++ < 20) { logger::info("input: wheel code {} - pointer {} the bar", code, over ? "on" : "not on"); }
				if (!over) { return false; }
				bar::Scroll(a_movie, code == kMouseWheelUp ? -1 : 1);
				return true;
			}
			if (code != kMouseLeft && code != kMouseRight) { return false; }
			const auto device = a_button->GetDevice();
			if (!a_button->IsDown()) { return Swallowing(device, code, !a_button->IsPressed()); }
			int hit;
			const bool over = bar::Hover(a_movie, hit);
			logger::info("input: mouse button {} - pointer {} the bar, button {}", code, over ? "on" : "not on", hit);
			if (!over || hit == -2) { return false; }
			if (code == kMouseRight) {
				if (bar::BeginRename(a_movie, hit)) { EnsureOutermost(); }   // nothing for the Always worn box
			} else {
				bar::Click(a_movie, hit);
			}
			g_swallowing.push_back({ device, code });
			return true;
		}

		// True when the event is swallowed. Main thread, inside the game's input dispatch.
		bool Filter(RE::InputEvent* a_event, RE::GFxMovieView* a_movie)
		{
			auto* button = a_event->AsButtonEvent();
			if (!button) { return false; }
			const auto device = button->GetDevice();
			const auto code = button->GetIDCode();
			{   // 1.0.5: the first presses of each inventory opening are logged, so a test shows what reached the bar and what it decided
				static int logged = 0;
				if (button->IsDown() && !bar::Focused() ? logged < 60 : bar::Focused() || bar::Renaming()) {
					if (button->IsDown() && !bar::Focused()) { ++logged; }
					logger::info("input: device {} code 0x{:X} down {} -> {} (bar focused {}, renaming {})", static_cast<int>(device), code, button->IsDown(),
								 static_cast<int>(Classify(device, code)), bar::Focused(), bar::Renaming());
				}
			}
			if (bar::Renaming()) {
				const bool taken = FilterRename(button, a_movie);
				if (!bar::Renaming()) { g_swallowing.push_back({ device, code }); }   // Enter, Esc or Y closed it: its release is the keyboard's too
				return taken;
			}
			if (device == RE::INPUT_DEVICE::kMouse) { return FilterMouse(button, a_movie); }
			const Key key = Classify(device, code);
			if (key == Key::none) { return false; }

			if (!button->IsDown()) {   // a repeat or the release of a press the bar took stays with the bar
				return Swallowing(device, code, !button->IsPressed());
			}

			if (!bar::Focused()) {
				if (key != Key::up) { return false; }
				const int row = bar::ListIndex(a_movie);
				if (row > 0 || row == -2 || row == bar::kNoSelection) {
					static int told = 0;
					if (row == bar::kNoSelection) { logger::info("input: Up with no list row selected (Inventory Reforged's slots or the pointer hold it) - stays with the menu"); }
					else if (told++ < 20) { logger::info("input: Up with the list on row {} - stays with the list", row); }
					return false;
				}   // SkyUI's list takes Up unless it is on its first row (or empty); with no row selected the menu keeps it
				bar::Focus(a_movie, true);
				g_swallowing.push_back({ device, code });
				bar::Decide(std::format("Up on row {} - focus to the bar", row));
				return true;
			}

			switch (key) {
			case Key::left: bar::Move(a_movie, -1); break;
			case Key::right: bar::Move(a_movie, 1); break;
			case Key::accept: bar::Choose(a_movie); break;
			case Key::rename:   // Y / F2 on a loadout; on Always worn it only keeps the favourite button away from the list
				if (bar::FocusedOnLoadout() && bar::BeginRename(a_movie, bar::Cursor())) { EnsureOutermost(); }
				break;
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


		// 1.0.4 rebuild: the rename keyboard must be the ONLY reader of the keyboard and pad while it is open. Back Pocket
		// installed its PollInputDevices hook after this one, so its hook ran first and took the B key (its Pocket key)
		// and Perfected Wheeler's edit-mode key never reached this filter (2026-10-21 test: typing "b" opened Wheeler's
		// edit mode and the next letter and Enter were lost; this log saw no event for B at all). A second hook on the
		// same call site, installed the first time a rename opens, sits outside every hook installed before it: while
		// renaming it removes every button and character event from the list BEFORE the hooks and sinks behind it are
		// handed the list, so they never see a key; otherwise it passes the list straight on.
		struct OuterHook
		{
			static inline REL::Relocation<void(RE::BSTEventSource<RE::InputEvent*>*, RE::InputEvent**)> func;

			static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent** a_events)
			{
				if (a_events && bar::Renaming()) {
					auto* movie = InventoryInFront();
					int taken = 0;
					for (RE::InputEvent** link = a_events; *link;) {
						RE::InputEvent* ev = *link;
						bool take = false;
						if (auto* button = ev->AsButtonEvent()) {
							take = true;
							if (movie && bar::Renaming()) {
								const auto device = button->GetDevice();
								const auto code = button->GetIDCode();
								FilterRename(button, movie);
								if (!bar::Renaming()) { g_swallowing.push_back({ device, code }); }
							}
						} else if (ev->GetEventType() == RE::INPUT_EVENT_TYPE::kChar) {
							take = true;
						}
						if (take) {
							*link = ev->next;
							++taken;
							continue;
						}
						link = &ev->next;
					}
					if (taken) {
						static int told = 0;
						if (told++ < 30) { logger::info("input: rename open - {} event(s) taken before the other input hooks", taken); }
					}
				}
				func(a_dispatcher, a_events);
			}
		};

		std::uintptr_t g_outerTarget = 0;   // where the call site pointed right after the outer hook was written
		bool g_outerQueued = false;

		std::uintptr_t SiteTarget(std::uintptr_t a_site)
		{
			return a_site + 5 + *reinterpret_cast<const std::int32_t*>(a_site + 1);
		}

		void EnsureOutermost()
		{
			const std::uintptr_t site = kPollInputDevicesID.address() + kPollInputDevicesOffset;
			if (g_outerTarget != 0) {
				if (SiteTarget(site) != g_outerTarget) {
					logger::warn("input: another plugin hooked PollInputDevices after the rename swallow - it sees keys first now and cannot be blocked from here");
				}
				return;
			}
			if (g_outerQueued) { return; }
			g_outerQueued = true;
			// A task, not inline: the call site is being executed right now.
			SKSE::GetTaskInterface()->AddTask([site] {
				if (!REL::make_pattern<"E8">().match(site)) {
					logger::error("input: PollInputDevices site is no longer a call - the rename swallow is NOT outermost");
					return;
				}
				OuterHook::func = SKSE::GetTrampoline().write_call<5>(site, OuterHook::thunk);
				g_outerTarget = SiteTarget(site);
				logger::info("input: rename swallow installed ahead of every other PollInputDevices hook");
			});
		}

		struct PollInputDevicesHook
		{
			static inline REL::Relocation<void(RE::BSTEventSource<RE::InputEvent*>*, RE::InputEvent**)> func;

			static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent** a_events)
			{
				mainthread::Service();   // DevBench questions that must be answered on this thread
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
		SKSE::AllocTrampoline(14 * 4);   // this hook, and the rename swallow installed later
		PollInputDevicesHook::func = SKSE::GetTrampoline().write_call<5>(site, PollInputDevicesHook::thunk);
		logger::info("PollInputDevices hook installed at 0x{:X} (ID {}+0x{:X})", site, kPollInputDevicesID.id(), kPollInputDevicesOffset);
		SelfCheck::Set("Input hook", true, std::format("installed at 0x{:X} (ID {}+0x{:X})", site, kPollInputDevicesID.id(), kPollInputDevicesOffset));
		return true;
	}
}
