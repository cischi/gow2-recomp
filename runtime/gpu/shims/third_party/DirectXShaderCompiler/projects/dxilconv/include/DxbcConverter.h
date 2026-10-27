// Shim of the DirectXShaderCompiler's projects/dxilconv/include/DxbcConverter.h
// (Xenia's third_party/DirectXShaderCompiler submodule,
// https://github.com/xenia-project/xenia @ 95a5c3ee250f80c3b9d139658649d9ffb6db3eec;
// DirectXShaderCompiler is under the University of Illinois/NCSA license).
//
// xenia/ui/d3d12/d3d12_api.h includes it for IDxbcConverter, used only for
// debug DXIL disassembly (cvar d3d12_dxbc_disasm_dxilconv, false by
// default) in D3D12Shader and PipelineCache. In gow2 dxilconv.dll and
// dxcompiler.dll are never loaded (patch 0006 to the provider): the pointers to
// DxcCreateInstance stay nullptr and D3D12Provider::DxbcConverterCreateInstance
// returns E_NOINTERFACE before using the CLSID or IID. Only the minimal
// declaration of the interface is here; the GUIDs are those of the original
// header (never passed to a real implementation).
#ifndef GOW2_SHIM_DXBC_CONVERTER_H_
#define GOW2_SHIM_DXBC_CONVERTER_H_

#include <unknwn.h>

struct IDxcBlob;

struct __declspec(uuid("5F956ED5-78D1-4B15-8247-F7187614A041")) IDxbcConverter : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Convert(LPCVOID pDxbc, UINT32 DxbcSize, LPCWSTR pExtraOptions, LPVOID* ppDxil,
                                            UINT32* pDxilSize, LPWSTR* ppDiag) = 0;

  virtual HRESULT STDMETHODCALLTYPE ConvertInDriver(const UINT32* pBytecode, LPCVOID pInputSignature,
                                                    UINT32 NumInputSignatureElements, LPCVOID pOutputSignature,
                                                    UINT32 NumOutputSignatureElements,
                                                    LPCVOID pPatchConstantSignature,
                                                    UINT32 NumPatchConstantSignatureElements, LPCWSTR pExtraOptions,
                                                    IDxcBlob** ppDxilModule, LPWSTR* ppDiag) = 0;
};

__declspec(selectany) extern const CLSID CLSID_DxbcConverter = {
    0x4900391e, 0xb752, 0x4edd, {0xa8, 0x85, 0x6f, 0xb7, 0x6e, 0x25, 0xad, 0xdb}};

#endif  // GOW2_SHIM_DXBC_CONVERTER_H_
