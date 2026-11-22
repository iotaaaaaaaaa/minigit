// Small helpers shared by every module.
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Every user-facing failure is thrown as an Error and printed by main().
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Binary data is kept in std::string throughout: it can hold any byte,
// including '\0', and has value semantics.
std::string read_file(const fs::path& p);
bool try_read_file(const fs::path& p, std::string& out);

// Writes to a temporary file and renames it into place. A rename is atomic,
// so readers (and a crash) see either the old file or the new one, never a
// half-written file. Git uses the same trick for every file it updates.
void write_file_atomic(const fs::path& p, const std::string& data);

std::string to_hex(const std::string& raw);
std::string from_hex(const std::string& hex);
bool is_hex(const std::string& s);

// Splits text into lines, keeping each line's trailing '\n'. Keeping the
// newline means "last line has no newline" survives a split/join round trip.
std::vector<std::string> split_lines(const std::string& text);

std::string zlib_compress(const std::string& data);
std::string zlib_decompress(const std::string& data);

// SHA-1 digest as 20 raw bytes.
std::string sha1(const std::string& data);
