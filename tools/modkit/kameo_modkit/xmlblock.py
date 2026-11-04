"""Reader and writer for Kameo "kameoxmlblock" entries, and the level objects (placed instances) they carry.

A kameoxmlblock is a named table of named binary records ("sections"), built from an XML file by Rare's tools.
xmlBlockInitCallback turns its offsets into addresses at load; xmlGetSection(object, block hash, section hash) finds a
record by walking the xmlblock assets of the object's type. Values are big-endian except the two little-endian header
words at 0x10 and 0x14.

Entry layout (offsets from the entry start; "rel" offsets count from 0x20):
  0x00  "kameoxmlblock\\0" and two leftover bytes (90 7C or 15 00; never read)
  0x10  u32 LE 1, 0x14 u32 LE entry size, 0x18 8 zero bytes
  0x20  u32 block name hash (ELF hash, e.g. "levelObjects", "triggers", "objxml", or the owning type's name)
  0x24  s16 section count n, 0x26 u16 0
  0x28  rel offset of the hash table (always 0x14): n u32 section-name hashes, sorted ascending
  0x2C  rel offset of the record table: n + 1 u32 rel offsets; the last is the end of the last record
  0x30  rel offset of the name table: n u32 rel offsets of NUL-terminated names, packed right after the table
  then the names, zero padding to 4, the records in table order (each but the last a multiple of 4 bytes), and
  zeros to align32(align4(end) + 1): at least one byte past the 4-aligned end, so a block ending on 32 gets 32 more.
There are no CAFF relocations inside a kameoxmlblock, so a rewritten block needs no pointer list.

Level objects: the level type's own "xml\\bin\\levelObjects\\<Level>\\levelObjects.block" asset (block hash
"levelObjects"). Every record is one placed instance, named by its section name; objectLevelInitObjList lists them,
objectLevelCreateObjects creates those flagged to exist at start, triggers create the rest by name or group. Record
(levObjDef), 124 bytes then the type's parameters:
  +0   u32 objrefno: unique id of the instance (net sync, objectLevelGetLevObjDefFromObjrefno)
  +4   char[32] object type, looked up by name in the loaded kameoDatabases (e.g. "BaddieGenerator", "ShadowTroll")
  +36  char[32] AI DLL override, e.g. "AIShadowTrollEnterWell.dll" ("" = the type's own)
  +68  s16 group (-1 none; groups are created and killed together by triggers), +70 u16 0
  +72  f32 position x, y, z (Y up, game units)
  +84  f32 yaw, +88 pitch, +92 roll, in degrees; the object matrix is XMMatrixRotationRollPitchYaw(pitch, yaw, roll)
  +96  f32 uniform scale
  +100 u8 create at level start (1); 0 waits for a trigger. Objects in a group wait for their group either way.
  +104 f32 range (0 = default; > 0 sets the created object's range pair, objectCreateFull a9/a10)
  +108 u32 event the object waits for (-1 none), +112 u32 a second event (-1 none); copied to the object at create
  +116 u8 flags (copied to object +141), +117 3 bytes pad
  +120 f32 health scale (1.0; generators multiply their baddies' hit points by their own)
  +124 params: u32 parameter-set id, then that set's fields (see PARAM_FIELDS)
One record named "empty" (all zero, id 0) must stay: the game sizes its object list as the record count minus one.
PlrSetupPoint records are never created; levelCreatePlayerSetup reads them for spawn positions.

Usage: python -m kameo_modkit.xmlblock <file.lvl> [entry]          (lists the blocks, the objects and the trigger script)
       python -m kameo_modkit.xmlblock --verify <file.lvl>...          (parse -> write every kameoxmlblock, compare)
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass, field

from .caff import Caff, Entry
from .level import elf_hash

MAGIC = b"kameoxmlblock\0"
HEADER_SIZE = 0x20
RECORD_HEADER = 124
NAME_LIMIT = 34          # the object list keeps 35 bytes for a name (objectLevelInitObjList)

LEVEL_OBJECTS = elf_hash("levelObjects")   # 0x02BC76A3
TRIGGERS = elf_hash("triggers")            # 0x08FDDB23
OBJXML = elf_hash("objxml")                # 0x07591F3C
LEVEL_CLASS = elf_hash("level")            # 0x0072CCBC, the class of a level's own type
XMLBLOCK_ASSET = elf_hash("xmlblock")      # 0x0429326B, kameoDatabase asset type of an xml block
BLOCK_NAMES = {LEVEL_OBJECTS: "levelObjects", TRIGGERS: "triggers", OBJXML: "objxml"}
CLASS_NAMES = {elf_hash(n): n for n in ("level", "gameobj", "objdata", "effects", "movie", "simpleobj")}


def _u32(data: bytes, at: int) -> int:
    return struct.unpack_from(">I", data, at)[0]


def _align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def _cstr(data: bytes, at: int, size: int | None = None) -> str:
    end = data.index(b"\0", at) if size is None else at + (data[at:at + size] + b"\0").index(b"\0")
    return data[at:end].decode("latin1")


# ----------------------------------------------------------------------------------------------- the block container

@dataclass
class Section:
    name: str
    data: bytes
    stored_hash: int | None = None    # the hash as stored when it is not elf_hash(name) (never, in shipped data)

    @property
    def hash(self) -> int:
        return elf_hash_latin1(self.name) if self.stored_hash is None else self.stored_hash


def elf_hash_latin1(name: str) -> int:
    value = 0
    for byte in name.encode("latin1"):
        value = ((value << 4) + (byte - 256 if byte >= 128 else byte)) & 0xFFFFFFFF
        top = value & 0xF0000000
        if top:
            value ^= (top >> 24) | top
    return value


@dataclass
class XmlBlock:
    block_hash: int
    sections: list[Section] = field(default_factory=list)
    leftover: bytes = b"\x90\x7c"     # the two bytes after the magic
    version: int = 1                   # u32 LE at 0x10
    reserved: bytes = bytes(8)         # 0x18
    count_pad: int = 0                 # u16 after the section count

    @property
    def name(self) -> str:
        return BLOCK_NAMES.get(self.block_hash, f"{self.block_hash:08X}")

    @classmethod
    def parse(cls, data: bytes) -> "XmlBlock":
        if not data.startswith(MAGIC):
            raise ValueError("not a kameoxmlblock")
        if struct.unpack_from("<I", data, 0x14)[0] != len(data):
            raise ValueError("kameoxmlblock size field does not match the entry")
        block_hash, count, count_pad, hashes, records, names = struct.unpack_from(">IhHIII", data, 0x20)
        base = HEADER_SIZE
        offsets = [_u32(data, base + records + 4 * i) for i in range(count + 1)]
        sections = []
        for i in range(count):
            name = _cstr(data, base + _u32(data, base + names + 4 * i))
            stored = _u32(data, base + hashes + 4 * i)
            body = data[base + offsets[i]:base + offsets[i + 1]]
            sections.append(Section(name, body, None if stored == elf_hash_latin1(name) else stored))
        block = cls(block_hash, sections, data[14:16], struct.unpack_from("<I", data, 0x10)[0], data[0x18:0x20], count_pad)
        if (hashes, records, names) != block._table_offsets():
            raise ValueError("kameoxmlblock tables are not laid out the usual way")
        return block

    def _table_offsets(self) -> tuple[int, int, int]:
        n = len(self.sections)
        return 0x14, 0x14 + 4 * n, 0x14 + 4 * n + 4 * (n + 1)

    def to_bytes(self) -> bytes:
        n = len(self.sections)
        hashes, records, names = self._table_offsets()
        blob = bytearray()
        name_offsets = []
        cursor = names + 4 * n
        for section in self.sections:
            name_offsets.append(cursor + len(blob))
            blob += section.name.encode("latin1") + b"\0"
        cursor = _align(cursor + len(blob), 4)
        body = bytearray()
        record_offsets = []
        for i, section in enumerate(self.sections):
            record_offsets.append(cursor + len(body))
            body += section.data
            if i < n - 1:
                body += bytes(-len(body) % 4)
        record_offsets.append(cursor + len(body))
        out = bytearray(MAGIC + self.leftover)
        out += struct.pack("<I", self.version) + b"\0\0\0\0" + self.reserved
        out += struct.pack(">IhHIII", self.block_hash, n, self.count_pad, hashes, records, names)
        out += b"".join(struct.pack(">I", s.hash) for s in self.sections)
        out += b"".join(struct.pack(">I", o) for o in record_offsets)
        out += b"".join(struct.pack(">I", o) for o in name_offsets)
        out += blob
        out += bytes(HEADER_SIZE + cursor - len(out))
        out += body
        out += bytes(_align(_align(len(out), 4) + 1, 32) - len(out))
        struct.pack_into("<I", out, 0x14, len(out))
        return bytes(out)

    def pointers(self) -> list[int]:
        """CAFF relocation offsets inside the block: none, its offsets are block-relative."""
        return []

    def find(self, name: str) -> Section | None:
        key = elf_hash_latin1(name)
        return next((s for s in self.sections if s.hash == key), None)

    def sort(self) -> None:
        """Sections in ascending hash order, as every shipped block has them."""
        self.sections.sort(key=lambda s: s.hash)


# ----------------------------------------------------------------------------------------------- level objects

# Parameter sets by id (u32 at +124), named after the types that use them; fields are offsets into the params.
PARAM_SETS = {
    0: "none", 1: "baddie", 6: "generator", 8: "hole", 10: "camera", 11: "sound", 13: "skyCloud", 15: "plite",
    19: "platform", 20: "setupPoint", 21: "smashable", 25: "torch", 26: "torchSimple", 27: "emitter", 29: "door",
    30: "glow", 31: "grabPlant", 32: "iceDragon", 33: "rockGenerator", 36: "totemGate", 38: "vtxGrass",
    39: "vtxMote", 42: "sign", 44: "pickup", 47: "vtxSpawner", 48: "slinky", 49: "bombPlant", 51: "trollBoat",
    52: "rumble", 54: "spikeBlock", 55: "tribe", 58: "wildlife", 61: "zombieGrave", 62: "dragon", 63: "vine",
    64: "airship", 66: "screen", 67: "battleTank", 69: "catapult", 70: "fireCatapult", 71: "battleController",
    72: "warriorFiller", 73: "imposter", 75: "drapes",
}

# name: (offset, struct format); "s" fields are NUL-padded strings.
PARAM_FIELDS: dict[int, dict[str, tuple[int, str]]] = {
    20: {   # PlrSetupPoint (levelCreatePlayerSetup, getSetupPointOrientation)
        "flag": (4, "B"),            # copied to byte_82B71991 when the point is used
        "setup": (5, "B"),           # the setup number a level change asks for
        "player": (6, "B"),          # 0 player one, 1 player two (co-op); 2 seen once
    },
    6: {    # BaddieGenerator and the other generator types (baddieGeneratorInit, _baddieGenerator_tick, baddieGenMakeBaddie)
        "baddie": (4, "64s"),        # object type spawned ("NormalTroll", "elmBadPlantBeetle", ...)
        "max_alive": (68, "B"),      # slots: how many of its baddies may be alive at once
        "wave_threshold": (69, "B"), # a wave starts once this many slots are free (0: refill each slot at once)
        "start_count": (70, "B"),    # cap on the first wave after the start event
        "max_total": (71, "B"),      # stop after this many spawns (0: no limit)
        "wave_delay": (72, "f"),     # seconds before a wave once the threshold is met
        "unknown76": (76, "i"),
        "spawn_interval": (80, "f"), # seconds between baddies of one wave (0: the whole wave at once)
        "ai_dll": (84, "32s"),       # AI DLL given to each baddie ("GenericRelentless.dll", ...)
        "node_name": (116, "16s"),   # network node name to walk to (spawn style 1)
        "target": (132, "16s"),      # object asked to the baddie as its target/route
        "arg148": (148, "f"), "arg152": (152, "f"), "arg156": (156, "f"), "arg160": (160, "f"), "arg164": (164, "f"),
        "yaw_spread": (168, "f"),    # degrees of random yaw added to the generator's yaw
        "unknown172": (172, "i"),
        "spawn_style": (176, "B"),   # how the baddie arrives (baddieReady switch); 0 spawns a "BaddieSeed" instead
        "extra_slots": (177, "B"),   # slots spawned on request (ask messages), after max_alive
        "hidden": (178, "B"),        # no generator animation/visual
        "arg180": (180, "f"), "arg184": (184, "f"),
        "baddie_scale": (188, "f"),
        "unknown192": (192, "i"),
        "stop_event": (196, "i"),    # -1 none; when it becomes done the generator stops
    },
    1: {    # trolls and other AI baddies (objdataTrollInit reads these; the rest is per type)
        "arg12": (12, "f"), "event16": (16, "i"),
        "range52": (52, "f"), "range56": (56, "f"), "range60": (60, "f"),
        "arg78": (78, "B"), "ai_flags": (80, "B"),
    },
}


@dataclass
class Params:
    data: bytearray

    @property
    def kind(self) -> int:
        return _u32(self.data, 0) if len(self.data) >= 4 else -1

    @property
    def kind_name(self) -> str:
        return PARAM_SETS.get(self.kind, f"set{self.kind}")

    def fields(self) -> dict[str, tuple[int, str]]:
        return {k: v for k, v in PARAM_FIELDS.get(self.kind, {}).items() if v[0] + struct.calcsize(">" + v[1]) <= len(self.data)}

    def get(self, name: str):
        offset, fmt = self.fields()[name]
        value = struct.unpack_from(">" + fmt, self.data, offset)[0]
        return _cstr(value, 0, len(value)) if fmt.endswith("s") else value

    def set(self, name: str, value) -> None:
        offset, fmt = self.fields()[name]
        if fmt.endswith("s"):
            size = struct.calcsize(fmt)
            raw = value.encode("latin1")
            if len(raw) >= size:
                raise ValueError(f"{name} holds at most {size - 1} characters")
            value = raw.ljust(size, b"\0")
        struct.pack_into(">" + fmt, self.data, offset, value)

    def as_dict(self) -> dict:
        return {name: self.get(name) for name in self.fields()}


@dataclass
class LevelObject:
    """One placed instance. Unknown bytes (string padding, the u16 at +70, pads) stay as they were."""
    name: str
    raw: bytearray            # the 124-byte header as stored
    params: Params

    @classmethod
    def parse(cls, name: str, data: bytes) -> "LevelObject":
        if len(data) < RECORD_HEADER + 4:
            raise ValueError(f"{name}: {len(data)} bytes is too short for a level object")
        return cls(name, bytearray(data[:RECORD_HEADER]), Params(bytearray(data[RECORD_HEADER:])))

    def to_bytes(self) -> bytes:
        return bytes(self.raw) + bytes(self.params.data)

    def _string(self, at: int) -> str:
        return _cstr(self.raw, at, 32)

    def _set_string(self, at: int, value: str) -> None:
        if value == self._string(at):
            return
        raw = value.encode("latin1")
        if len(raw) > 31:
            raise ValueError("at most 31 characters")
        self.raw[at:at + 32] = raw.ljust(32, b"\0")

    def _get(self, at: int, fmt: str):
        values = struct.unpack_from(">" + fmt, self.raw, at)
        return values if len(values) > 1 else values[0]

    def _put(self, at: int, fmt: str, *values) -> None:
        struct.pack_into(">" + fmt, self.raw, at, *values)

    objrefno = property(lambda s: s._get(0, "I"), lambda s, v: s._put(0, "I", v))
    type_name = property(lambda s: s._string(4), lambda s, v: s._set_string(4, v))
    ai_dll = property(lambda s: s._string(36), lambda s, v: s._set_string(36, v))
    group = property(lambda s: s._get(68, "h"), lambda s, v: s._put(68, "h", v))
    position = property(lambda s: s._get(72, "3f"), lambda s, v: s._put(72, "3f", *v))
    rotation = property(lambda s: s._get(84, "3f"), lambda s, v: s._put(84, "3f", *v),
                        doc="(yaw, pitch, roll) in degrees, as stored")
    scale = property(lambda s: s._get(96, "f"), lambda s, v: s._put(96, "f", v))
    create_at_start = property(lambda s: s._get(100, "B"), lambda s, v: s._put(100, "B", v))
    activity_range = property(lambda s: s._get(104, "f"), lambda s, v: s._put(104, "f", v))
    event = property(lambda s: s._get(108, "i"), lambda s, v: s._put(108, "i", v))
    event2 = property(lambda s: s._get(112, "i"), lambda s, v: s._put(112, "i", v))
    flags = property(lambda s: s._get(116, "B"), lambda s, v: s._put(116, "B", v))
    health_scale = property(lambda s: s._get(120, "f"), lambda s, v: s._put(120, "f", v))

    def describe(self) -> str:
        x, y, z = self.position
        yaw, pitch, roll = self.rotation
        text = f"{self.name:30s} #{self.objrefno:<5d} {self.type_name:24s} pos=({x:8.2f},{y:8.2f},{z:8.2f})"
        if yaw or pitch or roll:
            text += f" rot=({yaw:.1f},{pitch:.1f},{roll:.1f})"
        if self.scale != 1.0:
            text += f" scale={self.scale:g}"
        extras = []
        if self.ai_dll:
            extras.append(f"ai={self.ai_dll}")
        if self.group != -1:
            extras.append(f"group={self.group}")
        if not self.create_at_start:
            extras.append("deferred")
        if self.activity_range:
            extras.append(f"range={self.activity_range:g}")
        if self.event != -1:
            extras.append(f"event={self.event}")
        if self.event2 != -1:
            extras.append(f"event2={self.event2}")
        if self.flags:
            extras.append(f"flags={self.flags}")
        if self.health_scale != 1.0:
            extras.append(f"health={self.health_scale:g}")
        values = self.params.as_dict()
        if values:
            extras.append(f"{self.params.kind_name}{{" + ", ".join(
                f"{k}={v:g}" if isinstance(v, float) else f"{k}={v!r}" if isinstance(v, str) else f"{k}={v}"
                for k, v in values.items()) + "}")
        elif self.params.kind:
            extras.append(f"{self.params.kind_name}[{len(self.params.data)} bytes]")
        return text + ("  " + " ".join(extras) if extras else "")


@dataclass
class LevelObjects:
    """The levelObjects block as instances. `block` keeps everything else (header bytes, the "empty" record)."""
    block: XmlBlock
    objects: list[LevelObject]

    @classmethod
    def parse(cls, data: bytes) -> "LevelObjects":
        return cls.from_block(XmlBlock.parse(data))

    @classmethod
    def from_block(cls, block: XmlBlock) -> "LevelObjects":
        if block.block_hash != LEVEL_OBJECTS:
            raise ValueError("not a levelObjects block")
        return cls(block, [LevelObject.parse(s.name, s.data) for s in block.sections])

    def to_bytes(self) -> bytes:
        names = [o.name for o in self.objects]
        if len({elf_hash_latin1(n) for n in names}) != len(names):
            raise ValueError("two objects share a name hash")
        if names.count("empty") != 1:
            raise ValueError('a level needs exactly one "empty" record')
        old = {s.name: s for s in self.block.sections}
        self.block.sections = [Section(o.name, o.to_bytes(), old[o.name].stored_hash if o.name in old else None)
                               for o in self.objects]
        return self.block.to_bytes()

    def get(self, name: str) -> LevelObject | None:
        return next((o for o in self.objects if o.name == name), None)

    def of_type(self, type_name: str) -> list[LevelObject]:
        return [o for o in self.objects if o.type_name == type_name]

    def next_objrefno(self) -> int:
        return max(o.objrefno for o in self.objects) + 1

    def add(self, template: LevelObject, name: str, **changes) -> LevelObject:
        """A copy of `template` named `name` with a fresh objrefno, inserted in hash order. `changes` sets
        LevelObject properties (position=(x, y, z), rotation=..., ...) or, prefixed "param_", parameter fields."""
        if len(name.encode("latin1")) > NAME_LIMIT:
            raise ValueError(f"object names hold at most {NAME_LIMIT} characters")
        key = elf_hash_latin1(name)
        if any(elf_hash_latin1(o.name) == key for o in self.objects):
            raise ValueError(f"an object already has the name (hash) of {name!r}")
        obj = LevelObject(name, bytearray(template.raw), Params(bytearray(template.params.data)))
        obj.objrefno = self.next_objrefno()
        for attribute, value in changes.items():
            if attribute.startswith("param_"):
                obj.params.set(attribute[6:], value)
            else:
                setattr(obj, attribute, value)
        self.objects.append(obj)
        self.objects.sort(key=lambda o: elf_hash_latin1(o.name))
        return obj

    def remove(self, name: str) -> None:
        if name == "empty":
            raise ValueError('the "empty" record has to stay')
        self.objects = [o for o in self.objects if o.name != name]


# ----------------------------------------------------------------------------------------------- triggers script

# The "level" section of the "triggers" block, read by triggerTick: rules made of conditions and actions, each op a
# u32 code then its fields, aligned to 4. (size, name, fields)
TRIGGER_OPS = {
    1: (8, "rule", {"mode": (4, "B"), "enabled": (5, "B")}),     # mode 0 all, 1 any, 2 exactly one condition
    2: (12, "ifEvent", {"event": (4, "i"), "done": (8, "B")}),    # true when eventDone(event) == done
    3: (28, "ifTrigger", {"trigger": (4, "20s"), "when": (24, "B")}),  # 0 inside, 1 entered, 2 either, 3 left, 4 always
    4: (12, "ifTask", {"task": (4, "i"), "active": (8, "B")}),    # task active (1) or complete (0)
    5: (8, "ifArrivedBySetup", {"setup": (4, "i")}),              # the setup point the player came in by (-1 any)
    6: (8, "ifArrivedBySetup2", {"setup": (4, "i")}),
    7: (16, "setEvent", {"once": (4, "B"), "event": (8, "i"), "value": (12, "B")}),
    8: (44, "levelChange", {"once": (4, "B"), "level": (8, "32s"), "setup": (40, "B")}),
    9: (12, "activateGroup", {"once": (4, "B"), "group": (8, "B")}),
    10: (40, "activateObject", {"once": (4, "B"), "object": (8, "32s")}),
    11: (40, "deactivateObject", {"once": (4, "B"), "object": (8, "32s")}),
    12: (12, "deactivateGroup", {"once": (4, "B"), "group": (8, "B")}),
    13: (80, "playFmv", {"once": (4, "B"), "name": (8, "64s")}),
    14: (16, "music", {"once": (4, "B"), "cue": (8, "i"), "fanfare": (12, "B")}),
    15: (80, "playMovie", {"once": (4, "B"), "name": (8, "64s")}),
    16: (20, "warpPlayer", {"once": (4, "B"), "setup": (8, "B"), "arg12": (12, "f"), "arg16": (16, "f")}),
    17: (28, "op17", {"once": (4, "B")}),
    18: (12, "nop18", {}), 19: (12, "nop19", {}),
    22: (12, "setArrivedBySetup", {"once": (4, "B"), "setup": (8, "H")}),
    23: (16, "depthEffect", {"once": (4, "B"), "effect": (8, "i"), "value": (12, "f")}),
    24: (40, "op24", {}),
}


def decode_trigger_script(data: bytes) -> list[tuple[int, str, dict]]:
    """[(offset, op name, fields)]; stops at an unknown op with ("?", {"code": ...})."""
    out = []
    at = 0
    while at + 4 <= len(data):
        code = _u32(data, at)
        if code not in TRIGGER_OPS:
            out.append((at, "?", {"code": code}))
            break
        size, name, fields = TRIGGER_OPS[code]
        values = {}
        for key, (offset, fmt) in fields.items():
            value = struct.unpack_from(">" + fmt, data, at + offset)[0]
            values[key] = _cstr(value, 0, len(value)) if fmt.endswith("s") else value
        out.append((at, name, values))
        at = _align(at + size, 4)
    return out


# ----------------------------------------------------------------------------------------------- archive helpers

def xmlblock_entries(archive: Caff) -> list[Entry]:
    return [e for e in archive.entries if e.read(archive).startswith(MAGIC)]


@dataclass
class TypeDef:
    name: str
    class_hash: int
    dll: str
    assets: list[tuple[int, int, Entry | None]]    # (asset type hash, asset name hash, entry)

    @property
    def class_name(self) -> str:
        return CLASS_NAMES.get(self.class_hash, f"{self.class_hash:08X}")


def database_types(archive: Caff) -> list[TypeDef]:
    """The object types a level's kameoDatabase defines. +0x24 points at the type table: count, records (name, 0,
    definition), ordinal table, hash index, names. A definition: u32 class hash, char[64] DLL, u32 asset count at +68,
    +72 pointer to that many pointers at asset records (u32 type hash, u32 name hash, pointer to the entry)."""
    entry = archive.entries[0]
    data = entry.read(archive)
    if not data.startswith(b"kameoDatabase"):
        raise ValueError("the archive does not start with a kameoDatabase")
    pointers = archive.pointers_from(entry)
    table = _u32(data, 0x24)
    count, records = _u32(data, table), _u32(data, table + 4)
    types = []
    for i in range(count):
        name_at, _, definition = struct.unpack_from(">III", data, records + 12 * i)
        assets = []
        asset_list = _u32(data, definition + 72)
        for j in range(_u32(data, definition + 68)):
            record = _u32(data, asset_list + 4 * j)
            target = pointers.get(record + 8)
            assets.append((_u32(data, record), _u32(data, record + 4), target[0] if target else None))
        types.append(TypeDef(_cstr(data, name_at), _u32(data, definition), _cstr(data, definition + 4, 64), assets))
    return types


def level_objects_entry(archive: Caff) -> Entry | None:
    """The level's own levelObjects block: an xml asset of the type whose class is "level"."""
    for typedef in database_types(archive):
        if typedef.class_hash != LEVEL_CLASS:
            continue
        for asset_type, _, entry in typedef.assets:
            if asset_type == XMLBLOCK_ASSET and entry is not None and _u32(entry.read(archive), 0x20) == LEVEL_OBJECTS:
                return entry
    return None


