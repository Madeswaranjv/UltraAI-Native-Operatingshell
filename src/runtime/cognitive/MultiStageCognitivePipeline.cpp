#include "MultiStageCognitivePipeline.h"

#include "ExecutionKernel.h"
#include "contract_enforcement.h"

#include "../../ai/model/IModelAdapter.h"
#include "../../ai/orchestration/MultiModelOrchestrator.h"
#include "../../core/state_manager.h"

#include <external/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ultra::runtime::cognitive {

namespace {

using ordered_json = nlohmann::ordered_json;

struct PlanStep {
  std::string tool;
  std::map<std::string, std::string> args;
  std::string purpose;
};

struct JsonStageResult {
  ai::model::ModelRequest request;
  ai::model::ModelResponse response;
  ordered_json parsed = ordered_json::object();
  ordered_json trace = ordered_json::object();
  std::string provider;
};

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string trimAscii(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1U);
}

std::string formatUtcTimestamp(
    const std::chrono::system_clock::time_point timestamp) {
  const std::time_t rawTime = std::chrono::system_clock::to_time_t(timestamp);
  std::tm utcTime{};
#if defined(_WIN32)
  gmtime_s(&utcTime, &rawTime);
#else
  gmtime_r(&rawTime, &utcTime);
#endif
  std::ostringstream stream;
  stream << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
  return stream.str();
}

std::string truncateForPrompt(std::string value, const std::size_t limit) {
  if (value.size() <= limit) {
    return value;
  }
  constexpr std::string_view kNotice = "\n...[truncated]";
  const std::size_t keep =
      limit > kNotice.size() ? limit - kNotice.size() : 0U;
  value.resize(keep);
  value += kNotice;
  return value;
}

std::string jsonArgumentValue(const ordered_json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<unsigned long long>());
  }
  if (value.is_number_float()) {
    return value.dump();
  }
  return value.dump();
}

ordered_json buildIntentJson(const intent::Intent& intentValue) {
  ordered_json payload = ordered_json::object();
  payload["goal"] = {{"type", intent::toString(intentValue.goal.type)},
                     {"target", intentValue.goal.target}};
  payload["constraints"] = {
      {"branch_scope", intentValue.constraints.branchScope},
      {"determinism_required", intentValue.constraints.determinismRequired},
      {"max_files_changed", intentValue.constraints.maxFilesChanged},
      {"max_impact_depth", intentValue.constraints.maxImpactDepth},
      {"token_budget", intentValue.constraints.tokenBudget},
  };
  payload["risk_tolerance"] = intent::toString(intentValue.risk);
  payload["options"] = {
      {"allow_cross_module_move", intentValue.options.allowCrossModuleMove},
      {"allow_public_api_change", intentValue.options.allowPublicAPIChange},
      {"allow_rename", intentValue.options.allowRename},
      {"allow_signature_change", intentValue.options.allowSignatureChange},
  };
  return payload;
}

ordered_json stringArrayJson(const std::vector<std::string>& values) {
  ordered_json payload = ordered_json::array();
  for (const std::string& value : values) {
    payload.push_back(value);
  }
  return payload;
}

std::string defaultTarget(const MultiStagePipelineRequest& request) {
  if (!request.resolvedIntent.goal.target.empty()) {
    return request.resolvedIntent.goal.target;
  }
  if (!request.targets.empty()) {
    return request.targets.front();
  }
  return "workspace_root";
}

ai::orchestration::TaskComplexity complexityForRequest(
    const MultiStagePipelineRequest& request) {
  if (request.policy.maxImpactDepth >= 4 ||
      request.policy.maxFilesChanged >= 6 ||
      request.resolvedIntent.options.allowPublicAPIChange) {
    return ai::orchestration::TaskComplexity::High;
  }
  if (lowerAscii(request.requestedRole) == "analyzer" &&
      !request.requiresPlanning) {
    return ai::orchestration::TaskComplexity::Low;
  }
  return ai::orchestration::TaskComplexity::Medium;
}

ai::orchestration::TaskPriority priorityForRequest(
    const MultiStagePipelineRequest& request) {
  return request.resolvedIntent.options.allowPublicAPIChange
             ? ai::orchestration::TaskPriority::Urgent
             : ai::orchestration::TaskPriority::Standard;
}

ai::orchestration::OrchestrationContext buildStageContext(
    const MultiStagePipelineRequest& request,
    const std::string& role,
    const ai::orchestration::TaskType taskType,
    const std::size_t tokenBudget) {
  ai::orchestration::OrchestrationContext context;
  context.taskType = taskType;
  context.complexity = complexityForRequest(request);
  context.priority = priorityForRequest(request);
  context.tokenBudget = tokenBudget;
  context.modelRoleHint = role;
  return context;
}

