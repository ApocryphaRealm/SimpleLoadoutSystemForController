#pragma once

#include <string>

// The loadouts themselves. Everything the player wears or holds while a loadout is active IS that loadout: switching
// away moves the worn gear into the loadout's own container (one per loadout, in SimpleLoadoutSystemForController.esp -
// non-respawning, persistent, in a never-resetting cell), out of the inventory and its carry weight; selecting a
// loadout brings its gear back and equips it. Unequipping inside a loadout simply leaves the item in the inventory.
// All of it runs on the main thread (SKSE task); the bar only asks.
namespace loadouts
{
	void Init();                        // kDataLoaded: find the containers
	void RegisterSerialization();       // co-save: the active loadout and which weapons sat in the left hand

	int Active();                       // -1 when none
	int StorageReady();                 // how many containers were found

	// Select a loadout (0-based), or -1 to deselect. Returns false - with a notification - when it cannot happen now
	// (in combat, the containers missing). The move itself runs as a task after the current input dispatch.
	bool Request(int a_loadout, std::string& a_why);

	std::string ContentsJson();         // DevBench: active, worn items, each container's items, inventory weight
}
