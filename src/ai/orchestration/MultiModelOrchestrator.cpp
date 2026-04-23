#include "MultiModelOrchestrator.h"

#include "../../runtime/cognitive/tool_router/ToolRouter.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>

namespace ultra::ai::orchestration {

namespace {

struct FailureRecord {
  std::string provider;
  model::ModelErrorCode code{model::ModelErrorCode::ProviderUnavailable};
  std::string message;
  std::uint64_t latencyMs{0U};
};

struct UltraContextBundle {
  std::string roleLabel;
  std::string taskTypeLabel;
  std::string taskId;
  std::string primaryTarget;
  std::string sourceFile;
  std::string block;
  std::vector<std::string> sources;
};

constexpr std::size_t kContextBlockLimitChars = 4096U;
constexpr std::size_t kContextSectionLimitChars = 1600U;
constexpr std::size_t kContextTopLineLimit = 18U;
constexpr std::size_t kContextSourceWindowLines = 24U;

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string canonicalRoleName(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  value = lowerAscii(value.substr(first, last - first + 1U));
  if (value.empty() || value == "auto") {
    return {};
  }
  if (value == "plan" || value == "planner" || value == "planning") {
    return "planner";
  }
  if (value == "verify" || value == "verifier" || value == "verification" ||
      value == "validate" || value == "validation") {
    return "verifier";
  }
  if (value == "code" || value == "coder" || value == "coding" ||
      value == "code_generation") {
    return "coder";
  }
  if (value == "analysis" || value == "analyzer" || value == "analyse" ||
      value == "analyze") {
    return "analyzer";
  }
  return {};
}

void pushUnique(std::vector<std::string>& values, const std::string& value) {
  if (value.empty()) {
    return;
  }
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(value);
  }
}

bool containsValue(const std::vector<std::string>& values,
                   const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::string roleForTaskType(const OrchestrationContext& context) {
  if (!context.modelRoleHint.empty()) {
    const std::string explicitRole = canonicalRoleName(context.modelRoleHint);
    if (!explicitRole.empty()) {
      return explicitRole;
    }
  }
  switch (context.taskType) {
    case TaskType::Planning:
      return "planner";
    case TaskType::Coding:
      return "coder";
    case TaskType::Analysis:
      return "analyzer";
    case TaskType::Reasoning:
      return {};
  }
  return {};
}

std::string requestResolvedRole(const model::ModelRequest& request) {
  if (!request.contextPayload.is_object() ||
      !request.contextPayload.contains("model_role") ||
      !request.contextPayload.at("model_role").is_string()) {
    return {};
  }
  return canonicalRoleName(request.contextPayload.at("model_role").get<std::string>());
}

nlohmann::ordered_json sortJsonKeys(const nlohmann::ordered_json& value) {
  if (value.is_array()) {
    nlohmann::ordered_json sorted = nlohmann::ordered_json::array();
    for (const auto& item : value) {
      sorted.push_back(sortJsonKeys(item));
    }
    return sorted;
  }

  if (!value.is_object()) {
    return value;
  }

  std::vector<std::pair<std::string, nlohmann::ordered_json>> entries;
  entries.reserve(value.size());
  for (auto it = value.begin(); it != value.end(); ++it) {
    entries.emplace_back(it.key(), sortJsonKeys(it.value()));
  }

  std::sort(entries.begin(), entries.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });

  nlohmann::ordered_json sorted = nlohmann::ordered_json::object();
  for (auto& [key, item] : entries) {
    sorted[key] = std::move(item);
  }
  return sorted;
}

nlohmann::ordered_json defaultRoutingConfiguration() {
  // Default routing: best model per task type.
  // Users override this via .ultra/model_routing.json.
  nlohmann::ordered_json routing = nlohmann::ordered_json::object();
  routing["analysis"]  = "anthropic";
  routing["coding"]    = "ollama";
  routing["planning"]  = "deepseek";
  routing["reasoning"] = "deepseek";

  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["routing"] = std::move(routing);
  return payload;
}

bool isRetryableFailure(const model::ModelErrorCode code) {
  switch (code) {
    case model::ModelErrorCode::ProviderUnavailable:
    case model::ModelErrorCode::ModelTimeout:
    case model::ModelErrorCode::RateLimited:
    case model::ModelErrorCode::InvalidResponse:
      return true;
    case model::ModelErrorCode::None:
      return false;
  }
  return true;
}

model::ModelErrorCode collapseFailureCode(
    const std::vector<FailureRecord>& failures) {
  for (const FailureRecord& failure : failures) {
    if (failure.code == model::ModelErrorCode::RateLimited) {
      return failure.code;
    }
  }
  for (const FailureRecord& failure : failures) {
    if (failure.code == model::ModelErrorCode::ModelTimeout) {
      return failure.code;
    }
  }
  for (const FailureRecord& failure : failures) {
    if (failure.code == model::ModelErrorCode::InvalidResponse) {
      return failure.code;
    }
  }
  for (const FailureRecord& failure : failures) {
    if (failure.code == model::ModelErrorCode::ProviderUnavailable) {
      return failure.code;
    }
  }
  return model::ModelErrorCode::ProviderUnavailable;
}

std::string buildFailureMessage(const std::vector<FailureRecord>& failures) {
  if (failures.empty()) {
    return "No model providers were available for orchestration.";
  }

  std::ostringstream stream;
  stream << "No model provider succeeded. ";
  for (std::size_t index = 0U; index < failures.size(); ++index) {
    const FailureRecord& failure = failures[index];
    if (index != 0U) {
      stream << " | ";
    }
    stream << failure.provider << " [" << model::toString(failure.code) << "]";
    if (!failure.message.empty()) {
      stream << ": " << failure.message;
    }
  }
  return stream.str();
}

std::string trimAscii(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1U);
}

