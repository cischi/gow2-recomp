# FidelityFX SDK (API headers) vendored

- Progetto: AMD FidelityFX SDK, https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
- Tag `v2.3.0`, commit `60f4ea81909200d8542eca14dccb2628b763a9a3`
- License: `LICENSE.md` (the SDK's license.md). All files listed below, DLLs included, are in its MIT list;
  the rest of the SDK has the AMD binary-only license and is not in this repository.
- Copied unchanged, at the same relative path as in the SDK (relative `#include`s stay valid):
  - `Kits/FidelityFX/api/include/ffx_api.h`
  - `Kits/FidelityFX/api/include/ffx_api_types.h`
  - `Kits/FidelityFX/api/include/ffx_api_loader.h`
  - `Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h`
  - `Kits/FidelityFX/upscalers/include/ffx_upscale.h`
  - `Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h`
  - `Kits/FidelityFX/framegeneration/include/ffx_framegeneration_api_types.h`
  - `Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h`

## DLLs (not versioned)

At configure time `cmake/FidelityFX.cmake` downloads, from `Kits/FidelityFX/signedbin/` of the same tag, the DLLs
signed by Advanced Micro Devices (Authenticode verified on 2026-10-21) and checks the SHA-256:

| File | Bytes | SHA-256 |
|---|---|---|
| `amd_fidelityfx_loader_dx12.dll` | 26.376 | `e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa` |
| `amd_fidelityfx_upscaler_dx12.dll` | 28.761.864 | `d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46` |
| `amd_fidelityfx_framegeneration_dx12.dll` | 40.085.776 | `02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18` |

The frame generation DLL is in a separate list in `cmake/FidelityFX.cmake`
(`GOW2_FIDELITYFX_FG_DLLS`): if it is missing or has a different hash, FSR stays available and frame generation does not.
Like the others it is covered by the SDK v2.3.0 license (`LICENSE.md`, `signedbin/` list).

The build copies them next to `gow2.exe` with `licenses/FidelityFX-SDK-LICENSE.md`. Without network:
`-DGOW2_FIDELITYFX_DIR=<folder with the three DLLs>` (same hashes). If they are missing, `gow2.exe` still works and
the FSR upscaler is reported as unavailable (`[gpu]` line in the log).
