"""Reader and writer for a level's AI navigation data: the "kameonetwork" and "kameozones" entries of .lvl archives.

Both entries start with the usual 32-byte asset header: name[16], u32 LE version (network 1 or 2, zones 4),
u32 LE entry size, 8 zero bytes. Neither has relocations; the loader copies everything it needs.

kameonetwork (NetworkUnpack, big-endian, never byte-swapped):
  u32 node count, then each node: a 48-byte record followed by its links
    +0  position x, y, z          +12 f32 radius (1, 3 or 6 in shipped data)   +16 f32 weight (1.0, 0.7)
    +20 name[16] ("*" for an unnamed node)   +36 u32 ELF hash of the name   +40 u8 link count
    +41 3 pad bytes (0xCC or 0)   +44 u32 links pointer (tool garbage; the loader replaces it)
    link: u32 target node index, f32 length
  The game keeps the records as an array of 48 (network / 0x82B71DAC) with each node's links on the heap.
  A few shipped entries have stale bytes after the last node; they are kept as `trailing`.
  Named nodes are what BaddieGenerator node_name, the AI's hide/route planner (AiActionPlanRoute, NetworkFindRoute,
  "Hide" nodes) and ZoneSetupGameZones ("GROUP<n>" nodes give a zone its group) look up.

kameozones (ZoneEditor2_loadFile; little-endian, swapped on load):
  u32 zone count, then each zone: an 80-byte record, its points, and its height grid
    +0  name[16]
    +16 the region the Geo* functions take (zone + 16):
        +16 centre x, base y, centre z   +28 min x, min z, max x, max z   +44 s16 point count (+46 pad)
        +48 f32 height (0 = any height; else GeoVecPointInRegion3d wants base y - 0.5 <= y <= base y + height + 0.5)
        +52 u16 has grid (+54 pad)   +56 s32 grid columns   +60 s32 grid rows
        +64 / +68 / +72 points, normals, grid pointers (zero in files)
    +76 u8 kind (0, 3, 4, 5 ship; ZoneSetupGameZones makes a flat kind-0 zone kind 1; zoneGetZoneBy3DPoint takes
        a wanted kind, 255 = anything but 2, which trolls pass (one troll type passes 5)) (+77 pad)
    points: count * (x, y, z), an outline in the XZ plane (either winding)
    grid, when present: (columns + 2) * (rows + 2) f32 ground heights over the min/max box (GeoRegionGetGroundPos)
  u32 obstacle count, then 28-byte obstacles: x, y, z, 4 unswapped bytes, f32 radius, f32 height, 4 unswapped bytes.
    Each joins every zone whose outline holds its XZ position; objectKeepInZoneWithObstacleAvoidance steers round them.
  u32 barrier count, then barriers: a 12-byte header (+0 pointer, +4 u8 point count, +8 f32 height) and its points.
    A barrier joins every zone holding one of its points; zoneLineIntersectsBarrier (AI sight and the troll's
    safe-area check) treats it as a wall of that height along the polyline.

How a troll uses a zone (objdataTrollTick): while it has one it is clamped back inside the outline every tick
(GeoKeepVecInRegion) and its world-polygon collision ("wallhit1", mask 0x170003FF) stays off; only a failed zone
lookup (or, for a few troll kinds, standing within 3 units of the edge) turns that collision on for a moment, and
AiGeneralPreProcess then walks a zoneless troll to the nearest zone edge and kills it if it stays out. Walls alone
do not hold trolls: a level needs zones that match its floor. arena_zones / grid_network / testarena_layout build
them for a custom room.

Usage: python -m kameo_modkit.network <file.lvl> ...   (describes both entries and checks they round-trip)
"""

from __future__ import annotations

import math
import struct
import sys
from dataclasses import dataclass, field

from .caff import Caff, Entry

HEADER = 0x20