bool startsWithCommandError(const std::string& value) {
  return value.rfind("ERROR:", 0U) == 0U || value.rfind("[ERROR]", 0U) == 0U;
}

std::string truncateForContext(std::string value, const std::size_t limit) {
  if (value.size() <= limit) {
    return value;
  }
  constexpr std::string_view kNotice = "\n...[truncated]";
  const std::size_t keep = limit > kNotice.size() ? limit - kNotice.size() : 0U;
  value.resize(keep);
  value += kNotice;
  return value;
}

bool looksLikeFilePath(const std::string_view value) {
  if (value.empty()) {
    return false;
  }
  if (value.find('/') != std::string_view::npos ||
      value.find('\\') != std::string_view::npos) {
    return true;
  }
  const std::size_t dotPos = value.rfind('.');
  return dotPos != std::string_view::npos && dotPos > 0U &&
         (value.size() - dotPos) <= 8U;
}

std::string contextTypeLabel(const std::string& role, const TaskType taskType) {
  const std::string normalizedRole = canonicalRoleName(role);
  if (normalizedRole == "planner") {
    return "planning";
  }
  if (normalizedRole == "coder") {
    return "code";
  }
  if (normalizedRole == "verifier") {
    return "verify";
  }
  if (normalizedRole == "analyzer") {
    return "analysis";
  }
  switch (taskType) {
    case TaskType::Planning:
      return "planning";
    case TaskType::Coding:
      return "code";
    case TaskType::Analysis:
      return "analysis";
    case TaskType::Reasoning:
      return "reasoning";
  }
  return "analysis";
}

std::string firstStringFromArray(const nlohmann::ordered_json& value) {
  if (!value.is_array()) {
    return {};
  }
  for (const auto& item : value) {
    if (item.is_string()) {
      const std::string candidate = trimAscii(item.get<std::string>());
      if (!candidate.empty()) {
        return candidate;
      }
    }
  }
  return {};
}

std::string extractNestedTarget(const nlohmann::ordered_json& value) {
  if (!value.is_object()) {
    return {};
  }
  if (value.contains("goal") && value.at("goal").is_object()) {
    const auto& goal = value.at("goal");
    if (goal.contains("target") && goal.at("target").is_string()) {
      return trimAscii(goal.at("target").get<std::string>());
    }
  }
  return {};
}

