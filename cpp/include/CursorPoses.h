// CursorPoses.h -- cursor-type snap poses loaded from a text file at startup.
//
// Each entry maps a system cursor (by friendly name OR raw OCR numeric id) to a
// pair of target angles. Loading them from cursors.conf means new cursor types
// can be added or retuned without recompiling. Platform independent (pure
// parsing) so it is covered by the tests.
//
// File format (one entry per line; '#' starts a comment):
//     <name-or-ocrid> = <theta1_deg> , <theta2_deg>
// A ':' may be used instead of '=', and parentheses around the angles are
// ignored, so all of these are equivalent:
//     hand = -30, 90
//     hand : (-30, 90)
//     32649 = -30, 90
#pragma once

#include <string>
#include <vector>

namespace cursors {

struct Pose {
    int         ocrId;   // Windows OCR_* system-cursor id
    std::string name;    // as written in the file (for diagnostics)
    double      theta1;  // radians
    double      theta2;  // radians
};

// Built-in poses, used when cursors.conf is absent. Angles in radians.
std::vector<Pose> defaults();

// Parse `path` into `out` (replacing its contents). Angles in the file are in
// DEGREES and stored as radians. Later entries for the same OCR id overwrite
// earlier ones. Returns false if the file can't be opened. Malformed lines are
// skipped, with a note appended to `warnings` when provided.
bool load(const std::string& path, std::vector<Pose>& out, std::string* warnings = nullptr);

// Map a friendly cursor name (case-insensitive) to its OCR id, or -1 if unknown.
int ocrIdForName(const std::string& name);

// The annotated default cursors.conf text, and a helper to write it.
std::string defaultText();
bool writeDefault(const std::string& path);

} // namespace cursors
