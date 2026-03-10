#include "SemanticMemory.h"

#include <external/json.hpp>

#include <algorithm>
#include <fstream>
#include <mutex>

namespace ultra::memory {

namespace {

bool lessHistory(const SymbolHistoryEntry& left, const SymbolHistoryEntry& right) {
  if (left.version != right.version) {
    return left.version < right.version;
  }
  if (left.nodeId != right.nodeId) {
    return left.nodeId < right.nodeId;
  }
  if (left.changeType != right.changeType) {
    return left.changeType < right.changeType;
  }
  if (left.symbolName != right.symbolName) {
    return left.symbolName < right.symbolName;
  }
  return left.signature < right.signature;
}

}  // namespace

void SemanticMemory::trackSymbolEvolution(const std::string& nodeId,
                                          const std::string& symbolName,
                                          const std::string& signature,
                                          const std::string& changeType,
                                          const std::uint64_t version,
                                          const std::string& predecessorNodeId) {
  if (nodeId.empty()) {
    return;
  }

  std::unique_lock lock(mutex_);
  std::string stableIdentity;
  if (!predecessorNodeId.empty()) {
    const auto predecessorIt = stableIdentityByNode_.find(predecessorNodeId);
    if (predecessorIt != stableIdentityByNode_.end()) {
      stableIdentity = predecessorIt->second;
    }
  }
  if (stableIdentity.empty()) {
    const auto existingIt = stableIdentityByNode_.find(nodeId);
    if (existingIt != stableIdentityByNode_.end()) {
      stableIdentity = existingIt->second;
    }
  }
  if (stableIdentity.empty()) {
    stableIdentity = makeStableIdentity(nodeId);
  }

  stableIdentityByNode_[nodeId] = stableIdentity;

  SymbolHistoryEntry entry;
  entry.version = version;
  entry.stableIdentity = stableIdentity;
  entry.nodeId = nodeId;
  entry.symbolName = symbolName;
  entry.signature = signature;
  entry.changeType = changeType;

  std::vector<SymbolHistoryEntry>& history = historyByStableIdentity_[stableIdentity];
  const auto duplicateIt = std::find_if(
      history.begin(), history.end(),
      [&entry](const SymbolHistoryEntry& existing) {
        return existing.version == entry.version && existing.nodeId == entry.nodeId &&
               existing.symbolName == entry.symbolName &&
               existing.signature == entry.signature &&
               existing.changeType == entry.changeType;
      });
  if (duplicateIt != history.end()) {
    return;
  }
  history.push_back(std::move(entry));
  std::sort(history.begin(), history.end(), lessHistory);
}

std::string SemanticMemory::resolveStableIdentity(const std::string& nodeId) const {
  std::shared_lock lock(mutex_);
  const auto it = stableIdentityByNode_.find(nodeId);
  if (it == stableIdentityByNode_.end()) {
    return {};
  }
  return it->second;
}

std::vector<SymbolHistoryEntry> SemanticMemory::getSymbolHistory(
    const std::string& stableIdentity) const {
  std::shared_lock lock(mutex_);
  const auto it = historyByStableIdentity_.find(stableIdentity);
  if (it == historyByStableIdentity_.end()) {
    return {};
  }
  std::vector<SymbolHistoryEntry> out = it->second;
  std::sort(out.begin(), out.end(), lessHistory);
  return out;
}

bool SemanticMemory::persistToFile(const std::filesystem::path& path) const {
  std::map<std::string, std::string> stableIdentityByNode;
  std::map<std::string, std::vector<SymbolHistoryEntry>> historyByStableIdentity;
  {
    std::shared_lock lock(mutex_);
    stableIdentityByNode = stableIdentityByNode_;
    historyByStableIdentity = historyByStableIdentity_;
  }

  nlohmann::ordered_json payload;
  payload["schema_version"] = kSchemaVersion;
  payload["stable_identities"] = nlohmann::ordered_json::object();
  for (const auto& [nodeId, stableIdentity] : stableIdentityByNode) {
    payload["stable_identities"][nodeId] = stableIdentity;
  }

  payload["history"] = nlohmann::ordered_json::object();
  for (auto& [stableIdentity, history] : historyByStableIdentity) {
    std::sort(history.begin(), history.end(), lessHistory);
    payload["history"][stableIdentity] = nlohmann::ordered_json::array();
    for (const SymbolHistoryEntry& entry : history) {
      nlohmann::ordered_json item;
      item["version"] = entry.version;
      item["stable_identity"] = entry.stableIdentity;
      item["node_id"] = entry.nodeId;
      item["symbol_name"] = entry.symbolName;
      item["signature"] = entry.signature;
      item["change_type"] = entry.changeType;
      payload["history"][stableIdentity].push_back(std::move(item));
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << payload.dump(2);
  return static_cast<bool>(out);
}

bool SemanticMemory::loadFromFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }

  nlohmann::json payload;
  try {
    in >> payload;
  } catch (...) {
    return false;
  }
  if (!payload.is_object()) {
    return false;
  }
  if (payload.value("schema_version", 0U) != kSchemaVersion) {
    return false;
  }
  if (!payload.contains("stable_identities") ||
      !payload["stable_identities"].is_object()) {
    return false;
  }
  if (!payload.contains("history") || !payload["history"].is_object()) {
    return false;
  }

  std::map<std::string, std::string> stableIdentityByNode;
  std::map<std::string, std::vector<SymbolHistoryEntry>> historyByStableIdentity;

  for (auto it = payload["stable_identities"].begin();
       it != payload["stable_identities"].end(); ++it) {
    if (!it.value().is_string()) {
      continue;
    }
    stableIdentityByNode[it.key()] = it.value().get<std::string>();
  }

  for (auto it = payload["history"].begin(); it != payload["history"].end();
       ++it) {
    if (!it.value().is_array()) {
      continue;
    }
    std::vector<SymbolHistoryEntry> history;
    for (const nlohmann::json& entryJson : it.value()) {
      if (!entryJson.is_object()) {
        continue;
      }
      SymbolHistoryEntry entry;
      entry.version = entryJson.value("version", 0U);
      entry.stableIdentity =
          entryJson.value("stable_identity", std::string{});
      entry.nodeId = entryJson.value("node_id", std::string{});
      entry.symbolName = entryJson.value("symbol_name", std::string{});
      entry.signature = entryJson.value("signature", std::string{});
      entry.changeType = entryJson.value("change_type", std::string{});
      history.push_back(std::move(entry));
    }
    std::sort(history.begin(), history.end(), lessHistory);
    historyByStableIdentity[it.key()] = std::move(history);
  }

  std::unique_lock lock(mutex_);
  stableIdentityByNode_ = std::move(stableIdentityByNode);
  historyByStableIdentity_ = std::move(historyByStableIdentity);
  return true;
}

std::string SemanticMemory::makeStableIdentity(const std::string& nodeId) {
  return "stable:" + nodeId;
}

}  // namespace ultra::memory
