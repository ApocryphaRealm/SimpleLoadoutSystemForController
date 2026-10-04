#include "Loadouts.h"

#include "SelfCheck.h"
#include "Settings.h"
#include "utils/Logger.h"
#include "utils/Strings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <set>
#include <vector>

#undef GetObject   // Windows.h maps it to GetObjectW, which hides InventoryEntryData::GetObject

namespace loadouts
{
	namespace
	{
		inline constexpr const char* kPlugin = "SimpleLoadoutSystemForController.esp";
		inline constexpr RE::FormID kFirstContainer = 0x803;   // build-esp.py: REFR 0x803 + slot
		inline constexpr RE::FormID kRightHand = 0x13F42, kLeftHand = 0x13F43;   // Skyrim.esm equip slots

		inline constexpr std::uint32_t kRecord = 'SLSC';
		inline constexpr std::uint32_t kActive = 'ACTV';
		inline constexpr std::uint32_t kLeft = 'LEFT';
		inline constexpr std::uint32_t kKeep = 'KEEP';
		inline constexpr std::uint32_t kMaxKept = 64;

		// One always-worn piece: the item and its enchantment (0 for none) - so a player-enchanted Gold Ring is told
		// apart from a plain one, while tempering does not matter. Which hand a weapon was in is kept for putting it back.
		struct Kept
		{
			RE::FormID object = 0;
			RE::FormID enchantment = 0;
			bool left = false;
		};

		std::array<RE::TESObjectREFR*, settings::kMaxLoadouts> g_storage{};
		int g_found = 0;
		std::atomic<int> g_active{ -1 };
		std::atomic<bool> g_busy{ false };
		std::mutex g_lock;
		std::array<std::set<RE::FormID>, settings::kMaxLoadouts> g_leftHand;   // weapons each loadout had in the left hand
		std::vector<Kept> g_kept;                                                 // the always-worn set (g_lock)

		struct Worn
		{
			RE::TESBoundObject* object;
			RE::ExtraDataList* extra;   // the item's own data (enchantment, tempering, name) - nullptr for plain stacks
			std::int32_t count;
			bool left;
			bool quest;
		};

		bool IsEquipment(const RE::TESBoundObject* a_object)
		{
			// The owner's rule (plan default 2): armour, clothing, jewellery, weapons, shields, ammunition. Torches
			// (LIGH), spells, shouts and powers are left alone.
			// Only what the player can see in the inventory: non-playable or nameless items are other mods' hidden
			// worn pieces (TNG's cover, body and skin items) - a switch stored one on 2026-09-27; they are never touched.
			if (!a_object || !(a_object->IsArmor() || a_object->IsWeapon() || a_object->IsAmmo())) { return false; }
			const char* name = a_object->GetName();
			return a_object->GetPlayable() && name && *name;
		}

		std::vector<Worn> WornItems(RE::Actor* a_actor)
		{
			std::vector<Worn> out;
			auto inventory = a_actor->GetInventory([](RE::TESBoundObject& a_object) { return IsEquipment(&a_object); });
			for (auto& [object, data] : inventory) {
				auto& [count, entry] = data;
				if (!entry || !entry->extraLists) { continue; }
				const bool quest = entry->IsQuestObject();
				for (auto* extra : *entry->extraLists) {
					if (!extra) { continue; }
					const bool right = extra->HasType(RE::ExtraDataType::kWorn);
					const bool left = extra->HasType(RE::ExtraDataType::kWornLeft);
					if (!right && !left) { continue; }
					// Ammunition is worn as a whole stack; everything else one piece per list.
					const std::int32_t n = object->IsAmmo() ? count : std::max(1, extra->GetCount());
					out.push_back({ object, extra, n, left, quest });
				}
			}
			return out;
		}

		const RE::BGSEquipSlot* Slot(bool a_left)
		{
			return RE::TESForm::LookupByID<RE::BGSEquipSlot>(a_left ? kLeftHand : kRightHand);
		}

		RE::FormID EnchantmentOf(const RE::ExtraDataList* a_extra)
		{
			const auto* e = a_extra ? a_extra->GetByType<RE::ExtraEnchantment>() : nullptr;
			return e && e->enchantment ? e->enchantment->GetFormID() : 0;
		}

		bool IsKept(const RE::TESBoundObject* a_object, const RE::ExtraDataList* a_extra)
		{
			const RE::FormID object = a_object->GetFormID();
			const RE::FormID enchantment = EnchantmentOf(a_extra);
			std::scoped_lock l(g_lock);
			return std::ranges::any_of(g_kept, [&](const Kept& k) { return k.object == object && k.enchantment == enchantment; });
		}