std::vector<std::string> plannerToolNames() {
  return {"query_symbol",  "read_source",   "read_file",
          "list_dir",      "search_files",  "impact_analysis",
          "simulate_intent", "get_context", "get_status"};
}

std::string joinNames(const std::vector<std::string>& values) {
  std::ostringstream stream;
  for (std::size_t index = 0U; index < values.size(); ++index) {
    if (index != 0U) {
      stream << ", ";
    }
    stream << values[index];
  }
  return stream.str();
}

std::optional<ordered_json> parseJsonPayload(const std::string& text,
                                             std::string* failureDetail = nullptr) {
  if (failureDetail != nullptr) {
    failureDetail->clear();
  }
  const std::string trimmed = trimAscii(text);
  if (trimmed.empty()) {
    if (failureDetail != nullptr) {
      *failureDetail = "payload is empty after trimming.";
    }
    return std::nullopt;
  }
  try {
    ordered_json parsed = ordered_json::parse(trimmed);
    if (parsed.is_object()) {
      return parsed;
    }
    if (failureDetail != nullptr) {
      *failureDetail = "parsed payload is valid JSON but not an object.";
    }
  } catch (const nlohmann::json::exception& ex) {
    if (failureDetail != nullptr && failureDetail->empty()) {
      *failureDetail = ex.what();
    }
  } catch (...) {
    if (failureDetail != nullptr && failureDetail->empty()) {
      *failureDetail = "unknown JSON parse failure.";
    }
  }

  std::size_t candidateEnd = trimmed.size();
  while (candidateEnd > 0U) {
    std::size_t lineStart = trimmed.rfind('\n', candidateEnd - 1U);
    lineStart = lineStart == std::string::npos ? 0U : lineStart + 1U;
    while (lineStart < candidateEnd &&
           (trimmed[lineStart] == ' ' || trimmed[lineStart] == '\t' ||
            trimmed[lineStart] == '\r')) {
      ++lineStart;
    }
    if (lineStart < candidateEnd && trimmed[lineStart] == '{') {
      try {
        ordered_json parsed =
            ordered_json::parse(trimmed.substr(lineStart, candidateEnd - lineStart));
        if (parsed.is_object()) {
          return parsed;
        }
        if (failureDetail != nullptr && failureDetail->empty()) {
          *failureDetail = "candidate payload is valid JSON but not an object.";
        }
      } catch (const nlohmann::json::exception& ex) {
        if (failureDetail != nullptr && failureDetail->empty()) {
          *failureDetail = ex.what();
        }
      } catch (...) {
        if (failureDetail != nullptr && failureDetail->empty()) {
          *failureDetail = "unknown JSON candidate parse failure.";
        }
      }
    }
    if (lineStart == 0U) {
      break;
    }
    candidateEnd = lineStart - 1U;
  }
  if (failureDetail != nullptr && failureDetail->empty()) {
    *failureDetail = "no object-valued JSON candidate found in payload.";
  }
  return std::nullopt;
}

std::string providerForOrchestrator(
    const std::shared_ptr<ai::orchestration::IMultiModelOrchestrator>& orchestrator) {
  if (const auto* multi =
          dynamic_cast<const ai::orchestration::MultiModelOrchestrator*>(
              orchestrator.get());
      multi != nullptr) {
    return multi->lastDecision().selectedProvider;
  }
  return {};
}

