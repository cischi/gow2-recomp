#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>

#include <vector>

#include "guest_memory.h"
#include "imports.h"

struct LoadedImage {
  uint32_t base, size, entry;
  // Value of the optional header XEX_HEADER_SYSTEM_FLAGS (0x30000); 0 if absent.
  uint32_t system_flags = 0;
  // XEX_HEADER_DEFAULT_STACK_SIZE (0x20200): stack of threads created with
  // StackSize 0; 0 if absent.
  uint32_t default_stack_size = 0;
  // The first Xex2Header::headerSize bytes of the XEX (header and optional headers),
  // as Xenia copies them into guest memory for the module's LDR.
  std::vector<uint8_t> header;
  // Import records of header 0x000103FF, with the original type/ordinal.
  std::vector<ImportRecord> imports;
};
enum class LoadError { FileMissing, ParseFailed };

// Decrypts/decompresses the XEX with XenonUtils and copies the image to
// mem.base + image.base (bytes not produced by the loader stay zero).
// ParseImage receives the XEX with the import key zeroed, so the import
// records stay the original ones (big-endian, thunks not rewritten): they are
// prepared by ResolveImports.
// Moves the heap start to align_up(base + size, 0x10000).
// `mem` must already be initialized (otherwise ParseFailed).
// Every error is also written with LogLine.
std::expected<LoadedImage, LoadError> LoadImage(GuestMemory& mem, const std::filesystem::path& xex);
