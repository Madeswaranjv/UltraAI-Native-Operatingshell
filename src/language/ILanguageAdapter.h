#pragma once

#include "../graph/DependencyGraph.h"
#include "../scanner/FileInfo.h"
#include "external/json.hpp"
#include <filesystem>
#include <vector>
//E:\Projects\Ultra\src\language\ILanguageAdapter.h
namespace ultra::language {

class ILanguageAdapter {
 public:
  virtual ~ILanguageAdapter() = default;

  virtual std::vector<ultra::scanner::FileInfo> scan(
      const std::filesystem::path& root) = 0;

  virtual ultra::graph::DependencyGraph buildGraph(
      const std::vector<ultra::scanner::FileInfo>& files) = 0;

  virtual void analyze(const std::filesystem::path& root) = 0;

  virtual void build(const std::filesystem::path& root) = 0;

  virtual void buildIncremental(const std::filesystem::path& root) = 0;

  /** [Experimental] Incremental compile with cl.exe; fallback to full build. */
  virtual void buildFast(const std::filesystem::path& root) = 0;

  virtual nlohmann::json generateContext(
      const std::filesystem::path& root) = 0;

  /** Generate context using AST extraction (with line numbers). */
  virtual nlohmann::json generateContextWithAst(
      const std::filesystem::path& root) = 0;

  virtual bool applyPatch(const std::filesystem::path& root,
                         const std::filesystem::path& diffFile) = 0;

  virtual int getLastBuildExitCode() const { return 0; }
};

}  // namespace ultra::language
