Simple Loadout System for Controller
====================================
Version 1.0.0

Loadouts at the top of SkyUI's inventory, built for the controller. Press Up on the first row of the item list to
reach the loadout bar, pick a loadout, and everything you equip while it is active becomes that loadout. Switch to
another loadout - or back to none - and the gear you were wearing goes into that loadout's own storage, out of your
inventory and your carry weight. Pick it again and it all comes back and is equipped.


HOW IT WORKS
------------
- A row of loadout buttons (five by default) sits at the top of the inventory. The search box moves to a row under
  it; the categories and the item list move down one row.
- Controller: D-pad Up on the list's top row goes to the bar. Left / Right choose a loadout, A selects it (A on the
  active loadout deselects it), D-pad Down or B goes back to the list. Keyboard: the arrow keys, Enter and Esc.
- Selecting a loadout takes off everything you are wearing. Those items stay in your inventory - they belonged to no
  loadout. If the loadout already holds gear, it comes out of storage and is equipped, weapons in the hand they were
  in.
- While a loadout is active, whatever you equip is part of it, and whatever you unequip leaves it and is an ordinary
  inventory item again, ready for another loadout.
- Switching away stores everything you are wearing in that loadout's storage. The inventory itself is the loadout
  view: a loadout's gear is in your inventory only while that loadout is active.
- Armour, clothing, jewellery, weapons, shields and ammunition are loadout gear. Spells, shouts, powers and torches
  are left alone. Quest items never leave your inventory. Items that other mods equip out of sight are never touched.
- Each loadout has its own storage container, placed by the plugin in a cell of its own: the containers never
  respawn and the cell never resets, so stored gear is safe.
- You cannot change loadouts in combat.


SETTINGS
--------
Data/SKSE/Plugins/SimpleLoadoutSystemForController.ini (edit it with the game closed):
- iLoadoutCount   how many loadout buttons (1-10, default 5)
- sLoadoutName1-10   the buttons' names ("Loadout N" when empty)
- uLogLevel   log detail (2 = info)


REQUIREMENTS
------------
- SKSE64
- Address Library for SKSE Plugins
- SkyUI

Skyrim SE 1.5.97, AE 1.6.x, and 1.7.x (a separate 1.7 build, chosen in the installer).


BUILDING
--------
Visual Studio 2022 with the C++ workload, and VCPKG_ROOT pointing at a vcpkg checkout.
- SE/AE line: configure.bat, then build.bat (CommonLibSSE-NG 3.7).
- 1.7 line: configure17.bat, then build17.bat (CommonLibSSE-NG 7.2).
tools/build-esp.py writes SimpleLoadoutSystemForController.esp.


LICENCE
-------
GPL-3.0-or-later (LICENSE, NOTICE.md). Components under other licences: THIRD_PARTY_NOTICES.md.
