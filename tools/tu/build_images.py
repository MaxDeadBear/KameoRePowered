"""Builds the vanilla and title-update game images (as loaded at 0x82000000) from default.xex and default.xexp,
mirroring rexglue's XexModule::ReadImageCompressed and ApplyPatch, with its own AES and LZX from rexruntimerd.dll.
Writes out/tu/vanilla_image.bin and out/tu/tu_image.bin. Needs a relwithdebinfo build for rexruntimerd.dll and the
title update's delta patch at assets/TU/default.xexp."""
import ctypes, hashlib, os, struct

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HERE = os.path.join(ROOT, "out", "tu")
DLL_DIR = os.path.join(ROOT, "out", "build", "relwithdebinfo")
XEX = os.path.join(ROOT, "assets", "default.xex")
XEXP = os.path.join(ROOT, "assets", "TU", "default.xexp")
os.makedirs(HERE, exist_ok=True)
RETAIL_KEY = bytes([0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91])

os.add_dll_directory(DLL_DIR)
rt = ctypes.CDLL(os.path.join(DLL_DIR, "rexruntimerd.dll"))
aes = getattr(rt, "?aes_decrypt_buffer@@YAXPEBE0_KPEAE1@Z")
aes.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
lzx = getattr(rt, "?lzx_decompress@@YAHPEBX_KPEAX1I21@Z")
lzx.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t]
delta = getattr(rt, "?lzxdelta_apply_patch@@YAHPEAUxex2_delta_patch@rex@@_KIPEAX@Z")
delta.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p]


def decrypt(key, data):
    out = ctypes.create_string_buffer(len(data))
    aes(key, bytes(data), len(data), out, len(data))
    return out.raw


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def opt(header, key):
    for i in range(u32(header, 0x14)):
        k, v = struct.unpack_from(">II", header, 0x18 + 8 * i)
        if k == key:
            return v
    raise KeyError(hex(key))


def blocks(data, first_size, first_hash):
    """Yields each block's payload (after its 24-byte next-block info), checking hashes."""
    p, size, digest = 0, first_size, first_hash
    while size:
        block = data[p:p + size]
        assert hashlib.sha1(block).digest() == digest, "block hash mismatch: wrong key"
        nsize, ndigest = struct.unpack_from(">I", block, 0)[0], block[4:24]
        yield block[24:]
        p += size
        size, digest = nsize, ndigest


xex = open(XEX, "rb").read()
header = bytearray(xex[:u32(xex, 8)])
sec = u32(header, 0x10)
image_size = u32(header, sec + 4)
session = decrypt(RETAIL_KEY, header[sec + 0x150:sec + 0x160])
ff = opt(header, 0x3FF)
enc, comp = struct.unpack_from(">HH", header, ff + 4)
window, first_size = struct.unpack_from(">II", header, ff + 8)
first_hash = header[ff + 16:ff + 36]
assert comp == 2, f"expected normal compression, got {comp}"
body = xex[len(header):]
if enc == 1:
    body = decrypt(session, body)
stream = bytearray()
for payload in blocks(body, first_size, first_hash):
    q = 0
    while True:
        n = (payload[q] << 8) | payload[q + 1]
        q += 2
        if not n:
            break
        stream += payload[q:q + n]
        q += n
image = ctypes.create_string_buffer(max(image_size, 0x2000000))
assert lzx(bytes(stream), len(stream), image, image_size, window, None, 0) == 0, "lzx failed"
assert image.raw[:2] == b"MZ"
open(os.path.join(HERE, "vanilla_image.bin"), "wb").write(image.raw[:image_size])
print("vanilla image", hex(image_size))

# The patch.
xexp = open(XEXP, "rb").read()
pheader = xexp[:u32(xexp, 8)]
d = opt(pheader, 0x5FF)
(target_headers, hs_off, hs_size, ht_off, is_off, is_size, it_off) = struct.unpack_from(">7I", pheader, d + 0x30)
info_compressed = struct.unpack_from(">H", pheader, d + 0x4C + 10)[0]
target = target_headers or ht_off + hs_size
hbuf = ctypes.create_string_buffer(bytes(header) + bytes(max(0, target - len(header))), max(len(header), target))
if hs_off:
    ctypes.memmove(ctypes.addressof(hbuf) + ht_off, ctypes.addressof(hbuf) + hs_off, hs_size)
pff = opt(pheader, 0x3FF)
penc = struct.unpack_from(">H", pheader, pff + 4)[0]
pwindow, pfirst_size = struct.unpack_from(">II", pheader, pff + 8)
pfirst_hash = pheader[pff + 16:pff + 36]
info = ctypes.create_string_buffer(pheader[d + 0x4C:d + 0x4C + info_compressed + 0xC] + bytes(64))
assert delta(info, info_compressed + 0xC, window, hbuf) == 0, "header patch failed"
patched = hbuf.raw[:target]
psec = u32(patched, 0x10)
new_size = u32(patched, psec + 4)
base_session = decrypt(RETAIL_KEY, patched[psec + 0x150:psec + 0x160])
xsec = u32(pheader, 0x10)
patch_session = decrypt(base_session, pheader[xsec + 0x150:xsec + 0x160])
assert decrypt(base_session, pheader[d + 0x20:d + 0x30]) == session, "image key check failed"
pdata = xexp[len(pheader):]
if penc == 1:
    pdata = decrypt(patch_session, pdata)
if is_off:
    ctypes.memmove(ctypes.addressof(image) + it_off, ctypes.addressof(image) + is_off, is_size)
target_image = it_off + is_size
if target_image < image_size:
    ctypes.memset(ctypes.addressof(image) + target_image, 0, image_size - target_image)
for payload in blocks(pdata, pfirst_size, pfirst_hash):
    buf = ctypes.create_string_buffer(bytes(payload) + bytes(64))
    assert delta(buf, len(payload), pwindow, image) == 0, "image patch failed"
open(os.path.join(HERE, "tu_image.bin"), "wb").write(image.raw[:new_size])
print("tu image", hex(new_size))
