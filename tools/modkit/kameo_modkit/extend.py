"""Adds draws to a model: the material a glTF gives as a copy of one of the game's (graph<g>_mesh<m>_draw<d>.001, as
Blender names a duplicated material) becomes a draw of its own beside that one, with its own triangles, its own bone
palette and, where its images differ, its own textures.

A new draw borrows everything else from its template, the draw it copies: the vertex buffer, the shaders, the render
state. It is added in three places:
  the template's ndPushBuffer gains a slot (its five arrays copied longer to the end of the rendergraph, and a new
  index buffer header the game fills in at load, ndPushBufferInitNode);
  the command stream gains packets right after the template's draw: its textures bound (per shader variant, as the
  material's own 0x2D packets bind them, but from new palette slots), its bone palette, its draw, and the template's
  textures and palette put back. Every pointer into the stream past that point moves up;
  for a new texture, a new texture asset entry (a copy of the template's, with a copy of its data) and a new slot at
  the end of the rendergraph's texture palette (+84, its length at +32), the instance block that copies the palette
  (its layout descriptor at +136) a slot longer.
The importer then fills the new draws like any other: their triangles, bone palette and images.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .caff import Caff, Entry, Relocation
from .graph import Ref, Resolver, rendergraph_roots
from .material import texture_palette
from .mesh import collect_meshes
from .texture import read_texture

INDEX_HEADER_BYTES = 32


class Builder:
    """An archive being grown: entry contents, pointers (relocations) and new entries."""

    def __init__(self, archive: Caff):
        self.archive = archive
        self.contents: dict[int, bytearray] = {}
        self.pointers: dict[tuple[int, int], list[int]] = {(r.source, r.target): list(r.offsets) for r in archive.relocations}

    def data(self, index: int) -> bytearray:
        if index not in self.contents:
            self.contents[index] = bytearray(self.archive.entries[index - 1].read(self.archive))
        return self.contents[index]

    def u32(self, index: int, offset: int) -> int:
        return struct.unpack_from(">I", self.data(index), offset)[0]

    def put(self, index: int, offset: int, value: int) -> None:
        struct.pack_into(">I", self.data(index), offset, value)

    def target(self, index: int, offset: int) -> int | None:
        """The entry a pointer field points into, or None when the field is not a pointer."""
        for (source, target), offsets in self.pointers.items():
            if source == index and offset in offsets:
                return target
        return None

    def append(self, index: int, blob: bytes, alignment: int = 4) -> int:
        data = self.data(index)
        data += bytes(-len(data) % alignment)
        offset = len(data)
        data += blob
        return offset

    def forget(self, source: int, offset: int) -> None:
        """No longer a pointer: a field of data nothing uses any more."""
        for (s, _), offsets in self.pointers.items():
            if s == source and offset in offsets:
                offsets.remove(offset)

    def point(self, source: int, offset: int, target: int, value: int) -> None:
        self.put(source, offset, value)
        self.pointers.setdefault((source, target), []).append(offset)

    def insert(self, index: int, at: int, blob: bytes) -> None:
        """`blob` put into entry `index` at `at`: pointers to `at` or beyond move up, as do pointer fields there."""
        size = len(blob)
        for (source, target), offsets in self.pointers.items():
            if target != index:
                continue
            for offset in offsets:
                value = self.u32(source, offset)
                if value >= at:
                    self.put(source, offset, value + size)
        self.data(index)[at:at] = blob
        for (source, _), offsets in self.pointers.items():
            if source == index:
                offsets[:] = [o + size if o >= at else o for o in offsets]

    def add_entry(self, template: int, blob: bytes) -> int:
        """A new entry like `template` (same section and type, a symbol of its own), placed after its section's others."""
        old = self.archive.entries[template - 1]
        section_end = max(e.offset + e.size for e in self.archive.entries if e.section == old.section)
        index = len(self.archive.entries) + 1
        symbol = self.archive.symbol_count + 1
        self.archive.symbol_count = symbol
        self.archive.entries.append(Entry(index, symbol, (section_end + 4095) // 4096 * 4096 + 4096 * index, len(blob), old.section, old.section_type))
        self.contents[index] = bytearray(blob)
        for (source, target), offsets in list(self.pointers.items()):
            if source == template:
                self.pointers.setdefault((index, index if target == template else target), []).extend(offsets)
        return index

    def finish(self) -> Caff:
        self.archive.relocations = [Relocation(s, t, sorted(o)) for (s, t), o in sorted(self.pointers.items()) if o]
        return Caff.parse(self.archive.write({i: bytes(d) for i, d in self.contents.items()}))


@dataclass
class NewDraw:
    key: tuple[int, int, int, int]  # (graph, mesh, draw, copy)
    textures: dict[str, int] = field(default_factory=dict)  # slot -> the glTF image it shows, where it differs from the template's


@dataclass
class Added:
    """What the importer needs about the draws added: each by (push buffer offset, slot)."""
    identity: dict[tuple[int, int, int, int], tuple[int, int]] = field(default_factory=dict)
    palettes: dict[tuple[int, int], tuple[int, int]] = field(default_factory=dict)  # draw -> its palette list (entry, offset)
    textures: dict[tuple[int, int], dict[str, tuple[int, int]]] = field(default_factory=dict)  # draw -> slot -> texture asset (entry, offset)


def _packets(data: bytearray, start: int, stop_at_jump: bool = True):
    offset = start
    while offset + 4 <= len(data):
        size, command, flags = struct.unpack_from(">HBB", data, offset)
        if size < 4:
            return
        yield offset, size, command, flags
        offset += size
        if command in (0x19, 0x1D) and stop_at_jump:
            return


def _palette_packet(data: bytearray, start: int) -> int | None:
    """The bone palette packet (0x13) of a skin node's run, which can set a render state (0x0D) before it."""
    for offset, _, command, _ in _packets(data, start):
        if command == 0x13:
            return offset
        if command in (0x01, 0x30, 0x31):
            return None
    return None


def _texture_packet(palette_index: int, sampler: int) -> bytes:
    """0x2C: set the texture `sampler` samples to the texture palette's slot `palette_index`."""
    return struct.pack(">HBBIIHH", 16, 0x2C, 0x04, palette_index, 0, sampler, 0)


def add_draws(archive: Caff, new_draws: list[NewDraw], images_of_template) -> tuple[Caff, Added]:
    """`archive` with `new_draws` added. `images_of_template(key)` gives the glTF images a template's material shows,
    by slot, to tell which of a new draw's textures are its own."""
    resolver = Resolver(archive)
    builder = Builder(archive)
    added = Added()
    copies: dict[tuple, tuple[int, int]] = {}  # (template texture asset, glTF image) -> its copy's palette index and entry

    graphs = [e for e in archive.entries if resolver.type_name(e) == "rendergraph"]
    draws = {}
    for graph, entry in enumerate(graphs):
        for number, mesh in enumerate(collect_meshes(resolver, rendergraph_roots(resolver, entry))):
            for d, draw in enumerate(mesh.draws):
                draws[(graph, number, d)] = (draw, entry)
    stream_entries = {e.index for e in archive.entries if archive.sections[e.section - 1].name == ".stream"}

    by_push: dict[tuple[int, int], list[NewDraw]] = {}
    for new in new_draws:
        template, _ = draws[new.key[:3]]
        by_push.setdefault((template.push.ref.entry.index, template.push.ref.offset), []).append(new)

    for (graph_entry, push_offset), group in by_push.items():
        # The push buffer's arrays, longer: header pointers, vertex counts, index pointers, primitive types and counts.
        count = builder.u32(graph_entry, push_offset + 48)
        arrays = [builder.u32(graph_entry, push_offset + k) for k in (52, 56, 60, 64, 68)]
        index_target = builder.target(graph_entry, arrays[2])
        old_values = [[builder.u32(graph_entry, a + 4 * i) for i in range(count)] for a in arrays]
        total = count + len(group)
        new_arrays = [builder.append(graph_entry, bytes(4 * total)) for _ in arrays]
        headers = [builder.append(graph_entry, bytes(INDEX_HEADER_BYTES), 32) for _ in group]
        for slot in range(total):
            new = group[slot - count] if slot >= count else None
            template_slot = draws[new.key[:3]][0].slot if new else slot
            builder.point(graph_entry, new_arrays[0] + 4 * slot, graph_entry, headers[slot - count] if new else old_values[0][slot])
            builder.put(graph_entry, new_arrays[1] + 4 * slot, old_values[1][template_slot])
            builder.point(graph_entry, new_arrays[2] + 4 * slot, index_target, old_values[2][template_slot])
            builder.put(graph_entry, new_arrays[3] + 4 * slot, old_values[3][template_slot])
            builder.put(graph_entry, new_arrays[4] + 4 * slot, old_values[4][template_slot] if not new else 1)
        for array in arrays:
            for i in range(count):
                builder.forget(graph_entry, array + 4 * i)
        builder.put(graph_entry, push_offset + 48, total)
        for k, offset in zip((52, 56, 60, 64, 68), new_arrays):
            builder.put(graph_entry, push_offset + k, offset)

        for position, new in enumerate(group):
            slot = count + position
            template, graph = draws[new.key[:3]]
            identity = (push_offset, slot)
            added.identity[new.key] = identity

            # Its own bone palette, a copy of the template's to start from.
            skin_list = resolver.pointer(template.skin.ref, 48) if template.skin is not None else None
            palette_offset = None
            if skin_list is not None:
                length = resolver.u32(template.skin.ref, 52)
                palette_offset = builder.append(graph_entry, bytes(builder.data(skin_list.entry.index)[skin_list.offset:skin_list.offset + length]))
                added.palettes[identity] = (graph_entry, palette_offset)

            # Its own textures, copies of the template's, where its images differ. A template whose material is not in
            # the glTF (its faces deleted, a whole body replaced) shows no image, so every image of the new draw is its own.
            slots: dict[str, int] = {}
            new_assets: dict[str, tuple[int, int]] = {}
            template_images = images_of_template(new.key[:3])
            palette = texture_palette(resolver, Ref(graph, 0))
            for slot_name, image in new.textures.items():
                if template_images.get(slot_name) == image:
                    continue
                asset, old_index = _template_texture(resolver, template, palette, slot_name)
                if asset is None:
                    continue
                # Draws split from one material show one copy of each texture.
                shared = (asset.entry.index, asset.offset, image)
                if shared not in copies:
                    copies[shared] = _copy_texture(builder, resolver, graph.index, asset)
                new_index, new_asset = copies[shared]
                slots[slot_name] = (old_index, new_index)
                new_assets[slot_name] = (new_asset, 0)
            added.textures[identity] = new_assets

            # The packets, right after each of the template's draws.
            header_pointer = headers[position]
            template_header = old_values[0][template.slot]
            for stream in stream_entries:
                _insert_draw(builder, resolver, stream, graph_entry, template, template_header, header_pointer, palette_offset, slots)
    return builder.finish(), added


def _template_texture(resolver: Resolver, template, palette, slot_name: str):
    """The template's texture for a slot, as the export chose it, and its palette index."""
    from .material import read_material

    for sampler in read_material(resolver, template.material).samplers:
        if sampler.dynamic or sampler.lightmap or sampler.palette_index >= len(palette) or palette[sampler.palette_index] is None:
            continue
        normal = read_texture(resolver, palette[sampler.palette_index]).format_name == "DXN"
        if (slot_name == "normal" and normal) or (slot_name == "colour" and not normal and sampler.channel == "colour"):
            return palette[sampler.palette_index], sampler.palette_index
    return None, None


def _copy_texture(builder: Builder, resolver: Resolver, graph_entry: int, asset: Ref) -> tuple[int, int]:
    """A new texture asset copying `asset`, its data copied too, put at the end of the rendergraph's texture palette."""
    archive = builder.archive
    template_entry = asset.entry.index
    data_entry = builder.target(template_entry, 40)
    data_offset = builder.u32(template_entry, 40)
    starts = sorted({builder.u32(source, offset) for (source, target), offsets in builder.pointers.items() if target == data_entry for offset in offsets})
    following = [s for s in starts if s > data_offset]
    end = following[0] if following else len(builder.data(data_entry))
    copy = bytes(builder.data(data_entry)[data_offset:end])
    new_entry = builder.add_entry(template_entry, bytes(builder.data(template_entry)))
    new_data = builder.append(data_entry, copy, 4096)
    builder.put(new_entry, 40, new_data)

    # The palette, copied longer, with the new texture last.
    list_offset = builder.u32(graph_entry, 84)
    # Its length is +32: the list has no end marker, and pointers can follow it.
    entries = [(builder.target(graph_entry, list_offset + 4 * i), builder.u32(graph_entry, list_offset + 4 * i)) for i in range(builder.u32(graph_entry, 32))]
    entries.append((new_entry, 0))
    new_list = builder.append(graph_entry, bytes(4 * (len(entries) + 1)))
    for i, (target, value) in enumerate(entries):
        builder.point(graph_entry, new_list + 4 * i, target, value)
    builder.put(graph_entry, 84, new_list)
    # The rendergraph keeps the palette's length too (+32), and each model instance draws from its own copy of the
    # palette (sgSetupInstance copies that many slots), the last buffer of the instance block its layout descriptor
    # (+136: block size, then field/offset pairs placing the block's buffers) lays out. The block grows by a slot, or
    # the slot past its end is overwritten by what follows the instance.
    builder.put(graph_entry, 32, len(entries))
    descriptor = builder.u32(graph_entry, 136)
    block = builder.u32(graph_entry, descriptor)
    fixups = [struct.unpack_from(">II", builder.data(graph_entry), descriptor + 20 + 8 * i) for i in range(builder.u32(graph_entry, descriptor + 16))]
    slot = next((i for i, (field, _) in enumerate(fixups) if field == 4), None)
    if slot is None:
        raise ValueError("this model's instance layout has no texture palette; adding a texture to it is not supported")
    if fixups[slot][1] + 4 * (len(entries) - 1) == block:
        builder.put(graph_entry, descriptor, block + 4)
    else:
        # A palette followed by other buffers moves to the end of the block, where it can grow.
        moved = (block + 15) // 16 * 16
        builder.put(graph_entry, descriptor + 20 + 8 * slot + 4, moved)
        builder.put(graph_entry, descriptor, moved + 4 * len(entries))
    return len(entries) - 1, new_entry


def _insert_draw(builder: Builder, resolver: Resolver, stream: int, graph_entry: int, template, template_header: int, header: int, palette_offset: int | None, slots: dict[str, tuple[int, int]]) -> None:
    data = builder.data(stream)
    sites = sorted(offset - 16 for offset in builder.pointers.get((stream, graph_entry), []) if builder.u32(stream, offset) == template_header and offset >= 16 and data[offset - 14] in (0x01, 0x30, 0x31))
    if not sites:
        return
    # The template's bone palette packet and its material's variant jump, from their nodes' entry points.
    skin_packet = _palette_packet(data, builder.u32(graph_entry, template.skin.ref.offset + 40)) if template.skin is not None else None
    variant_jump = builder.u32(graph_entry, template.material.ref.offset + 40) if template.material is not None else None

    for site in reversed(sites):
        draw_size = struct.unpack_from(">H", data, site)[0]
        at = site + draw_size
        blob = bytearray()
        fields: list[tuple[int, int, int]] = []  # (offset in blob, target entry, value; a stream value is relative to `at`)

        def textures(restore: bool) -> None:
            if not slots or variant_jump is None:
                return
            size, command, flags = struct.unpack_from(">HBB", data, variant_jump)
            if command != 0x16:
                return
            targets = [builder.u32(stream, variant_jump + 8 + 4 * v) for v in range((size - 8) // 4)]
            jump_at = len(blob)
            blob.extend(data[variant_jump:variant_jump + 8] + bytes(4 * len(targets)))
            ends = []
            for v, target in enumerate(targets):
                fields.append((jump_at + 8 + 4 * v, stream, len(blob)))
                for offset, psize, pcommand, _ in _packets(data, target):
                    if pcommand != 0x2D:
                        continue
                    for i in range(struct.unpack_from(">H", data, offset + 8)[0]):
                        palette_index, sampler = struct.unpack_from(">IH", data, offset + 12 + 8 * i)
                        for old_index, new_index in slots.values():
                            if palette_index == old_index:
                                blob.extend(_texture_packet(old_index if restore else new_index, sampler))
                ends.append(len(blob))
                blob.extend(struct.pack(">HBBI", 8, 0x19, 0x04, 0))
            for end in ends:
                fields.append((end + 4, stream, len(blob)))

        textures(restore=False)
        if palette_offset is not None and skin_packet is not None:
            psize = struct.unpack_from(">H", data, skin_packet)[0]
            packet = bytearray(data[skin_packet:skin_packet + psize])
            packet[10] = 0
            fields.append((len(blob) + 4, graph_entry, palette_offset))
            blob.extend(packet)
        draw = bytearray(data[site:site + draw_size])
        fields.append((len(blob) + 16, graph_entry, header))
        blob.extend(draw)
        textures(restore=True)
        if palette_offset is not None and skin_packet is not None:
            psize = struct.unpack_from(">H", data, skin_packet)[0]
            packet = bytearray(data[skin_packet:skin_packet + psize])
            packet[10] = 0
            fields.append((len(blob) + 4, graph_entry, builder.u32(stream, skin_packet + 4)))
            blob.extend(packet)

        builder.insert(stream, at, bytes(blob))
        for offset, target, value in fields:
            builder.point(stream, at + offset, target, at + value if target == stream else value)
        data = builder.data(stream)
        skin_packet = _palette_packet(data, builder.u32(graph_entry, template.skin.ref.offset + 40)) if template.skin is not None else None
        variant_jump = builder.u32(graph_entry, template.material.ref.offset + 40) if template.material is not None else None
