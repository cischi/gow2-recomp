#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Builds a minimal XDVDFS image. Paths containing '/' create subdirectories.
std::vector<uint8_t> BuildImage(const std::vector<std::pair<std::string, std::string>>& files,
                                uint64_t partition_offset);