std::string extractPrimaryTarget(const model::ModelRequest& request) {
  if (!request.contextPayload.is_object()) {
    return {};
  }

  const auto& payload = request.contextPayload;
  if (payload.contains("target") && payload.at("target").is_string()) {
    return trimAscii(payload.at("target").get<std::string>());
  }
  if (payload.contains("file") && payload.at("file").is_string()) {
    return trimAscii(payload.at("file").get<std::string>());
  }
  if (payload.contains("source_file") && payload.at("source_file").is_string()) {
    return trimAscii(payload.at("source_file").get<std::string>());
  }
  if (payload.contains("targets")) {
    const std::string first = firstStringFromArray(payload.at("targets"));
    if (!first.empty()) {
      return first;
    }
  }
  if (payload.contains("file_targets")) {
    const std::string first = firstStringFromArray(payload.at("file_targets"));
    if (!first.empty()) {
      return first;
    }
  }
  if (payload.contains("intent")) {
    const std::string nested = extractNestedTarget(payload.at("intent"));
    if (!nested.empty()) {
      return nested;
    }
  }
  if (payload.contains("resolved_intent")) {
    const std::string nested = extractNestedTarget(payload.at("resolved_intent"));
    if (!nested.empty()) {
      return nested;
    }
  }
  return {};
}

std::string extractTaskId(const model::ModelRequest& request,
                          const std::string& fallbackRole) {
  if (!request.contextPayload.is_object()) {
    return fallbackRole;
  }
  const auto& payload = request.contextPayload;
  if (payload.contains("task_id") && payload.at("task_id").is_string()) {
    const std::string taskId = trimAscii(payload.at("task_id").get<std::string>());
    if (!taskId.empty()) {
      return taskId;
    }
  }
  if (payload.contains("stage") && payload.at("stage").is_string()) {
    const std::string stage = trimAscii(payload.at("stage").get<std::string>());
    if (!stage.empty()) {
      return stage;
    }
  }
  return fallbackRole;
}

std::string summarizeJson(const nlohmann::ordered_json& value,
                          const std::size_t limit) {
  if (value.is_null()) {
    return {};
  }
  return truncateForContext(value.dump(2), limit);
}

std::string requestSummary(const model::ModelRequest& request) {
  if (!request.contextPayload.is_object()) {
    return {};
  }
  const auto& payload = request.contextPayload;
  if (payload.contains("intent")) {
    return summarizeJson(payload.at("intent"), 900U);
  }
  if (payload.contains("resolved_intent")) {
    return summarizeJson(payload.at("resolved_intent"), 900U);
  }

  nlohmann::ordered_json summary = nlohmann::ordered_json::object();
  const char* const keys[] = {
      "action_kind",
      "details",
      "estimated_dependency_depth",
      "estimated_files_changed",
      "target",
      "tool_required",
  };
  for (const char* key : keys) {
    if (payload.contains(key)) {
      summary[key] = payload.at(key);
    }
  }
  return summarizeJson(summary, 700U);
}

std::string extractDefinedInPath(const std::string& aiQueryOutput) {
  std::istringstream stream(aiQueryOutput);
  std::string line;
  while (std::getline(stream, line)) {
    const std::string trimmed = trimAscii(line);
    constexpr std::string_view kPrefix = "Defined in:";
    if (trimmed.rfind(kPrefix, 0U) == 0U) {
      return trimAscii(trimmed.substr(kPrefix.size()));
    }
  }
  return {};
}

std::string topLines(const std::string& text,
                     const std::size_t maxLines,
                     const std::size_t maxChars) {
  std::istringstream stream(text);
  std::string line;
  std::string output;
  std::size_t lines = 0U;
  while (lines < maxLines && std::getline(stream, line)) {
    const std::string trimmed = trimAscii(line);
    if (trimmed.empty()) {
      continue;
    }
    if (!output.empty()) {
      output += '\n';
    }
    output += trimmed;
    ++lines;
    if (output.size() >= maxChars) {
      break;
    }
  }
  return truncateForContext(output, maxChars);
}

std::string sourceSnippet(const std::string& sourceText,
                          const std::string& targetHint,
                          const std::size_t maxLines,
                          const std::size_t maxChars) {
  std::vector<std::string> lines;
  std::istringstream stream(sourceText);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  if (lines.empty()) {
    return {};
  }

  std::size_t startLine = 0U;
  if (!targetHint.empty()) {
    const std::string loweredNeedle = lowerAscii(targetHint);
    for (std::size_t index = 0U; index < lines.size(); ++index) {
      if (lowerAscii(lines[index]).find(loweredNeedle) != std::string::npos) {
        startLine = index > 6U ? index - 6U : 0U;
        break;
      }
    }
  }

  const std::size_t endLine = std::min(lines.size(), startLine + maxLines);
  std::string snippet;
  for (std::size_t index = startLine; index < endLine; ++index) {
    if (!snippet.empty()) {
      snippet += '\n';
    }
    snippet += lines[index];
    if (snippet.size() >= maxChars) {
      break;
    }
  }
  return truncateForContext(trimAscii(snippet), maxChars);
}