		std::uint32_t SlotBits(const RE::TESObjectARMO* a_armor)
		{
#if RUNTIME_LINE == 17
			return a_armor->GetSlotMask().underlying();   // CommonLibSSE-NG 7.x returns an EnumSet
#else
			return static_cast<std::uint32_t>(a_armor->GetSlotMask());
#endif
		}

		void Notify(const std::string& a_text)
		{
#if RUNTIME_LINE == 17
			RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), nullptr, true);   // CommonLibSSE-NG 7.x has no RE::DebugNotification
#else
			RE::DebugNotification(a_text.c_str());
#endif
		}

		// A translated text with its "{}" filled in. Done by hand, not std::format: a translation file that mangled the
		// braces would otherwise throw on the main thread.
		std::string Fill(const char* a_text, const std::string& a_value)
		{
			std::string s = a_text ? a_text : "";
			if (const auto at = s.find("{}"); at != std::string::npos) { s.replace(at, 2, a_value); }
			return s;
		}

		std::string Name(int a_loadout)
		{
			if (a_loadout == kAlwaysWorn) { return "Always worn"; }   // log text: a tool reads it, so it stays English
			if (a_loadout < 0) { return "none"; }
			const auto& names = settings::Get().names;
			return a_loadout >= 0 && a_loadout < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(a_loadout)]
																				   : std::format("Loadout {}", a_loadout + 1);
		}

		// Everything worn goes: into a_into's container when given (the loadout being left), otherwise it only comes
		// off and stays in the inventory (it belonged to no loadout). Quest items never leave the inventory. Always-worn
		// pieces stay on.
		int StoreWorn(RE::PlayerCharacter* a_player, int a_into, int& a_questKept)
		{
			auto* equip = RE::ActorEquipManager::GetSingleton();
			RE::TESObjectREFR* storage = a_into >= 0 ? g_storage[static_cast<std::size_t>(a_into)] : nullptr;
			std::set<RE::FormID> left;
			int moved = 0;
			for (const auto& w : WornItems(a_player)) {
				if (IsKept(w.object, w.extra)) {
					logger::debug("store: {:08X} \"{}\" is always worn - left on", w.object->GetFormID(), w.object->GetName());
					continue;
				}
				if (storage && !w.quest) {
					if (w.left && w.object->IsWeapon()) { left.insert(w.object->GetFormID()); }
					// Removing a worn item unequips it; the item keeps its own ExtraDataList.
					a_player->RemoveItem(w.object, w.count, RE::ITEM_REMOVE_REASON::kStoreInContainer, w.object->IsAmmo() ? nullptr : w.extra, storage);
					++moved;
				} else {
					if (w.quest && storage) { ++a_questKept; }
					// A hand slot only for the left hand; the engine's default covers the right hand and two-handers.
					if (equip) { equip->UnequipObject(a_player, w.object, w.object->IsAmmo() ? nullptr : w.extra, w.count, w.object->IsWeapon() && w.left ? Slot(true) : nullptr, false, false, true, true); }
				}
			}
			if (a_into >= 0) {
				std::scoped_lock l(g_lock);
				g_leftHand[static_cast<std::size_t>(a_into)] = std::move(left);
			}
			return moved;
		}

		// What tells one copy of an item from another: a piece with none of these is equipped as a plain item.
		struct Identity
		{
			bool special = false;
			const RE::EnchantmentItem* enchantment = nullptr;
			float health = 1.0F;
			std::string name;
		};

		Identity IdentityOf(RE::TESBoundObject* a_object, RE::ExtraDataList* a_extra)
		{
			Identity id;
			if (!a_extra) { return id; }
			if (const auto* e = a_extra->GetByType<RE::ExtraEnchantment>(); e && e->enchantment) {
				id.enchantment = e->enchantment;
				id.special = true;
			}
			if (const auto* h = a_extra->GetByType<RE::ExtraHealth>(); h) {
				id.health = h->health;
				id.special = id.special || h->health != 1.0F;
			}
			if (a_extra->HasType(RE::ExtraDataType::kTextDisplayData)) {
				const char* n = a_extra->GetDisplayName(a_object);
				id.name = n ? n : "";
				id.special = true;
			}
			return id;
		}

		// The same piece, now in the player's inventory and not worn - or nullptr for a plain one.
		RE::ExtraDataList* FindInInventory(RE::PlayerCharacter* a_player, RE::TESBoundObject* a_object, const Identity& a_id)
		{
			if (!a_id.special) { return nullptr; }
			auto* changes = a_player->GetInventoryChanges();
			if (!changes || !changes->entryList) { return nullptr; }
			for (auto* entry : *changes->entryList) {
				if (!entry || entry->object != a_object || !entry->extraLists) { continue; }
				for (auto* x : *entry->extraLists) {
					if (!x || x->HasType(RE::ExtraDataType::kWorn) || x->HasType(RE::ExtraDataType::kWornLeft)) { continue; }
					const Identity other = IdentityOf(a_object, x);
					if (other.enchantment == a_id.enchantment && other.health == a_id.health && other.name == a_id.name) { return x; }
				}
			}
			return nullptr;
		}

		// The loadout's container empties into the inventory and every piece is equipped; weapons go back to the hand
		// they were in.
		int Restore(RE::PlayerCharacter* a_player, int a_from)
		{
			auto* storage = g_storage[static_cast<std::size_t>(a_from)];
			auto* equip = RE::ActorEquipManager::GetSingleton();
			if (!storage || !equip) { return 0; }
			std::set<RE::FormID> left;
			{
				std::scoped_lock l(g_lock);
				left = g_leftHand[static_cast<std::size_t>(a_from)];
			}
			struct Piece
			{
				RE::TESBoundObject* object;
				RE::ExtraDataList* extra;
				std::int32_t count;
			};
			std::vector<Piece> pieces;
			auto contents = storage->GetInventory();
			for (auto& [object, data] : contents) {
				auto& [count, entry] = data;
				if (count <= 0 || !object) { continue; }
				std::int32_t listed = 0;
				if (entry && entry->extraLists) {
					for (auto* extra : *entry->extraLists) {
						if (!extra) { continue; }
						const std::int32_t n = std::max(1, extra->GetCount());
						pieces.push_back({ object, extra, n });
						listed += n;
					}
				}
				if (count > listed) { pieces.push_back({ object, nullptr, count - listed }); }
			}
			// A copy of an always-worn piece that is already on stays in storage: equipping it would push the worn copy
			// off, and the next switch would then leave it on (it is always worn) and drop it out of this loadout.
			std::set<std::pair<RE::FormID, RE::FormID>> keptOn;
			for (const auto& w : WornItems(a_player)) {
				if (IsKept(w.object, w.extra)) { keptOn.insert({ w.object->GetFormID(), EnchantmentOf(w.extra) }); }
			}
			int restored = 0;
			bool leftUsed = false;
			for (const auto& p : pieces) {
				if (keptOn.contains({ p.object->GetFormID(), EnchantmentOf(p.extra) })) {
					logger::debug("restore: {:08X} \"{}\" is always worn and already on - left in storage", p.object->GetFormID(), p.object->GetName());
					continue;
				}
				// The move may merge or free the ExtraDataList it is given, so p.extra is never used after RemoveItem
				// (1.0.0 equipped through it and crashed on a later switch, 2026-09-27). What makes the piece itself -
				// its enchantment, tempering and name - is noted first, and the same piece is found again in the
				// player's inventory after the move.
				const Identity id = IdentityOf(p.object, p.extra);
				storage->RemoveItem(p.object, p.count, RE::ITEM_REMOVE_REASON::kStoreInContainer, p.extra, a_player);
				const RE::BGSEquipSlot* slot = nullptr;
				if (p.object->IsWeapon()) {
					const bool wantLeft = !leftUsed && left.contains(p.object->GetFormID());
					leftUsed = leftUsed || wantLeft;
					slot = wantLeft ? Slot(true) : nullptr;   // the default slot covers the right hand and two-handers
				}
				equip->EquipObject(a_player, p.object, FindInInventory(a_player, p.object, id), p.count, slot, false, false, true, true);
				++restored;
			}
			return restored;
		}

		// Leaving the "Always worn" box: whatever is worn now is the set.
		int CaptureKept(RE::PlayerCharacter* a_player)
		{
			std::vector<Kept> kept;
			for (const auto& w : WornItems(a_player)) {
				const Kept k{ w.object->GetFormID(), EnchantmentOf(w.extra), w.left && w.object->IsWeapon() };
				const bool dup = std::ranges::any_of(kept, [&](const Kept& o) { return o.object == k.object && o.enchantment == k.enchantment; });
				if (dup || kept.size() >= kMaxKept) { continue; }
				kept.push_back(k);
				logger::debug("always worn: {:08X} \"{}\" (enchantment {:08X}{})", k.object, w.object->GetName(), k.enchantment, k.left ? ", left hand" : "");
			}
			const int n = static_cast<int>(kept.size());
			std::scoped_lock l(g_lock);
			g_kept = std::move(kept);
			return n;
		}

		// The piece of a_kept in the inventory and not worn: its ExtraDataList, nullptr for a plain one (a_found says
		// whether there was one at all), and how many to equip.
		RE::ExtraDataList* FindKept(RE::PlayerCharacter* a_player, RE::TESBoundObject* a_object, const Kept& a_kept, bool& a_found, std::int32_t& a_count)
		{
			a_found = false;
			a_count = 1;
			auto inventory = a_player->GetInventory([a_object](RE::TESBoundObject& a_o) { return &a_o == a_object; });
			for (auto& [object, data] : inventory) {
				auto& [count, entry] = data;
				if (count <= 0) { continue; }
				std::int32_t listed = 0;
				if (entry && entry->extraLists) {
					for (auto* x : *entry->extraLists) {
						if (!x) { continue; }
						listed += std::max(1, x->GetCount());
						if (x->HasType(RE::ExtraDataType::kWorn) || x->HasType(RE::ExtraDataType::kWornLeft)) { continue; }
						if (EnchantmentOf(x) != a_kept.enchantment) { continue; }
						a_found = true;
						a_count = object->IsAmmo() ? count : 1;
						return object->IsAmmo() ? nullptr : x;
					}
				}
				if (a_kept.enchantment == 0 && count > listed) {   // a plain copy with no data of its own
					a_found = true;
					a_count = object->IsAmmo() ? count : 1;
					return nullptr;
				}
			}
			return nullptr;
		}

		// After every switch: each always-worn piece that is in the inventory and not on goes back on, unless the slot
		// is taken - then the loadout's piece wins (the owner, 2026-10-04) and it waits for a switch that frees the slot.
		int ReapplyKept(RE::PlayerCharacter* a_player)
		{
			auto* equip = RE::ActorEquipManager::GetSingleton();
			if (!equip) { return 0; }
			std::vector<Kept> kept;
			{
				std::scoped_lock l(g_lock);
				kept = g_kept;
			}
			int reapplied = 0;
			for (const auto& k : kept) {
				auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(k.object);
				if (!object) {
					logger::debug("always worn: {:08X} no longer exists - skipped", k.object);
					continue;
				}
				const auto worn = WornItems(a_player);
				if (std::ranges::any_of(worn, [&](const Worn& w) { return w.object == object && EnchantmentOf(w.extra) == k.enchantment; })) { continue; }
				bool taken = false;
				if (const auto* armor = object->As<RE::TESObjectARMO>()) {
					const std::uint32_t bits = SlotBits(armor);
					taken = std::ranges::any_of(worn, [&](const Worn& w) {
						const auto* other = w.object->As<RE::TESObjectARMO>();
						return other && (SlotBits(other) & bits) != 0;
					});
				} else if (object->IsWeapon()) {
					taken = a_player->GetEquippedObject(k.left) != nullptr;
				} else if (object->IsAmmo()) {
					taken = a_player->GetCurrentAmmo() != nullptr;
				}
				if (taken) {
					logger::debug("always worn: {:08X} \"{}\" - its slot holds the loadout's piece, left off", k.object, object->GetName());
					continue;
				}
				bool found = false;
				std::int32_t count = 1;
				auto* extra = FindKept(a_player, object, k, found, count);
				if (!found) {
					logger::debug("always worn: {:08X} \"{}\" is not in the inventory - skipped", k.object, object->GetName());
					continue;
				}
				const RE::BGSEquipSlot* slot = object->IsWeapon() && k.left ? Slot(true) : nullptr;
				equip->EquipObject(a_player, object, extra, count, slot, false, false, true, true);
				logger::debug("always worn: {:08X} \"{}\" put back on", k.object, object->GetName());
				++reapplied;
			}
			return reapplied;
		}

		void Switch(int a_to)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				g_busy = false;
				return;
			}
			const int from = g_active.load();
			int keptCount = -1;
			if (from == kAlwaysWorn) { keptCount = CaptureKept(player); }
			int questKept = 0;
			const int stored = StoreWorn(player, from >= 0 ? from : -1, questKept);
			const int restored = a_to >= 0 ? Restore(player, a_to) : 0;
			const int reapplied = ReapplyKept(player);
			g_active = a_to;
			// The open inventory rebuilds its list from the engine's data (the inventory update message alone left
			// SkyUI showing stored items, 2026-09-27).
			if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) {
				if (auto menu = ui->GetMenu<RE::InventoryMenu>(); menu && menu->GetRuntimeData().itemList) {
					menu->GetRuntimeData().itemList->Update(player);
				}
			}
			RE::SendUIMessage::SendInventoryUpdateMessage(player, nullptr);
			logger::info("switch {} -> {}: {} piece(s) stored, {} restored, {} always-worn put back on{}{}", Name(from), Name(a_to), stored, restored,
						 reapplied, keptCount >= 0 ? std::format(", always-worn set is now {} piece(s)", keptCount) : "",
						 questKept ? std::format(", {} quest item(s) kept in the inventory", questKept) : "");
			if (keptCount >= 0) { Notify(Fill(strings::TR("SLSC_AlwaysWornSet", "Always worn: {} item(s)"), std::to_string(keptCount))); }
			if (questKept) { Notify(Fill(strings::TR("SLSC_QuestKept", "{} quest item(s) cannot be stored and stay in your inventory"), std::to_string(questKept))); }
			g_busy = false;
		}

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			if (!a_intfc->OpenRecord(kActive, 1)) { return; }
			const std::int32_t active = g_active.load();
			a_intfc->WriteRecordData(active);
			std::scoped_lock l(g_lock);
			for (std::uint32_t i = 0; i < settings::kMaxLoadouts; ++i) {
				if (g_leftHand[i].empty()) { continue; }
				if (!a_intfc->OpenRecord(kLeft, 1)) { continue; }
				const std::uint32_t n = static_cast<std::uint32_t>(g_leftHand[i].size());
				a_intfc->WriteRecordData(i);
				a_intfc->WriteRecordData(n);
				for (const auto id : g_leftHand[i]) { a_intfc->WriteRecordData(id); }
			}
			if (!g_kept.empty() && a_intfc->OpenRecord(kKeep, 1)) {
				const std::uint32_t n = static_cast<std::uint32_t>(g_kept.size());
				a_intfc->WriteRecordData(n);
				for (const auto& k : g_kept) {
					const std::uint8_t left = k.left ? 1 : 0;
					a_intfc->WriteRecordData(k.object);
					a_intfc->WriteRecordData(k.enchantment);
					a_intfc->WriteRecordData(left);
				}
			}
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			g_active = -1;
			std::scoped_lock l(g_lock);
			for (auto& s : g_leftHand) { s.clear(); }
			g_kept.clear();
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			OnRevert(a_intfc);
			std::uint32_t type, version, length;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type == kActive && version == 1) {
					std::int32_t active = -1;
					a_intfc->ReadRecordData(active);
					g_active = (active == kAlwaysWorn || (active >= -1 && active < settings::kMaxLoadouts)) ? active : -1;
				} else if (type == kLeft && version == 1) {
					std::uint32_t slot = 0, n = 0;
					a_intfc->ReadRecordData(slot);
					a_intfc->ReadRecordData(n);
					std::set<RE::FormID> ids;
					for (std::uint32_t k = 0; k < n && k < 64; ++k) {
						RE::FormID id = 0, resolved = 0;
						a_intfc->ReadRecordData(id);
						if (a_intfc->ResolveFormID(id, resolved)) { ids.insert(resolved); }
					}
					if (slot < settings::kMaxLoadouts) {
						std::scoped_lock l(g_lock);
						g_leftHand[slot] = std::move(ids);
					}
				} else if (type == kKeep && version == 1) {
					std::uint32_t n = 0;
					a_intfc->ReadRecordData(n);
					std::vector<Kept> kept;
					for (std::uint32_t k = 0; k < n && k < kMaxKept; ++k) {
						RE::FormID object = 0, enchantment = 0;
						std::uint8_t left = 0;
						a_intfc->ReadRecordData(object);
						a_intfc->ReadRecordData(enchantment);
						a_intfc->ReadRecordData(left);
						Kept item{ 0, 0, left != 0 };
						// An item or enchantment whose plugin is gone drops out of the set.
						if (!a_intfc->ResolveFormID(object, item.object)) { continue; }
						if (enchantment != 0 && !a_intfc->ResolveFormID(enchantment, item.enchantment)) { continue; }
						kept.push_back(item);
					}
					std::scoped_lock l(g_lock);
					g_kept = std::move(kept);
				}
			}
			std::size_t kept;
			{
				std::scoped_lock l(g_lock);
				kept = g_kept.size();
			}
			logger::info("co-save read: active {}, {} always-worn piece(s)", Name(g_active.load()), kept);
		}
	}

	void Init()
	{
		auto* data = RE::TESDataHandler::GetSingleton();
		g_found = 0;
		for (int i = 0; i < settings::kMaxLoadouts; ++i) {
			g_storage[static_cast<std::size_t>(i)] = data ? data->LookupForm<RE::TESObjectREFR>(kFirstContainer + i, kPlugin) : nullptr;
			if (g_storage[static_cast<std::size_t>(i)]) { ++g_found; }
		}
		const bool ok = g_found == settings::kMaxLoadouts;
		logger::info("storage: {} of {} containers found in {}", g_found, settings::kMaxLoadouts, kPlugin);
		SelfCheck::Set("Storage", ok, ok ? std::format("{} containers in {}", g_found, kPlugin)
										 : std::format("only {} of {} containers - is {} enabled?", g_found, settings::kMaxLoadouts, kPlugin));
	}

	void RegisterSerialization()
	{
		auto* s = SKSE::GetSerializationInterface();
		if (!s) {
			SelfCheck::Set("Co-save", false, "no serialization interface");
			return;
		}
		s->SetUniqueID(kRecord);
		s->SetSaveCallback(OnSave);
		s->SetLoadCallback(OnLoad);
		s->SetRevertCallback(OnRevert);
		SelfCheck::Set("Co-save", true, "registered");
	}

	int Active() { return g_active.load(); }

	int StorageReady() { return g_found; }

	bool Request(int a_loadout, std::string& a_why)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			a_why = "no player";
			return false;
		}
		if (g_found < settings::Get().count) {
			a_why = std::format("the loadout storage is missing - is {} enabled?", kPlugin);
			Notify(Fill(strings::TR("SLSC_StorageMissing", "The loadout storage is missing - is {} enabled?"), kPlugin));
			return false;
		}
		if (player->IsInCombat()) {
			a_why = "in combat";
			Notify(strings::TR("SLSC_InCombat", "You cannot change loadouts in combat"));
			return false;
		}
		if (g_busy.exchange(true)) {
			a_why = "a switch is already running";
			return false;
		}
		SKSE::GetTaskInterface()->AddTask([a_loadout] { Switch(a_loadout); });
		return true;
	}

	std::string ContentsJson()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) { return R"({"ok":false,"error":"no player"})"; }
		auto quote = [](const char* a_s) {
			std::string out = "\"";
			for (const char* p = a_s ? a_s : ""; *p; ++p) {
				if (*p == '"' || *p == '\\') { out += '\\'; }
				if (static_cast<unsigned char>(*p) >= 0x20) { out += *p; }
			}
			return out + "\"";
		};
		std::string worn;
		for (const auto& w : WornItems(player)) {
			const char* name = w.extra ? w.extra->GetDisplayName(w.object) : nullptr;
			worn += std::format("{}{{\"name\":{},\"formId\":\"{:08X}\",\"count\":{},\"left\":{},\"extra\":{}}}", worn.empty() ? "" : ",",
								quote(name && *name ? name : w.object->GetName()), w.object->GetFormID(), w.count, w.left, w.extra != nullptr);
		}
		std::string storage;
		for (int i = 0; i < settings::Get().count; ++i) {
			std::string items;
			if (auto* c = g_storage[static_cast<std::size_t>(i)]) {
				for (auto& [object, data] : c->GetInventory()) {
					if (data.first <= 0) { continue; }
					items += std::format("{}{{\"name\":{},\"count\":{}}}", items.empty() ? "" : ",", quote(object->GetName()), data.first);
				}
			}
			storage += std::format("{}[{}]", i ? "," : "", items);
		}
		std::string kept;
		{
			std::scoped_lock l(g_lock);
			for (const auto& k : g_kept) {
				const auto* form = RE::TESForm::LookupByID(k.object);
				kept += std::format("{}{{\"name\":{},\"formId\":\"{:08X}\",\"enchantment\":\"{:08X}\",\"left\":{}}}", kept.empty() ? "" : ",",
									quote(form ? form->GetName() : ""), k.object, k.enchantment, k.left);
			}
		}
		const float weight = player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kInventoryWeight);
		return std::format(R"({{"ok":true,"op":"contents","active":{},"inventoryWeight":{:.1f},"worn":[{}],"alwaysWorn":[{}],"storage":[{}]}})", g_active.load(),
						   weight, worn, kept, storage);
	}
}
