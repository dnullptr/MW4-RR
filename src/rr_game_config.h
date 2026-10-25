#pragma once
#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// The game's settings file against INSTALLATION.txt's required values. CPU only and report only:
// nothing here may block RR or switch it off.
namespace rrgameconfig {
// "DxrMode@1;57752;20945 = High // comment" is name DxrMode, index 1, value High.
struct Entry {
    std::string name, index, value;
};
enum class FileState { Read, NotFound, Unreadable };
struct File {
    FileState state{FileState::NotFound};
    bool readonly{};
    std::string text;
};
inline std::string Trim(const std::string &text) {
    const auto first = text.find_first_not_of(" \t\r"), last = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}
// ASCII, case-insensitive.
inline bool Same(std::string_view a, std::string_view b) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) {
               return lower(x) == lower(y);
           });
}
// Blank lines, "//" lines and lines without "=" are skipped.
inline std::vector<Entry> Parse(const std::string &text) {
    std::vector<Entry> entries;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        line = Trim(line);
        const auto equals = line.find('='), comment = line.find("//");
        if (equals == std::string::npos || comment < equals)
            continue;
        const auto key = line.substr(0, equals);
        const auto end = key.find_first_of("@;");
        Entry entry;
        entry.name = Trim(key.substr(0, end));
        if (end != std::string::npos && key[end] == '@') {
            const auto rest = key.substr(end + 1);
            entry.index = Trim(rest.substr(0, rest.find(';')));
        }
        auto value = line.substr(equals + 1);
        value.resize(std::min(value.size(), value.find("//")));
        entry.value = Trim(value);
        if (!entry.name.empty())
            entries.push_back(std::move(entry));
    }
    return entries;
}
// index null: any index.
inline bool Is(const Entry &entry, const char *name, const char *index) {
    return Same(entry.name, name) && (!index || Same(entry.index, index));
}
inline const Entry *Find(const std::vector<Entry> &entries, const char *name,
                         const char *index = nullptr) {
    for (const auto &entry : entries)
        if (Is(entry, name, index))
            return &entry;
    return nullptr;
}
// INSTALLATION.txt lines 70-90. Every entry of a ruled name must comply, and a missing one is a
// warning. DxrMode has two meanings by index: @0 must be On, @1 selects a quality, not On.
struct Rule {
    const char *name, *index, *value;
    bool forbidden; // value is the one that must not be set
};
inline constexpr Rule rules[] = {
    {"MotionBlur", nullptr, "Off", false},
    {"WeaponMotionBlur", nullptr, "false", false},
    {"EnableVelocityBasedBlur", nullptr, "false", false},
    {"DofEnable", nullptr, "Off", false},
    {"DofWeaponDisable", nullptr, "true", false},
    {"DofWorldDisable", nullptr, "true", false},
    {"AATechniquePreferredMP", nullptr, "DLSS", false},
    {"DLSSModeMP", nullptr, "DLSS", false},
    {"DLSSModelUI", nullptr, "TRANSFORMER", false},
    {"DxrDenoiser", nullptr, "DLSS Ray Reconstruction", false},
    {"DynamicSceneResolution", nullptr, "false", false},
    {"DxrMode", "0", "On", false},
    {"DxrMode", "1", "On", true},
};
// Logged so the live values are visible: the first entry of the name, or the given index.
struct Shown {
    const char *name, *index;
};
inline constexpr Shown shown[] = {
    {"EnableVelocityBasedBlur", nullptr}, {"DLSSModelUI", nullptr},
    {"DxrDenoiser", nullptr},             {"DLSSSharpnessMP", nullptr},
    {"DLSSFrameGeneration", nullptr},     {"DLSSFrameGenerationmultiType", nullptr},
    {"DLSSPerfModeMP", nullptr},          {"DxrMode", "0"},
    {"DxrMode", "1"},                     {"VRS", nullptr},
    {"MaxFpsInGame", nullptr},            {"MaxFpsOutOfFocus", nullptr},
    {"SustainabilityReduceQualityIdle", nullptr},
};
inline std::string Label(const char *name, const char *index) {
    return index ? std::string(name) + "@" + index : std::string(name);
}
// Locale-free: a digit 1-9 before any exponent.
inline bool Nonzero(const std::string &number) {
    for (const char c : number) {
        if (c == 'e' || c == 'E')
            break;
        if (c >= '1' && c <= '9')
            return true;
    }
    return false;
}
// Appends the values line, a warning per violation, then the notes; returns the warning count.
// The notes are advice, not INSTALLATION rules, and are not counted.
inline unsigned Check(const std::string &text, std::vector<std::string> &lines) {
    const auto entries = Parse(text);
    std::string values = "Game config values: ";
    for (const auto &item : shown) {
        const auto *entry = Find(entries, item.name, item.index);
        if (&item != shown)
            values += ", ";
        values += Label(item.name, item.index) + "=" + (entry ? entry->value : "<missing>");
    }
    lines.push_back(values);
    unsigned warnings = 0;
    for (const auto &rule : rules) {
        const auto label = Label(rule.name, rule.index);
        const auto wanted =
            (rule.forbidden ? std::string("a value other than ") : std::string()) + rule.value;
        bool found = false;
        for (const auto &entry : entries) {
            if (!Is(entry, rule.name, rule.index))
                continue;
            found = true;
            if (Same(entry.value, rule.value) == rule.forbidden) {
                lines.push_back("Game config warning: " + label + "=" + entry.value +
                                "; INSTALLATION.txt requires " + wanted);
                ++warnings;
            }
        }
        if (!found && !rule.forbidden) {
            lines.push_back("Game config warning: " + label +
                            " not found; INSTALLATION.txt requires " + wanted);
            ++warnings;
        }
    }
    const auto *generation = Find(entries, "DLSSFrameGeneration");
    if (generation && Same(generation->value, "true"))
        lines.push_back("Game config note: frame generation is on; RR health frames= counts "
                        "rendered frames only, judge RR with it off");
    const auto *sharpness = Find(entries, "DLSSSharpnessMP");
    if (sharpness && Nonzero(sharpness->value))
        lines.push_back("Game config note: DLSSSharpnessMP=" + sharpness->value +
                        " may drive a game sharpening pass after RR");
    return warnings;
}
// All "Game config" lines for the file as read, the state line first; returns the warning count.
inline unsigned Report(const File &file, bool vrs_allows, std::vector<std::string> &lines) {
    if (file.state != FileState::Read) {
        lines.push_back(file.state == FileState::NotFound ? "Game config: file not found"
                                                           : "Game config: file unreadable");
        return 0;
    }
    lines.push_back(std::string("Game config: read-only=") + (file.readonly ? "yes" : "no") +
                    ", VRS gate allows RR=" + (vrs_allows ? "yes" : "no"));
    return Check(file.text, lines);
}
} // namespace rrgameconfig
