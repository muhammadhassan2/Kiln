#pragma once

#include <string>
#include <utility>
#include <vector>

namespace app {

struct Template {
    const char* name;
    const char* description;
    std::vector<std::pair<std::string, std::string>> passes;  // {pass name, source}
};

const std::vector<Template>& templates();

}  // namespace app
