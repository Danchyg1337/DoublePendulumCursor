#include "ConfigFile.h"
#include "Config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace cfg {

// The single active configuration.
Settings g;

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string stripComment(const std::string& s) {
    const std::size_t h = s.find('#');
    return h == std::string::npos ? s : s.substr(0, h);
}

bool parseDouble(const std::string& v, double& out) {
    try { std::size_t p; out = std::stod(v, &p); return p > 0; }
    catch (...) { return false; }
}

bool parseInt(const std::string& v, int& out) {
    double d;
    if (!parseDouble(v, d)) return false;
    out = static_cast<int>(d);
    return true;
}

bool parseColor(const std::string& v, Rgb& out) {
    std::stringstream ss(v);
    std::string part;
    int c[3];
    for (int i = 0; i < 3; ++i) {
        if (!std::getline(ss, part, ',')) return false;
        int val;
        if (!parseInt(trim(part), val)) return false;
        c[i] = val < 0 ? 0 : (val > 255 ? 255 : val);
    }
    out = { static_cast<unsigned char>(c[0]),
            static_cast<unsigned char>(c[1]),
            static_cast<unsigned char>(c[2]) };
    return true;
}

} // namespace

bool loadConfig(const std::string& path, std::string* warnings) {
    std::ifstream in(path);
    if (!in) return false;

    std::ostringstream warn;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(stripComment(line));
        if (line.empty()) continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            warn << "line " << lineNo << ": no '=' -- skipped\n";
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        bool ok = true;

        // Doubles.
        if      (key == "L1") ok = parseDouble(val, g.L1);
        else if (key == "L2") ok = parseDouble(val, g.L2);
        else if (key == "M1") ok = parseDouble(val, g.M1);
        else if (key == "M2") ok = parseDouble(val, g.M2);
        else if (key == "G")  ok = parseDouble(val, g.G);
        else if (key == "FRICTION")        ok = parseDouble(val, g.FRICTION);
        else if (key == "ACCEL_SMOOTHING") ok = parseDouble(val, g.ACCEL_SMOOTHING);
        else if (key == "MAX_ACCEL")       ok = parseDouble(val, g.MAX_ACCEL);
        else if (key == "SNAP_STIFFNESS")  ok = parseDouble(val, g.SNAP_STIFFNESS);
        else if (key == "WELL_RADIUS")     ok = parseDouble(val, g.WELL_RADIUS);
        else if (key == "WELL_STIFFNESS")  ok = parseDouble(val, g.WELL_STIFFNESS);
        else if (key == "SETTLE_ANGLE_TOL") ok = parseDouble(val, g.SETTLE_ANGLE_TOL);
        else if (key == "SETTLE_VEL_TOL")   ok = parseDouble(val, g.SETTLE_VEL_TOL);
        else if (key == "SNAP_GRACE_PERIOD") ok = parseDouble(val, g.SNAP_GRACE_PERIOD);
        else if (key == "SHOW_INTENSITY") ok = parseDouble(val, g.SHOW_INTENSITY);
        else if (key == "HIDE_INTENSITY") ok = parseDouble(val, g.HIDE_INTENSITY);
        else if (key == "BEAT_OFFSET_MS") ok = parseDouble(val, g.BEAT_OFFSET_MS);
        // Ints.
        else if (key == "PIVOT_RADIUS") ok = parseInt(val, g.PIVOT_RADIUS);
        else if (key == "BOB_RADIUS")   ok = parseInt(val, g.BOB_RADIUS);
        else if (key == "ROD_WIDTH")    ok = parseInt(val, g.ROD_WIDTH);
        else if (key == "SUBSTEPS")     ok = parseInt(val, g.SUBSTEPS);
        else if (key == "FPS_CAP")      ok = parseInt(val, g.FPS_CAP);
        else if (key == "MONITOR_HZ")   ok = parseInt(val, g.MONITOR_HZ);
        else if (key == "DANCER_ENABLED") ok = parseInt(val, g.DANCER_ENABLED);
        else if (key == "GIF_SIZE")       ok = parseInt(val, g.GIF_SIZE);
        else if (key == "GIF_OFFSET_X")   ok = parseInt(val, g.GIF_OFFSET_X);
        else if (key == "GIF_OFFSET_Y")   ok = parseInt(val, g.GIF_OFFSET_Y);
        // Strings.
        else if (key == "ACTORS_DIR") {
            std::string v = val;
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            ok = !v.empty();
            if (ok) g.ACTORS_DIR = v;
        }
        // Colours.
        else if (key == "COLOR_ROD")   ok = parseColor(val, g.COLOR_ROD);
        else if (key == "COLOR_PIVOT") ok = parseColor(val, g.COLOR_PIVOT);
        else if (key == "COLOR_BOB1")  ok = parseColor(val, g.COLOR_BOB1);
        else if (key == "COLOR_BOB2")  ok = parseColor(val, g.COLOR_BOB2);
        else {
            warn << "line " << lineNo << ": unknown key '" << key << "'\n";
            ok = true; // unknown key isn't a value error
        }

        if (!ok) warn << "line " << lineNo << ": bad value for '" << key << "'\n";
    }

    // keep the dancer geometry sane
    g.GIF_SIZE = std::max(16, std::min(g.GIF_SIZE, 512));
    g.GIF_OFFSET_X = std::max(0, g.GIF_OFFSET_X);
    g.GIF_OFFSET_Y = std::max(0, g.GIF_OFFSET_Y);

    if (warnings) *warnings = warn.str();
    return true;
}

