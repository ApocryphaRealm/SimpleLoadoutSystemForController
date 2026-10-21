#include "Bar.h"

#include "Loadouts.h"
#include "SelfCheck.h"
#include "Settings.h"
#include "utils/Logger.h"
#include "utils/Strings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <mutex>

namespace bar
{
	namespace
	{
		// SkyUI's inventory panel (read from the running movie, 2026-09-27): inventoryLists holds the category label
		// strip (local y 5-44, empty in most skins), the category icons (y 36-93), the item list (y 94-448), and the
		// search box (x 533) and column button (x 556) at y 11.
		//
		// Layout (the owner, 2026-09-27: "move the filter to be below the loadouts and above the categories"): the bar
		// takes the top strip at full width, the search box and column button drop to a row under it, and the
		// categories and the item list move down by that row. The category-name strip is hidden - the bar is there.
		inline constexpr const char* kListsPath = "_root.Menu_mc.inventoryLists";
		inline constexpr const char* kItemListPath = "_root.Menu_mc.inventoryLists.itemList";
		inline constexpr const char* kBarName = "loadoutBar";
		inline constexpr const char* kBarPath = "_root.Menu_mc.inventoryLists.loadoutBar";
		inline constexpr double kLeft = 10.0;
		inline constexpr double kTop = 4.0;
		inline constexpr double kWidth = 565.0;   // the width of the category icons below it
		inline constexpr double kHeight = 26.0;
		inline constexpr double kGap = 6.0;
		inline constexpr double kFilterRowY = kTop + kHeight + 4.0;   // the search box and column button's new row
		inline constexpr double kShift = 24.0;                          // how far the categories and list move down

		// Move one of SkyUI's clips: to an absolute y (a_absolute) or by an offset.
		void Place(RE::GFxValue& a_lists, const char* a_name, double a_y, bool a_absolute)
		{
			RE::GFxValue clip, y;
			if (!a_lists.GetMember(a_name, &clip) || !clip.IsDisplayObject() || !clip.GetMember("_y", &y) || !y.IsNumber()) {
				logger::warn("layout: {} not found in SkyUI's inventory - left where it is", a_name);
				return;
			}
			clip.SetMember("_y", RE::GFxValue(a_absolute ? a_y : y.GetNumber() + a_y));
		}

		void Layout(RE::GFxValue& a_lists)
		{
			Place(a_lists, "searchWidget", kFilterRowY, true);
			Place(a_lists, "columnSelectButton", kFilterRowY, true);
			Place(a_lists, "categoryList", kShift, false);
			Place(a_lists, "itemList", kShift, false);
			// The list's frame stays where it was, so the list gives up the shifted height: one row fewer, or the last
			// row sits outside the frame (2026-09-27 capture, eight items).
			RE::GFxValue list, height, rows, entry;
			if (a_lists.GetMember("itemList", &list) && list.IsDisplayObject() && list.GetMember("_listHeight", &height) && height.IsNumber() &&
				list.GetMember("_maxListIndex", &rows) && rows.IsNumber()) {
				const double rowHeight = list.GetMember("entryHeight", &entry) && entry.IsNumber() && entry.GetNumber() > 0 ? entry.GetNumber() : kShift;
				const int fewer = static_cast<int>(std::ceil(kShift / rowHeight));
				list.SetMember("_listHeight", RE::GFxValue(height.GetNumber() - kShift));
				list.SetMember("_maxListIndex", RE::GFxValue(std::max(1.0, rows.GetNumber() - fewer)));
				list.Invoke("InvalidateData");
			} else {
				logger::warn("layout: SkyUI's list height not found - a long list may run past its frame");
			}
			RE::GFxValue label;
			if (a_lists.GetMember("categoryLabel", &label) && label.IsDisplayObject()) { label.SetMember("_visible", RE::GFxValue(false)); }
		}

		inline constexpr std::uint32_t kFill = 0x1A1A1A;
		inline constexpr std::uint32_t kFillActive = 0x6B5A36;
		inline constexpr std::uint32_t kEdge = 0x5A5A5A;
		inline constexpr std::uint32_t kEdgeCursor = 0xFFFFFF;
		inline constexpr std::uint32_t kText = 0xD8D8D8;
		inline constexpr std::uint32_t kTextActive = 0xFFFFFF;

		std::mutex g_lock;
		Snapshot g_snap;
		RE::GFxMovieView* g_movie = nullptr;   // the opening the bar was drawn into (identity only, never dereferenced late)

