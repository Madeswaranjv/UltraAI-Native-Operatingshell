#include "InitCommand.h"
#include "CommandOptionsParser.h"
#include "../scaffolds/ScaffoldFactory.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <utility>

namespace ultra::cli {

namespace {

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool startsWithDash(const std::string& value) {
  return !value.empty() && value.front() == '-';
}

}  // namespace

InitCommand::InitCommand()
    : m_ownedEnvironment(std::make_unique<ultra::scaffolds::DefaultScaffoldEnvironment>()),
      m_environment(m_ownedEnvironment.get()) {}

InitCommand::InitCommand(ultra::scaffolds::IScaffoldEnvironment& environment)
    : m_environment(&environment) {}

std::string InitCommand::supportedStackMessage() {
  return "Please specify scaffold stack. Supported: react, next, django, python, cmake, rust";
}

int InitCommand::execute(const std::vector<std::string>& args) {
  const ParsedInitArgs parsed = parseArgs(args);
  if (!parsed.ok) {
    std::cout << "[ERROR] " << parsed.error << '\n';
    return 1;
  }

  std::unique_ptr<ultra::scaffolds::ScaffoldBase> scaffold =
      ultra::scaffolds::createScaffold(parsed.stack, *m_environment);
  if (!scaffold) {
    std::cout << "[ERROR] " << supportedStackMessage() << '\n';
    return 1;
  }

  scaffold->generate(parsed.projectName, parsed.options);
  return scaffold->lastExitCode();
}

InitCommand::ParsedInitArgs InitCommand::parseArgs(
    const std::vector<std::string>& args) const {
  ParsedInitArgs result;

  if (args.empty()) {
    result.error = supportedStackMessage();
    return result;
  }

  result.stack = toLower(args[0]);
  if (!ultra::scaffolds::isSupportedStack(result.stack)) {
    result.error = supportedStackMessage();
    return result;
  }

  if (args.size() < 2 || args[1].empty() || startsWithDash(args[1])) {
    result.error = "Please specify project name for stack '" + result.stack +
                   "'. Usage: ultra init <stack> <name> [--template <template>] [flags]";
    return result;
  }

  result.projectName = args[1];
  result.options.destinationRoot = m_environment->currentPath();

  std::vector<std::string> passthroughTokens;
  for (std::size_t i = 2; i < args.size(); ++i) {
    const std::string& token = args[i];
    const std::string normalized = toLower(token);

    if (normalized == "--template") {
      if (i + 1 >= args.size() || args[i + 1].empty() ||
          startsWithDash(args[i + 1])) {
        result.error =
            "Flag --template requires a template value, for example --template react-ts";
        return result;
      }
      result.options.templateName = args[i + 1];
      ++i;
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
    if (normalized == "--force") {
      result.options.force = true;
      continue;
    }

    if (normalized == "--") {
      result.options.passthroughFlags =
          CommandOptionsParser::joinForShell(args, i + 1);
      result.ok = true;
      return result;
    }

    if (startsWithDash(token)) {
      passthroughTokens.push_back(token);
      continue;
    }

    result.error = "Unexpected argument for init: " + token;
    return result;
  }

  if (!passthroughTokens.empty()) {
    result.options.passthroughFlags =
        CommandOptionsParser::joinForShell(passthroughTokens);
  }

  result.ok = true;
  return result;
}

}  // namespace ultra::cli
