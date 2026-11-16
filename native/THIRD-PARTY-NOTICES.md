# Dependencies of the native diagnostics and capture module

These notices supplement the original root THIRD-PARTY-NOTICES.txt, which is
preserved unchanged for the imported v0.22 package. They do not describe the
contents of vendor/v0.22/dxgi.dll.

## OpenXR SDK / statically linked loader

- Source: https://github.com/KhronosGroup/OpenXR-SDK
- Pinned release: release-1.1.53, commit 105a2a674cd7aae85de8d6a43f2d2cbdaa80af60.
- Archive SHA-256: 9c67c2d158f8b71a19d9d0b1e73043681cc4df6e90d207417a964b8d6aee1766.
- Copyright (c) 2017-2025 The Khronos Group Inc.
- Apache License 2.0: see licenses/OpenXR-Apache-2.0.txt.
- The loader is statically linked into tdvr_diagnostic and tdvr_capture.dll.
  Runtime startup in capture is opt-in; the default path does not call XR.
- Original source is fetched under build/_deps (or the chosen CMake build tree)
  and compiled without source modifications. SDK 1.1.53 headers are used;
  the diagnostic requests OpenXR API 1.0 plus XR_KHR_D3D12_enable.

## JsonCpp (bundled in the OpenXR loader)

- Copyright (c) 2007-2010 Baptiste Lepilleur and The JsonCpp Authors.
- MIT / public domain: complete upstream terms in licenses/JsonCpp-LICENSE.txt.

## MinHook (capture DLL only)

- Source: https://github.com/TsudaKageyu/minhook
- Pinned tag: v1.3.4; archive SHA-256
  172708123daa0c98d20d3a980b16a50be14af243dc95dee6f79c24193ad010e4.
- Copyright (C) 2009-2017 Tsuda Kageyu; BSD-2-Clause and included HDE notices.
- Complete upstream license including HDE: licenses/MinHook-LICENSE.txt.
- Statically linked into tdvr_capture.dll; no source modifications.

Keep this file and the licenses directory with any diagnostic executable or DLL.
No game, SteamVR, or ALVR binaries or configuration are distributed.