		// Layout (the owner, 2026-10-21): "Always worn" is fixed at the far left, before Loadout 1; the loadouts scroll
		// inside a fixed-width strip beside it, so ten of them never run past the bar's width (the category icons'
		// width - the bar lives in SkyUI's left-hand lists, clear of Inventory Reforged's right-hand panel).
		inline constexpr int kVisible = 5;                 // loadout buttons shown at once
		inline constexpr double kAlwaysWidth = 100.0;      // the fixed "Always worn" box
		inline constexpr double kArrow = 14.0;             // the gutter at each end of the strip for the "more" arrows
		inline constexpr double kStripX = kLeft + kAlwaysWidth + kGap;                       // strip start (arrow gutter first)
		inline constexpr double kButtonsX = kStripX + kArrow;                                // first loadout button
		inline constexpr double kButtonsW = kLeft + kWidth - kArrow - kButtonsX;             // the buttons' share of the strip
		inline constexpr double kButtonW = (kButtonsW - kGap * (kVisible - 1)) / kVisible;   // one loadout button
		inline constexpr int kAlways = -1;                 // cursor value of the "Always worn" box

		int Visible(int a_count) { return std::min(a_count, kVisible); }
		int MaxFirst(int a_count) { return std::max(0, a_count - kVisible); }
		double SlotX(int a_slot) { return kButtonsX + a_slot * (kButtonW + kGap); }

		// The cursor value for a loadouts::Active() value (callers check for -1, none, first).
		int ButtonOf(int a_active) { return a_active == loadouts::kAlwaysWorn ? kAlways : a_active; }

		// Keep the highlighted loadout in the visible window (caller holds g_lock).
		void KeepInView(Snapshot& a_s)
		{
			a_s.first = std::clamp(a_s.first, 0, MaxFirst(a_s.count));
			if (a_s.cursor < 0) { return; }
			if (a_s.cursor < a_s.first) { a_s.first = a_s.cursor; }
			if (a_s.cursor >= a_s.first + kVisible) { a_s.first = a_s.cursor - kVisible + 1; }
			a_s.first = std::clamp(a_s.first, 0, MaxFirst(a_s.count));
		}

		RE::GFxValue Num(double a_v) { return RE::GFxValue(a_v); }

		void Rect(RE::GFxValue& a_clip, double a_x, double a_y, double a_w, double a_h, std::uint32_t a_fill, double a_alpha,
				  std::uint32_t a_edge, double a_edgeWidth)
		{
			std::array<RE::GFxValue, 3> line{ Num(a_edgeWidth), Num(static_cast<double>(a_edge)), Num(100.0) };
			a_clip.Invoke("lineStyle", line);
			std::array<RE::GFxValue, 2> fill{ Num(static_cast<double>(a_fill)), Num(a_alpha) };
			a_clip.Invoke("beginFill", fill);
			std::array<RE::GFxValue, 2> p{ Num(a_x), Num(a_y) };
			a_clip.Invoke("moveTo", p);
			p = { Num(a_x + a_w), Num(a_y) };
			a_clip.Invoke("lineTo", p);
			p = { Num(a_x + a_w), Num(a_y + a_h) };
			a_clip.Invoke("lineTo", p);
			p = { Num(a_x), Num(a_y + a_h) };
			a_clip.Invoke("lineTo", p);
			p = { Num(a_x), Num(a_y) };
			a_clip.Invoke("lineTo", p);
			a_clip.Invoke("endFill");
		}

		// One button: its own clip (drawn shape) and a text field. Redrawn whole on every state change. The slots are
		// fixed places on the bar ("aw" for Always worn, "s0".. for the visible loadouts); scrolling changes the text.
		struct Look
		{
			double y = kTop;
			double h = kHeight;
			double maxSize = 15.0;
			bool boxed = true;          // false: just the text (a hint line)
			const char* align = "center";
		};

