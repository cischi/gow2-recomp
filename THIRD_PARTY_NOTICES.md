# Third-party notices

gow2-recomp is licensed under the GNU General Public License v3.0 (`LICENSE`). It contains, links or loads the
third-party components listed below, each under its own license. `NOTICE` lists the files of this project derived
from other projects. Short license texts are reproduced here verbatim from the file named in each entry; long texts
(GPL, LGPL, the full FidelityFX SDK license) are referenced by path.

Sections: [Compiled into gow2.exe](#compiled-into-gow2exe), [Loaded at runtime and shipped with the
release](#loaded-at-runtime-and-shipped-with-the-release), [Downloaded by the user](#downloaded-by-the-user),
[Build and test only](#build-and-test-only).

## Compiled into gow2.exe

### Unleashed Recomp

- https://github.com/hedge-dev/UnleashedRecomp, commit `cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c`
- Copyright (c) hedge-dev and contributors
- License: GPL-3.0 (same text as `LICENSE`). Derived files and files following its design: see `NOTICE`.

### XenonRecomp (XenonUtils)

- https://github.com/hedge-dev/XenonRecomp, commit `ddd128bcca99fe8bfbb99bea583c972351fa6ace` (submodule
  `thirdparty/XenonRecomp`, with the patches in `patches/xenonrecomp/`). The recompiler generates the C++ of the game
  at build time; its `XenonUtils` library (XEX loading) is linked into `gow2.exe`.
- License: MIT (`thirdparty/XenonRecomp/LICENSE.md`):

```text
# MIT License

Copyright (c) 2025 hedge-dev and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

- The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

XenonUtils brings in these components, bundled as submodules of XenonRecomp:

#### libmspack (LZX decompressor)

- https://github.com/kyz/libmspack, v1.11; `mspack/lzxd.c` only. (C) 2003-2023 Stuart Caie.
- License: LGPL-2.1 (`thirdparty/XenonRecomp/thirdparty/libmspack/libmspack/COPYING.LIB`, same text as `thirdparty/ffmpeg/COPYING.LGPLv2.1`).
  Statically linked into the GPL-3.0 binary under section 3 of the LGPL-2.1.

#### PowerPC disassembler (GNU binutils)

- `thirdparty/XenonRecomp/thirdparty/disasm/`, from GDB, GAS and the GNU binutils. Copyright Free Software Foundation, Inc.
- License: GPL-2.0-or-later (stated in the file headers), used here under GPL-3.0.

#### tiny-AES-c

- https://github.com/kokke/tiny-AES-c
- License: The Unlicense (`thirdparty/XenonRecomp/thirdparty/tiny-AES-c/unlicense.txt`):

```text
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or
distribute this software, either in source code form or as a compiled
binary, for any purpose, commercial or non-commercial, and by any
means.

In jurisdictions that recognize copyright laws, the author or authors
of this software dedicate any and all copyright interest in the
software to the public domain. We make this dedication for the benefit
of the public at large and to the detriment of our heirs and
successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this
software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org/>
```

#### TinySHA1

- `thirdparty/XenonRecomp/thirdparty/TinySHA1/TinySHA1.hpp`, license in its header:

```text
 * TinySHA1 - a header only implementation of the SHA1 algorithm in C++. Based
 * on the implementation in boost::uuid::details.
 *
 * SHA1 Wikipedia Page: http://en.wikipedia.org/wiki/SHA-1
 *
 * Copyright (c) 2012-22 SAURAV MOHAPATRA <mohaps@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

#### SIMDe

- https://github.com/simd-everywhere/simde (header-only, used for the VMX intrinsics of the recompiled code)
- License: MIT (`thirdparty/XenonRecomp/thirdparty/simde/COPYING`):

```text
Copyright (c) 2017 Evan Nemerson <evan@nemerson.com>

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### toml++

- https://github.com/marzer/tomlplusplus, v3.4.0 (submodule of XenonRecomp), reads `gow2.toml`.
- License: MIT (`thirdparty/XenonRecomp/thirdparty/tomlplusplus/LICENSE`):

```text
MIT License

Copyright (c) Mark Gillard <mark.gillard@outlook.com.au>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### {fmt}

- https://github.com/fmtlib/fmt, 11.x (submodule of XenonRecomp), used by the Xenia GPU code.
- License: MIT (`thirdparty/XenonRecomp/thirdparty/fmt/LICENSE`):

```text
Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

--- Optional exception to the license ---

As an exception, if, as a result of your compiling your source code, portions
of this Software are embedded into a machine-executable object form of such
source code, you may redistribute such embedded portions in such object form
without including the above copyright and permission notices.
```

### xxHash

- https://github.com/Cyan4973/xxHash (submodule of XenonRecomp), used by the Xenia GPU code.
- License: BSD-2-Clause (`thirdparty/XenonRecomp/thirdparty/xxHash/LICENSE`):

```text
xxHash Library
Copyright (c) 2012-2021 Yann Collet
All rights reserved.

BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### Xenia and xenia-canary

- https://github.com/xenia-project/xenia (commit `95a5c3ee250f80c3b9d139658649d9ffb6db3eec`: kernel semantics and
  `runtime/kernel/nt_status_table.inc`) and https://github.com/xenia-canary/xenia-canary (commit
  `e6cd2e4bc998609ca25e24a24c8e32b66c5908ad`: GPU and APU code vendored in `thirdparty/xenia/`).
- License: BSD-3-Clause (`thirdparty/xenia/LICENSE`):

```text
Copyright (c) 2015, Ben Vanik.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the project nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

Third-party code vendored with Xenia (`thirdparty/xenia/third_party/`, details in `thirdparty/xenia/VENDORED.md`):

#### AMD FidelityFX CAS

- Used by the sharpening shaders. License: MIT (`thirdparty/xenia/third_party/FidelityFX-CAS/LICENSE.txt`):

```text
Copyright (c) 2020 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

#### NVIDIA FXAA 3.11

- Used by the FXAA anti-aliasing. License: BSD-3-Clause (`thirdparty/xenia/third_party/fxaa/LICENSE`):

```text
BSD 3-Clause License

Copyright (c) 2014-2015, NVIDIA CORPORATION. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

  * Neither the name of NVIDIA CORPORATION nor the names of its
    contributors may be used to endorse or promote products derived
    from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

#### DXBCChecksum

- Copyright 2008-2016 (c), Advanced Micro Devices, Inc. (AMD Developer Tools Team), with an MD5 implementation derived
  from the RSA Data Security, Inc. MD5 Message Digest Algorithm. The complete license notes are in the headers of
  `thirdparty/xenia/third_party/dxbc/DXBCChecksum.h` and `DXBCChecksum.cpp`.

#### DirectXShaderCompiler (declaration only)

- https://github.com/microsoft/DirectXShaderCompiler, University of Illinois/NCSA Open Source License. The shim
  `runtime/gpu/shims/third_party/DirectXShaderCompiler/projects/dxilconv/include/DxbcConverter.h` repeats one interface
  declaration; full license text in `NOTICE`.

### FFmpeg

- https://github.com/has207/FFmpeg (fork used by xenia-canary), commit `d980192e175e6ff95bcd287af77e16fcb6597974`:
  a libavcodec/libavutil subset vendored in `thirdparty/ffmpeg/` (XMA audio decoding). Copyright (c) the FFmpeg
  developers.
- License: LGPL-2.1-or-later (`thirdparty/ffmpeg/COPYING.LGPLv2.1`, `thirdparty/ffmpeg/LICENSE.md`). The complete
  source of the subset is in `thirdparty/ffmpeg/`. Some files have their own permissive license (ISC, zlib, MIT, IJG):
  see `NOTICE` and `thirdparty/ffmpeg/VENDORED.md`. This software is based in part on the work of the Independent
  JPEG Group.

### o1heap

- https://github.com/pavel-kirienko/o1heap, tag 3.0.0 (guest heap allocator).
- License: MIT (`thirdparty/o1heap/LICENSE`):

```text
MIT License

Copyright (c) 2020 Pavel Kirienko

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### SDL 3

- https://github.com/libsdl-org/SDL, tag release-3.4.18 (gamepads), statically linked.
- License: zlib (`thirdparty/SDL/LICENSE.txt`):

```text
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
  
This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:
  
1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required. 
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

The HIDAPI code included in SDL (`thirdparty/SDL/src/hidapi`) is available under GPL-3.0, BSD or the original HIDAPI
license, at the user's choice (`thirdparty/SDL/src/hidapi/LICENSE.txt`); this project uses it under GPL-3.0.

### Dear ImGui

- https://github.com/ocornut/imgui, tag v1.93.0 (launcher and F1 overlay).
- License: MIT (`thirdparty/imgui/LICENSE.txt`):

```text
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### NVAPI (extract)

- https://github.com/NVIDIA/nvapi, commit `70d337d`: `thirdparty/nvapi/nvapi_vrr.h` declares the structures needed to
  read the monitor's VRR state. No NVIDIA library is linked: the driver's `nvapi64.dll` is loaded at runtime if present.
- License: MIT (`thirdparty/nvapi/License.txt`):

```text
nvapi.lib, nvapi64.lib, nvapia64.lib and nvapia64ec.lib are licensed under the following terms:

SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: MIT

Permission is hereby granted, free of charge, to any person obtaining a 
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

### AMD FidelityFX SDK (API headers)

- https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag v2.3.0. Only the API headers listed in
  `thirdparty/fidelityfx/VENDORED.md` are in the repository; they are in the MIT list of the SDK license (text below).

## Loaded at runtime and shipped with the release

### AMD FidelityFX SDK DLLs (FSR upscaling and frame generation)

- `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll`,
  signed by Advanced Micro Devices, from `Kits/FidelityFX/signedbin/` of FidelityFX SDK v2.3.0.
- The full SDK license is `thirdparty/fidelityfx/LICENSE.md` (copied next to `gow2.exe` as
  `licenses/FidelityFX-SDK-LICENSE.md`). The SDK's default license is binary-only; these DLLs and the API headers are
  among the files that the SDK license places under the following MIT terms:

```text
Copyright (C) Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions: 

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software. 

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. 
```

### gow2_dlss.dll (DLSS bridge)

- Sources in `runtime/gpu/dlss_bridge/`, built with the NVIDIA DLSS SDK (https://github.com/NVIDIA/DLSS, tag
  `v310.9.16`), which is not in the repository. `gow2.exe` loads the bridge at runtime through a C ABI.
- License of the bridge: MIT (`runtime/gpu/dlss_bridge/LICENSE.txt`):

```text
MIT License

Copyright (c) 2026 gow2 contributors

This license applies only to the files in this directory (the gow2_dlss bridge).
The NVIDIA DLSS SDK it is built with is licensed separately by NVIDIA under the
NVIDIA RTX SDKs License and is not included in this repository.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

- The bridge is linked with `nvsdk_ngx_s.lib` and uses the headers of the NVIDIA DLSS SDK, licensed under the NVIDIA
  RTX SDKs License (the `LICENSE.txt` of https://github.com/NVIDIA/DLSS; builds with the SDK copy it next to `gow2.exe`
  as `licenses/NVIDIA-RTX-SDKs-LICENSE.txt`).
- Attribution: this software contains source code provided by NVIDIA Corporation. It uses NVIDIA DLSS through the
  NVIDIA DLSS SDK. NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation. DLSS works only on NVIDIA RTX GPUs.

## Downloaded by the user

These are not distributed by this project. The launcher downloads them only when the user asks, from their official
sources, and verifies a pinned SHA-256.

### NVIDIA DLSS DLLs

- `nvngx_dlss.dll` and `nvngx_dlssg.dll`, version 310.9.16, from
  https://github.com/NVIDIA/DLSS/tree/v310.9.16/lib/Windows_x86_64/rel ("Download NVIDIA DLSS" button).
- License: NVIDIA RTX SDKs License (https://github.com/NVIDIA/DLSS/blob/v310.9.16/LICENSE.txt). Copyright NVIDIA
  Corporation.

### OptiScaler

- https://github.com/optiscaler/OptiScaler; the launcher installs v0.1.34 of the DLSSNR-F5 fork by janblade,
  https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass ("Install OptiScaler" button).
- License: GPL-3.0. OptiScaler bundles third-party components under their own licenses, described in its release.
- `nvngx_dlssnr.dll` (neural rendering) is neither provided nor downloaded by this project.

## Build and test only

These are not part of the release binaries.

- **XenonRecomp** (MIT, above) also runs at build time to translate `default.xex` into C++.
- **Catch2** (https://github.com/catchorg/Catch2, v3.7.1, fetched by `cmake/Catch2.cmake`): Boost Software License 1.0,
  C++ tests only.
- **Microsoft fxc (Windows SDK)**: compiles the Xenia shaders at build time; not redistributed.