JsonStageResult invokeJsonStage(
    const MultiStagePipelineRequest& pipelineRequest,
    const std::shared_ptr<ai::orchestration::IMultiModelOrchestrator>& orchestrator,
    const std::string& stageName,
    const std::string& role,
    const ai::orchestration::TaskType taskType,
    ai::model::ModelRequest request) {
  if (!orchestrator) {
    throw std::runtime_error("Model orchestrator is unavailable.");
  }

  if (!request.contextPayload.is_object()) {
    request.contextPayload = ordered_json::object();
  }
  request.contextPayload["model_role"] = role;
  request.contextPayload["requested_role"] = pipelineRequest.requestedRole;
  request.contextPayload["stage"] = stageName;
  request.contextPayload["target"] = defaultTarget(pipelineRequest);
  request.contextPayload["task_id"] = "pipeline." + stageName;

  const ai::orchestration::OrchestrationContext context =
      buildStageContext(pipelineRequest, role, taskType, request.maxTokens);
  if (stageName == "planner") {
    std::ofstream file("C:\\Temp\\ultra_planner_prompt.txt", std::ios::app);
    file << "\n=============================\n";
    file << "[PLANNER PROMPT]\n";
    file << request.prompt << "\n";
    file.close();
    std::cout << "[PLANNER SYSTEM PROMPT]\n"
              << request.systemPrompt << std::endl;
    std::cout << "[PLANNER INPUT PROMPT]\n"
              << request.prompt << std::endl;
    std::cout << "[PLANNER ROLE]\n"
              << context.modelRoleHint << std::endl;
  }
  const ai::model::ModelResponse response = orchestrator->generate(request, context);

  JsonStageResult result;
  result.request = std::move(request);
  result.response = response;
  result.provider = providerForOrchestrator(orchestrator);
  result.trace["stage"] = stageName;
  result.trace["role"] = role;
  result.trace["orchestration_context"] = ai::orchestration::toJson(context);
  result.trace["request"] = ai::model::toJson(result.request);
  result.trace["response"] = ai::model::toJson(response);
  if (!result.provider.empty()) {
    result.trace["selected_provider"] = result.provider;
  }

  if (!response.ok) {
    throw std::runtime_error(stageName + " stage failed: " +
                             (response.errorMessage.empty()
                                  ? std::string("model response was unsuccessful.")
                                  : response.errorMessage));
  }
  if (!response.toolCalls.empty()) {
    throw std::runtime_error(stageName +
                             " stage must return JSON text only, not tool calls.");
  }

  std::string parseFailureDetail;
  if (stageName == "planner") {
    const std::string raw = response.textOutput;
    std::ofstream file("C:\\Temp\\ultra_planner_raw.txt", std::ios::app);
    file << "\n=============================\n";
    file << "[PLANNER RAW OUTPUT]\n";
    file << raw << "\n";
    file.close();
    std::cout << "[PLANNER RAW OUTPUT]\n"
              << raw << std::endl;
    std::cout << "[PLANNER PARSE INPUT]\n"
              << raw << std::endl;
  }
  const std::optional<ordered_json> parsed =
      parseJsonPayload(response.textOutput, &parseFailureDetail);
  if (!parsed.has_value()) {
    if (stageName == "planner") {
      std::cout << "[PLANNER JSON PARSE ERROR]\n";
      if (!parseFailureDetail.empty()) {
        std::cout << parseFailureDetail << std::endl;
      }
    }
    throw std::runtime_error(stageName +
                             " stage did not return a valid JSON object.");
  }
  result.parsed = *parsed;
  result.trace["parsed"] = result.parsed;
  if (stageName == "planner") {
    std::cout << "[PLANNER PARSED JSON]\n"
              << result.parsed.dump(2) << std::endl;
  }
  return result;
}

std::optional<std::vector<PlanStep>> parsePlanSteps(const ordered_json& plan,
                                                    std::string& error) {
  std::vector<PlanStep> steps;
  if (!plan.contains("steps")) {
    error.clear();
    return steps;
  }
  if (!plan.at("steps").is_array()) {
    error = "Planner output field 'steps' must be an array.";
    return std::nullopt;
  }

  for (const auto& item : plan.at("steps")) {
    if (item.is_string()) {
      PlanStep step;
      step.tool = item.get<std::string>();
      steps.push_back(std::move(step));
      continue;
    }
    if (!item.is_object()) {
      error = "Planner step entries must be objects or strings.";
      return std::nullopt;
    }

    PlanStep step;
    step.tool = item.value("tool", std::string{});
    step.purpose = item.value("purpose", item.value("reason", std::string{}));
    const ordered_json args =
        item.contains("args") && item.at("args").is_object()
            ? item.at("args")
            : ordered_json::object();
    for (auto argIt = args.begin(); argIt != args.end(); ++argIt) {
      step.args[argIt.key()] = jsonArgumentValue(argIt.value());
    }
    if (step.tool.empty()) {
      error = "Planner step objects require a non-empty 'tool' field.";
      return std::nullopt;
    }
    steps.push_back(std::move(step));
  }

  error.clear();
  return steps;
}

bool allowedPlannerTool(const std::string& tool) {
  static const std::set<std::string> kAllowed = []() {
    const std::vector<std::string> names = plannerToolNames();
    return std::set<std::string>(names.begin(), names.end());
  }();
  return kAllowed.find(tool) != kAllowed.end();
}