		void DrawButton(RE::GFxMovieView* a_movie, RE::GFxValue& a_bar, const std::string& name, int a_depth, double x, double w,
						const std::string& a_text, bool a_cursor, bool a_active, const Look& a_look = {})
		{
			RE::GFxValue button;
			if (!a_bar.GetMember(name.c_str(), &button) || !button.IsDisplayObject()) {
				if (!a_bar.CreateEmptyMovieClip(&button, name.c_str(), a_depth) || !button.IsDisplayObject()) {
					logger::warn("bar: could not create button clip {}", name);
					return;
				}
			}
			button.Invoke("clear");
			if (a_look.boxed) {
				Rect(button, x, a_look.y, w, a_look.h, a_active ? kFillActive : kFill, a_active ? 95.0 : 80.0,
					 a_cursor ? kEdgeCursor : kEdge, a_cursor ? 2.0 : 1.0);
			}

			RE::GFxValue field;
			if (!button.GetMember("label", &field) || !field.IsDisplayObject()) {
				std::array<RE::GFxValue, 6> args{ RE::GFxValue("label"), Num(1.0), Num(x), Num(a_look.y + 4.0), Num(w), Num(a_look.h - 6.0) };
				button.Invoke("createTextField", nullptr, args.data(), args.size());
				if (!button.GetMember("label", &field) || !field.IsDisplayObject()) {
					logger::warn("bar: could not create the label of {}", name);
					return;
				}
				field.SetMember("embedFonts", RE::GFxValue(true));
				field.SetMember("selectable", RE::GFxValue(false));
			}
			RE::GFxValue format;
			a_movie->CreateObject(&format, "TextFormat");
			RE::GFxValue font;
			a_movie->CreateString(&font, "$EverywhereMediumFont");
			format.SetMember("font", font);
			format.SetMember("color", Num(static_cast<double>(a_active ? kTextActive : kText)));
			RE::GFxValue align;
			a_movie->CreateString(&align, a_look.align);
			format.SetMember("align", align);
			std::string text = a_text;
			field.SetText(text.c_str());
			// 15 pt, smaller until the name fits its button; at the smallest size a name that still does not fit is cut
			// and ends in "...".
			RE::GFxValue measured;
			auto fits = [&] { return !field.GetMember("textWidth", &measured) || !measured.IsNumber() || measured.GetNumber() <= w - 6.0; };
			double size = a_look.maxSize;
			for (;; size -= 1.0) {
				format.SetMember("size", Num(size));
				std::array<RE::GFxValue, 1> fmt{ format };
				field.Invoke("setTextFormat", fmt);
				if (size <= 10.0 || fits()) { break; }
			}
			bool cut = false;
			while (!fits() && text.size() > 1) {
				do { text.pop_back(); } while (text.size() > 1 && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80);   // whole UTF-8 characters
				field.SetText((text + "...").c_str());
				std::array<RE::GFxValue, 1> fmt{ format };
				field.Invoke("setTextFormat", fmt);
				cut = true;
			}
			logger::debug("bar: button {} \"{}\"{} at {} pt", name, text, cut ? " (cut)" : "", size);
		}

		// A small filled triangle in the strip's left or right gutter.
		void Triangle(RE::GFxValue& a_clip, double a_x, bool a_left)
		{
			const double mid = kTop + kHeight / 2.0;
			std::array<RE::GFxValue, 3> line{ Num(0.0), Num(0.0), Num(0.0) };
			a_clip.Invoke("lineStyle", line);
			std::array<RE::GFxValue, 2> fill{ Num(static_cast<double>(kTextActive)), Num(90.0) };
			a_clip.Invoke("beginFill", fill);
			const double tip = a_left ? a_x + 2.0 : a_x + kArrow - 2.0, base = a_left ? a_x + kArrow - 3.0 : a_x + 3.0;
			std::array<RE::GFxValue, 2> p{ Num(tip), Num(mid) };
			a_clip.Invoke("moveTo", p);
			p = { Num(base), Num(mid - 6.0) };
			a_clip.Invoke("lineTo", p);
			p = { Num(base), Num(mid + 6.0) };
			a_clip.Invoke("lineTo", p);
			p = { Num(tip), Num(mid) };
			a_clip.Invoke("lineTo", p);
			a_clip.Invoke("endFill");
		}

		// A translated text with its "{}" filled in, by hand (a mangled translation must not throw on the main thread).
		std::string Fill(const char* a_text, const std::string& a_value)
		{
			std::string s = a_text ? a_text : "";
			if (const auto at = s.find("{}"); at != std::string::npos) { s.replace(at, 2, a_value); }
			return s;
		}

		// The name on a loadout's button: the one the player gave it (saved with the game), else the INI's, else "Loadout N".
		std::string LoadoutName(int a_i)
		{
			if (auto custom = loadouts::CustomName(a_i); !custom.empty()) { return custom; }
			const auto& names = settings::Get().names;
			return a_i < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(a_i)] : std::format("Loadout {}", a_i + 1);
		}