def elf_hash(name: str) -> int:
    value = 0
    for byte in name.encode("latin-1"):
        value = ((value << 4) + byte) & 0xFFFFFFFF
        top = value & 0xF0000000
        if top:
            value ^= top >> 24
        value &= ~top & 0xFFFFFFFF
    return value


def _name(raw: bytes) -> str:
    return raw.split(b"\0")[0].decode("latin-1")


def _name_field(name: str, raw: bytes | None = None) -> bytes:
    """`name` as a 16-byte field, keeping the bytes after the terminator of `raw` (shipped files have junk there)."""
    encoded = name.encode("latin-1")
    if len(encoded) > 15:
        raise ValueError(f"name {name!r} is longer than 15 bytes")
    if raw is not None and _name(raw) == name:
        return raw
    return (encoded + b"\0" * 16)[:16]


@dataclass
class Header:
    name: str
    version: int
    tail: bytes = b"\0" * 8

    @classmethod
    def parse(cls, data: bytes) -> "Header":
        version, _size = struct.unpack_from("<II", data, 16)
        return cls(_name(data[:16]), version, data[24:32])

    def to_bytes(self, size: int, raw: bytes | None = None) -> bytes:
        head = raw[:16] if raw is not None else (self.name.encode() + b"\0" * 16)[:16]
        return head + struct.pack("<II", self.version, size) + self.tail


# ---------------------------------------------------------------------------------------------------------- network

@dataclass
class Link:
    target: int
    length: float


@dataclass
class Node:
    position: tuple[float, float, float]
    radius: float = 1.0
    weight: float = 1.0
    name: str = "*"
    links: list[Link] = field(default_factory=list)
    hash: int | None = None          # None: the ELF hash of name
    pad: bytes = b"\xcc\xcc\xcc"
    links_pointer: int = 0
    raw_name: bytes | None = None

    def name_hash(self) -> int:
        return elf_hash(self.name) if self.hash is None else self.hash


@dataclass
class Network:
    header: Header
    nodes: list[Node] = field(default_factory=list)
    raw_head: bytes | None = None
    trailing: bytes = b""

    @classmethod
    def parse(cls, data: bytes) -> "Network":
        if not data.startswith(b"kameonetwork\0"):
            raise ValueError("not a kameonetwork entry")
        count = struct.unpack_from(">I", data, HEADER)[0]
        at = HEADER + 4
        nodes = []
        for _ in range(count):
            x, y, z, radius, weight = struct.unpack_from(">5f", data, at)
            name_hash, link_count = struct.unpack_from(">IB", data, at + 36)
            raw_name = data[at + 20:at + 36]
            node = Node((x, y, z), radius, weight, _name(raw_name), [], name_hash, data[at + 41:at + 44],
                        struct.unpack_from(">I", data, at + 44)[0], raw_name)
            if node.hash == elf_hash(node.name):
                node.hash = None
            at += 48
            for _ in range(link_count):
                target, length = struct.unpack_from(">If", data, at)
                node.links.append(Link(target, length))
                at += 8
            nodes.append(node)
        # A few shipped entries carry stale bytes after the last node (the loader never reads them).
        return cls(Header.parse(data), nodes, data[:16], data[at:])

    def to_bytes(self) -> bytes:
        body = bytearray(struct.pack(">I", len(self.nodes)))
        for node in self.nodes:
            if len(node.links) > 255:
                raise ValueError("a node has more than 255 links")
            body += struct.pack(">5f", *node.position, node.radius, node.weight)
            body += _name_field(node.name, node.raw_name)
            body += struct.pack(">IB", node.name_hash(), len(node.links)) + node.pad[:3].ljust(3, b"\0")
            body += struct.pack(">I", node.links_pointer)
            for link in node.links:
                body += struct.pack(">If", link.target, link.length)
        body += self.trailing
        return self.header.to_bytes(HEADER + len(body), self.raw_head) + bytes(body)

    def check(self) -> list[str]:
        problems = []
        for i, node in enumerate(self.nodes):
            for link in node.links:
                if not 0 <= link.target < len(self.nodes):
                    problems.append(f"node {i} links to missing node {link.target}")
        return problems

    def describe(self) -> str:
        named = {}
        for node in self.nodes:
            if node.name != "*":
                named[node.name] = named.get(node.name, 0) + 1
        links = sum(len(n.links) for n in self.nodes)
        lines = [f"network v{self.header.version}: {len(self.nodes)} nodes, {links} links"]
        if named:
            lines.append("  named: " + ", ".join(f"{k} x{v}" if v > 1 else k for k, v in sorted(named.items())))
        return "\n".join(lines)


