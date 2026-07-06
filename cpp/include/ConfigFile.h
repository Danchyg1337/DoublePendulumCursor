// ConfigFile.h -- load cfg::g from a simple text file.
//
// Format: one `key = value` per line; blank lines and lines starting with '#'
// are ignored; inline '#' comments are allowed. Colours are "R,G,B". Angles are
// given in DEGREES and converted to radians on load. Any key omitted from the
// file keeps its built-in default, so a partial config is fine.
//
// Platform independent (pure std iostream) so it is covered by the tests.
#pragma once

#include <string>

namespace cfg {

// Apply settings from `path` to the global cfg::g. Returns false if the file
// could not be opened (in which case cfg::g is left at its defaults). Unknown
// keys and malformed values are skipped with a note appended to `warnings`.
bool loadConfig(const std::string& path, std::string* warnings = nullptr);

// The full annotated default config text (used to create a starter file).
std::string defaultConfigText();

// Write defaultConfigText() to `path`. Returns false on I/O failure.
bool writeDefaultConfig(const std::string& path);

} // namespace cfg