ai::model::ModelRequest buildPlannerRequest(
    const MultiStagePipelineRequest& request) {
  ai::model::ModelRequest modelRequest;
  modelRequest.systemPrompt =
      "You are Ultra's planner. Planning only. Return valid JSON only. "
      "Never write code, diffs, patches, or verification. Output schema: "
      "{\"intent\":\"...\",\"steps\":[{\"tool\":\"...\",\"args\":{...},"
      "\"purpose\":\"...\"}],\"requires_code\":true}. Use only these tools: " +
      joinNames(plannerToolNames()) + ".";

  std::ostringstream prompt;
  prompt << "User prompt: " << request.rawPrompt << "\n";
  prompt << "Action: " << request.actionLabel << "\n";
  prompt << "Requested role: " << request.requestedRole << "\n";
  prompt << "Targets: " << stringArrayJson(request.targets).dump() << "\n";
  prompt << "Constraints: " << stringArrayJson(request.constraints).dump() << "\n";
  prompt << "Resolved intent: " << buildIntentJson(request.resolvedIntent).dump()
         << "\n";
  prompt << "If the user is asking whether a change is safe or might break "
            "something, prefer simulate_intent or impact_analysis.\n";
  prompt << "If the user only wants an explanation, use read-only discovery "
            "tools and set requires_code to false.\n";
  prompt << "Return JSON only.";
  modelRequest.prompt = prompt.str();
  modelRequest.temperature = 0.0;
  modelRequest.maxTokens =
      std::clamp<std::size_t>(request.policy.maxTokenBudget / 3U, 512U, 1536U);
  modelRequest.contextPayload = {
      {"action", request.actionLabel},
      {"intent", buildIntentJson(request.resolvedIntent)},
      {"model_role", "planner"},
      {"requested_role", request.requestedRole},
      {"stage", "planner"},
      {"targets", request.targets},
  };
  return modelRequest;
}

ai::model::ModelRequest buildAnalyzerRequest(
    const MultiStagePipelineRequest& request,
    const ordered_json& plan,
    const ordered_json& toolResults) {
  ai::model::ModelRequest modelRequest;
  modelRequest.systemPrompt =
      "You are Ultra's analyzer. Reasoning only. Return valid JSON only. "
      "Never produce code, patches, or tool calls. Output schema: "
      "{\"summary\":\"...\",\"reasoning\":\"...\",\"risk\":\"low|medium|high\","
      "\"file_targets\":[...],\"recommended_actions\":[...]}";

  std::ostringstream prompt;
  prompt << "User prompt: " << request.rawPrompt << "\n";
  prompt << "Resolved intent: " << buildIntentJson(request.resolvedIntent).dump()
         << "\n";
  prompt << "Planner output: " << truncateForPrompt(plan.dump(), 4000U) << "\n";
  prompt << "Tool results: " << truncateForPrompt(toolResults.dump(), 6000U)
         << "\n";
  prompt << "Return JSON only.";
  modelRequest.prompt = prompt.str();
  modelRequest.temperature = 0.0;
  modelRequest.maxTokens =
      std::clamp<std::size_t>(request.policy.maxTokenBudget / 3U, 512U, 1536U);
  modelRequest.contextPayload = {
      {"intent", buildIntentJson(request.resolvedIntent)},
      {"model_role", "analyzer"},
      {"planner_output", plan},
      {"requested_role", request.requestedRole},
      {"stage", "analyzer"},
      {"tool_results", toolResults},
  };
  return modelRequest;
}

ai::model::ModelRequest buildCoderRequest(
    const MultiStagePipelineRequest& request,
    const ordered_json& plan,
    const ordered_json& analyzerOutput) {
  ai::model::ModelRequest modelRequest;
  modelRequest.systemPrompt =
      "You are Ultra's coder. Code generation only. Return only apply_patch "
      "tool calls or a single JSON object like "
      "{\"tool\":\"apply_patch\",\"changes\":\"<unified diff>\"}. No prose.";

  std::ostringstream prompt;
  prompt << "User prompt: " << request.rawPrompt << "\n";
  prompt << "Resolved intent: " << buildIntentJson(request.resolvedIntent).dump()
         << "\n";
  prompt << "Planner output: " << truncateForPrompt(plan.dump(), 4000U) << "\n";
  prompt << "Analyzer output: " << truncateForPrompt(analyzerOutput.dump(), 4000U)
         << "\n";
  prompt << "Target: " << defaultTarget(request) << "\n";
  prompt << "Use apply_patch only. No explanations.";
  modelRequest.prompt = prompt.str();
  modelRequest.temperature = 0.0;
  modelRequest.maxTokens =
      std::clamp<std::size_t>(request.policy.maxTokenBudget / 2U, 768U, 2048U);
  modelRequest.toolsAvailable = {"apply_patch"};
  modelRequest.contextPayload = {
      {"analyzer_output", analyzerOutput},
      {"intent", buildIntentJson(request.resolvedIntent)},
      {"model_role", "coder"},
      {"planner_output", plan},
      {"requested_role", request.requestedRole},
      {"stage", "coder"},
      {"target", defaultTarget(request)},
      {"tool_required", "apply_patch"},
  };
  return modelRequest;
}

