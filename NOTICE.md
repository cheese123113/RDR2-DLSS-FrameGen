# Third-party notices

## Vulkan headers (`third_party/`)

Copyright The Khronos Group Inc. Licensed under Apache-2.0 OR MIT (see the SPDX headers in each file).
https://github.com/KhronosGroup/Vulkan-Headers

## NVIDIA DLSS SDK

Not included in this repository. Building requires the NVIDIA DLSS SDK (https://github.com/NVIDIA/DLSS), and the release
binary statically links its NGX library (`nvsdk_ngx_s.lib`) under NVIDIA's SDK license. `nvngx_dlssg.dll` is NVIDIA's
runtime and must be obtained from the SDK.

## Savage Wind Recomp

The NGX start-up/evaluation structure and the matrix helpers in `src/fg.cpp` were adapted (by way of a TOTK PC recomp
port) from Savage Wind Recomp's `runtime/nvn/vulkan/dlss.cpp`:

```
MIT License

Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors
Copyright (c) 2026 Peiikai

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
