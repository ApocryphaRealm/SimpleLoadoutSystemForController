#pragma once

// The game's input dispatch (BSInputDeviceManager::PollInputDevices -> DispatchInputEvent), the path Back Pocket for
// Controller proved: the event list is edited before any menu - SkyUI included - sees it. Up from the item list's first
// row moves onto the loadout bar; while the bar has focus its D-pad / arrow / accept / back presses are the bar's.
namespace InputHook
{
	bool Install();
}
