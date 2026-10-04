#pragma once

#include <string>

// The loadouts themselves. Everything the player wears or holds while a loadout is active IS that loadout: switching
// away moves the worn gear into the loadout's own container (one per loadout, in SimpleLoadoutSystemForController.esp -
// non-respawning, persistent, in a never-resetting cell), out of the inventory and its carry weight; selecting a
// loadout brings its gear back and equips it. Unequipping inside a loadout simply leaves the item in the inventory.
// All of it runs on the main thread (SKSE task); the bar only asks.
//
// Always worn (1.0.2, asked for on the Nexus page, 2026-10-04): gear that no switch stores or takes off - a ring or
// necklace worn with every loadout. The set is chosen the way a loadout is: select the "Always worn" box, wear what
// should always be worn, and whatever is worn when the box is left becomes the set. A loadout's own piece in the same
// slot wins (the owner's choice); the always-worn piece goes back on after any later switch where its slot is free.
namespace loadouts
{
	inline constexpr int kAlwaysWorn = -2;   // Active() / Request() value for the "Always worn" box

	void Init();                        // kDataLoaded: find the containers
	void RegisterSerialization();       // co-save: the active loadout, left-hand weapons, the always-worn set

	int Active();                       // a loadout (0-based), -1 when none, kAlwaysWorn while that box is selected
	int StorageReady();                 // how many containers were found

	// Select a loadout (0-based), kAlwaysWorn, or -1 to deselect. Returns false - with a notification - when it cannot
	// happen now (in combat, the containers missing). The move itself runs as a task after the current input dispatch.
	bool Request(int a_loadout, std::string& a_why);

	std::string ContentsJson();         // DevBench: active, worn items, the always-worn set, each container, weight
}
