"""Scan engine.dll for workshop-related strings and for thiscall vtable dispatch
patterns, to confirm how the engine actually reaches ISteamRemoteStorage."""
import re
import sys

path = sys.argv[1]
data = open(path, "rb").read()

print(f"file={path} size={len(data):#x}")

# 1) ASCII strings of interest
patterns = [
    rb"STEAMREMOTESTORAGE_INTERFACE_VERSION[0-9]*",
    rb"SteamInternal_CreateInterface",
    rb"SteamAPI_SteamRemoteStorage",
    rb"GetPublishedFileDetails",
    rb"EnumerateUserSubscribedFiles",
    rb"UGCDownload",
    rb"workshop\\%",
    rb"CWorkshopManager",
    rb"CWorkshopFileInfoManager",
]
for pat in patterns:
    hits = [m.start() for m in re.finditer(pat, data)]
    sample = []
    for off in hits[:6]:
        end = data.index(b"\0", off)
        sample.append(data[off:end].decode("latin1"))
    print(f"{pat.decode():45s} hits={len(hits)}  {sample}")

# 2) thiscall dispatch patterns: ff 50 <imm8> and ff 90 <imm32>
imm8 = {}
for m in re.finditer(rb"\xff\x50(.)", data, re.S):
    imm8[m.group(1)[0]] = imm8.get(m.group(1)[0], 0) + 1
print("\nff 50 imm8 counts (top):",
      sorted(imm8.items(), key=lambda kv: -kv[1])[:12])

for target in (0x60, 0x68, 0x6C, 0x9C, 0xAC):
    n8 = len(re.findall(rb"\xff\x50" + bytes([target]), data))
    n32 = len(re.findall(rb"\xff\x90" + target.to_bytes(4, "little"), data))
    n_mov8 = len(re.findall(rb"\x8b\x40" + bytes([target]), data))
    n_mov32 = len(re.findall(rb"\x8b\x80" + target.to_bytes(4, "little"), data))
    print(f"offset {target:#04x}: call[{target:#x}]={n8} call32={n32} mov8={n_mov8} mov32={n_mov32}")
