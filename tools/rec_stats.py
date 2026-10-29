# The title thread's recording cost in a run's match: from the perf lines (vulkan.cpp, deko.cpp) of the windows with
# at least 150 draws a frame, rec ms a frame and rec us a draw (median, p90).
# python -I tools/rec_stats.py <log> [<log>...]
import re
import sys


def windows(path):
    lines = open(path, errors="replace").read().splitlines()
    for i, line in enumerate(lines):
        if not re.search(r"perf: [\d.]+ fps", line):
            continue
        for nxt in lines[i + 1:i + 4]:
            m = re.search(r"rec ([\d.]+), wait ([\d.]+) \(([\d./]+)\).*?; (\d+) draws", nxt)
            if m:
                # the wait for the frame before (the last kind) happens after the recording is timed
                frame = float(m[3].split("/")[-1])
                yield float(m[1]), float(m[2]) - frame, float(m[4])
                break


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))]


for path in sys.argv[1:]:
    rows = [w for w in windows(path) if w[2] >= 150]
    if not rows:
        print(f"{path}: no match windows")
        continue
    # rec includes the title's waits inside the calls (resource, read-back...): the recording alone is rec - those
    own = [r - w for r, w, _ in rows]
    per = [1000 * (r - w) / d for r, w, d in rows]
    print(f"{path}: {len(rows)} windows, draws {pct([d for *_, d in rows], .5):.0f}; recording ms a frame median "
          f"{pct(own, .5):.3f} p90 {pct(own, .9):.3f}; us a draw median {pct(per, .5):.2f} p90 {pct(per, .9):.2f}")
