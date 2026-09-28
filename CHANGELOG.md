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
