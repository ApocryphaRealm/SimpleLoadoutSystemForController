# Changelog - Simple Loadout System for Controller

Every version, beside the code it describes. Status is the version ledger's word for the build.

## 1.0.5 - 2026-10-27 - untested

### Fixed
- With Inventory Reforged, D-pad Up while its slots have the controller (after R3) no longer jumps to the loadout bar. An item list with rows but no row selected (Inventory Reforged's slot mode, or the pointer off the list) now leaves Up to the menu; only the first row, or an empty list, moves Up to the bar. Before, every Up in the slots moved to the bar, and Left/Right then walked the loadout tabs instead of the slots (owner report 2026-10-27).

## 1.0.4 - 2026-10-21 - working

### Changed
- The bar: "Always worn" now stands fixed at the far left, before Loadout 1. The loadouts sit in a fixed-width strip beside it, five at a time, and scroll. The strip ends where the old bar ended (the category icons' width), so ten loadouts no longer shrink every button. A triangle at the strip's left or right end says more loadouts lie that way.
- iLoadoutCount now defaults to 10 (was 5). An existing INI keeps its own number; set it to 10 to see all ten.
- Controller: Left / Right walk Always worn, Loadout 1, 2 ... and the strip scrolls to keep the highlighted one in view. Mouse: the wheel over the bar scrolls the strip; a left click on a button selects it.

### Added
- Rename a loadout: Y on the controller while a loadout is highlighted on the bar (F2 on the keyboard, right-click on the button). An on-screen keyboard opens over the inventory: D-pad walks the keys, A types, X erases, Y saves, B cancels (with a real keyboard: type, Enter saves, Esc cancels). The name is kept per loadout in the SKSE co-save; an empty name goes back to the INI name or "Loadout N"; a name too long for its button ends in "...". A button hint shows while the bar has the controller.
- Eight new translated strings in all eleven languages.

### Diagnostics (rebuild after the first in-game test, 2026-10-21)
- The first test beside Inventory Reforged reported no response to the bar and the log held nothing after "bar: drawn". Every focus, move, select, click, wheel and rename step is now logged at info, the first 60 presses reaching the bar's input filter are logged with what they were taken for, and a list row or pointer position that cannot be read is logged as a warning saying which part was missing.

### Fixed in the rebuild
- The rename keyboard is drawn directly under the inventory movie's root and raised above every other clip (checked every frame while it is open), so Inventory Reforged's item window no longer covers it.

### Fixed in the second rebuild
- Typing in the rename keyboard no longer leaks to other mods. Back Pocket's input hook was installed after this mod's, so it ran first and took the B key (its Pocket key); Perfected Wheeler then saw B and opened its edit mode, and the following letter and Enter were lost. The first time a rename opens, the mod now puts a second hook on the same game call, outside every hook installed before it. While the keyboard is open that hook removes every key, pad and mouse button and every character event from the list before any other hook or event sink is handed it.
- Ammunition is moved and put back on as one stack in one piece (it was split into several pieces when its extra data counted differently), and the log now records every ammunition store and restore and the total of each ammunition type across the inventory and the ten containers before and after each switch; a total that changes is logged as a warning. This follows the report of 23 extra Iron Arrows after a save and load; the cause was not proven offline.

### Fixed in the third rebuild
- While the rename keyboard is open the inventory's 3D item is hidden (the engine draws it above the whole menu, so it showed through the keys) with the game's own GameDelegate call "UpdateItem3D", and shown again when the keyboard closes if a row is highlighted.
- A worn marker left on an empty stack after a load (a worn "Iron Arrow" with count 0) is no longer treated as a piece: it is skipped by the worn scan, so it is not stored, counted, captured into Always worn or listed.

### Tested
- In game (SE line, Njordlinger Test, DLL fcb7c9cd, 2026-10-21, three runs; evidence in _work\sls-1.0.4-final-2026-10-21, _work\skyrim-round3-2026-10-21\C and _work\skyrim-round4-2026-10-21\C): ten loadouts in the scrolling strip with Always worn fixed on the left; mouse wheel and clicks; renaming with F2, right-click and pad Y through the on-screen keyboard, Esc / B cancelling; names and the active loadout kept across save and load; switching after a load; the keyboard above Inventory Reforged's item window; no key leaking to Back Pocket or Perfected Wheeler while typing; the 3D item hidden while the keyboard is open; no zero-count arrow entry. The 1.7 line is built from the same source and was not run.

### Save compatibility
- Loadouts 1-5 and Always worn are untouched: the same ten storage containers (REFR 0x803 + slot) and the same co-save records. Names are a new record ('NAME'); a save made before this build simply has none, and one made after it loads in 1.0.3 with the record ignored.

### Known
- An arrow count change across save/load seen in testing was traced to outside SLS (no SLS ammo movement): the SLS log showed no ammunition store or restore at that moment, every ammunition before/after total matched, and in another run the count changed the other way (73 -> 50) after a restart with no SLS action.

## 1.0.3 - 2026-10-13 - working

### Added
- New INI option [General] bAllowSwitchInCombat (default 0). At 0 nothing changes: a switch in combat is refused with 'You cannot change loadouts in combat'. At 1 loadouts can be switched during combat; everything else about a switch is the same. Set it in SimpleLoadoutSystemForController.ini with the game closed (asked for on the Nexus page by screamline).

### Changed
- DevBench 'loadouts' state reports allowSwitchInCombat; the log's settings line names it.

### Tested
- In game (SE line, Njordlinger, player in combat state): at bAllowSwitchInCombat=0 a switch in combat is refused with "You cannot change loadouts in combat" and nothing moves; at 1 (kept across a restart) a switch in combat works in both directions with no refusal and the worn weapons swap; out of combat both settings switch as before; the weapon tie is unchanged. The 1.7 line is built from the same source and was not run.
- Another mod that blocks equipping in combat (No Combat Equip) still stops the weapon swap itself; turn it off to switch weapons in combat.

### Known
- Unchanged by design (the author's ruling, 2026-10-13): a weapon equipped while a loadout is active belongs to that loadout and is stored with it on a switch. To use it with another loadout, take it out of the first loadout into the inventory first.

## 1.0.2 - 2026-10-04 - working

- New: Always worn. The bar gains a last box, "Always worn", for gear that stays on through every loadout switch - a
  ring or necklace worn with every outfit (asked for on the Nexus page). Select it, wear what should always be worn,
  and leave it: whatever is worn then is the set. No switch stores or unequips those pieces. A loadout's own piece
  for the same slot wins; the always-worn piece then waits in the inventory and goes back on after the next switch
  that frees the slot. A copy of an always-worn piece stored in a loadout stays in storage while the worn one is on.
  The set (item and enchantment, so an enchanted ring is told from a plain one) is kept in the SKSE co-save.
- The bar's text and the mod's messages are translated into the eleven languages (Interface/Translations), following
  the game's language. A button name too long for its box is drawn smaller instead of running out of it.
- DevBench "contents" lists the always-worn set.
- Tested in game (SE line, Njordlinger): boots and a ring as always worn, a loadout's own boots and ring winning the
  slot and the always-worn piece going back on, and the set surviving a save and reload. The 1.7 line is built from the
  same source and was not run.

## 1.0.1 - 2026-09-27 - working

- Fixed a crash when switching back to a loadout. Bringing gear out of a loadout's storage equipped it through the
  item's old data, which the game can merge or free while it moves the item; the crash came on a later switch. Each
  piece is now found again in your inventory after the move (by its enchantment, tempering and name) and equipped from
  there. Tested: five round trips with a full armour set and a weapon, no crash.

## 1.0.0 - 2026-09-27 - working

First version.
- A loadout bar at the top of SkyUI's inventory, drawn at runtime - SkyUI's files are never edited. The search box and
  column button move to a row under it; the categories and the item list move down one row, and the list shows one
  row fewer so it stays inside its frame.
- Controller: D-pad Up on the item list's top row (or with nothing highlighted) reaches the bar; Left / Right choose a
  loadout; A selects it, or deselects the active one; D-pad Down or B returns to the list. Keyboard: arrow keys, Enter
  or E, Esc. Every other press reaches SkyUI untouched.
- Selecting a loadout takes off everything worn (it stays in the inventory), then brings the loadout's gear out of its
  storage and equips it, weapons in the hand they were in. Whatever is equipped while a loadout is active is the
  loadout; whatever is unequipped leaves it. Switching away stores everything worn in that loadout's storage, out of
  the inventory. The open inventory updates at once.
- One storage container per loadout (ten), in SimpleLoadoutSystemForController.esp: non-respawning, persistent,
  player-owned, in a cell whose encounter zone never resets. Items keep their own data (enchantments, tempering,
  names).
- Loadout gear is armour, clothing, jewellery, weapons, shields and ammunition. Quest items never leave the inventory;
  items other mods equip out of sight (non-playable or nameless) are never touched. No switching in combat.
- The active loadout and each loadout's left-hand weapons are kept in the SKSE co-save.
- INI: iLoadoutCount (1-10, default 5), sLoadoutName1-10, uLogLevel (info). A self-check report is written beside
  the log.
- Builds for SE/AE (CommonLibSSE-NG 3.7) and 1.7 (CommonLibSSE-NG 7.2), chosen in the installer.
- Tested in game on the SE/AE line with a controller: switching, storing and restoring (including a left-hand weapon),
  deselecting, and a save and reload in a loadout. The 1.7 line is built from the same source and was not run.
