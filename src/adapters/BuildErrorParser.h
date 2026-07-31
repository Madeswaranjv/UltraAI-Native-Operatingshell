#pragma once
#include <string>

namespace ultra::adapters {

class BuildErrorParser {
public:
    static std::string parse(const std::string& rawOutput);
};

} // namespace ultra::adapters