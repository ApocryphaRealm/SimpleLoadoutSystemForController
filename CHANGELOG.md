# Changelog - Simple Loadout System for Controller

Every version, beside the code it describes. Status is the version ledger's word for the build.

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
