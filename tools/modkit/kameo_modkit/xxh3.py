"""XXH3-64 with a seed, the hash the game names textures by (TextureContentHash in src/gpu/textures/texture_pack.cpp).

Blender's Python has NumPy but not the xxhash package, so this follows the reference (xxhash.h, XXH3_64bits_withSeed)
directly: the short inputs in plain Python and the long input's stripes summed a block at a time in NumPy. The xxhash
package is used instead where it is installed.
"""

from __future__ import annotations

import struct

import numpy as np

try:
    import xxhash as _xxhash
except ImportError:
    _xxhash = None

MASK = (1 << 64) - 1
PRIME32_1, PRIME32_2, PRIME32_3 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D
PRIME64_1, PRIME64_2, PRIME64_3, PRIME64_4, PRIME64_5 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5
PRIME_MX1, PRIME_MX2 = 0x165667919E3779F9, 0x9FB21C651E98DF25

SECRET = bytes([
    0xB8, 0xFE, 0x6C, 0x39, 0x23, 0xA4, 0x4B, 0xBE, 0x7C, 0x01, 0x81, 0x2C, 0xF7, 0x21, 0xAD, 0x1C,
    0xDE, 0xD4, 0x6D, 0xE9, 0x83, 0x90, 0x97, 0xDB, 0x72, 0x40, 0xA4, 0xA4, 0xB7, 0xB3, 0x67, 0x1F,
    0xCB, 0x79, 0xE6, 0x4E, 0xCC, 0xC0, 0xE5, 0x78, 0x82, 0x5A, 0xD0, 0x7D, 0xCC, 0xFF, 0x72, 0x21,
    0xB8, 0x08, 0x46, 0x74, 0xF7, 0x43, 0x24, 0x8E, 0xE0, 0x35, 0x90, 0xE6, 0x81, 0x3A, 0x26, 0x4C,
    0x3C, 0x28, 0x52, 0xBB, 0x91, 0xC3, 0x00, 0xCB, 0x88, 0xD0, 0x65, 0x8B, 0x1B, 0x53, 0x2E, 0xA3,
    0x71, 0x64, 0x48, 0x97, 0xA2, 0x0D, 0xF9, 0x4E, 0x38, 0x19, 0xEF, 0x46, 0xA9, 0xDE, 0xAC, 0xD8,
    0xA8, 0xFA, 0x76, 0x3F, 0xE3, 0x9C, 0x34, 0x3F, 0xF9, 0xDC, 0xBB, 0xC7, 0xC7, 0x0B, 0x4F, 0x1D,
    0x8A, 0x51, 0xE0, 0x4B, 0xCD, 0xB4, 0x59, 0x31, 0xC8, 0x9F, 0x7E, 0xC9, 0xD9, 0x78, 0x73, 0x64,
    0xEA, 0xC5, 0xAC, 0x83, 0x34, 0xD3, 0xEB, 0xC3, 0xC5, 0x81, 0xA0, 0xFF, 0xFA, 0x13, 0x63, 0xEB,
    0x17, 0x0D, 0xDD, 0x51, 0xB7, 0xF0, 0xDA, 0x49, 0xD3, 0x16, 0x55, 0x26, 0x29, 0xD4, 0x68, 0x9E,
    0x2B, 0x16, 0xBE, 0x58, 0x7D, 0x47, 0xA1, 0xFC, 0x8F, 0xF8, 0xB8, 0xD1, 0x7A, 0xD0, 0x31, 0xCE,
    0x45, 0xCB, 0x3A, 0x8F, 0x95, 0x16, 0x04, 0x28, 0xAF, 0xD7, 0xFB, 0xCA, 0xBB, 0x4B, 0x40, 0x7E,
])
STRIPE, STRIPES_PER_BLOCK = 64, (len(SECRET) - 64) // 8
BLOCK = STRIPE * STRIPES_PER_BLOCK


def _u64(data: bytes, at: int) -> int:
    return struct.unpack_from("<Q", data, at)[0]


def _u32(data: bytes, at: int) -> int:
    return struct.unpack_from("<I", data, at)[0]


def _fold(a: int, b: int) -> int:
    product = a * b
    return (product & MASK) ^ (product >> 64)


def _avalanche(h: int) -> int:
    h ^= h >> 37
    h = (h * PRIME_MX1) & MASK
    return h ^ (h >> 32)


def _xxh64_avalanche(h: int) -> int:
    h ^= h >> 33
    h = (h * PRIME64_2) & MASK
    h ^= h >> 29
    h = (h * PRIME64_3) & MASK
    return h ^ (h >> 32)


def _rotl(x: int, r: int) -> int:
    return ((x << r) | (x >> (64 - r))) & MASK


def _mix16(data: bytes, at: int, secret: bytes, key: int, seed: int) -> int:
    return _fold(_u64(data, at) ^ ((_u64(secret, key) + seed) & MASK), _u64(data, at + 8) ^ ((_u64(secret, key + 8) - seed) & MASK))