def link_nodes(nodes: list[Node], pairs) -> None:
    """Links each (a, b) both ways, with the XZ+Y distance as the length."""
    for a, b in pairs:
        pa, pb = nodes[a].position, nodes[b].position
        length = sum((p - q) ** 2 for p, q in zip(pa, pb)) ** 0.5
        nodes[a].links.append(Link(b, length))
        nodes[b].links.append(Link(a, length))


# ------------------------------------------------------------------------------------------------------------ zones

@dataclass
class Zone:
    name: str
    points: list[tuple[float, float, float]]
    height: float = 0.0
    kind: int = 0
    centre: tuple[float, float, float] | None = None       # None: computed as the game does (GeoUpdateRegionBounds)
    bounds: tuple[float, float, float, float] | None = None # min x, min z, max x, max z; None: computed
    grid: list[float] | None = None
    grid_size: tuple[int, int] = (0, 0)                    # columns, rows (kept even without a grid)
    raw: bytes | None = None                               # the 80-byte record as read, for its pad bytes

    def computed_centre(self) -> tuple[float, float, float]:
        n = len(self.points)
        if not n:
            return (0.0, 0.0, 0.0)
        return (sum(p[0] for p in self.points) / n, min(p[1] for p in self.points), sum(p[2] for p in self.points) / n)

    def computed_bounds(self) -> tuple[float, float, float, float]:
        xs = [p[0] for p in self.points] or [0.0]
        zs = [p[2] for p in self.points] or [0.0]
        return (min(xs), min(zs), max(xs), max(zs))

    def contains(self, x: float, z: float) -> bool:
        """Even-odd test, as GeoPointInRegion (which ignores outlines of fewer than three points)."""
        if len(self.points) < 3:
            return False
        inside = False
        pts = self.points
        for i in range(len(pts)):
            x1, _, z1 = pts[i]
            x2, _, z2 = pts[(i + 1) % len(pts)]
            if (z1 > z) != (z2 > z) and x < x1 + (z - z1) * (x2 - x1) / (z2 - z1):
                inside = not inside
        return inside


@dataclass
class Obstacle:
    position: tuple[float, float, float]
    radius: float
    height: float
    extra: bytes = b"\0" * 4      # +12, not byte-swapped by the loader
    tail: bytes = b"\0" * 4       # +24, not byte-swapped


@dataclass
class Barrier:
    points: list[tuple[float, float, float]]
    height: float
    head: bytes = b"\0" * 8       # +0 pointer, +4 count (rewritten), +5 pad


