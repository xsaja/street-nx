"""Tables from the console profiler (switch/profiler.cpp, profile = true in the settings).
python -I prof_report.py <prof.bin> <street.elf> <street.log> [--from S] [--to S] [--top N] [--thread NAME]

prof.bin holds 32 byte samples (slot, time in 10 ms units, pc, lr, five return addresses, as offsets into the NRO,
which are ELF addresses). The log's last "prof: slots" line names the slots. Symbols come from devkitA64's nm.
Per thread: self time by function (where the pc was), the share in recompiled game code (sub_XXXXXXXX) against the
runtime, the callers of the hottest runtime functions (by lr), and an inclusive view for game functions (on the pc,
lr or return chain of a sample: a lower bound, the chain stops at five frames or where frame pointers are missing)."""
import argparse
import bisect
import collections
import os
import re
import struct
import subprocess
import sys

NM = r'D:\msys64\opt\devkitpro\devkitA64\bin\aarch64-none-elf-nm.exe'


def symbols(elf):
    out = subprocess.run([NM, '--numeric-sort', '--defined-only', '-C', elf], capture_output=True, text=True,
                         check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'TtWw':
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def slot_names(log):
    names = {}
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            if 'prof: slots' in line:
                names = dict(m.groups() for m in re.finditer(r' (\d+)=(\S+)', line.split('prof: slots', 1)[1]))
    return {int(k): v for k, v in names.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bin')
    ap.add_argument('elf')
    ap.add_argument('log')
    ap.add_argument('--from', dest='t0', type=float, default=0)
    ap.add_argument('--to', dest='t1', type=float, default=1e9)
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--thread', default=None)
    a = ap.parse_args()

    addrs, names = symbols(a.elf)

    def name(off):
        if off == 0xFFFFFFFF:
            return '?'
        i = bisect.bisect_right(addrs, off) - 1
        return names[i] if i >= 0 else '?'

    slots = slot_names(a.log)
    data = open(a.bin, 'rb').read()
    per = collections.defaultdict(list)
    wraps, last = 0, 0
    for i in range(0, len(data) - 31, 32):
        slot, t, pc, lr, *ret = struct.unpack_from('<HHII5I', data, i)
        if t + 30000 < last:  # the 16 bit time wraps after 655 s
            wraps += 1
        last = t
        sec = (t + wraps * 65536) / 100.0
        if a.t0 <= sec <= a.t1:
            per[slot].append((pc, lr, ret))
    total = sum(len(v) for v in per.values())
    print(f'{total} samples ({len(data) // 32} in the file), {a.t0:g}-{a.t1:g} s of profiler time')
    for slot, samples in sorted(per.items(), key=lambda kv: -len(kv[1])):
        tname = slots.get(slot, f'slot{slot}')
        if a.thread and a.thread not in tname:
            continue
        n = len(samples)
        selfc = collections.Counter(name(pc) for pc, _, _ in samples)
        game = sum(c for f, c in selfc.items() if f.startswith('sub_'))
        print(f'\n=== {tname}: {n} samples ({n / 1000:.1f} s busy at 1 kHz); game code {100 * game / n:.1f} %, '
              f'runtime {100 * (n - game) / n:.1f} %')
        print('  self time:')
        for f, c in selfc.most_common(a.top):
            print(f'  {100 * c / n:6.2f} %  {f[:110]}')
        # who calls the hottest runtime functions (memcpy and the like)
        hot_rt = [f for f, _ in selfc.most_common(12) if not f.startswith('sub_')][:6]
        for f in hot_rt:
            callers = collections.Counter(name(lr) for pc, lr, _ in samples if name(pc) == f)
            line = ', '.join(f'{g[:60]} {100 * c / selfc[f]:.0f}%' for g, c in callers.most_common(4))
            print(f'  callers of {f[:60]}: {line}')
        incl = collections.Counter()
        for pc, lr, ret in samples:
            seen = {name(x) for x in (pc, lr, *ret)}
            incl.update(g for g in seen if g.startswith('sub_'))
        print('  game functions on the stack (inclusive, lower bound):')
        for f, c in incl.most_common(a.top):
            print(f'  {100 * c / n:6.2f} %  {f}')


if __name__ == '__main__':
    sys.exit(main())
