# Writes LICENSES.txt at the repo root: every license of what goes into street.nro, then what only the PC builds and the
# recompiler use. package_switch.ps1 copies it beside the NRO.
# python tools/make_licenses.py [--mesa D:/nvk/mesa-switch] [--ref ../ref]
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ZLIB = """Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held
liable for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter
it and redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If
   you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not
   required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original
   software.
3. This notice may not be removed or altered from any source distribution.
"""

ZSTD = """BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided with the distribution.
 * Neither the name Facebook, nor Meta, nor the names of its contributors may be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""

EXPAT = """Copyright (c) 1998-2000 Thai Open Source Software Center Ltd and Clark Cooper
Copyright (c) 2001-2025 Expat maintainers

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
"""

LIBNX = """Copyright 2017-2018 libnx Authors

Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted,
provided that the above copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.
"""

DEKO3D = """deko3d (fincs), the Switch's native graphics API library (libdeko3d from devkitPro), zlib license:

Copyright (C) 2018-2020 fincs

This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held
liable for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter
it and redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If
   you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not
   required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original
   software.
3. This notice may not be removed or altered from any source distribution.
"""

TOOLCHAIN = """street.nro is built with devkitA64 (GCC) and links its C and C++ runtimes: newlib (BSD-style licenses of its
authors, https://sourceware.org/newlib/COPYING.NEWLIB) and libstdc++/libgcc, which are GPL-3.0 with the GCC Runtime
Library Exception (https://www.gnu.org/licenses/gcc-exception-3.1.html). The exception lets a program compiled with GCC
carry these runtime libraries under the program's own terms.
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mesa', default='D:/nvk/mesa-switch')
    ap.add_argument('--ref', default='S:/Proyectos/MVP-Switch/ref')
    a = ap.parse_args()
    mesa, ref = Path(a.mesa), Path(a.ref)

    def file(p):
        p = Path(p)
        if not p.exists():
            raise SystemExit(f'missing license file: {p}')
        return p.read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')

    nro = [
        ('MVP05 XBE Recomp, Title-ID Update Beta 3 (maddog847, https://github.com/maddog847/MVP-Baseball-2005-XBERecomp): SDK-LICENSE.txt as shipped in its package',
         file(ROOT / 'SDK-LICENSE.txt')),
        ('MVP05 XBE Recomp: THIRD_PARTY.txt as shipped in its package', file(ROOT / 'THIRD_PARTY.txt')),
        ('XboxXBE-Recomp SDK (TGP482), https://github.com/TGP482/XboxXBE-Recomp', file(ROOT / 'sdk/LICENSE')),
        ('Black-Android (RanggaCasper), https://github.com/RanggaCasper/Black-Android',
         file(ref / 'Black-Android/LICENSE')),
        ('glslang (The Khronos Group and contributors)', file(ROOT / 'third_party/glslang/LICENSE.txt')),
        ('Dear ImGui (Omar Cornut)', file(ROOT / 'third_party/imgui/LICENSE.txt')),
        ('toml++ (Mark Gillard)', file(ROOT / 'third_party/tomlplusplus/LICENSE')),
        ('Vulkan-Headers (The Khronos Group)', file(ROOT / 'third_party/Vulkan-Headers/LICENSE.md')),
        ('Mesa / NVK / NAK: mesa-switch (danfromtico), 26.2.4 build by NaGaa95 - license.rst',
         file(mesa / 'docs/license.rst')),
        ('Mesa: MIT', file(mesa / 'licenses/MIT')),
        ('Mesa: Apache-2.0 (parts of NAK and its Rust crates)', file(mesa / 'licenses/Apache-2.0')),
        ('Mesa: BSL-1.0', file(mesa / 'licenses/BSL-1.0')),
        ('deko3d (fincs)', DEKO3D),
        ('uam 1.1.0 (fincs), shader compiler built in as a library, with the patches of SwitchWakerHD (third_party/uam)',
         file(ROOT / 'third_party/uam/LICENSE')),
        ('SPIRV-Cross (The Khronos Group and contributors), vulkan-sdk-1.4.363.0', file(ROOT / 'third_party/SPIRV-Cross/LICENSE')),
        ('SMAA 2.8 (Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez), '
         'https://github.com/iryoku/smaa: shaders and lookup tables (third_party/smaa), used when smaa = true',
         file(ROOT / 'third_party/smaa/LICENSE.txt')),
        ('defjam-sw (R-drg), https://github.com/R-drg/defjam-sw: switch/profiler.cpp is adapted from its profiler',
         file(ref / 'defjam-sw/LICENSE')),
        ('Zstandard (from devkitPro portlibs)', ZSTD),
        ('Expat (from devkitPro portlibs)', EXPAT),
        ('zlib (from devkitPro portlibs)', ZLIB),
        ('libnx (switchbrew)', LIBNX),
        ('devkitA64 runtime libraries', TOOLCHAIN),
    ]
    other = [
        ('SDL 3 (Sam Lantinga): PC builds only, its license is in THIRD_PARTY.txt above', None),
        ('Zydis (zyantific): the recompiler (xbe_recomp) only, nothing of it goes into street.nro',
         file(ROOT / 'third_party/zydis/LICENSE')),
    ]

    out = [
        'FIFA Street - Nintendo Switch homebrew port (street-nx)',
        '',
        'street.nro contains code translated from the game\'s default.xbe by the recompiler. It contains none of the game\'s',
        'data files: the player copies them from their own disc. FIFA Street is a trademark of Electronic Arts; this',
        'project has no relation to Electronic Arts, FIFA or Nintendo.',
        '',
        'street-nx is built on mvp-nx (the MVP Baseball 2005 port, https://github.com/xsaja/mvp-nx, GPL-3.0). Its Switch layer',
        'is based on BLACK-NX-PORT (gerisson1996, https://github.com/gerisson1996/BLACK-NX-PORT), which publishes no license',
        'file.',
        '',
        'The code of street-nx itself is licensed under the GNU General Public License version 3 (the LICENSE file of the',
        'repository, https://www.gnu.org/licenses/gpl-3.0.html), as mvp-nx\'s is. The parts below keep their own licenses.',
        '',
        'Below: the licenses of everything street.nro carries, then of what only the PC builds and the tools use.',
    ]
    for title, text in nro + other:
        out += ['', '', '=' * 20 + ' ' + title + ' ' + '=' * 20, '']
        if text:
            out.append(text.rstrip('\n'))
    (ROOT / 'LICENSES.txt').write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\r\n')
    print('LICENSES.txt:', len(out), 'lines')


if __name__ == '__main__':
    main()