ai::model::ModelRequest buildVerifierRequest(
    const MultiStagePipelineRequest& request,
    const ordered_json& analyzerOutput,
    const runtime::Result& coderResult) {
  ai::model::ModelRequest modelRequest;
  modelRequest.systemPrompt =
      "You are Ultra's verifier. Validation only. Return valid JSON only. "
      "Never produce code, patches, or tool calls. Output schema: "
      "{\"verdict\":\"pass|fail\",\"summary\":\"...\",\"risks\":[...],"
      "\"should_commit\":true,\"confidence\":\"low|medium|high\","
      "\"commit_message\":\"...\"}";

  std::ostringstream prompt;
  prompt << "User prompt: " << request.rawPrompt << "\n";
  prompt << "Resolved intent: " << buildIntentJson(request.resolvedIntent).dump()
         << "\n";
  prompt << "Analyzer output: " << truncateForPrompt(analyzerOutput.dump(), 4000U)
         << "\n";
  prompt << "Coder execution payload: "
         << truncateForPrompt(coderResult.payload.dump(), 6000U) << "\n";
  prompt << "Coder message: " << coderResult.message << "\n";
  prompt << "Return JSON only.";
  modelRequest.prompt = prompt.str();
  modelRequest.temperature = 0.0;
  modelRequest.maxTokens = 1024U;
  modelRequest.contextPayload = {
      {"analyzer_output", analyzerOutput},
      {"coder_message", coderResult.message},
      {"coder_payload", coderResult.payload},
      {"intent", buildIntentJson(request.resolvedIntent)},
      {"model_role", "verifier"},
      {"requested_role", request.requestedRole},
      {"stage", "verifier"},
  };
  return modelRequest;
}

runtime::Action buildToolAction(const MultiStagePipelineRequest& request,
                                const PlanStep& step,
                                const CognitiveState& state,
                                const std::size_t index) {
  runtime::Action action;
  action.id = "pipeline.tool." + std::to_string(index + 1U);
  action.type = runtime::ActionType::ToolExecution;
  action.target = defaultTarget(request);
  action.branch = state.snapshot.branch.toString();
  action.snapshotVersion = state.snapshot.version;
  action.toolName = lowerAscii(step.tool);
  action.toolArgs = step.args;
  return action;
}

runtime::Action buildCoderAction(const MultiStagePipelineRequest& request,
                                 const ordered_json& plan,
                                 const ordered_json& analyzerOutput,
                                 const CognitiveState& state) {
  runtime::Action action;
  action.id = "pipeline.coder";
  action.type = runtime::ActionType::ModelGenerate;
  action.target = defaultTarget(request);
  action.branch = state.snapshot.branch.toString();
  action.snapshotVersion = state.snapshot.version;
  action.modelRequest = buildCoderRequest(request, plan, analyzerOutput);
  action.orchestrationContext = buildStageContext(
      request,
      "coder",
      ai::orchestration::TaskType::Coding,
      action.modelRequest->maxTokens);
  return action;
}

runtime::Action buildCommitAction(const std::filesystem::path& projectRoot,
                                  const std::string& target,
                                  const std::string& branch,
                                  const std::uint64_t snapshotVersion,
                                  const std::string& commitMessage) {
  runtime::Action action;
  action.id = "pipeline.commit";
  action.type = runtime::ActionType::ToolExecution;
  action.target = target;
  action.branch = branch;
  action.snapshotVersion = snapshotVersion;
  action.toolName = "run_command";
  action.toolArgs["cwd"] = projectRoot.string();
  action.toolArgs["command"] =
      "git add -A && git commit -m \"" + commitMessage + "\"";
  return action;
}

std::string sanitizedCommitMessage(std::string value) {
  std::replace(value.begin(), value.end(), '\r', ' ');
  std::replace(value.begin(), value.end(), '\n', ' ');
  std::replace(value.begin(), value.end(), '"', '\'');
  value = trimAscii(std::move(value));
  return value.empty() ? std::string("Ultra verifier approved staged change.")
                       : value;
}

