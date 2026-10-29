# third_party/smaa/SMAA.hlsl as a C++ raw string the deko3d backend compiles at run time: third_party/smaa/SMAA.hlsl.inc
# python -I tools/smaa_inc.py   (run from the repo root)
src = open("third_party/smaa/SMAA.hlsl", "rb").read()
text = bytes(b for b in src if b < 0x80).decode("ascii").replace("\r\n", "\n")
assert ")SMAA\"" not in text
open("third_party/smaa/SMAA.hlsl.inc", "w", newline="\n").write('R"SMAA(' + text + ')SMAA"\n')