def triggers_entry(archive: Caff) -> Entry | None:
    for typedef in database_types(archive):
        if typedef.class_hash != LEVEL_CLASS:
            continue
        for asset_type, _, entry in typedef.assets:
            if asset_type == XMLBLOCK_ASSET and entry is not None and _u32(entry.read(archive), 0x20) == TRIGGERS:
                return entry
    return None


def load_level_objects(archive: Caff) -> tuple[Entry, LevelObjects]:
    entry = level_objects_entry(archive)
    if entry is None:
        raise ValueError("no levelObjects block in this archive")
    return entry, LevelObjects.parse(entry.read(archive))


def write_level_objects(archive: Caff, entry: Entry, objects: LevelObjects) -> bytes:
    """The archive with `entry` replaced by `objects`. The block has no relocations, so nothing else changes."""
    return archive.write({entry.index: objects.to_bytes()})


def verify(path: str) -> tuple[int, int, list[str]]:
    """Parses and rewrites every kameoxmlblock (and the level objects) of `path`: (blocks, identical, problems)."""
    archive = Caff.load(path)
    problems = []
    blocks = same = 0
    sources = {r.source for r in archive.relocations if r.offsets}
    for entry in xmlblock_entries(archive):
        blocks += 1
        data = entry.read(archive)
        if entry.index in sources:
            problems.append(f"entry {entry.index}: has relocations")
        try:
            out = XmlBlock.parse(data).to_bytes()
        except ValueError as error:
            problems.append(f"entry {entry.index}: {error}")
            continue
        if out == data:
            same += 1
        else:
            at = next((i for i, (a, b) in enumerate(zip(out, data)) if a != b), min(len(out), len(data)))
            problems.append(f"entry {entry.index}: differs at {at:#x} ({len(out)} vs {len(data)} bytes)")
    entry = level_objects_entry(archive)
    if entry is not None:
        data = entry.read(archive)
        if LevelObjects.parse(data).to_bytes() != data:
            problems.append(f"entry {entry.index}: level objects do not round-trip")
    return blocks, same, problems


