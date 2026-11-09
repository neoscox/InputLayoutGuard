# Input Layout Guard

An SKSE plugin for Skyrim SE/AE/VR. It stops one broken mesh from turning every NPC face black (or sky-blue), and names the mesh that caused it.

- **Download:** Nexus Mods only (link TBD). This repository hosts the source code; no binaries are published here.
- **Players:** the Nexus Mods page explains the symptom, what the plugin does, requirements and how to read the log.
- Each Nexus file is built from the matching version tag in this repository, e.g. `v1.0.0`.

## How it works

1. Skyrim caches D3D11 input layouts by `vertexDesc & vertexShader.inputMask`. That key does not identify the vertex shader, and a failed `CreateInputLayout` is cached as NULL.
2. Picture a normal-less mesh whose shader needs `NORMAL`/`BINORMAL`, e.g. a scalp missing `Model_Space_Normals`. It makes the creation fail, and the NULL is then bound for every mesh sharing the key. Vanilla FaceGen heads share that key.

The plugin's hooks:

- **`ID3D11Device::CreateInputLayout`** (MinHook). On an engine-originated failure it parses the VS input signature (DXBC `ISGN`). It appends the missing semantics on unbound input slot 15, which reads as zero, retries, and logs the mesh, owner and plugin.
- **`ID3D11DeviceContext::IASetInputLayout`** (MinHook on the d3d11 functions). When a repaired layout is bound, it substitutes a variant built for `RendererShadowState::currentVertexShader`, so every shader sharing the key gets a register-correct layout.
- **`BSShader::SetupGeometry`** (vtable slot 6) of the Lighting, Effect and Utility shaders. It records the geometry being set up, for attribution only.

## Building

Requirements:

- Visual Studio 2022 with the C++ desktop workload
- CMake 3.26+
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set

```bat
git clone --recursive https://github.com/neoscox/InputLayoutGuard.git
cd InputLayoutGuard
build.bat                          :: build + package InputLayoutGuard-<version>.zip
build.bat "C:\path\to\mod folder"  :: ...and copy the DLL into <folder>\SKSE\Plugins
```

The first build compiles CommonLibSSE-NG and takes a while. CI (`.github/workflows/build.yml`) builds every push to check that the source compiles.

## Repository layout

| Path | |
|---|---|
| `src/main.cpp` | the whole plugin |
| `dist/InputLayoutGuard.ini` | default settings shipped in the archive |
| `extern/CommonLibSSE-NG` | submodule, alandtse/CommonLibVR `ng` @ 7.5.1 |
| `package.ps1` | builds the release archive layout |

## AI assistance

This project was developed with the help of AI (Anthropic's Claude). The AI helped with root-cause analysis, the plugin code and the documentation. The author reviewed the results and tested them in-game.

## License

Copyright (C) 2026 neoscox.

This program is licensed under the **GNU General Public License v3.0 or later** ([LICENSE](LICENSE)). It links CommonLibSSE-NG, which is GPL-3.0-or-later with a Modding Exception and a GPL-3.0 Linking Exception, so the combined plugin is distributed under the GPL.

Third-party components and their licenses are listed in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