void appendSection(std::string& block,
                   const std::string& heading,
                   std::string content) {
  content = trimAscii(std::move(content));
  if (content.empty()) {
    return;
  }
  if (!block.empty()) {
    block += "\n\n";
  }
  block += heading;
  block += ":\n";
  block += content;
}

std::optional<std::string> fetchUltraOutput(
    runtime::cognitive::tool_router::ToolRouter& router,
    const std::filesystem::path& projectRoot,
    const std::string& tool,
    std::map<std::string, std::string> args,
    const std::string& sourceName,
    const std::string& failureLabel,
    std::vector<std::string>& sources) {
  args["cwd"] = projectRoot.string();
  const std::string output = trimAscii(router.route_and_execute(tool, args));
  if (output.empty() || startsWithCommandError(output)) {
    std::cout << "[CONTEXT_ERROR] failed to fetch " << failureLabel << std::endl;
    return std::nullopt;
  }
  pushUnique(sources, sourceName);
  return output;
}

UltraContextBundle buildUltraContext(const model::ModelRequest& request,
                                     const OrchestrationContext& context,
                                     const std::filesystem::path& projectRoot,
                                     const std::string& routedRole) {
  UltraContextBundle bundle;
  bundle.roleLabel = routedRole.empty() ? toString(context.taskType) : routedRole;
  bundle.taskTypeLabel = contextTypeLabel(bundle.roleLabel, context.taskType);
  bundle.taskId = extractTaskId(request, bundle.roleLabel);
  bundle.primaryTarget = extractPrimaryTarget(request);

  runtime::cognitive::tool_router::ToolRouter router;

  std::string requestPayloadSummary = requestSummary(request);
  if (!requestPayloadSummary.empty()) {
    appendSection(bundle.block, "Intent Payload", requestPayloadSummary);
  }

  if (bundle.taskTypeLabel == "planning") {
    if (const auto astOutput = fetchUltraOutput(router,
                                                projectRoot,
                                                "ast_context",
                                                {{"path", "."}},
                                                "context_ast",
                                                "context --ast for project root",
                                                bundle.sources);
        astOutput.has_value()) {
      appendSection(bundle.block,
                    "Project Structure",
                    topLines(*astOutput, kContextTopLineLimit, 1200U));
    }
    bundle.block = truncateForContext(bundle.block, kContextBlockLimitChars);
    return bundle;
  }

  std::string queryOutput;
  if (!bundle.primaryTarget.empty() && !looksLikeFilePath(bundle.primaryTarget)) {
    if (const auto fetched = fetchUltraOutput(router,
                                              projectRoot,
                                              "query_symbol",
                                              {{"target", bundle.primaryTarget}},
                                              "ai_query",
                                              "ai_query for symbol " + bundle.primaryTarget,
                                              bundle.sources);
        fetched.has_value()) {
      queryOutput = *fetched;
    }
  }

  if (bundle.sourceFile.empty() && !queryOutput.empty()) {
    bundle.sourceFile = extractDefinedInPath(queryOutput);
  }
  if (bundle.sourceFile.empty() && looksLikeFilePath(bundle.primaryTarget)) {
    bundle.sourceFile = bundle.primaryTarget;
  }

  if (bundle.taskTypeLabel == "analysis") {
    if (!queryOutput.empty()) {
      appendSection(bundle.block,
                    "Symbol Summary",
                    topLines(queryOutput, kContextTopLineLimit, 1200U));
    }
    if (!bundle.primaryTarget.empty()) {
      if (const auto impactOutput = fetchUltraOutput(router,
                                                     projectRoot,
                                                     "impact_analysis",
                                                     {{"target", bundle.primaryTarget}},
                                                     "ai_impact",
                                                     "ai_impact for target " +
                                                         bundle.primaryTarget,
                                                     bundle.sources);
          impactOutput.has_value()) {
        appendSection(bundle.block,
                      "Impact Graph",
                      topLines(*impactOutput, kContextTopLineLimit, 1200U));
      }
    }
    if (!bundle.primaryTarget.empty() && !looksLikeFilePath(bundle.primaryTarget)) {
      if (const auto contextOutput = fetchUltraOutput(router,
                                                      projectRoot,
                                                      "query_context",
                                                      {{"query", bundle.primaryTarget}},
                                                      "context_query",
                                                      "context query for symbol " +
                                                          bundle.primaryTarget,
                                                      bundle.sources);
          contextOutput.has_value()) {
        appendSection(bundle.block,
                      "Relevant Structure",
                      topLines(*contextOutput, kContextTopLineLimit, 1200U));
      }
    }
  } else if (bundle.taskTypeLabel == "code") {
    if (!queryOutput.empty()) {
      appendSection(bundle.block,
                    "Dependencies",
                    topLines(queryOutput, kContextTopLineLimit, 1200U));
    }
    if (!bundle.sourceFile.empty()) {
      if (const auto sourceOutput = fetchUltraOutput(router,
                                                     projectRoot,
                                                     "read_source",
                                                     {{"file", bundle.sourceFile}},
                                                     "ai_source",
                                                     "ai_source for file " +
                                                         bundle.sourceFile,
                                                     bundle.sources);
          sourceOutput.has_value()) {
        appendSection(bundle.block,
                      "Relevant Code",
                      sourceSnippet(*sourceOutput,
                                    bundle.primaryTarget,
                                    kContextSourceWindowLines,
                                    kContextSectionLimitChars));
      }
    }
    if (!bundle.primaryTarget.empty()) {
      if (const auto impactOutput = fetchUltraOutput(router,
                                                     projectRoot,
                                                     "impact_analysis",
                                                     {{"target", bundle.primaryTarget}},
                                                     "ai_impact",
                                                     "ai_impact for target " +
                                                         bundle.primaryTarget,
                                                     bundle.sources);
          impactOutput.has_value()) {
        appendSection(bundle.block,
                      "Impact Chain",
                      topLines(*impactOutput, kContextTopLineLimit, 1200U));
      }
    }
  } else if (bundle.taskTypeLabel == "verify") {
    if (!bundle.primaryTarget.empty()) {
      if (const auto impactOutput = fetchUltraOutput(router,
                                                     projectRoot,
                                                     "impact_analysis",
                                                     {{"target", bundle.primaryTarget}},
                                                     "ai_impact",
                                                     "ai_impact for target " +
                                                         bundle.primaryTarget,
                                                     bundle.sources);
          impactOutput.has_value()) {
        appendSection(bundle.block,
                      "Impact Chain",
                      topLines(*impactOutput, kContextTopLineLimit, 1200U));
      }
    }
    if (const auto verifyOutput = fetchUltraOutput(router,
                                                   projectRoot,
                                                   "verify_index",
                                                   {},
                                                   "ai_verify",
                                                   "ai_verify",
                                                   bundle.sources);
        verifyOutput.has_value()) {
      appendSection(bundle.block,
                    "Verification Snapshot",
                    topLines(*verifyOutput, kContextTopLineLimit, 900U));
    }
  }

  bundle.block = truncateForContext(bundle.block, kContextBlockLimitChars);
  return bundle;
}