# ----------------------------------------------------------------------------------------------- command line

def _print_block(archive: Caff, entry: Entry, level_entry: Entry | None, trig_entry: Entry | None) -> None:
    block = XmlBlock.parse(entry.read(archive))
    print(f"entry {entry.index}: {block.name} block, {len(block.sections)} sections, {entry.size} bytes")
    if level_entry is not None and entry.index == level_entry.index:
        for obj in LevelObjects.from_block(block).objects:
            print("  " + obj.describe())
    elif trig_entry is not None and entry.index == trig_entry.index:
        for section in block.sections:
            print(f"  [{section.name}]")
            for at, name, values in decode_trigger_script(section.data):
                print(f"    {at:4x} {name:18s} " + " ".join(f"{k}={v!r}" for k, v in values.items()))
    else:
        for section in block.sections:
            print(f"  {section.name:40s} {len(section.data):5d} bytes  {section.data[:24].hex(' ', 4)}")


def main(argv: list[str]) -> int:
    if len(argv) >= 3 and argv[1] == "--verify":
        total = identical = 0
        for path in argv[2:]:
            blocks, same, problems = verify(path)
            total += blocks
            identical += same
            for problem in problems:
                print(f"{path}: {problem}")
        print(f"{identical}/{total} kameoxmlblocks rewrite byte-identical")
        return 0 if identical == total else 1
    if len(argv) < 2:
        print(__doc__.split("Usage:")[1].strip())
        return 1
    archive = Caff.load(argv[1])
    level_entry = level_objects_entry(archive)
    trig_entry = triggers_entry(archive)
    if len(argv) > 2:
        entry = archive.entries[int(argv[2]) - 1]
        if not entry.read(archive).startswith(MAGIC):
            print(f"entry {entry.index} is not a kameoxmlblock")
            return 1
        _print_block(archive, entry, level_entry, trig_entry)
        return 0
    types = database_types(archive)
    type_names = {elf_hash_latin1(t.name): t.name for t in types}
    print(f"{argv[1]}: kameoxmlblocks")
    for entry in xmlblock_entries(archive):
        block = XmlBlock.parse(entry.read(archive))
        name = BLOCK_NAMES.get(block.block_hash) or type_names.get(block.block_hash, block.name)
        first = ", ".join(s.name for s in block.sections[:4])
        print(f"  entry {entry.index:4d} {name:24s} {len(block.sections):4d} sections {entry.size:7d} bytes  {first}")
    for typedef in types:
        if typedef.class_hash == LEVEL_CLASS:
            print(f"level type {typedef.name} ({typedef.dll})")
    print("object types:", " ".join(f"{t.name}({t.class_name}{', ' + t.dll if t.dll else ''})" for t in types))
    for entry in (level_entry, trig_entry):
        if entry is not None:
            _print_block(archive, entry, level_entry, trig_entry)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
