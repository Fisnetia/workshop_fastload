"""Minimal PE32 reader: resolve export RVAs and dump raw bytes so we can see how
steam_api.dll's flat ISteamRemoteStorage wrappers dispatch into the vtable."""
import struct
import sys

data = open(sys.argv[1], "rb").read()

pe = struct.unpack_from("<I", data, 0x3C)[0]
assert data[pe:pe + 4] == b"PE\0\0", "not PE"
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
magic = struct.unpack_from("<H", data, opt)[0]
assert magic == 0x10B, "not PE32"
image_base = struct.unpack_from("<I", data, opt + 28)[0]
export_rva, export_size = struct.unpack_from("<II", data, opt + 96)

sections = []
sec_off = opt + 224
for i in range(nsec):
    s = sec_off + i * 40
    name = data[s:s + 8].rstrip(b"\0").decode("latin1")
    vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", data, s + 8)
    sections.append((name, vaddr, vsize, rptr, rsize))


def rva2off(rva):
    for name, vaddr, vsize, rptr, rsize in sections:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return rptr + (rva - vaddr)
    raise KeyError(hex(rva))


ed = rva2off(export_rva)
nnames, _, aof, aon, _, aoo = struct.unpack_from("<IIIIII", data, ed + 20)
print(f"image_base={image_base:#x} exports={nnames}")

wanted = {
    "SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails",
    "SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles",
    "SteamAPI_ISteamRemoteStorage_UGCDownload",
    "SteamAPI_ISteamRemoteStorage_UGCRead",
    "SteamAPI_ISteamRemoteStorage_GetUGCDetails",
    "SteamAPI_SteamRemoteStorage_v016",
    "SteamInternal_CreateInterface",
}

for i in range(nnames):
    name_rva = struct.unpack_from("<I", data, rva2off(aon) + i * 4)[0]
    off = rva2off(name_rva)
    end = data.index(b"\0", off)
    name = data[off:end].decode("latin1")
    if name not in wanted:
        continue
    func_rva = struct.unpack_from("<I", data, rva2off(aof) + i * 4)[0]
    foff = rva2off(func_rva)
    blob = data[foff:foff + 48]
    print(f"\n{name}")
    print(f"  rva={func_rva:#x} ord_idx={struct.unpack_from('<H', data, rva2off(aoo) + i * 2)[0]}")
    print("  " + " ".join(f"{b:02X}" for b in blob))