std::string joinedSources(const std::vector<std::string>& sources) {
  if (sources.empty()) {
    return "none";
  }
  std::ostringstream stream;
  for (std::size_t index = 0U; index < sources.size(); ++index) {
    if (index != 0U) {
      stream << ",";
    }
    stream << sources[index];
  }
  return stream.str();
}

}  // namespace

MultiModelOrchestrator::MultiModelOrchestrator(
    std::filesystem::path projectRoot,
    std::shared_ptr<model::ModelAdapterRegistry> registry)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()),
      configPath_(projectRoot_ / ".ultra" / "model_routing.json"),
      registry_(registry != nullptr ? std::move(registry)
                                    : model::ModelAdapterRegistry::createDefault(
                                          projectRoot_)),
      routingConfig_(defaultRoutingConfiguration()) {
  std::string ignored;
  reloadConfiguration(ignored);
}

model::ModelResponse MultiModelOrchestrator::generate(
    const model::ModelRequest& request,
    const OrchestrationContext& context) {
  lastDecision_ = OrchestrationDecision{};
  OrchestrationContext effectiveContext = context;
  const std::string requestRole = requestResolvedRole(request);
  if (!requestRole.empty()) {
    effectiveContext.modelRoleHint = requestRole;
  }
  const std::string routedRole = roleForTaskType(effectiveContext);
  const bool tracePlannerRouting =
      routedRole == "planner" ||
      canonicalRoleName(effectiveContext.modelRoleHint) == "planner" ||
      effectiveContext.taskType == TaskType::Planning;
  lastDecision_.routingKey =
      routedRole.empty() ? toString(effectiveContext.taskType) : routedRole;
  lastDecision_.attemptedProviders = buildCandidateProviders(effectiveContext);
  if (tracePlannerRouting) {
    std::ostringstream attemptedProviders;
    for (std::size_t index = 0U; index < lastDecision_.attemptedProviders.size();
         ++index) {
      if (index != 0U) {
        attemptedProviders << ", ";
      }
      attemptedProviders << lastDecision_.attemptedProviders[index];
    }
    std::cout << "[ROLE HINT] " << effectiveContext.modelRoleHint << std::endl;
    std::cout << "[ROUTED ROLE] " << routedRole << std::endl;
    std::cout << "[ATTEMPTED PROVIDERS] " << attemptedProviders.str()
              << std::endl;
  }

  UltraContextBundle ultraContext =
      buildUltraContext(request, effectiveContext, projectRoot_, routedRole);
  model::ModelRequest enrichedRequest = request;
  if (!enrichedRequest.contextPayload.is_object()) {
    enrichedRequest.contextPayload = nlohmann::ordered_json::object();
  }
  if (!routedRole.empty()) {
    enrichedRequest.contextPayload["model_role"] = routedRole;
  }
  if (!ultraContext.taskId.empty()) {
    enrichedRequest.contextPayload["task_id"] = ultraContext.taskId;
  }
  if (!ultraContext.primaryTarget.empty()) {
    enrichedRequest.contextPayload["target"] = ultraContext.primaryTarget;
  }
  if (!ultraContext.sourceFile.empty()) {
    enrichedRequest.contextPayload["source_file"] = ultraContext.sourceFile;
  }
  enrichedRequest.contextPayload["ultra_context_meta"] = {
      {"size_bytes", ultraContext.block.size()},
      {"sources", ultraContext.sources},
      {"task_type", ultraContext.taskTypeLabel},
  };
  if (!ultraContext.block.empty() &&
      enrichedRequest.prompt.rfind("[ULTRA_CONTEXT]\n", 0U) != 0U) {
    enrichedRequest.prompt = "[ULTRA_CONTEXT]\n" + ultraContext.block +
                             "\n\n[USER_PROMPT]\n" + enrichedRequest.prompt;
  }

  std::cout << "[TASK] id="
            << (ultraContext.taskId.empty() ? "unknown" : ultraContext.taskId)
            << " type=" << ultraContext.taskTypeLabel << std::endl;
  std::cout << "[CONTEXT] type=" << ultraContext.taskTypeLabel
            << " size=" << ultraContext.block.size()
            << " sources=" << joinedSources(ultraContext.sources) << std::endl;

  if (!registry_) {
    model::ModelResponse response;
    response.ok = false;
    response.errorCode = model::ModelErrorCode::ProviderUnavailable;
    response.errorMessage = "Model adapter registry is unavailable.";
    return response;
  }

  std::vector<FailureRecord> failures;
  failures.reserve(lastDecision_.attemptedProviders.size());

  for (std::size_t index = 0U; index < lastDecision_.attemptedProviders.size();
       ++index) {
    const std::string& provider = lastDecision_.attemptedProviders[index];
    std::string error;
    std::unique_ptr<model::IModelAdapter> adapter =
        registry_->create(provider, error);
    if (!adapter) {
      failures.push_back(FailureRecord{
          provider,
          model::ModelErrorCode::ProviderUnavailable,
          error.empty() ? "Provider is unavailable." : error,
          0U,
      });
      continue;
    }

    model::ModelRequest routedRequest = enrichedRequest;
    if (!routedRole.empty()) {
      if (!routedRequest.contextPayload.is_object()) {
        routedRequest.contextPayload = nlohmann::ordered_json::object();
      }
      routedRequest.contextPayload["model_role"] = routedRole;
    }
    const nlohmann::ordered_json providerConfig =
        registry_->providerConfiguration(provider);
    const std::string selectedModel = model::configuredModelNameForRequest(
        providerConfig, routedRequest, adapter->modelInfo().modelName);
    if (tracePlannerRouting) {
      std::cout << "[SELECTED PROVIDER] " << provider << std::endl;
      std::cout << "[SELECTED MODEL] " << selectedModel << std::endl;
    }
    std::cout << "[LLM] invoking "
              << (routedRole.empty() ? toString(effectiveContext.taskType)
                                     : routedRole)
              << std::endl;
    model::ModelResponse response = adapter->generate(routedRequest);
    adapter->shutdown();
    if (response.ok) {
      lastDecision_.selectedProvider = provider;
      lastDecision_.fallbackUsed = index > 0U;
      return response;
    }

    const model::ModelErrorCode code =
        response.errorCode == model::ModelErrorCode::None
            ? model::ModelErrorCode::InvalidResponse
            : response.errorCode;
    failures.push_back(FailureRecord{
        provider,
        code,
        response.errorMessage.empty()
            ? "Provider returned an unsuccessful response."
            : response.errorMessage,
        response.latencyMs,
    });

    if (!isRetryableFailure(code)) {
      break;
    }
  }

  model::ModelResponse response;
  response.ok = false;
  response.errorCode = collapseFailureCode(failures);
  response.errorMessage = buildFailureMessage(failures);
  response.finishReason = "fallback_exhausted";
  for (const FailureRecord& failure : failures) {
    response.latencyMs += failure.latencyMs;
  }
  lastDecision_.fallbackUsed = failures.size() > 1U;
  return response;
}

