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

		double ButtonWidth(int a_count) { return (kWidth - kGap * (a_count - 1)) / a_count; }

		// The bar holds the loadouts and, after them, the "Always worn" box: count + 1 buttons.
		int Buttons(int a_count) { return a_count + 1; }

		// The button index for a loadouts::Active() value.
		int ButtonOf(int a_active, int a_count) { return a_active == loadouts::kAlwaysWorn ? a_count : a_active; }

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

		// One button: its own clip (drawn shape) and a text field. Redrawn whole on every state change. a_count is the
		// number of buttons; the last one is "Always worn".
		void DrawButton(RE::GFxMovieView* a_movie, RE::GFxValue& a_bar, int a_i, int a_count, bool a_cursor, bool a_active)
		{
			RE::GFxValue button;
			const std::string name = std::format("b{}", a_i);
			if (!a_bar.GetMember(name.c_str(), &button) || !button.IsDisplayObject()) {
				if (!a_bar.CreateEmptyMovieClip(&button, name.c_str(), a_i + 1) || !button.IsDisplayObject()) {
					logger::warn("bar: could not create button clip {}", name);
					return;
				}
			}
			button.Invoke("clear");
			const double w = ButtonWidth(a_count);
			const double x = kLeft + a_i * (w + kGap);
			Rect(button, x, kTop, w, kHeight, a_active ? kFillActive : kFill, a_active ? 95.0 : 80.0,
				 a_cursor ? kEdgeCursor : kEdge, a_cursor ? 2.0 : 1.0);

			RE::GFxValue field;
			if (!button.GetMember("label", &field) || !field.IsDisplayObject()) {
				std::array<RE::GFxValue, 6> args{ RE::GFxValue("label"), Num(1.0), Num(x), Num(kTop + 4.0), Num(w), Num(kHeight - 6.0) };
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
			a_movie->CreateString(&align, "center");
			format.SetMember("align", align);
			const auto& names = settings::Get().names;
			const bool alwaysWorn = a_i == a_count - 1;
			const std::string text = alwaysWorn ? strings::TR("SLSC_AlwaysWorn", "Always worn")
								   : a_i < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(a_i)]
																			: std::format("Loadout {}", a_i + 1);
			field.SetText(text.c_str());
			// 15 pt, smaller until the name fits its button (ten loadouts and "Always worn" share 565 px).
			RE::GFxValue measured;
			double size = 15.0;
			for (;; size -= 1.0) {
				format.SetMember("size", Num(size));
				std::array<RE::GFxValue, 1> fmt{ format };
				field.Invoke("setTextFormat", fmt);
				if (size <= 10.0 || !field.GetMember("textWidth", &measured) || !measured.IsNumber() || measured.GetNumber() <= w - 6.0) { break; }
			}
			logger::debug("bar: button {} \"{}\" at {} pt", a_i, text, size);
		}

		void Redraw(RE::GFxMovieView* a_movie)
		{
			if (!a_movie) { return; }
			RE::GFxValue bar;
			if (!a_movie->GetVariable(&bar, kBarPath) || !bar.IsDisplayObject()) { return; }
			Snapshot s;
			{
				std::scoped_lock l(g_lock);
				s = g_snap;
			}
			const int buttons = Buttons(s.count);
			const int active = ButtonOf(s.active, s.count);
			for (int i = 0; i < buttons; ++i) {
				DrawButton(a_movie, bar, i, buttons, s.focused && i == s.cursor, i == active);
			}
		}
	}

	void Decide(const std::string& a_what)
	{
		{
			std::scoped_lock l(g_lock);
			g_snap.lastDecision = a_what;
		}
		logger::debug("bar: {}", a_what);
	}

	void Ensure(RE::GFxMovieView* a_movie)
	{
		if (!a_movie) { return; }
		{
			std::scoped_lock l(g_lock);
			g_snap.inventoryOpen = true;
			g_snap.count = settings::Get().count;
			g_snap.active = loadouts::Active();   // from the co-save after a load, or the last switch
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
		RE::GFxValue list, selected, entry, filtered;
		if (a_movie && a_movie->GetVariable(&list, kItemListPath) && list.IsObject() && list.GetMember("selectedIndex", &selected) &&
			selected.IsNumber()) {
			index = static_cast<int>(selected.GetNumber());
			if (index >= 0) {
				index = -2;
				if (list.GetMember("selectedEntry", &entry) && entry.IsObject() && entry.GetMember("filteredIndex", &filtered) && filtered.IsNumber()) {
					index = static_cast<int>(filtered.GetNumber());
				}
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
			if (a_on) { g_snap.cursor = g_snap.active != -1 ? ButtonOf(g_snap.active, g_snap.count) : 0; }
		}
		Redraw(a_movie);
	}

	void Move(RE::GFxMovieView* a_movie, int a_delta)
	{
		{
			std::scoped_lock l(g_lock);
			if (g_snap.count <= 0) { return; }
			const int buttons = Buttons(g_snap.count);
			g_snap.cursor = (g_snap.cursor + a_delta + buttons) % buttons;
		}
		Redraw(a_movie);
	}

	void Choose(RE::GFxMovieView* a_movie)
	{
		int target;
		{
			std::scoped_lock l(g_lock);
			const int chosen = g_snap.cursor == g_snap.count ? loadouts::kAlwaysWorn : g_snap.cursor;
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
