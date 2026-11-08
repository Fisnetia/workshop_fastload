import re
gpd=[]; enum=[]; init=None
for line in open("logs/rewrite-1.log", encoding="utf-8", errors="replace"):
    m = re.match(r"\[\s*([\d.]+)\]\s+(\S+)\s+(.*)", line)
    if not m: continue
    t, tag, rest = float(m.group(1)), m.group(2), m.group(3)
    if tag=="GPD":
        mm=re.search(r"id=(\d+)\s+maxSecondsOld=(\d+)\s+->\s+(\d+)", rest)
        if mm: gpd.append((t, mm.group(1), int(mm.group(2)), int(mm.group(3))))
    elif tag=="ENUM": enum.append((t,rest.strip()))
    elif tag=="INIT" and "done" in rest: init=(t,rest.strip())
print("INIT:", init)
print("GPD count:", len(gpd), "unique:", len({g[1] for g in gpd}))
if gpd:
    print(f"first {gpd[0][0]:.3f}s last {gpd[-1][0]:.3f}s span {gpd[-1][0]-gpd[0][0]:.3f}s")
    print("rewritten:", sum(1 for g in gpd if g[2]!=g[3]), "kept:", sum(1 for g in gpd if g[2]==g[3]))
    print("output values:", sorted({g[3] for g in gpd}))
    kept=[g for g in gpd if g[2]==g[3]]
    for g in kept: print("  kept id=%s at %.3fs" % (g[1], g[0]))
for t,r in enum: print(f"ENUM {t:8.3f}s {r}")