bool MultiModelOrchestrator::reloadConfiguration(std::string& error) {
  routingConfig_ = defaultRoutingConfiguration();

  if (!std::filesystem::exists(configPath_)) {
    error.clear();
    return true;
  }

  if (!std::filesystem::is_regular_file(configPath_)) {
    error = "Model routing configuration exists but is not a file: " +
            configPath_.generic_string();
    return false;
  }

  std::ifstream input(configPath_);
  if (!input) {
    error = "Failed to open model routing configuration: " +
            configPath_.generic_string();
    return false;
  }

  std::stringstream buffer;
  buffer << input.rdbuf();

  nlohmann::ordered_json parsed;
  try {
    parsed = nlohmann::ordered_json::parse(buffer.str());
  } catch (const nlohmann::json::exception& ex) {
    error = "Failed to parse model routing configuration: " +
            std::string(ex.what());
    return false;
  }

  if (!parsed.is_object()) {
    error = "Model routing configuration root must be an object.";
    return false;
  }

  if (!parsed.contains("routing") || !parsed.at("routing").is_object()) {
    error = "Model routing configuration must contain object field 'routing'.";
    return false;
  }

  nlohmann::ordered_json merged = defaultRoutingConfiguration();
  for (auto it = parsed.at("routing").begin(); it != parsed.at("routing").end();
       ++it) {
    if (!it.value().is_string()) {
      error = "Routing target for key '" + it.key() + "' must be a string.";
      return false;
    }
    merged["routing"][lowerAscii(it.key())] =
        normalizeProviderName(it.value().get<std::string>());
  }

  routingConfig_ = sortJsonKeys(merged);
  error.clear();
  return true;
}