@dataclass
class Zones:
    header: Header
    zones: list[Zone] = field(default_factory=list)
    obstacles: list[Obstacle] = field(default_factory=list)
    barriers: list[Barrier] = field(default_factory=list)
    raw_head: bytes | None = None
    trailing: bytes = b""

    @classmethod
    def parse(cls, data: bytes) -> "Zones":
        if not data.startswith(b"kameozones\0"):
            raise ValueError("not a kameozones entry")
        at = HEADER
        count = struct.unpack_from("<I", data, at)[0]
        at += 4
        zones = []
        for _ in range(count):
            record = data[at:at + 80]
            centre = struct.unpack_from("<3f", record, 16)
            bounds = struct.unpack_from("<4f", record, 28)
            n = struct.unpack_from("<h", record, 44)[0]
            height = struct.unpack_from("<f", record, 48)[0]
            has_grid = struct.unpack_from("<H", record, 52)[0]
            columns, rows = struct.unpack_from("<ii", record, 56)
            at += 80
            points = [struct.unpack_from("<3f", data, at + 12 * i) for i in range(n)]
            at += 12 * n
            grid = None
            if has_grid:
                cells = (columns + 2) * (rows + 2)
                grid = list(struct.unpack_from(f"<{cells}f", data, at))
                at += 4 * cells
            zones.append(Zone(_name(record[:16]), points, height, record[76], centre, bounds, grid, (columns, rows),
                              record))
        count = struct.unpack_from("<I", data, at)[0]
        at += 4
        obstacles = []
        for _ in range(count):
            x, y, z = struct.unpack_from("<3f", data, at)
            radius, height = struct.unpack_from("<2f", data, at + 16)
            obstacles.append(Obstacle((x, y, z), radius, height, data[at + 12:at + 16], data[at + 24:at + 28]))
            at += 28
        count = struct.unpack_from("<I", data, at)[0]
        at += 4
        barriers = []
        for _ in range(count):
            head = data[at:at + 8]
            n = data[at + 4]
            height = struct.unpack_from("<f", data, at + 8)[0]
            at += 12
            barriers.append(Barrier([struct.unpack_from("<3f", data, at + 12 * i) for i in range(n)], height, head))
            at += 12 * n
        return cls(Header.parse(data), zones, obstacles, barriers, data[:16], data[at:])

    def to_bytes(self) -> bytes:
        body = bytearray(struct.pack("<I", len(self.zones)))
        for zone in self.zones:
            record = bytearray(zone.raw if zone.raw is not None else bytes(80))
            if zone.raw is None:
                record[46:48] = b"\xcc\xcc"
                record[54:56] = b"\xcc\xcc"
                record[77:80] = b"\xcc\xcc\xcc"
            record[:16] = _name_field(zone.name, bytes(record[:16]) if zone.raw is not None else None)
            struct.pack_into("<3f", record, 16, *(zone.centre or zone.computed_centre()))
            struct.pack_into("<4f", record, 28, *(zone.bounds or zone.computed_bounds()))
            struct.pack_into("<h", record, 44, len(zone.points))
            struct.pack_into("<f", record, 48, zone.height)
            struct.pack_into("<H", record, 52, 1 if zone.grid is not None else 0)
            struct.pack_into("<ii", record, 56, *zone.grid_size)
            struct.pack_into("<III", record, 64, 0, 0, 0)
            record[76] = zone.kind
            body += record
            for point in zone.points:
                body += struct.pack("<3f", *point)
            if zone.grid is not None:
                columns, rows = zone.grid_size
                if len(zone.grid) != (columns + 2) * (rows + 2):
                    raise ValueError(f"zone {zone.name}: grid needs {(columns + 2) * (rows + 2)} heights")
                body += struct.pack(f"<{len(zone.grid)}f", *zone.grid)
        body += struct.pack("<I", len(self.obstacles))
        for o in self.obstacles:
            body += struct.pack("<3f", *o.position) + o.extra + struct.pack("<2f", o.radius, o.height) + o.tail
        body += struct.pack("<I", len(self.barriers))
        for b in self.barriers:
            if len(b.points) > 255:
                raise ValueError("a barrier has more than 255 points")
            head = bytearray(b.head)
            head[4] = len(b.points)
            body += bytes(head) + struct.pack("<f", b.height)
            for point in b.points:
                body += struct.pack("<3f", *point)
        body += self.trailing
        return self.header.to_bytes(HEADER + len(body), self.raw_head) + bytes(body)

    def zone_at(self, x: float, y: float, z: float, kind: int = 255) -> Zone | None:
        """The zone a troll standing at (x, y, z) would be in (zoneGetZoneBy3DPoint; status switches ignored)."""
        for zone in self.zones:
            runtime_kind = zone.kind
            if runtime_kind == 0 and _flat(zone):
                runtime_kind = 1
            if not (runtime_kind == kind or (kind == 255 and runtime_kind != 2)):
                continue
            if zone.height:
                base = (zone.centre or zone.computed_centre())[1]
                if not base - 0.5 <= y <= base + zone.height + 0.5:
                    continue
            if zone.contains(x, z):
                return zone
        return None

    def describe(self) -> str:
        lines = [f"zones v{self.header.version}: {len(self.zones)} zones, {len(self.obstacles)} obstacles, "
                 f"{len(self.barriers)} barriers"]
        for zone in self.zones:
            b = zone.bounds or zone.computed_bounds()
            lines.append(f"  {zone.name:<15} kind {zone.kind} {len(zone.points):3} points  x {b[0]:.1f}..{b[2]:.1f}"
                         f"  z {b[1]:.1f}..{b[3]:.1f}  base y {(zone.centre or zone.computed_centre())[1]:.1f}"
                         f"  height {zone.height:g}" + (f"  grid {zone.grid_size}" if zone.grid is not None else ""))
        return "\n".join(lines)


