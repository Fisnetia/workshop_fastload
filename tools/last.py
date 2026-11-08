import re, statistics
p = "logs/after-steam-restart.log"
gpd=[]; enum=[]; init=None
for line in open(p, encoding="utf-8", errors="replace"):
    m = re.match(r"\[\s*([\d.]+)\]\s+(\S+)\s+(.*)", line)
    if not m: continue
    t, tag, rest = float(m.group(1)), m.group(2), m.group(3)
    if tag=="GPD":
        mm=re.search(r"id=(\d+)\s+maxSecondsOld=(\d+)\s+->\s+(\d+)", rest)
        if mm: gpd.append((t, mm.group(1), int(mm.group(2)), int(mm.group(3))))
    elif tag=="ENUM": enum.append((t,rest.strip()))
    elif tag=="INIT" and "done" in rest: init=(t,rest.strip())
print("banner:", open(p, encoding="utf-8", errors="replace").readline().strip())
print("init  :", init[1] if init else "?")
if gpd:
    ts=[g[0] for g in gpd]; gaps=[b-a for a,b in zip(ts,ts[1:])]
    print(f"GPD {len(gpd)}  span {ts[-1]-ts[0]:.3f}s  ({ts[0]:.3f} -> {ts[-1]:.3f})")
    print(f"gap median {statistics.median(gaps)*1000:.0f}ms  min {min(gaps)*1000:.0f}  max {max(gaps)*1000:.0f}")
    print(f"rewritten {sum(1 for g in gpd if g[2]!=g[3])}  kept {sum(1 for g in gpd if g[2]==g[3])}")
    for g in gpd:
        if g[2]==g[3]: print(f"  kept id={g[1]} at {g[0]:.3f}s")
else: print("NO GPD LINES")
for t,r in enum: print(f"ENUM {t:8.3f}s {r}")
