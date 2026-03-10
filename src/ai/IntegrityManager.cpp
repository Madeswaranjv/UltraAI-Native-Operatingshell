#include "IntegrityManager.h"

namespace ultra::ai {

CoreIndex IntegrityManager::buildCoreIndex(const bool runtimeActive,
                                           const Sha256Hash& filesTblHash,
                                           const Sha256Hash& symbolsTblHash,
                                           const Sha256Hash& depsTblHash,
                                           const Sha256Hash& projectRootHash,
                                           const Sha256Hash& indexHash) {
  CoreIndex core;
  core.magic = kCoreMagic;
  core.indexVersion = kIndexVersion;
  core.schemaVersion = kSchemaVersion;
  core.runtimeActive = runtimeActive ? 1U : 0U;
  core.reserved = {0U, 0U, 0U};
  core.filesTblHash = filesTblHash;
  core.symbolsTblHash = symbolsTblHash;
  core.depsTblHash = depsTblHash;
  core.projectRootHash = projectRootHash;
  core.indexHash = indexHash;
  return core;
}

Sha256Hash IntegrityManager::computeProjectRootHash(
    const std::vector<FileRecord>& files) {
  Sha256Accumulator accumulator;
  for (const FileRecord& file : files) {
    accumulator.update(file.path);
    accumulator.update(file.hash.data(), file.hash.size());
  }
  return accumulator.finalize();
}

bool IntegrityManager::computeTableHash(const std::filesystem::path& tablePath,
                                        Sha256Hash& outHash,
                                        std::string& error) {
  return sha256OfFile(tablePath, outHash, error);
}

bool IntegrityManager::computeIndexHash(const std::filesystem::path& filesTablePath,
                                        const std::filesystem::path& symbolsTablePath,
                                        const std::filesystem::path& depsTablePath,
                                        Sha256Hash& outHash,
                                        std::string& error) {
  Sha256Accumulator accumulator;
  if (!accumulator.updateFromFile(filesTablePath, error)) {
    return false;
  }
  if (!accumulator.updateFromFile(symbolsTablePath, error)) {
    return false;
  }
  if (!accumulator.updateFromFile(depsTablePath, error)) {
    return false;
  }
  outHash = accumulator.finalize();
  return true;
}

bool IntegrityManager::verify(const CoreIndex& core,
                              const Sha256Hash& filesTblHash,
                              const Sha256Hash& symbolsTblHash,
                              const Sha256Hash& depsTblHash,
                              const Sha256Hash& projectRootHash,
                              const Sha256Hash& indexHash,
                              std::string& error) {
  if (core.magic != kCoreMagic) {
    error = "core.idx magic mismatch";
    return false;
  }
  if (core.indexVersion != kIndexVersion) {
    error = "core.idx index_version mismatch";
    return false;
  }
  if (core.schemaVersion != kSchemaVersion) {
    error = "core.idx schema_version mismatch";
    return false;
  }
  if (!hashesEqual(core.filesTblHash, filesTblHash)) {
    error = "files.tbl hash mismatch";
    return false;
  }
  if (!hashesEqual(core.symbolsTblHash, symbolsTblHash)) {
    error = "symbols.tbl hash mismatch";
    return false;
  }
  if (!hashesEqual(core.depsTblHash, depsTblHash)) {
    error = "deps.tbl hash mismatch";
    return false;
  }
  if (!hashesEqual(core.projectRootHash, projectRootHash)) {
    error = "project_root_hash mismatch";
    return false;
  }
  if (!hashesEqual(core.indexHash, indexHash)) {
    error = "index_hash mismatch";
    return false;
  }
  return true;
}

}  // namespace ultra::ai