std::string summaryFromAnalyzer(const ordered_json& analyzerOutput) {
  if (analyzerOutput.contains("summary") &&
      analyzerOutput.at("summary").is_string()) {
    return analyzerOutput.at("summary").get<std::string>();
  }
  if (analyzerOutput.contains("reasoning") &&
      analyzerOutput.at("reasoning").is_string()) {
    return analyzerOutput.at("reasoning").get<std::string>();
  }
  return analyzerOutput.dump(2);
}

std::string summaryFromVerifier(const ordered_json& verifierOutput) {
  if (verifierOutput.contains("summary") &&
      verifierOutput.at("summary").is_string()) {
    return verifierOutput.at("summary").get<std::string>();
  }
  if (verifierOutput.contains("verdict") &&
      verifierOutput.at("verdict").is_string()) {
    return verifierOutput.at("verdict").get<std::string>();
  }
  return verifierOutput.dump(2);
}

std::string defaultRoleForAction(std::string action) {
  action = lowerAscii(trimAscii(std::move(action)));
  if (action == "add" || action == "fix" || action == "refactor" ||
      action == "remove" || action == "test") {
    return "coder";
  }
  if (action == "analyse" || action == "analyze" || action == "explain") {
    return "analyzer";
  }
  if (action == "optimise" || action == "optimize") {
    return "planner";
  }
  return "planner";
}

runtime::Result executeAuthorizedAction(runtime::ExecutionKernel& executionKernel,
                                        const runtime::Action& action,
                                        const CognitiveState& state,
                                        const contracts::LoopPhase phase) {
  const contracts::ScopedLoopPhase scopedPhase(phase);
  const contracts::ScopedTaskGraphAuthorization authorization(action.id);
  return executionKernel.execute(action, state);
}

}  // namespace

MultiStageCognitivePipeline::MultiStageCognitivePipeline(
    core::StateManager& stateManager,
    std::shared_ptr<ai::orchestration::IMultiModelOrchestrator> orchestrator)
    : stateManager_(stateManager),
      orchestrator_(orchestrator != nullptr
                        ? std::move(orchestrator)
                        : ai::orchestration::MultiModelOrchestrator::createDefault(
                              stateManager.projectRoot())) {}

