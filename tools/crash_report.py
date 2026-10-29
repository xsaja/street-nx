# Names a PC crash: the host function at the faulting address (from the newest crash dump and the exe's PDB) and the
# recompiled function holding each guest return address of the log's "error: [esp+N] ADDR" lines.
# python -I tools/crash_report.py <test folder> <exe name>   (run from the repo root)
import bisect, glob, os, re, struct, subprocess, sys

LLVM = r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\llvm-symbolizer.exe"


def module_base(dump, exe):
    d = open(dump, "rb").read()
    n, off = struct.unpack_from("<II", d, 8)
    for i in range(n):
        kind, _, rva = struct.unpack_from("<III", d, off + i * 12)
        if kind != 4:  # ModuleListStream
            continue
        for j in range(struct.unpack_from("<I", d, rva)[0]):
            m = rva + 4 + j * 108
            base = struct.unpack_from("<Q", d, m)[0]
            name_rva = struct.unpack_from("<I", d, m + 20)[0]
            size = struct.unpack_from("<I", d, name_rva)[0]
            if d[name_rva + 4:name_rva + 4 + size].decode("utf-16le").lower().endswith(exe.lower()):
                return base
    return None


def main(folder, exe):
    log = open(sorted(glob.glob(os.path.join(folder, "logs", "*.log")), key=os.path.getmtime)[-1]).read()
    funcs = sorted(int(x, 16) for x in set(re.findall(r"sub_([0-9A-F]{8})", open("game/generated/game/x86_func_mapping.cpp").read())))
    names = {}
    for line in open("game/config/game.toml"):
        m = re.match(r"^(\w+)\s*=\s*0x([0-9A-Fa-f]+)", line)
        if m and int(m.group(2), 16):
            names[int(m.group(2), 16)] = m.group(1)
    m = re.search(r"exception \w+ at ([0-9A-F]+)", log)
    dumps = sorted(glob.glob(os.path.join(folder, "CrashDumps", "*.dmp")), key=os.path.getmtime)
    if m and dumps:
        base = module_base(dumps[-1], exe)
        if base is not None:
            pc = int(m.group(1), 16) - base + 0x140000000
            out = subprocess.run([LLVM, "--obj=build-pc/" + exe, hex(pc)], capture_output=True, text=True).stdout
            print("host pc in", out.splitlines()[0])
    for slot, addr in re.findall(r"\[esp\+([0-9A-F]+)\] ([0-9A-F]{8})", log):
        a = int(addr, 16)
        i = bisect.bisect_right(funcs, a) - 1
        if i >= 0 and a - funcs[i] < 0x4000:
            f = funcs[i]
            print(f"[esp+{slot}] {a:08X} in sub_{f:08X} +{a - f:#x} {names.get(f, '')}")


main(sys.argv[1], sys.argv[2])
