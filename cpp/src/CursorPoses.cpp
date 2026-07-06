#include "CursorPoses.h"
#include "Config.h"   // cfg::deg2rad and OCR_* ids

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace cursors {
namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Remove characters that are noise in the angle field: parentheses & spaces.
std::string cleanAngles(std::string s) {
    std::string out;
    for (char c : s)
        if (c != '(' && c != ')' && !std::isspace(static_cast<unsigned char>(c)))
            out += c;
    return out;
}

bool isNumericId(const std::string& s) {
    if (s.empty()) return false;
    std::size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

bool parseDouble(const std::string& v, double& out) {
    try { std::size_t p; out = std::stod(v, &p); return p > 0; }
    catch (...) { return false; }
}

// Replace-or-append by OCR id so the last entry for an id wins.
void upsert(std::vector<Pose>& v, const Pose& p) {
    for (auto& e : v) {
        if (e.ocrId == p.ocrId) { e = p; return; }
    }
    v.push_back(p);
}

} // namespace

int ocrIdForName(const std::string& name) {
    const std::string n = lower(trim(name));
    // Friendly aliases first, then the canonical OCR_* names.
    if (n == "hand" || n == "pressable" || n == "link")      return cfg::OCR_HAND;
    if (n == "text" || n == "ibeam" || n == "beam")          return cfg::OCR_IBEAM;
    if (n == "vresize" || n == "sizens" || n == "ns")        return cfg::OCR_SIZENS;
    if (n == "hresize" || n == "sizewe" || n == "we")        return cfg::OCR_SIZEWE;
    if (n == "nwse" || n == "sizenwse" || n == "diag1")      return cfg::OCR_SIZENWSE;
    if (n == "nesw" || n == "sizenesw" || n == "diag2")      return cfg::OCR_SIZENESW;
    // Additional standard cursors (usable without recompiling).
    if (n == "wait" || n == "busy")                          return 32514;
    if (n == "cross" || n == "crosshair")                    return 32515;
    if (n == "up" || n == "uparrow")                         return 32516;
    if (n == "sizeall" || n == "move")                       return 32646;
    if (n == "no" || n == "unavailable")                     return 32648;
    if (n == "appstarting" || n == "working")                return 32650;
    if (n == "help")                                         return 32651;
    return -1;
}

std::vector<Pose> defaults() {
    return {
        { cfg::OCR_HAND,     "hand",    cfg::deg2rad(-30.0), cfg::deg2rad( 90.0) },
        { cfg::OCR_IBEAM,    "text",    cfg::deg2rad(  0.0), cfg::deg2rad(180.0) },
        { cfg::OCR_SIZENS,   "vresize", cfg::deg2rad(  0.0), cfg::deg2rad(180.0) },
        { cfg::OCR_SIZEWE,   "hresize", cfg::deg2rad(-90.0), cfg::deg2rad( 90.0) },
        { cfg::OCR_SIZENWSE, "nwse",    cfg::deg2rad(-135.0), cfg::deg2rad( 45.0) },
        { cfg::OCR_SIZENESW, "nesw",    cfg::deg2rad(135.0), cfg::deg2rad(-45.0) },
    };
}

bool load(const std::string& path, std::vector<Pose>& out, std::string* warnings) {
    std::ifstream in(path);
    if (!in) return false;

    out.clear();
    std::ostringstream warn;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        // Strip comment, then blank check.
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        const std::size_t sep = line.find_first_of("=:");
        if (sep == std::string::npos) {
            warn << "line " << lineNo << ": no '=' or ':' -- skipped\n";
            continue;
        }
        const std::string key = trim(line.substr(0, sep));
        const std::string rhs = cleanAngles(line.substr(sep + 1));

        std::stringstream ss(rhs);
        std::string a, b;
        if (!std::getline(ss, a, ',') || !std::getline(ss, b, ',')) {
            warn << "line " << lineNo << ": need two comma-separated angles\n";
            continue;
        }
        double t1, t2;
        if (!parseDouble(a, t1) || !parseDouble(b, t2)) {
            warn << "line " << lineNo << ": bad angle value\n";
            continue;
        }

        int ocr = isNumericId(key) ? std::stoi(key) : ocrIdForName(key);
        if (ocr < 0) {
            warn << "line " << lineNo << ": unknown cursor '" << key << "'\n";
            continue;
        }
        upsert(out, Pose{ ocr, key, cfg::deg2rad(t1), cfg::deg2rad(t2) });
    }

    if (warnings) *warnings = warn.str();
    return true;
}

std::string defaultText() {
    return
"# Cursor snap poses -- one entry per line, loaded at startup.\n"
"#   <cursor> = <theta1_deg> , <theta2_deg>\n"
"# <cursor> is a friendly name (below) OR a raw numeric OCR id, so you can add\n"
"# new cursor types without recompiling. ':' works instead of '=', and\n"
"# parentheses around the angles are ignored.\n"
"#\n"
"# Angles are degrees measured from straight-down, positive = clockwise.\n"
"# For a straight line along a resize axis, make theta2 = theta1 + 180.\n"
"#\n"
"# Known names: hand (pressable), text (ibeam), vresize (sizens),\n"
"#              hresize (sizewe), nwse (sizenwse), nesw (sizenesw),\n"
"#              wait, cross, up, sizeall, no, appstarting, help.\n"
"\n"
"hand    = -30, 90     # 'pressable' triangle\n"
"text    =   0, 180    # I-beam: vertical line (fold)\n"
"vresize =   0, 180    # vertical resize: vertical line\n"
"hresize = -90, 90     # horizontal resize: horizontal line\n"
"nwse    = -135, 45    # diagonal resize \"\\\": NW-SE line\n"
"nesw    = 135, -45    # diagonal resize \"/\": NE-SW line\n"
"\n"
"# Example: give the help cursor its own pose (uncomment to try)\n"
"# help  = 60, -60\n";
}

bool writeDefault(const std::string& path) {
    std::ofstream o(path);
    if (!o) return false;
    o << defaultText();
    return static_cast<bool>(o);
}

} // namespace cursors
