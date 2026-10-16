#include "shader/Diagnostics.h"

#include <regex>
#include <sstream>

namespace shader {

namespace {

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::vector<rhi::Diagnostic> parseCompileLog(const std::string& log) {
    static const std::regex apple(R"(^\s*(ERROR|WARNING):\s*(\d+):(\d+):\s*(.*)$)", std::regex::icase);
    static const std::regex mesa(R"(^\s*(\d+):(\d+)\(\d+\):\s*(error|warning)\s*:?\s*(.*)$)", std::regex::icase);
    static const std::regex nvidia(R"(^\s*(\d+)\((\d+)\)\s*:\s*(error|warning)\s*\w*\s*:?\s*(.*)$)", std::regex::icase);

    std::vector<rhi::Diagnostic> out;
    std::istringstream in(log);
    std::string line;
    while (std::getline(in, line)) {
        if (trim(line).empty()) continue;
        std::smatch m;
        rhi::Diagnostic d;
        if (std::regex_match(line, m, apple)) {
            d.isError = (m[1].str()[0] == 'E' || m[1].str()[0] == 'e');
            d.file = std::stoi(m[2].str());
            d.line = std::stoi(m[3].str());
            d.message = trim(m[4].str());
        } else if (std::regex_match(line, m, mesa)) {
            d.file = std::stoi(m[1].str());
            d.line = std::stoi(m[2].str());
            d.isError = (m[3].str()[0] == 'e' || m[3].str()[0] == 'E');
            d.message = trim(m[4].str());
        } else if (std::regex_match(line, m, nvidia)) {
            d.file = std::stoi(m[1].str());
            d.line = std::stoi(m[2].str());
            d.isError = (m[3].str()[0] == 'e' || m[3].str()[0] == 'E');
            d.message = trim(m[4].str());
        } else {
            d.line = 0;
            d.message = trim(line);
        }
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace shader
