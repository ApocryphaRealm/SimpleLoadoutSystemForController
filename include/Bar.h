#pragma once

#include <string>

namespace RE
{
	class GFxMovieView;
}

// The loadout bar drawn into SkyUI's inventory at runtime - SkyUI's files are never touched. Every call here runs
// on the game's main thread (from the input dispatch hook); DevBench reads only GetSnapshot().
namespace bar
{
	struct Snapshot
	{
		bool inventoryOpen = false;
		bool built = false;
		bool focused = false;
		int cursor = 0;          // the button the bar's highlight is on
		int active = -1;         // the selected loadout, -1 for none
		int count = 0;
		int listIndex = -2;      // SkyUI's item list selectedIndex (-2 = could not read)
		int builtCount = 0;      // how many inventory openings the bar was drawn into
		std::string lastDecision;
	};

	// Called once per input dispatch while the inventory is the menu in front: draws the bar into this opening
	// of the menu if it is not there yet.
	void Ensure(RE::GFxMovieView* a_movie);
	void Closed();                                   // the inventory went away: drop focus and the movie

	int ListIndex(RE::GFxMovieView* a_movie);        // SkyUI's highlighted row, -1 when empty, -2 when unreadable
	void Focus(RE::GFxMovieView* a_movie, bool a_on);
	void Move(RE::GFxMovieView* a_movie, int a_delta);
	void Choose(RE::GFxMovieView* a_movie);          // the highlighted button: select it, or deselect when it is active
	bool Focused();

	void Decide(const std::string& a_what);
	Snapshot GetSnapshot();

	// Research: list an ActionScript object's members in the open inventory. Asked from any thread; answered on the
	// main thread by the next input dispatch (ServiceInspect). Empty when nothing answered within a_timeoutMs.
	std::string Inspect(const std::string& a_path, int a_timeoutMs);
	void ServiceInspect(RE::GFxMovieView* a_movie);
}