def _flat(zone: Zone) -> bool:
    """GeoUpdateRegionBounds' answer: every point within 0.4 of the first one's height."""
    if not zone.points:
        return True
    y0 = zone.points[0][1]
    return all(abs(p[1] - y0) <= 0.4 for p in zone.points[1:])


# ---------------------------------------------------------------------------------------------------------- archive

def entry_of(archive: Caff, kind: str) -> Entry | None:
    tag = kind.encode() + b"\0"
    for entry in archive.entries:
        if entry.read(archive).startswith(tag):
            return entry
    return None


def load_network(archive: Caff) -> tuple[Entry, Network]:
    entry = entry_of(archive, "kameonetwork")
    if entry is None:
        raise ValueError("archive has no kameonetwork entry")
    return entry, Network.parse(entry.read(archive))


def load_zones(archive: Caff) -> tuple[Entry, Zones]:
    entry = entry_of(archive, "kameozones")
    if entry is None:
        raise ValueError("archive has no kameozones entry")
    return entry, Zones.parse(entry.read(archive))


# ------------------------------------------------------------------------------------------------------------ build
#
# A zone outline is one simple polygon. Shipped zones never use holes, and a zero-width slit to fake one would act
# as a wall for the AI's sight and rerouting tests (GeoPointIsVisible2GetEdge, zoneRerouteToTarget test the line to
# a target against every outline edge). Solids against a wall are cut out of the outline as notches; free-standing
# ones become obstacles, the circles the AI steers round (as the shipped levels do).

XZ = tuple[float, float]
Circle = tuple[float, float, float, float]   # x, z, radius, height


def arena_zones(base: Zones | None, outline: list[XZ], obstacles: list[Circle] = (), name: str = "Arena",
                y: float = 0.0, height: float = 20.0) -> Zones:
    """A zone file holding one zone: `outline` (x, z points of a simple polygon, either winding) at floor height `y`,
    `height` tall (0 = any height), with `obstacles` standing on that floor. It replaces every zone of the base file,
    so nothing else in the level can claim a troll."""
    header = base.header if base is not None else Header("kameozones", 4)
    zone = Zone(name, [(float(x), float(y), float(z)) for x, z in outline], height=height, kind=0)
    obs = [Obstacle((float(x), float(y), float(z)), float(r), float(h), b"\0\0\xcc\xcc", b"\0\0\xcc\xcc")
           for x, z, r, h in obstacles]
    return Zones(header, [zone], obs, [], base.raw_head if base is not None else None)


def _segments_cross(a: XZ, b: XZ, c: XZ, d: XZ) -> bool:
    def side(p, q, r):
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
    return side(c, d, a) * side(c, d, b) < 0 and side(a, b, c) * side(a, b, d) < 0


