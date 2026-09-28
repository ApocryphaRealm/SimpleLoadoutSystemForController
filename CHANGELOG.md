# Changelog - Simple Loadout System for Controller

Every version, beside the code it describes. Status is the version ledger's word for the build.

## 1.0.0 - in progress - untested

Stage 1 of the plan (4. plans\Simple Loadout System for Controller\plan.md): the bar and its controls, no equipment moves
yet.
- A loadout bar is drawn into SkyUI's inventory at runtime (SkyUI's files are never edited), at the top of the panel
  and the width of the category icons. The search box and column button move to a row under it and the categories
  and item list move down by that row (the owner: "move the filter to be below the loadouts and above the
  categories").
- Controller: D-pad Up on the item list's top row (or with nothing highlighted) moves onto the bar; Left / Right move
  along it; A selects the highlighted loadout, or deselects it when it is already active; D-pad Down or B goes back to
  the list. Keyboard: the arrow keys, Enter / E and Esc the same way. The top row is SkyUI's visible row
  (selectedEntry.filteredIndex), not its selectedIndex, which points into the whole entry array.
- The game's input dispatch hook (Back Pocket for Controller's PollInputDevices site) takes only the presses the bar
  uses; everything else reaches SkyUI untouched.
- INI: iLoadoutCount (1-10, default 5), sLoadoutName1..10, uLogLevel (info). Self-check report and the DevBench tool
  "loadouts" (state, and path inspection of the inventory's ActionScript objects).
- Proven on Njordlinger Test (Norden UI Black's inventory, SE 1.5.97) with a virtual DualShock 4 through Steam Input:
  Up on row 1 stays with SkyUI, Up on row 0 lands on the bar, Right / Right / Cross selects Loadout 3, Down returns.

Stage 2: storage and switching.
- SimpleLoadoutSystemForController.esp (tools\build-esp.py, raw records): ONE container per loadout, ten in all, as safe
  storage (the owner: "one per loadout and ... considered safe storage") - a non-respawning chest base, persistent
  references owned by the player, in an interior cell whose encounter zone never resets.
- Selecting a loadout takes off everything worn (it stays in the inventory - it belonged to no loadout), then the
  loadout's container empties into the inventory and every piece is equipped, weapons back in the hand they were in.
  Switching away (to another loadout or none) moves everything worn into the loadout's container, out of the
  inventory. Whatever is equipped while a loadout is active is the loadout; whatever is unequipped leaves it. Items
  keep their own data (moved with their ExtraDataList). Quest items never leave the inventory. No switching in combat.
- Hidden worn items - non-playable or nameless (TNG's cover, body and skin pieces) - are never touched: the first run
  stored one.
- The open inventory rebuilds its list after a switch (ItemList::Update), and the item list gives up the row the
  layout shifted it by, so a long list stays inside its frame.
- Co-save: the active loadout and each loadout's left-hand weapons. DevBench "loadouts" op=contents: worn items, each
  container's contents, inventory weight.
- Proven with the virtual DualShock 4 on the bar: none -> 1 strips; iron set (shield) worn in 1; 1 -> 2 stores it (container
  1); clothes worn in 2; 2 -> 1 restores the set and stores the clothes; 1 -> 3 with a war axe and a LEFT-hand dagger,
  away and back - the dagger returns to the left hand; deselect stores everything; a save made in 1 reloads into 1 with
  every container intact; with the inventory open, 2 -> 1 brings the six iron pieces back into the list (19 -> 25).
  Carry weight could not be shown in Njordlinger (Weightless NG makes armour weigh nothing).
