# Lists the library functions (D3D, DSOUND, XGRPH, XPP sections) the game's own code calls directly that have no
# name in game/config/game.toml: public XDK functions xbe_symbols did not find, which run their original code (and
# reach for the Xbox's hardware) instead of the host's. Also prints each one's size, ret and callers.
# python -I tools/unnamed_api.py   (run from the repo root, after a recompile)
import glob, re, struct
from collections import defaultdict

xbe = open("game/assets/default.xbe", "rb").read()
base = struct.unpack_from("<I", xbe, 0x104)[0]
count, sec_addr = struct.unpack_from("<II", xbe, 0x11C)
sections = []
for i in range(count):
    _, va, vs, _, _, name_va = struct.unpack_from("<IIIIII", xbe, sec_addr - base + i * 56)
    name = xbe[name_va - base:xbe.index(b"\0", name_va - base)].decode()
    sections.append((name, va, va + vs))


def section(a):
    return next((n for n, lo, hi in sections if lo <= a < hi), "?")


names = {}
for line in open("game/config/game.toml"):
    m = re.match(r"^(\w+)\s*=\s*0x([0-9A-Fa-f]+)", line)
    if m and int(m.group(2), 16):
        names[int(m.group(2), 16)] = m.group(1)

calls = defaultdict(set)
rets = {}
sizes = {}
for path in glob.glob("game/generated/game/x86_recomp.*.cpp"):
    current = None
    last_insn = 0
    for line in open(path):
        m = re.match(r"X86_FUNC\(__imp__sub_([0-9A-F]{8})\)", line)
        if m:
            current = int(m.group(1), 16)
            continue
        if current is None:
            continue
        m = re.match(r"\s*// ([0-9A-F]{8}) (.*)", line)
        if m:
            last_insn = int(m.group(1), 16)
            sizes[current] = max(sizes.get(current, 0), last_insn - current)
            if m.group(2).startswith("ret"):
                rets.setdefault(current, m.group(2))
            for t in re.findall(r"call 0x([0-9A-F]{8})", m.group(2)):
                calls[int(t, 16)].add(current)

lib = ("D3D", "DSOUND", "XGRPH", "XPP")
rows = []
for target, callers in calls.items():
    if section(target) not in lib or target in names:
        continue
    game_callers = sorted(c for c in callers if section(c) == ".text")
    if game_callers:
        rows.append((section(target), target, game_callers))
for sec, target, callers in sorted(rows):
    shown = " ".join(f"{c:06X}" for c in callers[:5]) + (" ..." if len(callers) > 5 else "")
    print(f"{sec:6s} sub_{target:08X} ~{sizes.get(target, 0):#6x} {rets.get(target, '?'):10s} {len(callers):3d} callers: {shown}")
print(f"{len(rows)} unnamed library functions called from the game")
