#include "ContextSliceProvider.h"

#include "UltraAuthorityAPI.h"

#include "../api/CognitiveKernelAPI.h"
#include "../core/state_manager.h"
#include "../runtime/cognitive/CognitiveRuntime.h"
#include "../types/BranchId.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <string>

namespace ultra::authority {

ContextSliceProvider::ContextSliceProvider(std::filesystem::path projectRoot)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()) {}

AuthorityContextResult ContextSliceProvider::getSlice(
    const AuthorityContextRequest& request) const {
  AuthorityContextResult result;
  result.success = false;

  try {
    core::StateManager stateManager(projectRoot_);
    std::string loadError;
    if (!stateManager.loadPersistedGraph(loadError)) {
      result.message = loadError.empty()
                           ? "Unable to load persisted graph state."
                           : loadError;
      return result;
    }

    if (!request.branchId.empty()) {
      stateManager.setActiveBranch(types::BranchId::fromString(request.branchId));
    }

    runtime::CognitiveRuntime runtime(stateManager);
    const std::size_t tokenBudget =
        request.tokenBudget == 0U
            ? std::numeric_limits<std::size_t>::max()
            : request.tokenBudget;
    runtime::CognitiveState state =
        runtime.createState(tokenBudget, request.relevanceProfile);
    runtime::SnapshotPinGuard pinGuard = runtime.pin(state);
    pinGuard.assertCurrent();

    api::Query query;
    query.kind = api::QueryKind::Auto;
    query.target = request.query;
    query.impactDepth = std::max<std::size_t>(1U, request.impactDepth);
    result.contextJson = api::CognitiveKernelAPI::getMinimalContext(state, query);

    const nlohmann::json payload = nlohmann::json::parse(result.contextJson);
    if (payload.contains("metadata") && payload["metadata"].is_object()) {
      result.estimatedTokens =
          payload["metadata"].value("estimatedTokens", 0U);
    }
    result.snapshotVersion = state.snapshot.version;
    result.snapshotHash = state.snapshot.deterministicHash();
    result.success = true;
    result.message = "ok";
    return result;
  } catch (const std::exception& ex) {
    result.message = ex.what();
    return result;
  } catch (...) {
    result.message = "Unknown failure while building context slice.";
    return result;
  }
}

}  // namespace ultra::authority
