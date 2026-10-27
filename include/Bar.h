#pragma once

#include <string>

namespace RE
{
	class GFxMovieView;
}

// The loadout bar drawn into SkyUI's inventory at runtime - SkyUI's files are never touched. Every call here runs
// on the game's main thread (from the input dispatch hook); DevBench reads only GetSnapshot().
//
// Layout (1.0.4): "Always worn" is fixed at the far left; the loadouts sit in a fixed-width strip beside it, five at a
// time, scrolled by the D-pad (the highlight is always kept in view) or the mouse wheel over the bar. A triangle at
// the strip's left or right end says more loadouts lie that way. Y on the controller (F2, or a right-click on the
// button) renames the highlighted loadout through an on-screen keyboard drawn into the same inventory.
namespace bar
{
	struct Snapshot
	{
		bool inventoryOpen = false;
		bool built = false;
		bool focused = false;
		int cursor = 0;          // the button the bar's highlight is on (-1 = the "Always worn" box, 0.. = a loadout)
		int first = 0;           // the first loadout shown in the strip
		int active = -1;         // loadouts::Active(): a loadout, -1 for none, loadouts::kAlwaysWorn
		int count = 0;           // loadout buttons; the bar draws the strip plus "Always worn"
		int listIndex = -2;      // SkyUI's item list selectedIndex (-2 = could not read)
		int builtCount = 0;      // how many inventory openings the bar was drawn into
		bool renaming = false;   // the rename keyboard is up
		int renameSlot = -1;     // the loadout being renamed
		std::string renameText;
		bool shiftHeld = false;  // a physical Shift key is down
		bool shift = false;      // the rename keyboard's Shift (the keyboard's own, or the Shift box)
		int keyRow = 0;          // the rename keyboard's highlighted key: row 0-3 keys, 4 = the action row
		int keyCol = 0;
		std::string lastDecision;
	};

	// One step on the rename keyboard.
	enum class RenameKey { up, down, left, right, accept, erase, save, cancel };

	// Called once per input dispatch while the inventory is the menu in front: draws the bar into this opening
	// of the menu if it is not there yet.
	void Ensure(RE::GFxMovieView* a_movie);
	void Closed();                                   // the inventory went away: drop focus and the movie

	inline constexpr int kNoSelection = -3;          // the list has rows but none is selected (IR's slots or the pointer hold it)
	int ListIndex(RE::GFxMovieView* a_movie);        // SkyUI's highlighted row, -1 when empty, -2 when unreadable, kNoSelection
	void Focus(RE::GFxMovieView* a_movie, bool a_on);
	void Move(RE::GFxMovieView* a_movie, int a_delta);
	void Choose(RE::GFxMovieView* a_movie);          // the highlighted button: select it, or deselect when it is active
	bool Focused();

	// Mouse. Hover: true when the pointer is on the bar's row; a_hit is then the button under it (-1 Always worn,
	// 0.. a loadout) or -2 for none. Scroll moves the strip one loadout. Click selects the button; RightClick renames it.
	bool Hover(RE::GFxMovieView* a_movie, int& a_hit);
	void Scroll(RE::GFxMovieView* a_movie, int a_delta);
	void Click(RE::GFxMovieView* a_movie, int a_hit);

	// Rename. Begin opens the keyboard for a loadout (false when the button is not a loadout). While it is up the input
	// hook gives every key to Rename* and swallows it.
	bool BeginRename(RE::GFxMovieView* a_movie, int a_loadout);
	bool Renaming();
	bool FocusedOnLoadout();                          // the highlight is on a loadout (not Always worn)
	int Cursor();
	void RenameStep(RE::GFxMovieView* a_movie, RenameKey a_key);
	void RenameChar(RE::GFxMovieView* a_movie, char a_char);   // a typed character, from a physical keyboard
	void RenameShift(bool a_held);

	void Decide(const std::string& a_what);
	Snapshot GetSnapshot();

	// Research: list an ActionScript object's members in the open inventory. Asked from any thread; answered on the
	// main thread by the next input dispatch (ServiceInspect). Empty when nothing answered within a_timeoutMs.
	std::string Inspect(const std::string& a_path, int a_timeoutMs);
	void ServiceInspect(RE::GFxMovieView* a_movie);
}
