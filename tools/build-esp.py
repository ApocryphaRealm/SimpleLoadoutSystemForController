#!/usr/bin/env python3
r"""Author SimpleLoadoutSystemForController.esp - the loadouts' storage, as raw TES4 records (no Creation Kit needed,
byte-for-byte reproducible, reviewable in a diff; the same way HjerimStart's plugin is written).

ONE CONTAINER PER LOADOUT, AND SAFE STORAGE (the owner, 2026-09-27: "Make sure that these containers you're using are
one per loadout and that they are considered safe storage"):

    CONT 0x800  SLSC_StorageChest        the container base - DATA flags 0: NO "Respawns" flag, so its contents are
                                         never reset (the same as a player-home chest)
    ECZN 0x801  SLSC_StorageZone         encounter zone, flag "Never Resets" - the cell never resets either
    CELL 0x802  SLSC_StorageCell         interior holding cell, no doors to it, XEZN -> the zone above
    REFR 0x803 .. 0x80C                  ten PERSISTENT references of the chest, one per loadout slot (1-10), owned by
                                         the player (XOWN 0x7) - persistent refs stay in memory and in the save
                                         whether or not the cell is loaded

Light-flagged (ESPFE), one master (Skyrim.esm), so its own records take on-disk index 01. The DLL finds the containers
with TESDataHandler::LookupForm(0x803 + slot, "SimpleLoadoutSystemForController.esp").

Usage:  python build-esp.py [out.esp]
"""
import os
import struct
import sys

PLUGIN = "SimpleLoadoutSystemForController.esp"
AUTHOR = "ApocryphaRealm"
DESCRIPTION = "Simple Loadout System for Controller - one safe storage container per loadout"
FORM_VERSION = 44   # SSE
LOADOUTS = 10       # the INI allows 1-10; every slot has its container whatever the count is

CHEST = 0x01000800
ZONE = 0x01000801
CELL = 0x01000802
FIRST_REF = 0x01000803
PLAYER_NPC = 0x00000007   # the Player's actor base (Skyrim.esm), the owner of every container


def sub(tag: bytes, data: bytes) -> bytes:
    if len(data) > 0xFFFF:
        raise ValueError("subrecord %s too long" % tag)
    return tag + struct.pack("<H", len(data)) + data


def zstr(s: str) -> bytes:
    return s.encode("cp1252") + b"\x00"


def record(tag: bytes, formid: int, data: bytes, flags: int = 0) -> bytes:
    return (tag + struct.pack("<IIII", len(data), flags, formid, 0) + struct.pack("<HH", FORM_VERSION, 0) + data)


def group(label: bytes, gtype: int, content: bytes) -> bytes:
    return b"GRUP" + struct.pack("<I", 24 + len(content)) + label + struct.pack("<iHHHH", gtype, 0, 0, 0, 0) + content


def u32(v: int) -> bytes:
    return struct.pack("<I", v)


def i32(v: int) -> bytes:
    return struct.pack("<i", v)


def chest() -> bytes:
    body = (sub(b"EDID", zstr("SLSC_StorageChest")) +
            sub(b"OBND", struct.pack("<6h", 0, 0, 0, 0, 0, 0)) +
            sub(b"FULL", zstr("Loadout Storage")) +
            sub(b"DATA", struct.pack("<Bf", 0x00, 0.0)))   # flags 0x00: NOT respawning; weight 0
    return record(b"CONT", CHEST, body)


def zone() -> bytes:
    # DATA: owner(FormID) location(FormID) rank(i8) minLevel(i8) flags(u8) maxLevel(i8)
    #   flags 0x01 = Never Resets
    body = (sub(b"EDID", zstr("SLSC_StorageZone")) +
            sub(b"DATA", struct.pack("<IIbbBb", 0, 0, 0, 0, 0x01, 0)))
    return record(b"ECZN", ZONE, body)


def cell() -> bytes:
    # DATA (SSE, 2 bytes): 0x0001 Is Interior Cell
    body = (sub(b"EDID", zstr("SLSC_StorageCell")) +
            sub(b"FULL", zstr("Loadout Storage")) +
            sub(b"DATA", struct.pack("<H", 0x0001)) +
            sub(b"XEZN", u32(ZONE)))
    return record(b"CELL", CELL, body)


def reference(slot: int) -> bytes:
    body = (sub(b"EDID", zstr("SLSC_LoadoutStorage%02d" % (slot + 1))) +
            sub(b"NAME", u32(CHEST)) +
            sub(b"XOWN", u32(PLAYER_NPC)) +
            sub(b"DATA", struct.pack("<6f", 100.0 * slot, 0.0, 0.0, 0.0, 0.0, 0.0)))
    return record(b"REFR", FIRST_REF + slot, body, flags=0x00000400)   # 0x400 Persistent


def main() -> None:
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "dist", PLUGIN)

    refs = b"".join(reference(i) for i in range(LOADOUTS))
    # Interior cell block / sub-block: the last and second-to-last decimal digits of the cell's object index.
    index = CELL & 0xFFFFFF
    block, subblock = index % 10, (index // 10) % 10
    cell_children = group(u32(CELL), 6, group(u32(CELL), 8, refs))
    cells = group(b"CELL", 0, group(i32(block), 2, group(i32(subblock), 3, cell() + cell_children)))
    body = group(b"CONT", 0, chest()) + group(b"ECZN", 0, zone()) + cells

    records = 3 + LOADOUTS                      # CONT, ECZN, CELL and the references
    groups = 3 + 2 + 2                          # the three top groups, block + sub-block, cell children + persistent
    hedr = sub(b"HEDR", struct.pack("<fII", 1.71, records + groups, FIRST_REF + LOADOUTS & 0xFFFFFF))
    tes4 = record(b"TES4", 0, hedr + sub(b"CNAM", zstr(AUTHOR)) + sub(b"SNAM", zstr(DESCRIPTION)) +
                  sub(b"MAST", zstr("Skyrim.esm")) + sub(b"DATA", struct.pack("<Q", 0)), flags=0x00000200)   # 0x200 light
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(tes4 + body)
    print(f"build-esp: wrote {out} ({len(tes4) + len(body)} bytes): 1 chest, 1 zone, 1 cell, {LOADOUTS} persistent "
          f"containers; cell block {block} sub-block {subblock}")


if __name__ == "__main__":
    main()