const std::filesystem::path& MultiModelOrchestrator::configPath() const noexcept {
  return configPath_;
}

nlohmann::ordered_json MultiModelOrchestrator::routingConfiguration() const {
  return routingConfig_;
}

const OrchestrationDecision& MultiModelOrchestrator::lastDecision() const noexcept {
  return lastDecision_;
}

std::shared_ptr<MultiModelOrchestrator> MultiModelOrchestrator::createDefault(
    const std::filesystem::path& projectRoot) {
  return std::make_shared<MultiModelOrchestrator>(projectRoot);
}

std::vector<std::string> MultiModelOrchestrator::availableProviders(
    const OrchestrationContext& context) const {
  if (!registry_) {
    return {};
  }

  std::vector<std::string> providers;
  const std::vector<std::string> allowed = normalizedAvailableModels(context);
  if (!allowed.empty()) {
    for (const std::string& provider : allowed) {
      if (registry_->hasProvider(provider)) {
        providers.push_back(provider);
      }
    }
    return providers;
  }

  providers = registry_->listProviders();
  for (std::string& provider : providers) {
    provider = normalizeProviderName(provider);
  }
  std::sort(providers.begin(), providers.end());
  providers.erase(std::unique(providers.begin(), providers.end()), providers.end());
  return providers;
}

