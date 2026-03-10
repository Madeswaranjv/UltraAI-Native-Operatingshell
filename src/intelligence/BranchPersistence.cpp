#include "BranchPersistence.h"
//E:\Projects\Ultra\src\intelligence\BranchPersistence.cpp
#include "../core/Logger.h"
#include <external/json.hpp>
#include <fstream>
#include <iostream>
#include <algorithm>
#include<set>
namespace ultra::intelligence {

BranchPersistence::BranchPersistence(const std::filesystem::path& baseDir) {
  branchesDir_ = baseDir / "branches";
}

bool BranchPersistence::save(const BranchStore& store) const {
  auto branches = store.getAll();
  
  // Ensure directory exists when saving (no ctor side-effects)
  std::error_code ec;
  if (!std::filesystem::create_directories(branchesDir_, ec)) {
    if (ec && !std::filesystem::exists(branchesDir_, ec)) {
      ultra::core::Logger::error(ultra::core::LogCategory::General, 
                                 "Failed to create directories: " + branchesDir_.string());
      return false;
    }
  }

  // Persist metadata (globalSequence, idCounter, lruOrder)
  {
    nlohmann::json meta;
    meta["idCounter"] = store.getIdCounter();
    meta["globalSequence"] = store.getGlobalSequence();
    meta["lruOrder"] = store.getLruOrder();

    std::filesystem::path metafile = branchesDir_ / "metadata.json.tmp";
    std::ofstream mout(metafile, std::ios::binary);
    if (!mout.is_open()) return false;
    mout << meta.dump(2);
    mout.close();
    std::filesystem::rename(metafile, branchesDir_ / "metadata.json");
  }

  for (const auto& b : branches) {
    nlohmann::json j;
    
    // Serialize fields in alphabetical order to ensure deterministic output
    j["branchId"] = b.branchId;

    // Confidence object: alphabetical keys
    nlohmann::json confJson;
    confJson["decisionReliabilityIndex"] = b.confidence.decisionReliabilityIndex;
    confJson["riskAdjustedConfidence"] = b.confidence.riskAdjustedConfidence;
    confJson["stabilityScore"] = b.confidence.stabilityScore;
    j["confidence"] = confJson;

    j["creationSequence"] = b.creationSequence;
    j["currentExecutionNodeId"] = b.currentExecutionNodeId;
    j["goal"] = b.goal;
    j["isOverlayResident"] = b.isOverlayResident;
    j["lastMutationSequence"] = b.lastMutationSequence;
    j["memorySnapshotId"] = b.memorySnapshotId;
    j["parentId"] = b.parentId;
    j["status"] = toString(b.status);

    // Sort vectors for deterministic serialization
    auto sortedDeps = b.dependencyReferences;
    std::sort(sortedDeps.begin(), sortedDeps.end());
    j["dependencyReferences"] = sortedDeps;
    
    auto sortedSubs = b.subBranches;
    std::sort(sortedSubs.begin(), sortedSubs.end());
    j["subBranches"] = sortedSubs;

    // Write file with proper path joining (atomic write)
    std::filesystem::path file = branchesDir_ / (b.branchId + ".json");
    std::filesystem::path tmpfile = branchesDir_ / (b.branchId + ".json.tmp");
    std::ofstream out(tmpfile, std::ios::binary);
    if (!out.is_open()) {
      ultra::core::Logger::error(ultra::core::LogCategory::General, 
                                 "Failed to open branch file for writing: " + tmpfile.string());
      return false;
    }
    
    out << j.dump(2);
    out.close();
    std::filesystem::rename(tmpfile, file);
  }
  return true;
}

bool BranchPersistence::load(BranchStore& store) const {
  std::error_code ec;
  if (!std::filesystem::exists(branchesDir_, ec)) {
    return false;
  }

  store.clear();

  // Load metadata if present (but defer LRU application until after branches are loaded)
  std::filesystem::path metafile = branchesDir_ / "metadata.json";
  std::vector<std::string> metaLru;
  if (std::filesystem::exists(metafile, ec)) {
    std::ifstream min(metafile, std::ios::binary);
    if (min.is_open()) {
      try {
        nlohmann::json meta;
        min >> meta;
        if (meta.contains("globalSequence")) store.setGlobalSequence(meta.value("globalSequence", 0ULL));
        if (meta.contains("idCounter")) store.setIdCounter(meta.value("idCounter", 0ULL));
        if (meta.contains("lruOrder") && meta["lruOrder"].is_array()) {
          metaLru = meta["lruOrder"].get<std::vector<std::string>>();
        }
      } catch (...) {
        // ignore malformed metadata and continue
      }
    }
  }

  // Collect entries and sort deterministically by filename
  std::vector<std::filesystem::directory_entry> entries;
  for (const auto& entry : std::filesystem::directory_iterator(branchesDir_, ec)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(), [](const std::filesystem::directory_entry& a, const std::filesystem::directory_entry& b){
    return a.path().filename().string() < b.path().filename().string();
  });

  for (const auto& entry : entries) {
    if (entry.path().filename() == "metadata.json") continue;
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in.is_open()) {
      ultra::core::Logger::warning(ultra::core::LogCategory::General, 
          "Failed to open branch file for reading: " + entry.path().string());
      continue;
    }

    try {
      nlohmann::json j;
      in >> j;

      Branch b;
      b.branchId = j.value("branchId", "");
      b.parentId = j.value("parentId", "");
      b.parentBranchId = b.parentId;
      b.goal = j.value("goal", "");
      b.currentExecutionNodeId = j.value("currentExecutionNodeId", "");
      b.isOverlayResident = j.value("isOverlayResident", false);
      b.memorySnapshotId = j.value("memorySnapshotId", "");
      
      // Load dependency references
      if (j.contains("dependencyReferences") && j["dependencyReferences"].is_array()) {
        b.dependencyReferences = j["dependencyReferences"].get<std::vector<std::string>>();
      }
      
      // Load sub branches
      if (j.contains("subBranches") && j["subBranches"].is_array()) {
        b.subBranches = j["subBranches"].get<std::vector<std::string>>();
      }
      
      // Load confidence struct with all fields
      ultra::types::Confidence conf;
      if (j.contains("confidence") && j["confidence"].is_object()) {
        conf.stabilityScore = j["confidence"].value("stabilityScore", 0.0);
        conf.riskAdjustedConfidence = j["confidence"].value("riskAdjustedConfidence", 0.0);
        conf.decisionReliabilityIndex = j["confidence"].value("decisionReliabilityIndex", 0.0);
      } else {
        // Fallback for old format
        conf.stabilityScore = j.value("confidence", 0.0);
      }
      b.confidence = conf;

      b.status = fromString(j.value("status", "Unknown"));
      b.creationSequence = j.value("creationSequence", 0ULL);
      b.lastMutationSequence = j.value("lastMutationSequence", b.creationSequence);

      store.insertLoaded(b);

    } catch (const std::exception& e) {
      ultra::core::Logger::warning(ultra::core::LogCategory::General, 
          "Failed to parse branch file: " + entry.path().string() + " - " + e.what());
    }
  }

  // Rebuild LRU deterministically: use metadata LRU as base, append any missing branch ids sorted lexicographically
  auto allIds = store.branchIdSnapshot();
  std::set<std::string> allSet(allIds.begin(), allIds.end());

  std::vector<std::string> finalLru;
  // Filter metaLru to only include ids that exist
  for (const auto& id : metaLru) {
    if (allSet.count(id)) finalLru.push_back(id);
  }

  // Append missing ids sorted lexicographically
  std::vector<std::string> missing;
  for (const auto& id : allIds) {
    if (std::find(finalLru.begin(), finalLru.end(), id) == finalLru.end()) missing.push_back(id);
  }
  std::sort(missing.begin(), missing.end());
  finalLru.insert(finalLru.end(), missing.begin(), missing.end());

  // Ensure deterministic LRU is set on the store
  store.setLruOrder(finalLru);

  return true;
}
}  // namespace ultra::intelligence
