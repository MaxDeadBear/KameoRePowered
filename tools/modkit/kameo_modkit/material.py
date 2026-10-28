"""Reads materials from ndShaderParam2 nodes.

ndShaderParam2 (ndShaderParam2Render, shaderDefaultLoadParams, SetColours, SetupTextures): +48 points at a
pass set whose +4 holds 40-byte passes; a pass's +0 is its parameter block:
  +0  colour constants, 20 bytes each (RGBA floats, register), count u16 at +22
  +16 texture samplers, 36 bytes each (texture palette index, lightmap flag bit 0 of +29), count u8 at +20
  +24 alpha test reference (u8)
  +76 named parameters, 16 bytes each (name, ?, slot, kind), count at +80; kind 2 names a colour constant and
      kind 3 a sampler slot, whose channel is named twice, by the channel ("colour0") and its Maya file node
A palette index above 0xFFFF is a texture the game supplies at run time (shadow maps, environment maps).
The rendergraph's texture palette is the pointer array at +84.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

from .graph import Node, Ref, Resolver

KIND_CONSTANT = 2
KIND_SAMPLER = 3
CHANNELS = ("colour", "specular", "transparency", "reflectivity", "bump", "normal", "incandescence", "rolloff")


@dataclass
class Sampler:
    slot: int
    palette_index: int
    lightmap: bool
    channel: str = ""
    file_node: str = ""

    @property
    def dynamic(self) -> bool:
        return self.palette_index > 0xFFFF


@dataclass
class Material:
    node: Node
    colours: list[tuple[float, float, float, float]] = field(default_factory=list)
    samplers: list[Sampler] = field(default_factory=list)
    names: list[tuple[str, int, int]] = field(default_factory=list)
    constants: dict[str, tuple[float, float, float, float]] = field(default_factory=dict)
    alpha_reference: int = 0

    def channel(self, name: str) -> Sampler | None:
        return next((s for s in self.samplers if s.channel == name and not s.dynamic), None)

    def base_colour(self) -> tuple[float, float, float, float] | None:
        """The named colour constant, or else the first constant that is a colour rather than a scalar (one value repeated in all four channels, such as a specular power)."""
        if "colour" in self.constants:
            return self.constants["colour"]
        for red, green, blue, alpha in self.colours:
            if max(red, green, blue) <= 1.0 and not (red == green == blue == alpha):
                return (red, green, blue, alpha)
        return None


def read_material(resolver: Resolver, node: Node) -> Material:
    material = Material(node)
    pass_set = resolver.pointer(node.ref, 48)
    passes = resolver.pointer(pass_set, 4) if pass_set else None
    params = resolver.pointer(passes, 0) if passes else None
    if params is None:
        return material
    colours = resolver.pointer(params, 0)
    for i in range(resolver.u16(params, 22) if colours else 0):
        item = Ref(colours.entry, colours.offset + 20 * i)
        material.colours.append(tuple(resolver.f32(item, k) for k in (0, 4, 8, 12)))
    samplers = resolver.pointer(params, 16)
    for i in range(resolver.u8(params, 20) if samplers else 0):
        item = Ref(samplers.entry, samplers.offset + 36 * i)
        material.samplers.append(Sampler(i, resolver.u32(item, 0), bool(resolver.u8(item, 29) & 1)))
    material.alpha_reference = resolver.u8(params, 24)
    names = resolver.pointer(params, 76)
    for i in range(resolver.u32(params, 80) if names else 0):
        item = Ref(names.entry, names.offset + 16 * i)
        material.names.append((resolver.string(resolver.pointer(item, 0)), resolver.u32(item, 8), resolver.u32(item, 12)))
    for name, slot, kind in material.names:
        if kind == KIND_CONSTANT and slot < len(material.colours):
            channel = re.sub(r"\d+$", "", name)
            if channel in CHANNELS and channel not in material.constants:
                material.constants[channel] = material.colours[slot]
            continue
        if kind != KIND_SAMPLER or slot >= len(material.samplers):
            continue
        sampler = material.samplers[slot]
        channel = re.sub(r"\d+$", "", name)
        if channel in CHANNELS and not sampler.channel:
            sampler.channel = channel
        elif name and not sampler.file_node:
            sampler.file_node = name
    return material


def texture_palette(resolver: Resolver, rendergraph: Ref) -> list[Ref | None]:
    """The rendergraph's texture palette: each entry the texture asset it names, in palette order."""
    palette = resolver.pointer(rendergraph, 84)
    if palette is None:
        return []
    # Its length is +32: the list has no end marker, and other pointers can follow it.
    return [resolver.pointer(palette, 4 * i) for i in range(resolver.u32(rendergraph, 32))]
