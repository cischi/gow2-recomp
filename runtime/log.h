#pragma once

#include <filesystem>
#include <string_view>

// Opens (truncating) the log file; an empty path closes the current one.
// With no open file the lines go to stderr only. If the file cannot be opened
// it says so on stderr and carries on with stderr only.
void LogInit(const std::filesystem::path&);
// Writes a line to the log file and to stderr. Thread-safe, immediate flush.
void LogLine(std::string_view);
// Variant for the crash handler: does not allocate. If the crash happened
// inside LogLine of the same thread (atomic flag of the lock owner)
// the line goes to stderr only; if the lock belongs to another thread it waits
// for it (at most 2 s) and also writes to the file.
void LogLineFromCrash(std::string_view);
