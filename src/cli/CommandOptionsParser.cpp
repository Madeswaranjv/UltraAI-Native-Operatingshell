#include "CommandOptionsParser.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace ultra::cli {

namespace {

bool startsWithDash(const std::string& token) {
  return !token.empty() && token.front() == '-';
}

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

}  // namespace

CommandOptionsParseResult CommandOptionsParser::parse(
    const std::vector<std::string>& args,
    bool allowPositional) {
  CommandOptionsParseResult result;
  result.ok = false;

  bool collectingNativeArgs = false;
  std::vector<std::string> nativeTokens;

  for (const std::string& token : args) {
    if (collectingNativeArgs) {
      nativeTokens.push_back(token);
      continue;
    }

    if (token == "--") {
      collectingNativeArgs = true;
      continue;
    }

    const std::string normalized = toLower(token);

    if (normalized == "--release") {
      result.options.release = true;
      continue;
    }
    if (normalized == "--debug") {
      result.options.debug = true;
      continue;
    }
    if (normalized == "--watch") {
      result.options.watch = true;
      continue;
    }
    if (normalized == "--parallel") {
      result.options.parallel = true;
      continue;
    }
    if (normalized == "--force") {
      result.options.force = true;
      continue;
    }
    if (normalized == "--clean") {
      result.options.clean = true;
      continue;
    }
    if (normalized == "--deep") {
      result.options.deep = true;
      continue;
    }
    if (normalized == "--verbose") {
      result.options.verbose = true;
      continue;
    }
    if (normalized == "--dry-run") {
      result.options.dryRun = true;
      continue;
    }
    if (normalized == "--json") {
      result.options.jsonOutput = true;
      continue;
    }

    if (startsWithDash(token)) {
      result.error = "Unknown flag: " + token;
      return result;
    }

    if (!allowPositional) {
      result.error = "Unexpected positional argument: " + token;
      return result;
    }

    result.positionalArgs.push_back(token);
  }

  if (result.options.release && result.options.debug) {
    result.error = "Flags --release and --debug are mutually exclusive.";
    return result;
  }

  if (!nativeTokens.empty()) {
    result.options.nativeArgs = joinForShell(nativeTokens);
  }

  result.ok = true;
  return result;
}

std::string CommandOptionsParser::joinForShell(
    const std::vector<std::string>& args,
    std::size_t startIndex) {
  if (startIndex >= args.size()) {
    return {};
  }

  std::ostringstream out;
  bool first = true;
  for (std::size_t i = startIndex; i < args.size(); ++i) {
    if (!first) {
      out << ' ';
    }
    first = false;
    out << quoteToken(args[i]);
  }
  return out.str();
}

std::string CommandOptionsParser::quoteToken(const std::string& token) {
  if (token.empty()) {
    return "\"\"";
  }

  bool needsQuotes = false;
  for (const char c : token) {
    if (c == ' ' || c == '\t' || c == '"') {
      needsQuotes = true;
      break;
    }
  }

  if (!needsQuotes) {
    return token;
  }

  std::string escaped;
  escaped.reserve(token.size() + 8);
  for (const char c : token) {
    if (c == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return "\"" + escaped + "\"";
}

}  // namespace ultra::cli
