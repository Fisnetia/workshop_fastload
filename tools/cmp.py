import re, sys, statistics
def analyze(path, label):
    gpd, enum, other = [], [], {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\[\s*([\d.]+)\]\s+(\S+)\s+(.*)", line)
        if not m: continue
        t, tag, rest = float(m.group(1)), m.group(2), m.group(3)
        if tag == "GPD":
            mm = re.search(r"id=(\d+)\s+maxSecondsOld=(\d+)", rest)
            if mm: gpd.append((t, mm.group(1), int(mm.group(2))))
        elif tag == "ENUM":
            enum.append((t, rest.strip()))
    print("=" * 68)
    print(label)
    if not gpd:
        print("  no GPD lines"); return
    ts = [g[0] for g in gpd]
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    print(f"  GPD calls      : {len(gpd)}  unique ids: {len({g[1] for g in gpd})}")
    print(f"  first / last   : {ts[0]:.3f}s / {ts[-1]:.3f}s   span {ts[-1]-ts[0]:.3f}s")
    if gaps:
        print(f"  gap median/mean: {statistics.median(gaps)*1000:.0f} ms / {statistics.mean(gaps)*1000:.0f} ms")
        print(f"  gap min/max    : {min(gaps)*1000:.0f} ms / {max(gaps)*1000:.0f} ms")
        buckets = {"<20ms":0,"20-50ms":0,"50-100ms":0,"100-500ms":0,">500ms":0}
        for g in gaps:
            ms = g*1000
            if ms < 20: buckets["<20ms"] += 1
            elif ms < 50: buckets["20-50ms"] += 1
            elif ms < 100: buckets["50-100ms"] += 1
            elif ms < 500: buckets["100-500ms"] += 1
            else: buckets[">500ms"] += 1
        print("  gap histogram  : " + "  ".join(f"{k}={v}" for k, v in buckets.items()))
    vals = sorted({g[2] for g in gpd})
    print(f"  maxSecondsOld  : values seen {vals}")
    for t, r in enum:
        print(f"  ENUM {t:8.3f}s  {r}")
    if len(enum) > 1:
        print(f"  ENUM #1->#2    : {enum[1][0]-enum[0][0]:.3f}s")
for p, l in [("logs/baseline-1.log", "mode=0  baseline run 1  (2026-11-08 03:05:48)"),
             ("logs/run-20261108-0240-GPD180.log", "mode=1  rewrite run    (2026-11-08 02:40)")]:
    try: analyze(p, l)
    except FileNotFoundError: print("missing", p)