std::vector<std::string> MultiModelOrchestrator::buildCandidateProviders(
    const OrchestrationContext& context) const {
  const std::vector<std::string> available = availableProviders(context);
  if (available.empty()) {
    return {};
  }

  std::vector<std::string> candidates;
  const auto appendRoleProviders = [&](const std::string& role) {
    if (!registry_ || role.empty()) {
      return;
    }
    for (const std::string& provider : registry_->getRoleProviders(role)) {
      if (containsValue(available, provider)) {
        pushUnique(candidates, provider);
      }
    }
  };
  appendRoleProviders(roleForTaskType(context));

  const std::string preferred = preferredProviderFor(context, available);
  pushUnique(candidates, preferred);

  for (const std::string& provider : available) {
    if (isLocalProvider(provider)) {
      pushUnique(candidates, provider);
    }
  }

  pushUnique(candidates, defaultProviderFor(available));
  for (const std::string& provider : available) {
    pushUnique(candidates, provider);
  }
  return candidates;
}

std::string MultiModelOrchestrator::preferredProviderFor(
    const OrchestrationContext& context,
    const std::vector<std::string>& availableProviders) const {
  const nlohmann::ordered_json routing =
      routingConfig_.value("routing", nlohmann::ordered_json::object());
  const std::string taskKey = toString(context.taskType);
  if (routing.contains(taskKey) && routing.at(taskKey).is_string()) {
    const std::string preferred =
        normalizeProviderName(routing.at(taskKey).get<std::string>());
    if (containsValue(availableProviders, preferred)) {
      return preferred;
    }
  }

  if (context.complexity == TaskComplexity::High && routing.contains("reasoning") &&
      routing.at("reasoning").is_string()) {
    const std::string reasoningProvider =
        normalizeProviderName(routing.at("reasoning").get<std::string>());
    if (containsValue(availableProviders, reasoningProvider)) {
      return reasoningProvider;
    }
  }

  if (context.latencyBudgetMs > 0U && context.latencyBudgetMs <= 250U) {
    for (const std::string& provider : availableProviders) {
      if (isLocalProvider(provider)) {
        return provider;
      }
    }
  }

  return {};
}

std::string MultiModelOrchestrator::defaultProviderFor(
    const std::vector<std::string>& availableProviders) const {
  if (availableProviders.empty()) {
    return {};
  }

  const nlohmann::ordered_json routing =
      routingConfig_.value("routing", nlohmann::ordered_json::object());
  if (routing.contains("analysis") && routing.at("analysis").is_string()) {
    const std::string analysisProvider =
        normalizeProviderName(routing.at("analysis").get<std::string>());
    if (containsValue(availableProviders, analysisProvider)) {
      return analysisProvider;
    }
  }

  return availableProviders.front();
}

bool MultiModelOrchestrator::isLocalProvider(
    const std::string& providerName) const {
  if (!registry_ || !registry_->hasProvider(providerName)) {
    return false;
  }

  std::string error;
  std::unique_ptr<model::IModelAdapter> adapter =
      registry_->create(providerName, error);
  if (!adapter) {
    return false;
  }

  const bool localProvider = adapter->modelInfo().localProvider;
  adapter->shutdown();
  return localProvider;
}

std::string MultiModelOrchestrator::normalizeProviderName(std::string value) {
  return lowerAscii(std::move(value));
}

std::string MultiModelOrchestrator::normalizeRoleName(std::string value) {
  return canonicalRoleName(std::move(value));
}

}  // namespace ultra::ai::orchestration