std::string defaultConfigText() {
    return
"# Flying double-pendulum cursor -- configuration\n"
"# Edit values and restart the app. Lines starting with '#' are comments.\n"
"# Any key you remove falls back to its built-in default.\n"
"# (Per-cursor snap angles live in a separate cursors.conf.)\n"
"\n"
"# ---- Geometry (pixels) ----\n"
"PIVOT_RADIUS = 5      # red dot: the true click point\n"
"BOB_RADIUS   = 3      # blue/green bobs\n"
"ROD_WIDTH    = 2\n"
"L1 = 20               # first arm length\n"
"L2 = 40               # second arm length\n"
"\n"
"# ---- Masses & forces ----\n"
"M1 = 1.0\n"
"M2 = 1.0\n"
"G  = 2200             # base gravity (px/s^2); higher = snappier swing\n"
"FRICTION = 0.35       # velocity decay per second; 0 = frictionless & chaotic\n"
"\n"
"# ---- Timing ----\n"
"SUBSTEPS = 6          # physics substeps per frame (stability)\n"
"FPS_CAP  = 240        # frame-rate ceiling\n"
"MONITOR_HZ = 0        # 0 = auto-detect refresh rate; else force e.g. 144\n"
"ACCEL_SMOOTHING = 0.25 # 0..1: higher = more responsive/jittery\n"
"MAX_ACCEL = 9000      # clamp on pivot acceleration\n"
"\n"
"# ---- Colours (R,G,B) ----\n"
"COLOR_ROD   = 20,20,20\n"
"COLOR_PIVOT = 230,40,40\n"
"COLOR_BOB1  = 30,90,220\n"
"COLOR_BOB2  = 30,160,90\n"
"\n"
"# ---- Snap behaviour ----\n"
"SNAP_STIFFNESS = 900       # forced homing spring (grace-period fallback)\n"
"WELL_RADIUS = 0.4          # rad: capture zone around a snap target\n"
"WELL_STIFFNESS = 122225    # gentle local attractor strength\n"
"SETTLE_ANGLE_TOL = 0.05    # rad: 'close enough' to lock\n"
"SETTLE_VEL_TOL = 0.3       # rad/s: 'slow enough' to lock\n"
"SNAP_GRACE_PERIOD = 0.5    # s a bob may fall in on its own before forcing\n"
"\n"
"# ---- Beat dancer (BPMidentifier) ----\n"
"# Listens to the default output device. While music plays and its intensity\n"
"# is high enough, a .gifbpm GIF dances on the beat at the cursor's bottom-right.\n"
"DANCER_ENABLED = 1         # 0 = pendulum only, no audio capture\n"
"ACTORS_DIR = actors        # folder with .gifbpm files (relative = next to the exe)\n"
"GIF_SIZE = 160             # px: GIF is scaled to fit this box\n"
"GIF_OFFSET_X = 24          # px right of the pointer tip\n"
"GIF_OFFSET_Y = 24          # px below the pointer tip\n"
"SHOW_INTENSITY = 0.70      # 0 calm .. 1 intense: appear at/above this\n"
"HIDE_INTENSITY = 0.62      # disappear below this (no flicker at the edge)\n"
"BEAT_OFFSET_MS = 30        # shift the beat later to match audio output latency\n";
}

bool writeDefaultConfig(const std::string& path) {
    std::ofstream out(path);
    if (!out) return false;
    out << defaultConfigText();
    return static_cast<bool>(out);
}

} // namespace cfg
