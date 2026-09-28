#include "Loadouts.h"

#include "SelfCheck.h"
#include "Settings.h"
#include "utils/Logger.h"

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

		std::array<RE::TESObjectREFR*, settings::kMaxLoadouts> g_storage{};
		int g_found = 0;
		std::atomic<int> g_active{ -1 };
		std::atomic<bool> g_busy{ false };
		std::mutex g_lock;
		std::array<std::set<RE::FormID>, settings::kMaxLoadouts> g_leftHand;   // weapons each loadout had in the left hand

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

		void Notify(const std::string& a_text)
		{
#if RUNTIME_LINE == 17
			RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), nullptr, true);   // CommonLibSSE-NG 7.x has no RE::DebugNotification
#else
			RE::DebugNotification(a_text.c_str());
#endif
		}

		std::string Name(int a_loadout)
		{
			const auto& names = settings::Get().names;
			return a_loadout >= 0 && a_loadout < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(a_loadout)]
																				   : std::format("Loadout {}", a_loadout + 1);
		}

		// Everything worn goes: into a_into's container when given (the loadout being left), otherwise it only comes
		// off and stays in the inventory (it belonged to no loadout). Quest items never leave the inventory.
		int StoreWorn(RE::PlayerCharacter* a_player, int a_into, int& a_questKept)
		{
			auto* equip = RE::ActorEquipManager::GetSingleton();
			RE::TESObjectREFR* storage = a_into >= 0 ? g_storage[static_cast<std::size_t>(a_into)] : nullptr;
			std::set<RE::FormID> left;
			int moved = 0;
			for (const auto& w : WornItems(a_player)) {
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
			int restored = 0;
			bool leftUsed = false;
			for (const auto& p : pieces) {
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

		void Switch(int a_to)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				g_busy = false;
				return;
			}
			const int from = g_active.load();
			int questKept = 0;
			const int stored = StoreWorn(player, from, questKept);
			const int restored = a_to >= 0 ? Restore(player, a_to) : 0;
			g_active = a_to;
			// The open inventory rebuilds its list from the engine's data (the inventory update message alone left
			// SkyUI showing stored items, 2026-09-27).
			if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME)) {
				if (auto menu = ui->GetMenu<RE::InventoryMenu>(); menu && menu->GetRuntimeData().itemList) {
					menu->GetRuntimeData().itemList->Update(player);
				}
			}
			RE::SendUIMessage::SendInventoryUpdateMessage(player, nullptr);
			logger::info("switch {} -> {}: {} piece(s) stored, {} restored{}", from >= 0 ? Name(from) : "none", a_to >= 0 ? Name(a_to) : "none",
						 stored, restored, questKept ? std::format(", {} quest item(s) kept in the inventory", questKept) : "");
			if (questKept) { Notify(std::format("{} quest item(s) cannot be stored and stay in your inventory", questKept)); }
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
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			g_active = -1;
			std::scoped_lock l(g_lock);
			for (auto& s : g_leftHand) { s.clear(); }
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			OnRevert(a_intfc);
			std::uint32_t type, version, length;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type == kActive && version == 1) {
					std::int32_t active = -1;
					a_intfc->ReadRecordData(active);
					g_active = (active >= -1 && active < settings::kMaxLoadouts) ? active : -1;
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
				}
			}
			logger::info("co-save read: active loadout {}", g_active.load() >= 0 ? Name(g_active.load()) : "none");
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
			Notify(a_why);
			return false;
		}
		if (player->IsInCombat()) {
			a_why = "in combat";
			Notify("You cannot change loadouts in combat");
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
		const float weight = player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kInventoryWeight);
		return std::format(R"({{"ok":true,"op":"contents","active":{},"inventoryWeight":{:.1f},"worn":[{}],"storage":[{}]}})", g_active.load(), weight, worn, storage);
	}
}