def check_outline(outline: list[XZ]) -> list[str]:
    """Problems that would break GeoPointInRegion / GeoFindClosestPointOnEdge: too few points, repeated points,
    crossing edges."""
    problems = []
    n = len(outline)
    if n < 3:
        problems.append("an outline needs at least three points")
    if n > 32767:
        problems.append("too many points for the s16 count")
    if len(set(outline)) != n:
        problems.append("repeated points")
    edges = [(outline[i], outline[(i + 1) % n]) for i in range(n)]
    for i in range(n):
        for j in range(i + 2, n):
            if not (i == 0 and j == n - 1) and _segments_cross(*edges[i], *edges[j]):
                problems.append(f"edges {i} and {j} cross")
    return problems


def _clear(zone: Zone, obstacles: list[Obstacle], a: XZ, b: XZ, clearance: float) -> bool:
    """Whether the walk from a to b stays inside the zone and out of every obstacle (sampled each unit)."""
    steps = max(1, int(((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5))
    for i in range(steps + 1):
        x, z = a[0] + (b[0] - a[0]) * i / steps, a[1] + (b[1] - a[1]) * i / steps
        if not zone.contains(x, z):
            return False
        for o in obstacles:
            if (x - o.position[0]) ** 2 + (z - o.position[2]) ** 2 < (o.radius + clearance) ** 2:
                return False
    return True


def grid_network(base: Network | None, zones: Zones, spacing: float = 12.0, y: float = 0.5, clearance: float = 0.5,
                 named: dict[str, tuple[float, float, float]] | None = None) -> Network:
    """Nodes on a `spacing` grid over the first zone, kept where they stand inside it and clear of its obstacles,
    each linked to its eight neighbours when the straight walk between them is clear too; then a named node for each
    of `named` (e.g. a BaddieGenerator node_name), linked to its nearest reachable grid node. NetworkNearestNode*,
    the AI's route planner (AiActionPlanRoute -> NetworkFindRoute) and the troll's way back into its zone
    (AiGeneralPreProcess -> NetworkNearestNodePosAndHeight) look nodes up here; the base level's nodes are dropped,
    since they stand in its old rooms."""
    header = base.header if base is not None else Header("kameonetwork", 2)
    zone = zones.zones[0]
    x0, z0, x1, z1 = zone.computed_bounds()
    obstacles = zones.obstacles
    columns, rows = int((x1 - x0) // spacing), int((z1 - z0) // spacing)
    ox, oz = (x0 + x1 - columns * spacing) / 2, (z0 + z1 - rows * spacing) / 2
    nodes: list[Node] = []
    at: dict[tuple[int, int], int] = {}
    for i in range(columns + 1):
        for j in range(rows + 1):
            p = (ox + i * spacing, oz + j * spacing)
            if _clear(zone, obstacles, p, p, clearance):
                at[i, j] = len(nodes)
                nodes.append(Node((p[0], y, p[1])))
    pairs = []
    for (i, j), a in at.items():
        for di, dj in ((1, 0), (0, 1), (1, 1), (1, -1)):
            b = at.get((i + di, j + dj))
            if b is not None and _clear(zone, obstacles, nodes[a].position[::2], nodes[b].position[::2], clearance):
                pairs.append((a, b))
    link_nodes(nodes, pairs)
    grid = len(nodes)
    for label, position in (named or {}).items():
        index = len(nodes)
        nodes.append(Node(tuple(map(float, position)), name=label))
        reachable = [g for g in range(grid)
                     if _clear(zone, obstacles, position[::2], nodes[g].position[::2], clearance)]
        if reachable:
            nearest = min(reachable, key=lambda g: sum((a - b) ** 2 for a, b in zip(nodes[g].position, position)))
            link_nodes(nodes, [(index, nearest)])
    return Network(header, nodes, base.raw_head if base is not None else None)


def testarena_layout(margin: float = 2.5, size: float = 120.0) -> tuple[list[XZ], list[Circle]]:
    """The walkable outline and obstacles for testarena.build() with its defaults: the room inset by `margin` (about a
    troll's radius), with notches for the solids that touch the walls (the three -X ramps, the +X staircase, the
    platform and its ramp) and circles for the free-standing ones (five step blocks, four pillars). Mirrors the
    numbers in testarena.build(); keep the two in step."""
    h, m = size / 2, margin
    w = h - m
    ramp_ends = [-h + 8 + 6.0 / math.tan(math.radians(d)) for d in (15, 30, 45)]   # high end of each -X ramp
    outline = [
        (-w, ramp_ends[0] + m),                    # -X wall, beside the 15-degree ramp (x -h+6..-h+12)
        (-h + 12 + m, ramp_ends[0] + m),
        (-h + 12 + m, ramp_ends[1] + m),           # 30-degree ramp (x -h+16..-h+22)
        (-h + 22 + m, ramp_ends[1] + m),
        (-h + 22 + m, ramp_ends[2] + m),           # 45-degree ramp (x -h+26..-h+32)
        (-h + 32 + m, ramp_ends[2] + m),
        (-h + 32 + m, -w),                         # -Z wall
        (h - 22 - m, -w),                          # staircase (x h-22..h-12, z -h+8..-h+24)
        (h - 22 - m, -h + 24 + m),
        (h - 12 + m, -h + 24 + m),
        (h - 12 + m, -w),
        (w, -w),                                   # +X wall up to the platform
        (w, h - 26 - m),
        (h - 18 + m, h - 26 - m),                  # platform (x h-30..h-6, z h-26..h-6) and its ramp
        (h - 18 + m, h - 58 - m),                  # (x h-26..h-18, z h-58..h-26)
        (h - 26 - m, h - 58 - m),
        (h - 26 - m, h - 26 - m),
        (h - 30 - m, h - 26 - m),
        (h - 30 - m, w),                           # +Z wall
        (-w, w),
    ]
    obstacles = []
    for i, height in enumerate((0.5, 1.0, 1.5, 2.0, 3.0)):                         # step blocks, 6 x 6
        obstacles.append((-20 + i * 9 + 3.0, 9.0, 3.0 * math.sqrt(2) + m, max(height, 1.0)))
    for x, z in ((-25, 30), (-5, 30), (-25, 45), (-5, 45)):                        # pillars, 4 x 4
        obstacles.append((float(x), float(z), 2.0 * math.sqrt(2) + m, 14.0))
    return outline, obstacles


# ------------------------------------------------------------------------------------------------------------- main

def verify(path: str) -> list[str]:
    """Round-trips both entries of one archive; returns the problems."""
    archive = Caff.load(path)
    problems = []
    for kind, parser in (("kameonetwork", Network.parse), ("kameozones", Zones.parse)):
        entry = entry_of(archive, kind)
        if entry is None:
            continue
        data = entry.read(archive)
        try:
            again = parser(data).to_bytes()
        except Exception as error:  # noqa: BLE001 - reported per file
            problems.append(f"{kind}: {error}")
            continue
        if again != data:
            first = next((i for i, (a, b) in enumerate(zip(again, data)) if a != b), min(len(again), len(data)))
            problems.append(f"{kind}: round trip differs at +{first:#x} ({len(data)} -> {len(again)} bytes)")
    return problems


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: python -m kameo_modkit.network <file.lvl> ...   (describes and round-trips network and zones)")
        return 1
    status = 0
    for path in argv[1:]:
        archive = Caff.load(path)
        print(path)
        for kind, loader in (("kameonetwork", load_network), ("kameozones", load_zones)):
            if entry_of(archive, kind) is not None:
                print("  " + loader(archive)[1].describe().replace("\n", "\n  "))
        for problem in verify(path):
            print("  PROBLEM:", problem)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
