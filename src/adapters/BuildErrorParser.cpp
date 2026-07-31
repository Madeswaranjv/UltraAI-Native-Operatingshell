#include "BuildErrorParser.h"
#include <sstream>
#include <vector>
#include <algorithm>

namespace ultra::adapters {

std::string BuildErrorParser::parse(const std::string& rawOutput) {
    std::istringstream stream(rawOutput);
    std::string line;
    std::vector<std::string> errors;
    
    bool capturingPythonTraceback = false;
    bool capturingRustError = false;

    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        
        // Remove trailing \r if present
        if (line.back() == '\r') {
            line.pop_back();
        }

        // C++ / GCC / Clang / MSVC
        if (line.find("error:") != std::string::npos || 
            line.find("fatal error:") != std::string::npos ||
            line.find(" error ") != std::string::npos ||
            line.find("error C") != std::string::npos || 
            line.find("error LNK") != std::string::npos) {
            errors.push_back(line);
        }
        else if (line.find("undefined reference to") != std::string::npos ||
                 line.find("unresolved external symbol") != std::string::npos) {
            errors.push_back(line);
        }
        // Rust
        else if (line.find("error[E") != std::string::npos ||
                 line.find("error: aborting due to") != std::string::npos) {
            errors.push_back(line);
            capturingRustError = true;
        }
        else if (capturingRustError && (line.find(" | ") != std::string::npos || line.find(" = ") != std::string::npos)) {
            errors.push_back(line);
        }
        else if (capturingRustError && line.find("error:") == std::string::npos && line.find("-->") == std::string::npos) {
            capturingRustError = false; 
        }
        // TypeScript / Node
        else if (line.find("TS") == 0 && line.find("error") != std::string::npos) {
            errors.push_back(line);
        }
        else if (line.find("ERR!") != std::string::npos) {
            errors.push_back(line);
        }
        // Python
        else if (line.find("Traceback (most recent call last):") != std::string::npos) {
            capturingPythonTraceback = true;
            errors.push_back(line);
        }
        else if (capturingPythonTraceback) {
            if (line.empty() || (line[0] != ' ' && line.find("Error") != std::string::npos)) { 
                errors.push_back(line);
                if (line.find("Error") != std::string::npos) {
                    capturingPythonTraceback = false;
                }
            } else {
                errors.push_back(line);
            }
        }
        // General / CMake / Test failures
        else if (line.find("CMake Error") != std::string::npos) {
            errors.push_back(line);
        }
        else if (line.find("FAILED") != std::string::npos || line.find("FAIL") != std::string::npos) {
            if (line.find("pass") == std::string::npos && line.length() < 120) {
                errors.push_back(line);
            }
        }
    }

    if (errors.empty()) {
        std::istringstream fallbackStream(rawOutput);
        std::vector<std::string> allLines;
        while (std::getline(fallbackStream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            allLines.push_back(line);
        }
        if (allLines.size() > 20) {
            allLines.erase(allLines.begin(), allLines.end() - 20);
        }
        std::string result = "No structured errors matched. Last lines of output:\n";
        for (const auto& l : allLines) {
            result += l + "\n";
        }
        return result;
    }

    std::string result;
    int count = 0;
    for (const auto& err : errors) {
        result += err + "\n";
        if (++count >= 20) break;
    }
    
    if (errors.size() > 20) {
        result += "... (" + std::to_string(errors.size() - 20) + " more error lines truncated)\n";
    }

    return result;
}

} // namespace ultra::adapters