def _short(data: bytes, seed: int) -> int:
    n = len(data)
    if n > 8:
        low = _u64(data, 0) ^ (((_u64(SECRET, 24) ^ _u64(SECRET, 32)) + seed) & MASK)
        high = _u64(data, n - 8) ^ (((_u64(SECRET, 40) ^ _u64(SECRET, 48)) - seed) & MASK)
        swapped = int.from_bytes(low.to_bytes(8, "little"), "big")
        return _avalanche((n + swapped + high + _fold(low, high)) & MASK)
    if n >= 4:
        seed ^= int.from_bytes((seed & 0xFFFFFFFF).to_bytes(4, "little"), "big") << 32
        keyed = (_u32(data, n - 4) + (_u32(data, 0) << 32)) ^ (((_u64(SECRET, 8) ^ _u64(SECRET, 16)) - seed) & MASK)
        keyed ^= _rotl(keyed, 49) ^ _rotl(keyed, 24)
        keyed = (keyed * PRIME_MX2) & MASK
        keyed ^= (keyed >> 35) + n
        keyed = (keyed * PRIME_MX2) & MASK
        return keyed ^ (keyed >> 28)
    if n:
        combined = (data[0] << 16) | (data[n >> 1] << 24) | data[n - 1] | (n << 8)
        return _xxh64_avalanche(combined ^ (((_u32(SECRET, 0) ^ _u32(SECRET, 4)) + seed) & MASK))
    return _xxh64_avalanche(seed ^ _u64(SECRET, 56) ^ _u64(SECRET, 64))


def _medium(data: bytes, seed: int) -> int:
    n = len(data)
    acc = (n * PRIME64_1) & MASK
    if n <= 128:
        for k in range((n - 1) // 32 + 1):
            acc += _mix16(data, 16 * k, SECRET, 32 * k, seed) + _mix16(data, n - 16 * (k + 1), SECRET, 32 * k + 16, seed)
        return _avalanche(acc & MASK)
    for k in range(8):
        acc += _mix16(data, 16 * k, SECRET, 16 * k, seed)
    acc = _avalanche(acc & MASK)
    for k in range(8, n // 16):
        acc += _mix16(data, 16 * k, SECRET, 16 * (k - 8) + 3, seed)
    acc += _mix16(data, n - 16, SECRET, 136 - 17, seed)
    return _avalanche(acc & MASK)


def _lanes(data: bytes, at: int, count: int) -> np.ndarray:
    return np.frombuffer(data, dtype="<u8", count=count, offset=at).astype(np.uint64)


def _stripe_sums(data: bytes, at: int, stripes: int, keys: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """The products and the swapped inputs `stripes` stripes from `at` add to the accumulators, each as eight lanes."""
    lanes = _lanes(data, at, stripes * 8).reshape(stripes, 8)
    keyed = lanes ^ keys[:stripes]
    products = (keyed & np.uint64(0xFFFFFFFF)) * (keyed >> np.uint64(32))
    return products.sum(axis=0, dtype=np.uint64), lanes.sum(axis=0, dtype=np.uint64)[[1, 0, 3, 2, 5, 4, 7, 6]]


def _long(data: bytes, seed: int) -> int:
    secret = SECRET if seed == 0 else b"".join(struct.pack("<QQ", (_u64(SECRET, k) + seed) & MASK, (_u64(SECRET, k + 8) - seed) & MASK) for k in range(0, len(SECRET), 16))
    keys = np.stack([_lanes(secret, 8 * s, 8) for s in range(STRIPES_PER_BLOCK)])
    scramble = [_u64(secret, len(secret) - 64 + 8 * i) for i in range(8)]
    acc = [PRIME32_3, PRIME64_1, PRIME64_2, PRIME64_3, PRIME64_4, PRIME32_2, PRIME64_5, PRIME32_1]
    n = len(data)
    blocks = (n - 1) // BLOCK

    def add(products: np.ndarray, inputs: np.ndarray) -> None:
        for i in range(8):
            acc[i] = (acc[i] + int(products[i]) + int(inputs[i])) & MASK

    for b in range(blocks):
        add(*_stripe_sums(data, b * BLOCK, STRIPES_PER_BLOCK, keys))
        for i in range(8):
            a = acc[i] ^ (acc[i] >> 47) ^ scramble[i]
            acc[i] = (a * PRIME32_1) & MASK
    stripes = ((n - 1) - BLOCK * blocks) // STRIPE
    if stripes:
        add(*_stripe_sums(data, blocks * BLOCK, stripes, keys))
    last = np.stack([_lanes(secret, len(secret) - 64 - 7, 8)])
    add(*_stripe_sums(data, n - STRIPE, 1, last))
    result = (n * PRIME64_1) & MASK
    for k in range(4):
        result += _fold(acc[2 * k] ^ _u64(secret, 11 + 16 * k), acc[2 * k + 1] ^ _u64(secret, 11 + 16 * k + 8))
    return _avalanche(result & MASK)


def xxh3_64(data: bytes, seed: int = 0) -> int:
    seed &= MASK
    if _xxhash is not None:
        return _xxhash.xxh3_64_intdigest(data, seed=seed)
    data = bytes(data)
    if len(data) <= 16:
        return _short(data, seed)
    if len(data) <= 240:
        return _medium(data, seed)
    return _long(data, seed)
