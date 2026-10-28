"""Reads a character's skeleton: the ndSkeleton joints and the ndMtxArray that maps skinning matrices to joints.

ndSkeleton (modelGetMaxModelAnimJointsAndSkeleton, BuildMatricesFromJoints, skeletonSubtractPivots):
  +48 joint count, +52 the joint array (inline after the node), 36 bytes a joint:
    +0  translation from the parent joint (3 floats)
    +12 pivot: the joint's bind position in model space (3 floats)
    +24 parent joint index (u16, 0xFFFF for a root)
    +34 scale compensation flag (u8)
  The bind pose has no rotation: a joint's world matrix is its parent's times its translation, and the skinning
  matrix subtracts the pivot (skeletonSubtractPivots), so vertices are stored in model space and a joint's inverse
  bind matrix is a translation by minus its pivot. An animation adds its own offset to the stored translation
  (BuildMatricesFromJoints) rather than replacing it, so a joint can be moved by changing both fields.

ndMtxArray (ndMtxArrayBuildMatrices), the ndSkeleton's child: +48 entries of 8 bytes, the first u16 a joint index,
+52 their count. The draws' palettes (ndRigidSkinIdx) index these matrices.
"""

from __future__ import annotations

from dataclasses import dataclass

from .graph import Node, Ref, Resolver


@dataclass
class Joint:
    index: int
    parent: int  # -1 for a root
    translation: tuple[float, float, float]
    pivot: tuple[float, float, float]


@dataclass
class Skeleton:
    joints: list[Joint]
    matrix_joints: list[int]  # ndMtxArray: the joint each skinning matrix follows
    array: Ref | None = None  # the ndSkeleton's joint records

    def joint_of_matrix(self, matrix: int) -> int:
        return self.matrix_joints[matrix] if 0 <= matrix < len(self.matrix_joints) else 0


def _find(nodes: list[Node], kind: str) -> Node | None:
    for node in nodes:
        if node.type == kind:
            return node
        found = _find(node.children, kind)
        if found:
            return found
    return None


def read_skeleton(resolver: Resolver, roots: list[Node]) -> Skeleton | None:
    node = _find(roots, "ndSkeleton")
    if node is None:
        return None
    count = resolver.u32(node.ref, 48)
    array = resolver.pointer(node.ref, 52)
    if array is None or count == 0:
        return None
    joints = []
    for i in range(count):
        base = i * 36
        translation = tuple(resolver.f32(array, base + 4 * k) for k in range(3))
        pivot = tuple(resolver.f32(array, base + 12 + 4 * k) for k in range(3))
        parent = resolver.u16(array, base + 24)
        joints.append(Joint(i, -1 if parent == 0xFFFF or parent >= count else parent, translation, pivot))
    matrix_joints: list[int] = []
    mtx = _find(node.children, "ndMtxArray")
    if mtx is not None:
        entries = resolver.pointer(mtx.ref, 48)
        for i in range(resolver.u32(mtx.ref, 52) if entries else 0):
            joint = resolver.u16(entries, i * 8)
            matrix_joints.append(joint if joint < count else 0)
    return Skeleton(joints, matrix_joints, array)
