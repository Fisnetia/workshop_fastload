import re, glob
files = ["logs/baseline-1.log","logs/baseline-2.log","logs/rewrite-1.log","logs/run-20261108-0240-GPD180.log"]
print(f"{'log':38} {'ENUM #1':>9} {'#2':>9} {'#3':>9} {'#4':>9} {'#1->#2':>8}  start")
for p in files:
    enum=[]
    for line in open(p, encoding="utf-8", errors="replace"):
        m = re.match(r"\[\s*([\d.]+)\]\s+ENUM\s+#(\d+)\s+startIndex=(\d+)", line)
        if m: enum.append((int(m.group(2)), float(m.group(1)), m.group(3)))
    enum.sort()
    ts = {n: t for n, t, s in enum}
    starts = ",".join(s for n, t, s in enum)
    gap = (ts[2]-ts[1]) if 1 in ts and 2 in ts else float('nan')
    print(f"{p:38} {ts.get(1,0):9.3f} {ts.get(2,0):9.3f} {ts.get(3,0):9.3f} {ts.get(4,0):9.3f} {gap:8.3f}  {starts}")
