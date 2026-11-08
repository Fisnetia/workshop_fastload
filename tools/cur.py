import re, statistics
p = r"E:\SteamLibrary\steamapps\common\Left 4 Dead 2\bin\neko\plugins\workshop_fastload.log"
gpd=[]
for line in open(p, encoding="utf-8", errors="replace"):
    m = re.match(r"\[\s*([\d.]+)\]\s+GPD\s+id=(\d+)\s+maxSecondsOld=(\d+)\s+->\s+(\d+)", line)
    if m: gpd.append((float(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4))))
ts=[g[0] for g in gpd]; gaps=[b-a for a,b in zip(ts,ts[1:])]
print("GPD", len(gpd), "span %.3fs" % (ts[-1]-ts[0]) if ts else "")
if gaps: print("median %.0fms  min %.0f  max %.0f" % (statistics.median(gaps)*1000, min(gaps)*1000, max(gaps)*1000))
print("rewritten", sum(1 for g in gpd if g[2]!=g[3]), "kept", sum(1 for g in gpd if g[2]==g[3]))
