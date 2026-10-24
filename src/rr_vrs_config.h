#pragma once
#include <string>
#include <sstream>
namespace rrvrsconfig {
inline bool Allowed(const std::string &text, bool readonly, bool allow_vrs) {
    if (!readonly)
        return false;
    std::istringstream input(text);
    unsigned found = 0;
    for (std::string line; std::getline(input, line);)
        if (line.rfind("VRS@", 0) == 0) {
            if (++found != 1)
                return false;
            const auto at = line.find('=');
            if (at == std::string::npos)
                return false;
            auto value = line.substr(at + 1);
            const auto comment = value.find("//");
            if (comment != std::string::npos)
                value.resize(comment);
            const auto first = value.find_first_not_of(" \t\r"),
                       last = value.find_last_not_of(" \t\r");
            if (first == std::string::npos)
                return false;
            value = value.substr(first, last - first + 1);
            if (value != "false" && !(allow_vrs && value == "true"))
                return false;
        }
    return found == 1;
}
} // namespace rrvrsconfig
