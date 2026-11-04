"""A test-level arena built in code: a walled box with ramps, stairs, step blocks, a platform and pillars, the kind of
grey-box map games use to try movement against.

Everything is in game space (Y up). Faces are flat-shaded (each face its own vertices) and wound so that
(v1 - v0) x (v2 - v0) is the outward normal, the front face for collision (hits.py) and drawing alike. UVs are
projected from world coordinates along the face's main axis, one texture tile every `tile` units, so textures keep
their size on every surface. Each triangle carries a part: FLOOR, WALL or BLOCK, for the material it draws with.

Usage: python -m kameo_modkit.testarena out.obj   (writes the arena as OBJ to look at)
"""

from __future__ import annotations

import math
import sys
from dataclasses import dataclass, field

import numpy as np

FLOOR, WALL, BLOCK = 0, 1, 2


@dataclass
class Mesh:
    vertices: list[tuple[float, float, float]] = field(default_factory=list)
    normals: list[tuple[float, float, float]] = field(default_factory=list)
    uvs: list[tuple[float, float]] = field(default_factory=list)
    triangles: list[tuple[int, int, int, int]] = field(default_factory=list)  # v0, v1, v2, part
    tile: float = 8.0

    def quad(self, corners, part: int) -> None:
        """A planar quad, corners in order around its front face (counter-clockwise seen from the front)."""
        p = np.array(corners, dtype=np.float64)
        normal = np.cross(p[1] - p[0], p[2] - p[0])
        length = np.linalg.norm(normal)
        if length < 1e-9:
            return
        normal /= length
        axis = int(np.argmax(np.abs(normal)))
        u_axis, v_axis = [(2, 1), (0, 2), (0, 1)][axis]
        first = len(self.vertices)
        for corner in p:
            self.vertices.append(tuple(corner))
            self.normals.append(tuple(normal))
            self.uvs.append((corner[u_axis] / self.tile, -corner[v_axis] / self.tile))
        self.triangles += [(first, first + 1, first + 2, part), (first, first + 2, first + 3, part)]

    def box(self, low, high, part: int = BLOCK, bottom: bool = False) -> None:
        """A solid box, faces outward (no bottom face by default: boxes stand on the floor)."""
        x0, y0, z0 = low
        x1, y1, z1 = high
        self.quad([(x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)], part)  # top
        self.quad([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], part)  # +Z
        self.quad([(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)], part)  # -Z
        self.quad([(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)], part)  # +X
        self.quad([(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)], part)  # -X
        if bottom:
            self.quad([(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], part)

    def ramp(self, x0: float, x1: float, z0: float, length: float, height: float, part: int = BLOCK) -> None:
        """A wedge rising along +Z from z0 to z0 + length, from the floor to `height`, between x0 and x1."""
        z1 = z0 + length
        self.quad([(x0, 0, z0), (x0, height, z1), (x1, height, z1), (x1, 0, z0)], part)  # slope
        self.quad([(x0, 0, z1), (x1, 0, z1), (x1, height, z1), (x0, height, z1)], part)  # back
        # Sides: triangles, as degenerate quads would be.
        for x, flip in ((x1, False), (x0, True)):
            corners = [(x, 0, z0), (x, 0, z1), (x, height, z1)]
            if flip:
                corners = corners[::-1]
            first = len(self.vertices)
            p = np.array(corners)
            n = np.cross(p[1] - p[0], p[2] - p[0])
            n /= np.linalg.norm(n)
            for c in corners:
                self.vertices.append(tuple(c))
                self.normals.append(tuple(n))
                self.uvs.append((c[2] / self.tile, -c[1] / self.tile))
            self.triangles.append((first, first + 1, first + 2, part))

    def arrays(self):
        return (np.array(self.vertices, np.float32), np.array(self.triangles, np.int64),
                np.array(self.normals, np.float32), np.array(self.uvs, np.float32))


def build(size: float = 120.0, wall: float = 14.0, tile: float = 8.0) -> Mesh:
    m = Mesh(tile=tile)
    h = size / 2
    # The floor and the walls of the room, facing in.
    m.quad([(-h, 0, -h), (-h, 0, h), (h, 0, h), (h, 0, -h)], FLOOR)
    m.quad([(-h, 0, h), (-h, wall, h), (h, wall, h), (h, 0, h)], WALL)      # far wall (+Z), faces -Z
    m.quad([(h, 0, -h), (h, wall, -h), (-h, wall, -h), (-h, 0, -h)], WALL)  # near wall (-Z), faces +Z
    m.quad([(h, 0, h), (h, wall, h), (h, wall, -h), (h, 0, -h)], WALL)      # +X wall, faces -X
    m.quad([(-h, 0, -h), (-h, wall, -h), (-h, wall, h), (-h, 0, h)], WALL)  # -X wall, faces +X

    # Ramps of 15, 30 and 45 degrees along the -X side, each 6 units wide and 6 high.
    for i, degrees in enumerate((15, 30, 45)):
        height = 6.0
        length = height / math.tan(math.radians(degrees))
        x0 = -h + 6 + i * 10
        m.ramp(x0, x0 + 6, -h + 8, length, height)

    # A staircase of 0.5-unit steps, 2 deep, up to 4 units, along the +X side.
    for i in range(8):
        m.box((h - 22, 0, -h + 8 + 2 * i), (h - 12, 0.5 * (i + 1), -h + 10 + 2 * i))

    # Step-height test: blocks of 0.5, 1, 1.5, 2 and 3 units in a row across the middle.
    for i, height in enumerate((0.5, 1.0, 1.5, 2.0, 3.0)):
        x = -20 + i * 9
        m.box((x, 0, 6), (x + 6, height, 12))

    # A platform 8 units up in the far corner, with a long ramp onto it.
    m.box((h - 30, 0, h - 26), (h - 6, 8.0, h - 6))
    m.ramp(h - 26, h - 18, h - 58, 32.0, 8.0)

    # Four pillars to walk around.
    for x, z in ((-25, 30), (-5, 30), (-25, 45), (-5, 45)):
        m.box((x - 2, 0, z - 2), (x + 2, wall, z + 2))
    return m


def write_obj(mesh: Mesh, path: str) -> None:
    with open(path, "w") as out:
        for v in mesh.vertices:
            out.write("v %.4f %.4f %.4f\n" % v)
        for n in mesh.normals:
            out.write("vn %.4f %.4f %.4f\n" % n)
        for t in mesh.uvs:
            out.write("vt %.4f %.4f\n" % t)
        names = {FLOOR: "floor", WALL: "wall", BLOCK: "block"}
        current = None
        for a, b, c, part in mesh.triangles:
            if part != current:
                out.write(f"usemtl {names[part]}\n")
                current = part
            out.write(f"f {a+1}/{a+1}/{a+1} {b+1}/{b+1}/{b+1} {c+1}/{c+1}/{c+1}\n")


if __name__ == "__main__":
    arena = build()
    if len(sys.argv) > 1:
        write_obj(arena, sys.argv[1])
    v, t, _, _ = arena.arrays()
    print(f"{len(t)} triangles, {len(v)} vertices, bounds {v.min(axis=0)} .. {v.max(axis=0)}")
