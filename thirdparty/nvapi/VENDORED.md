# NVAPI (minimal excerpt) vendored

- Progetto: NVIDIA NVAPI, https://github.com/NVIDIA/nvapi
- Commit `70d337db9186e968eab622f7e786de7e437faf3d` (branch `main` as of 2026-10-23; the repository has no version tags).
- License: `License.txt` (copied unchanged; SPDX MIT).
- `nvapi_vrr.h` is written by us with only the necessary declarations, copied unchanged:
  - from `nvapi.h`: `NV_GET_VRR_INFO_V1` and `NV_GET_VRR_INFO_VER1` (about lines 9692-9704);
    prototypes of `NvAPI_Initialize`, `NvAPI_DISP_GetDisplayIdByDisplayName`, `NvAPI_Disp_GetVRRInfo` (in a comment);
  - from `nvapi_lite_common.h`: `NvU32`, `MAKE_NVAPI_VERSION`, `NvAPI_Status` with `NVAPI_OK` (only the values used);
  - from `nvapi_interface.h`: the `nvapi_QueryInterface` IDs `0x0150e828` (`NvAPI_Initialize`, line 32),
    `0xae457190` (`NvAPI_DISP_GetDisplayIdByDisplayName`, line 183), `0xdf8fda57` (`NvAPI_Disp_GetVRRInfo`, line 201).
- No NVIDIA library in the repository or in the link: `nvapi64.dll` (driver) is loaded at runtime only in
  `runtime/platform/platform_win32_display.cpp`.