CognitiveLoopResult MultiStageCognitivePipeline::run(
    const MultiStagePipelineRequest& request) {
  CognitiveLoopResult result;
  const auto executionStartWall = std::chrono::system_clock::now();
  const auto executionStartMonotonic = std::chrono::steady_clock::now();
  const auto finalizeResult = [&result,
                               executionStartWall,
                               executionStartMonotonic]() {
    result.executionStartTime = formatUtcTimestamp(executionStartWall);
    result.executionEndTime =
        formatUtcTimestamp(std::chrono::system_clock::now());
    result.executionDurationSeconds = std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() -
                                        executionStartMonotonic)
                                        .count();
  };

  try {
    const std::size_t fallbackBudget =
        request.resolvedIntent.constraints.tokenBudget == 0U
            ? 4096U
            : request.resolvedIntent.constraints.tokenBudget;
    MultiStagePipelineRequest normalizedRequest = request;
    normalizedRequest.actionLabel =
        lowerAscii(trimAscii(normalizedRequest.actionLabel));
    normalizedRequest.requestedRole =
        lowerAscii(trimAscii(normalizedRequest.requestedRole));

    const intent::Intent normalizedIntent =
        intent::normalizeIntent(request.resolvedIntent, fallbackBudget);
    if (normalizedRequest.requestedRole.empty() ||
        normalizedRequest.requestedRole == "auto") {
      normalizedRequest.requestedRole =
          defaultRoleForAction(normalizedRequest.actionLabel);
    }
    normalizedRequest.resolvedIntent = normalizedIntent;
    normalizedRequest.policy = governance::normalizePolicy(
        request.policy, static_cast<int>(normalizedIntent.constraints.tokenBudget));
    if (normalizedRequest.targets.empty() &&
        !normalizedIntent.goal.target.empty()) {
      normalizedRequest.targets.push_back(normalizedIntent.goal.target);
    }

    const CognitiveState state =
        stateManager_.createCognitiveState(normalizedIntent.constraints.tokenBudget);
    runtime::ExecutionKernel executionKernel(stateManager_, orchestrator_);

    ordered_json pipelinePayload = ordered_json::object();
    pipelinePayload["mode"] = "multi_stage";
    pipelinePayload["requested_role"] = normalizedRequest.requestedRole;
    pipelinePayload["resolved_intent"] = buildIntentJson(normalizedIntent);
    pipelinePayload["stages"] = ordered_json::array();

    const JsonStageResult planner = invokeJsonStage(
        normalizedRequest,
        orchestrator_,
        "planner",
        "planner",
        ai::orchestration::TaskType::Planning,
        buildPlannerRequest(normalizedRequest));
    pipelinePayload["planner"] = planner.trace;
    pipelinePayload["stages"].push_back(planner.trace);

    std::string stepParseError;
    const std::optional<std::vector<PlanStep>> parsedSteps =
        parsePlanSteps(planner.parsed, stepParseError);
    if (!parsedSteps.has_value()) {
      throw std::runtime_error(stepParseError);
    }

    ordered_json executedTools = ordered_json::array();
    bool anyToolExecution = false;
    for (std::size_t index = 0U; index < parsedSteps->size(); ++index) {
      const PlanStep& step = parsedSteps->at(index);
      if (!allowedPlannerTool(lowerAscii(step.tool))) {
        throw std::runtime_error("Planner requested unsupported tool '" + step.tool +
                                 "' in the planning stage.");
      }

      const runtime::Action toolAction =
          buildToolAction(normalizedRequest, step, state, index);
      const runtime::Result toolResult = executeAuthorizedAction(
          executionKernel, toolAction, state, contracts::LoopPhase::PLAN);
      ordered_json toolTrace = {
          {"stage", "tool_execution"},
          {"step_index", index},
          {"tool", toolAction.toolName},
          {"args", step.args},
          {"purpose", step.purpose},
          {"message", toolResult.message},
          {"ok", toolResult.ok},
          {"payload", toolResult.payload},
      };
      executedTools.push_back(toolTrace);
      pipelinePayload["stages"].push_back(toolTrace);
      anyToolExecution = true;
      if (!toolResult.ok) {
        pipelinePayload["tool_execution"] = executedTools;
        throw std::runtime_error("Planner tool step failed: " +
                                 (toolResult.message.empty()
                                      ? toolAction.toolName
                                      : toolResult.message));
      }
    }
    pipelinePayload["tool_execution"] = executedTools;

    const JsonStageResult analyzer = invokeJsonStage(
        normalizedRequest,
        orchestrator_,
        "analyzer",
        "analyzer",
        ai::orchestration::TaskType::Analysis,
        buildAnalyzerRequest(normalizedRequest, planner.parsed, executedTools));
    pipelinePayload["analyzer"] = analyzer.trace;
    pipelinePayload["stages"].push_back(analyzer.trace);

    const std::string normalizedAction = normalizedRequest.actionLabel;
    const bool requiresCode =
        planner.parsed.value("requires_code",
                             normalizedRequest.requestedRole == "coder" ||
                                 normalizedAction == "add" ||
                                 normalizedAction == "fix" ||
                                 normalizedAction == "refactor" ||
                                 normalizedAction == "remove" ||
                                 normalizedAction == "test");

    result.executionPayload = pipelinePayload;
    result.providerUsed = analyzer.provider;
    result.toolRouterExecuted = anyToolExecution;

    if (!requiresCode) {
      result.ok = true;
      result.verifyStatus = "PASS";
      result.confidence =
          analyzer.parsed.value("confidence", std::string{"medium"});
      result.executionSummary = summaryFromAnalyzer(analyzer.parsed);
      result.llm_output = result.executionSummary;
      result.output = result.executionSummary;
      result.outputSource = "llm_output";
      finalizeResult();
      return result;
    }

    const runtime::Action coderAction =
        buildCoderAction(normalizedRequest, planner.parsed, analyzer.parsed, state);
    const runtime::Result coderResult = executeAuthorizedAction(
        executionKernel, coderAction, state, contracts::LoopPhase::EXECUTE);
    ordered_json coderTrace = {
        {"stage", "coder"},
        {"message", coderResult.message},
        {"ok", coderResult.ok},
        {"payload", coderResult.payload},
        {"request", coderAction.modelRequest.has_value()
                        ? ai::model::toJson(*coderAction.modelRequest)
                        : ordered_json::object()},
        {"orchestration_context",
         coderAction.orchestrationContext.has_value()
             ? ai::orchestration::toJson(*coderAction.orchestrationContext)
             : ordered_json::object()},
    };
    if (!executionKernel.lastSelectedProvider().empty()) {
      coderTrace["selected_provider"] = executionKernel.lastSelectedProvider();
    }
    pipelinePayload["coder"] = coderTrace;
    pipelinePayload["stages"].push_back(coderTrace);
    result.toolCallDetected =
        coderResult.payload.value("tool_call_detected", false);
    result.toolRouterExecuted =
        result.toolRouterExecuted ||
        coderResult.payload.value("tool_router_executed", false);
    if (coderResult.payload.contains("tool_execution") &&
        coderResult.payload.at("tool_execution").is_object()) {
      result.toolExecution = coderResult.payload.at("tool_execution");
    }
    result.providerUsed = executionKernel.lastSelectedProvider().empty()
                              ? analyzer.provider
                              : executionKernel.lastSelectedProvider();
    result.providerEndpoint = executionKernel.lastProviderEndpoint();
    if (!coderResult.ok) {
      result.ok = false;
      result.verifyStatus = "FAIL";
      result.confidence = "low";
      result.executionSummary = coderResult.message.empty()
                                    ? "Coder stage failed."
                                    : coderResult.message;
      result.llm_output = result.executionSummary;
      result.output = result.executionSummary;
      result.outputSource = "tool_execution";
      result.executionPayload = pipelinePayload;
      result.errorMessage = result.executionSummary;
      finalizeResult();
      return result;
    }

    const JsonStageResult verifier = invokeJsonStage(
        normalizedRequest,
        orchestrator_,
        "verifier",
        "verifier",
        ai::orchestration::TaskType::Analysis,
        buildVerifierRequest(normalizedRequest, analyzer.parsed, coderResult));
    pipelinePayload["verifier"] = verifier.trace;
    pipelinePayload["stages"].push_back(verifier.trace);
    if (result.providerUsed.empty()) {
      result.providerUsed = verifier.provider;
    }

    const std::string verdict =
        lowerAscii(verifier.parsed.value("verdict", std::string{"fail"}));
    result.ok = verdict == "pass";
    result.verifyStatus = result.ok ? "PASS" : "FAIL";
    result.confidence =
        verifier.parsed.value("confidence", std::string{"medium"});
    result.executionSummary =
        verifier.parsed.value("summary",
                              coderResult.message.empty()
                                  ? std::string("Verifier completed.")
                                  : coderResult.message);
    result.llm_output = summaryFromVerifier(verifier.parsed);
    result.output = result.llm_output;
    result.outputSource = "llm_output";

    const bool shouldCommit =
        normalizedRequest.autoCommit &&
        verifier.parsed.value("should_commit", result.ok);
    if (shouldCommit) {
      const std::string commitMessage = sanitizedCommitMessage(
          verifier.parsed.value("commit_message", result.executionSummary));
      const runtime::Action commitAction = buildCommitAction(
          stateManager_.projectRoot(),
          defaultTarget(normalizedRequest),
          state.snapshot.branch.toString(),
          state.snapshot.version,
          commitMessage);
      const runtime::Result commitResult = executeAuthorizedAction(
          executionKernel,
          commitAction,
          state,
          contracts::LoopPhase::VERIFY);
      ordered_json commitTrace = {
          {"stage", "commit"},
          {"message", commitResult.message},
          {"ok", commitResult.ok},
          {"payload", commitResult.payload},
      };
      pipelinePayload["commit"] = commitTrace;
      pipelinePayload["stages"].push_back(commitTrace);
      result.toolRouterExecuted =
          result.toolRouterExecuted ||
          commitResult.payload.value("tool_router_executed", false);
      if (!commitResult.ok) {
        result.ok = false;
        result.verifyStatus = "FAIL";
        result.confidence = "low";
        result.executionSummary = "Verifier passed but commit failed: " +
                                  (commitResult.message.empty()
                                       ? std::string("run_command failed.")
                                       : commitResult.message);
        result.llm_output = result.executionSummary;
        result.output = result.executionSummary;
        result.errorMessage = result.executionSummary;
      }
    } else {
      pipelinePayload["commit"] = {
          {"stage", "commit"},
          {"ok", false},
          {"skipped", true},
          {"reason", normalizedRequest.autoCommit ? "verifier_rejected_commit"
                                                  : "auto_commit_disabled"},
      };
      pipelinePayload["stages"].push_back(pipelinePayload["commit"]);
    }

    result.executionPayload = pipelinePayload;
    if (!result.ok) {
      result.errorMessage = result.executionSummary;
    }
  } catch (const std::exception& ex) {
    result.ok = false;
    result.verifyStatus = "FAIL";
    result.confidence = "low";
    result.executionSummary = ex.what();
    result.output = result.executionSummary;
    result.outputSource = "runtime_error";
    result.errorMessage = ex.what();
  } catch (...) {
    result.ok = false;
    result.verifyStatus = "FAIL";
    result.confidence = "low";
    result.executionSummary = "Multi-stage cognitive pipeline failed.";
    result.output = result.executionSummary;
    result.outputSource = "runtime_error";
    result.errorMessage = result.executionSummary;
  }

  finalizeResult();
  return result;
}

}  // namespace ultra::runtime::cognitive