		// The rename keyboard: a grid of keys drawn over the lists, walked with the D-pad.
		inline constexpr std::array<const char*, 4> kKeyRows{ "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-" };
		inline constexpr int kRenameRows = 5;   // four key rows and the action row
		inline constexpr int kActions = 5;      // Shift, Space, Back, Clear, Save
		inline constexpr double kPanelY = 60.0, kPanelH = 214.0, kCell = 30.0;

		void DrawRename(RE::GFxMovieView* a_movie, RE::GFxValue& a_bar, const Snapshot& a_s)
		{
			RE::GFxValue panel;
			if (!a_bar.GetMember("rename", &panel) || !panel.IsDisplayObject()) {
				if (!a_s.renaming) { return; }
				a_bar.CreateEmptyMovieClip(&panel, "rename", 40);
				if (!panel.IsDisplayObject()) { return; }
			}
			panel.SetMember("_visible", RE::GFxValue(a_s.renaming));
			if (!a_s.renaming) { return; }
			panel.Invoke("clear");
			Rect(panel, 4.0, kPanelY, kWidth + 12.0, kPanelH, 0x101010, 97.0, kEdge, 1.0);
			const Look wide{ kPanelY + 6.0, 28.0, 15.0, true, "left" };
			DrawButton(a_movie, panel, "title", 1, kLeft, kWidth - 12.0,
					   Fill(strings::TR("SLSC_RenameTitle", "Rename loadout {}"), std::to_string(a_s.renameSlot + 1)) + ": " + a_s.renameText + "_", false, true, wide);
			const double cell = (kWidth - 9.0 * 4.0) / 10.0;   // ten keys across the bar's width
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 10; ++c) {
					char ch = kKeyRows[static_cast<std::size_t>(r)][c];
					if (a_s.shift && ch >= 'a' && ch <= 'z') { ch = static_cast<char>(ch - 'a' + 'A'); }
					const Look look{ kPanelY + 40.0 + r * kCell, kCell - 4.0, 15.0, true, "center" };
					DrawButton(a_movie, panel, std::format("k{}_{}", r, c), 2 + r * 10 + c, kLeft + c * (cell + 4.0), cell, std::string(1, ch),
							   a_s.keyRow == r && a_s.keyCol == c, false, look);
				}
			}
			const std::array<std::string, kActions> labels{ strings::TR("SLSC_KeyShift", "Shift"), strings::TR("SLSC_KeySpace", "Space"),
															strings::TR("SLSC_KeyBack", "Back"), strings::TR("SLSC_KeyClear", "Clear"),
															strings::TR("SLSC_KeySave", "Save") };
			const double aw = (kWidth - 4.0 * 4.0) / kActions;
			for (int c = 0; c < kActions; ++c) {
				const Look look{ kPanelY + 40.0 + 4 * kCell, kCell - 4.0, 15.0, true, "center" };
				DrawButton(a_movie, panel, std::format("a{}", c), 50 + c, kLeft + c * (aw + 4.0), aw, labels[static_cast<std::size_t>(c)],
						   a_s.keyRow == 4 && a_s.keyCol == c, c == 0 && a_s.shift, look);
			}
			const Look foot{ kPanelY + 40.0 + 5 * kCell + 2.0, 20.0, 12.0, false, "left" };
			DrawButton(a_movie, panel, "foot", 60, kLeft, kWidth, strings::TR("SLSC_RenameHint", "A: type   X: erase   Y: save   B: cancel   (keyboard: type, Enter saves, Esc cancels)"),
					   false, false, foot);
		}

		void Redraw(RE::GFxMovieView* a_movie)
		{
			if (!a_movie) { return; }
			RE::GFxValue bar;
			if (!a_movie->GetVariable(&bar, kBarPath) || !bar.IsDisplayObject()) { return; }
			Snapshot s;
			{
				std::scoped_lock l(g_lock);
				if (g_snap.focused) { KeepInView(g_snap); }   // the highlight is always in view; the wheel may scroll it away only while off the bar
				g_snap.first = std::clamp(g_snap.first, 0, MaxFirst(g_snap.count));
				s = g_snap;
			}
			const int active = s.active == -1 ? -3 : ButtonOf(s.active);
			DrawButton(a_movie, bar, "aw", 1, kLeft, kAlwaysWidth, strings::TR("SLSC_AlwaysWorn", "Always worn"), s.focused && s.cursor == kAlways,
					   active == kAlways);
			const int visible = Visible(s.count);
			for (int slot = 0; slot < visible; ++slot) {
				const int i = s.first + slot;
				const std::string text = LoadoutName(i);
				DrawButton(a_movie, bar, std::format("s{}", slot), 2 + slot, SlotX(slot), kButtonW, text, s.focused && s.cursor == i, i == active);
			}
			// More loadouts past either end of the strip: an arrow in that end's gutter.
			RE::GFxValue arrows;
			if (!bar.GetMember("arrows", &arrows) || !arrows.IsDisplayObject()) { bar.CreateEmptyMovieClip(&arrows, "arrows", 20); }
			if (arrows.IsDisplayObject()) {
				arrows.Invoke("clear");
				if (s.first > 0) { Triangle(arrows, kStripX, true); }
				if (s.first + kVisible < s.count) { Triangle(arrows, kLeft + kWidth - kArrow, false); }
			}
			// The button hint, in the filter row's free left side, while the bar has the controller.
			const Look hint{ kFilterRowY, 20.0, 12.0, false, "left" };
			DrawButton(a_movie, bar, "hint", 30, kLeft, kWidth - 70.0,
					   s.focused && !s.renaming ? strings::TR("SLSC_HintBar", "Y: Rename   A: Select   B: Back") : std::string(), false, false, hint);
			DrawRename(a_movie, bar, s);
		}
	}

	void Decide(const std::string& a_what)
	{
		{
			std::scoped_lock l(g_lock);
			g_snap.lastDecision = a_what;
		}
		logger::info("bar: {}", a_what);   // info: every focus, move, select, click, wheel and rename step is in the log (1.0.5)
	}

	void Ensure(RE::GFxMovieView* a_movie)
	{
		if (!a_movie) { return; }
		{
			std::scoped_lock l(g_lock);
			g_snap.inventoryOpen = true;
			g_snap.count = settings::Get().count;
			g_snap.active = loadouts::Active();   // from the co-save after a load, or the last switch
			if (!g_snap.built) {                  // a fresh opening: the strip starts with the active loadout in view
				g_snap.first = 0;
				g_snap.renaming = false;
				if (g_snap.active >= 0) {
					const int keep = std::exchange(g_snap.cursor, g_snap.active);
					KeepInView(g_snap);
					g_snap.cursor = keep;
				}
			}
		}
		RE::GFxValue existing;
		if (a_movie == g_movie && a_movie->GetVariable(&existing, kBarPath) && existing.IsDisplayObject()) { return; }
		strings::Tick();   // once per opening: follows a language change (AMF's, or the game's)

		RE::GFxValue lists;
		if (!a_movie->GetVariable(&lists, kListsPath) || !lists.IsDisplayObject()) {
			return;   // SkyUI's panel is not up yet (first frame of the menu), or this is not a SkyUI inventory
		}
		RE::GFxValue barClip;
		RE::GFxValue depth;
		lists.Invoke("getNextHighestDepth", &depth);
		const std::int32_t d = depth.IsNumber() ? static_cast<std::int32_t>(depth.GetNumber()) : 1000;
		if (!lists.CreateEmptyMovieClip(&barClip, kBarName, d) || !barClip.IsDisplayObject()) {
			static bool warned = false;
			if (!warned) {
				warned = true;
				logger::warn("bar: could not create {} under {}", kBarName, kListsPath);
				SelfCheck::Set("Bar", false, "could not create the bar clip under SkyUI's inventoryLists");
			}
			return;
		}
		g_movie = a_movie;
		Layout(lists);
		int built;
		{
			std::scoped_lock l(g_lock);
			g_snap.built = true;
			built = ++g_snap.builtCount;
		}
		Redraw(a_movie);
		logger::info("bar: drawn into the inventory (opening {}), {} loadout buttons and Always worn at depth {}", built, settings::Get().count, d);
		if (built == 1) { SelfCheck::Set("Bar", true, "drawn into SkyUI's inventory"); }
	}

	void Closed()
	{
		std::scoped_lock l(g_lock);
		if (g_snap.inventoryOpen) {
			g_snap.inventoryOpen = false;
			g_snap.built = false;
			g_snap.focused = false;
			g_snap.listIndex = -2;
		}
		g_movie = nullptr;
	}

	int ListIndex(RE::GFxMovieView* a_movie)
	{
		int index = -2;
		// The ROW on screen, not selectedIndex: SkyUI's selectedIndex points into its whole entry array (every category,
		// in load order), while each entry's filteredIndex is its place in the visible, sorted list (read from the running
		// list, 2026-09-27 - selectedIndex was 3 on the top row). No selection at all (-1) counts as the top too.
		// 1.0.5: says WHY a read failed (once per kind) - with Inventory Reforged's panel active the list could not be read.
		RE::GFxValue list, selected, entry, filtered;
		const char* why = nullptr;
		if (!a_movie || !a_movie->GetVariable(&list, kItemListPath) || !list.IsObject()) {
			why = "the item list is not at _root.Menu_mc.inventoryLists.itemList";
		} else if (!list.GetMember("selectedIndex", &selected) || !selected.IsNumber()) {
			why = "the item list has no numeric selectedIndex";
		} else {
			index = static_cast<int>(selected.GetNumber());
			if (index >= 0) {
				index = -2;
				if (!list.GetMember("selectedEntry", &entry) || !entry.IsObject()) {
					why = "selectedIndex is set but selectedEntry is not an object";
				} else if (!entry.GetMember("filteredIndex", &filtered) || !filtered.IsNumber()) {
					why = "selectedEntry has no numeric filteredIndex";
				} else {
					index = static_cast<int>(filtered.GetNumber());
				}
			}
		}
		if (why) {
			static std::string last;
			if (last != why) {
				last = why;
				logger::warn("bar: cannot read the list row: {} - Up cannot move to the bar while this holds", why);
			}
		}
		std::scoped_lock l(g_lock);
		g_snap.listIndex = index;
		return index;
	}

	void Focus(RE::GFxMovieView* a_movie, bool a_on)
	{
		{
			std::scoped_lock l(g_lock);
			if (g_snap.focused == a_on) { return; }
			g_snap.focused = a_on;
			if (a_on) {
				g_snap.cursor = g_snap.active != -1 ? ButtonOf(g_snap.active) : 0;
				KeepInView(g_snap);
			}
		}
		Redraw(a_movie);
	}

	// Left and right walk Always worn, Loadout 1, 2 ... in the order they stand on the bar, and wrap round.
	void Move(RE::GFxMovieView* a_movie, int a_delta)
	{
		{
			std::scoped_lock l(g_lock);
			if (g_snap.count <= 0) { return; }
			const int places = g_snap.count + 1;
			const int at = (g_snap.cursor + 1 + a_delta % places + places) % places;
			g_snap.cursor = at - 1;
			KeepInView(g_snap);
		}
		Redraw(a_movie);
	}

	void Choose(RE::GFxMovieView* a_movie)
	{
		int target;
		{
			std::scoped_lock l(g_lock);
			const int chosen = g_snap.cursor == kAlways ? loadouts::kAlwaysWorn : g_snap.cursor;
			target = g_snap.active == chosen ? -1 : chosen;
		}
		std::string why;
		if (!loadouts::Request(target, why)) {
			Decide("switch refused: " + why);
			return;
		}
		{
			std::scoped_lock l(g_lock);
			g_snap.active = target;   // the move runs as a task right after this dispatch
		}
		Decide(target >= 0 ? std::format("loadout {} selected", target + 1) : target == loadouts::kAlwaysWorn ? std::string("Always worn selected") : std::string("loadout deselected"));
		Redraw(a_movie);
	}

	// The pointer's place in the bar's own coordinates (the bar is drawn in the lists' space, so _xmouse/_ymouse of
	// its clip are the numbers it was drawn with). False when the pointer cannot be read.
	namespace
	{
		bool Pointer(RE::GFxMovieView* a_movie, double& a_x, double& a_y)
		{
			RE::GFxValue bar, x, y;
			if (!a_movie || !a_movie->GetVariable(&bar, kBarPath) || !bar.IsDisplayObject() || !bar.GetMember("_xmouse", &x) || !bar.GetMember("_ymouse", &y) ||
				!x.IsNumber() || !y.IsNumber()) {
				static bool told = false;
				if (!told) { told = true; logger::warn("bar: the pointer position (_xmouse/_ymouse) cannot be read from the bar clip"); }
				return false;
			}
			a_x = x.GetNumber();
			a_y = y.GetNumber();
			return true;
		}
	}

	bool Hover(RE::GFxMovieView* a_movie, int& a_hit)
	{
		a_hit = -2;
		double x = 0, y = 0;
		const bool known = Pointer(a_movie, x, y);
		{
			static int told = 0;
			if (known && told++ < 20) { logger::info("bar: pointer at ({:.0f}, {:.0f}) in the bar's space", x, y); }
		}
		if (!known || y < kTop || y > kTop + kHeight || x < kLeft || x > kLeft + kWidth) { return false; }
		std::scoped_lock l(g_lock);
		if (x < kLeft + kAlwaysWidth) {
			a_hit = kAlways;
		} else {
			for (int slot = 0; slot < Visible(g_snap.count); ++slot) {
				if (x >= SlotX(slot) && x <= SlotX(slot) + kButtonW) {
					a_hit = g_snap.first + slot;
					break;
				}
			}
		}
		return true;
	}

	void Scroll(RE::GFxMovieView* a_movie, int a_delta)
	{
		{
			std::scoped_lock l(g_lock);
			const int before = g_snap.first;
			g_snap.first = std::clamp(g_snap.first + a_delta, 0, MaxFirst(g_snap.count));
			if (g_snap.focused && g_snap.cursor >= 0) {   // the highlight stays on a button that is shown
				g_snap.cursor = std::clamp(g_snap.cursor, g_snap.first, g_snap.first + Visible(g_snap.count) - 1);
			}
			if (g_snap.first == before) { return; }
		}
		Decide(std::format("wheel: strip scrolled by {}", a_delta));
		Redraw(a_movie);
	}

	void Click(RE::GFxMovieView* a_movie, int a_hit)
	{
		{
			std::scoped_lock l(g_lock);
			g_snap.cursor = a_hit;
		}
		Choose(a_movie);
	}

	bool Renaming()
	{
		std::scoped_lock l(g_lock);
		return g_snap.renaming;
	}

	bool FocusedOnLoadout()
	{
		std::scoped_lock l(g_lock);
		return g_snap.focused && g_snap.cursor >= 0;
	}

	int Cursor()
	{
		std::scoped_lock l(g_lock);
		return g_snap.cursor;
	}

	bool BeginRename(RE::GFxMovieView* a_movie, int a_loadout)
	{
		{
			std::scoped_lock l(g_lock);
			if (a_loadout < 0 || a_loadout >= g_snap.count || g_snap.renaming) { return false; }
			g_snap.renaming = true;
			g_snap.renameSlot = a_loadout;
			g_snap.renameText = loadouts::CustomName(a_loadout);
			g_snap.shift = false;
			g_snap.keyRow = 0;
			g_snap.keyCol = 0;
		}
		Decide(std::format("rename of loadout {} opened", a_loadout + 1));
		Redraw(a_movie);
		return true;
	}

	namespace
	{
		constexpr std::size_t kMaxName = 24;

		void Append(Snapshot& a_s, char a_c)
		{
			if (a_s.renameText.size() < kMaxName) { a_s.renameText += a_c; }
		}
	}

	void RenameChar(RE::GFxMovieView* a_movie, char a_char)
	{
		{
			std::scoped_lock l(g_lock);
			if (!g_snap.renaming) { return; }
			if (a_char == '\b') {
				if (!g_snap.renameText.empty()) { g_snap.renameText.pop_back(); }
			} else {
				if ((g_snap.shift || g_snap.shiftHeld) && a_char >= 'a' && a_char <= 'z') { a_char = static_cast<char>(a_char - 'a' + 'A'); }
				Append(g_snap, a_char);
			}
		}
		Redraw(a_movie);
	}

	void RenameShift(bool a_held)
	{
		std::scoped_lock l(g_lock);
		g_snap.shiftHeld = a_held;
	}

	void RenameStep(RE::GFxMovieView* a_movie, RenameKey a_key)
	{
		bool save = false, cancel = false;
		int slot = -1;
		std::string text;
		{
			std::scoped_lock l(g_lock);
			if (!g_snap.renaming) { return; }
			auto& s = g_snap;
			const int cols = s.keyRow == 4 ? kActions : 10;
			auto act = [&](int a_col) {
				switch (a_col) {
				case 0: s.shift = !s.shift; break;
				case 1: Append(s, ' '); break;
				case 2: if (!s.renameText.empty()) { s.renameText.pop_back(); } break;
				case 3: s.renameText.clear(); break;
				default: save = true; break;
				}
			};
			switch (a_key) {
			case RenameKey::left: s.keyCol = (s.keyCol + cols - 1) % cols; break;
			case RenameKey::right: s.keyCol = (s.keyCol + 1) % cols; break;
			case RenameKey::up:
			case RenameKey::down: {
				const int oldCols = cols;
				s.keyRow = (s.keyRow + (a_key == RenameKey::up ? kRenameRows - 1 : 1)) % kRenameRows;
				const int newCols = s.keyRow == 4 ? kActions : 10;
				s.keyCol = std::min(newCols - 1, s.keyCol * newCols / oldCols);   // the same place across the row
				break;
			}
			case RenameKey::accept:
				if (s.keyRow == 4) {
					act(s.keyCol);
				} else {
					char ch = kKeyRows[static_cast<std::size_t>(s.keyRow)][s.keyCol];
					if (s.shift && ch >= 'a' && ch <= 'z') { ch = static_cast<char>(ch - 'a' + 'A'); }
					Append(s, ch);
				}
				break;
			case RenameKey::erase: if (!s.renameText.empty()) { s.renameText.pop_back(); } break;
			case RenameKey::save: save = true; break;
			case RenameKey::cancel: cancel = true; break;
			}
			if (save || cancel) {
				slot = s.renameSlot;
				text = s.renameText;
				s.renaming = false;
				s.renameSlot = -1;
			}
		}
		if (save) {
			while (!text.empty() && text.back() == ' ') { text.pop_back(); }
			while (!text.empty() && text.front() == ' ') { text.erase(text.begin()); }
			loadouts::SetCustomName(slot, text);
			Decide(text.empty() ? std::format("loadout {} name cleared", slot + 1) : std::format("loadout {} renamed to \"{}\"", slot + 1, text));
		} else if (cancel) {
			Decide("rename cancelled");
		}
		Redraw(a_movie);
	}

	bool Focused()
	{
		std::scoped_lock l(g_lock);
		return g_snap.focused;
	}

	Snapshot GetSnapshot()
	{
		std::scoped_lock l(g_lock);
		return g_snap;
	}

	namespace
	{
		std::mutex g_inspectLock;
		std::condition_variable g_inspectCv;
		std::string g_inspectPath;
		std::string g_inspectResult;
		bool g_inspectDone = false;

		std::string Json(const std::string& a_s)
		{
			std::string out = "\"";
			for (char c : a_s) {
				if (c == '"' || c == '\\') { out += '\\'; }
				if (static_cast<unsigned char>(c) < 0x20) { continue; }
				out += c;
			}
			return out + "\"";
		}

		std::string Describe(const RE::GFxValue& a_v)
		{
			if (a_v.IsNumber()) { return std::format("{}", a_v.GetNumber()); }
			if (a_v.IsBool()) { return a_v.GetBool() ? "true" : "false"; }
			if (a_v.IsString()) { return Json(a_v.GetString()); }
			if (a_v.IsDisplayObject()) { return "\"<clip>\""; }
			if (a_v.IsArray()) { return std::format("\"<array {}>\"", a_v.GetArraySize()); }
			if (a_v.IsObject()) { return "\"<object>\""; }
			if (a_v.IsNull()) { return "null"; }
			return "\"<undefined>\"";
		}

		struct Visitor : RE::GFxValue::ObjectVisitor
		{
			std::string out;
			int n = 0;
			void Visit(const char* a_name, const RE::GFxValue& a_value) override
			{
				if (n++ >= 200) { return; }
				out += std::format("{}{}:{}", out.empty() ? "" : ",", Json(a_name ? a_name : ""), Describe(a_value));
			}
		};
	}

	std::string Inspect(const std::string& a_path, int a_timeoutMs)
	{
		std::unique_lock l(g_inspectLock);
		g_inspectPath = a_path;
		g_inspectDone = false;
		if (!g_inspectCv.wait_for(l, std::chrono::milliseconds(a_timeoutMs), [] { return g_inspectDone; })) {
			g_inspectPath.clear();
			return {};
		}
		return g_inspectResult;
	}

	void ServiceInspect(RE::GFxMovieView* a_movie)
	{
		std::string path;
		{
			std::scoped_lock l(g_inspectLock);
			if (g_inspectPath.empty()) { return; }
			path = std::exchange(g_inspectPath, {});
		}
		std::string result;
		RE::GFxValue v;
		if (!a_movie || !a_movie->GetVariable(&v, path.c_str())) {
			result = R"({"found":false})";
		} else if (v.IsObject()) {
			Visitor visitor;
			v.VisitMembers(&visitor);
			result = std::format(R"({{"found":true,"value":{},"members":{{{}}}}})", Describe(v), visitor.out);
		} else {
			result = std::format(R"({{"found":true,"value":{}}})", Describe(v));
		}
		{
			std::scoped_lock l(g_inspectLock);
			g_inspectResult = std::move(result);
			g_inspectDone = true;
		}
		g_inspectCv.notify_all();
	}
}
