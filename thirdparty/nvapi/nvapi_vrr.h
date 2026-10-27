// Minimal excerpt of NVAPI (https://github.com/NVIDIA/nvapi, commit
// 70d337db9186e968eab622f7e786de7e437faf3d): only what is needed to read the monitor's VRR state.
// Declarations copied unchanged from nvapi.h (NV_GET_VRR_INFO_V1, NV_GET_VRR_INFO_VER1) and from
// nvapi_lite_common.h (NvU32, NvAPI_Status, NVAPI_OK, MAKE_NVAPI_VERSION); IDs from nvapi_interface.h.
// No NVAPI #include and no linking: nvapi64.dll is loaded at runtime (nvapi_QueryInterface).
//
// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
#pragma once

typedef unsigned int NvU32;  // nvapi_lite_common.h (__clang__ / default branch)

#define MAKE_NVAPI_VERSION(typeName, ver) (NvU32)(sizeof(typeName) | ((ver) << 16))

typedef enum _NvAPI_Status {
  NVAPI_OK = 0,  // Success. Request is completed.
  NVAPI_ERROR = -1,
} NvAPI_Status;  // only the values used: the full enum is in nvapi_lite_common.h

typedef struct _NV_GET_VRR_INFO_V1 {
  NvU32 version;                         // [in]  Structure version
  NvU32 bIsVRREnabled : 1;               // [out] Set if VRR Mode is currently active on given display.
  NvU32 bIsVRRPossible : 1;              // [out] Set to true if VRR Mode is possible on the given display.
  NvU32 bIsVRRRequested : 1;             // [out] Set to true if VRR Mode is requested for the given display.
  NvU32 bIsVRRIndicatorEnabled : 1;      // [out] Set to true if the VRR indicator is enabled.
  NvU32 bIsDisplayInVRRMode : 1;         // [out] Set to true if the display is in VRR Mode.
  NvU32 reserved : 27;                   //       Reserved for future use.
  NvU32 reservedEx[4];                   //       Reserved for future use
} NV_GET_VRR_INFO_V1;

#define NV_GET_VRR_INFO_VER1 MAKE_NVAPI_VERSION(NV_GET_VRR_INFO_V1, 1)

// Prototypes (nvapi.h): NvAPI_Status NvAPI_Initialize();
//   NvAPI_Status NvAPI_DISP_GetDisplayIdByDisplayName(const char* displayName, NvU32* displayId);
//   NvAPI_Status NvAPI_Disp_GetVRRInfo(NvU32 displayId, NV_GET_VRR_INFO_V1* pVrrInfo);

// IDs for nvapi_QueryInterface (nvapi_interface.h).
constexpr NvU32 kNvAPI_Initialize_Id = 0x0150e828;
constexpr NvU32 kNvAPI_DISP_GetDisplayIdByDisplayName_Id = 0xae457190;
constexpr NvU32 kNvAPI_Disp_GetVRRInfo_Id = 0xdf8fda57;
