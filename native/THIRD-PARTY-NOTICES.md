# Dependencies of the standalone native diagnostics

These notices supplement the original root THIRD-PARTY-NOTICES.txt, which is
preserved unchanged for the imported v0.22 package. They do not describe the
contents of vendor/v0.22/dxgi.dll.

## OpenXR SDK / statically linked loader

- Source: https://github.com/KhronosGroup/OpenXR-SDK
- Pinned release: release-1.1.53, commit 105a2a674cd7aae85de8d6a43f2d2cbdaa80af60.
- Archive SHA-256: 9c67c2d158f8b71a19d9d0b1e73043681cc4df6e90d207417a964b8d6aee1766.
- Copyright (c) 2017-2025 The Khronos Group Inc.
- Apache License 2.0: see licenses/OpenXR-Apache-2.0.txt.
- Original source is fetched under build/_deps (or the chosen CMake build tree)
  and compiled without source modifications. SDK 1.1.53 headers are used;
  the diagnostic requests OpenXR API 1.0 plus XR_KHR_D3D12_enable.

## JsonCpp (bundled in the OpenXR loader)

- Copyright (c) 2007-2010 Baptiste Lepilleur and The JsonCpp Authors.
- MIT / public domain: complete upstream terms in licenses/JsonCpp-LICENSE.txt.

Keep this file and the licenses directory with any diagnostic executable.
No game, SteamVR, or ALVR binaries or configuration are distributed.
