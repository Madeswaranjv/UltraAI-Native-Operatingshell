#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ultra::scaffolds {

struct ScaffoldOptions {
  bool verbose{false};
  bool dryRun{false};
  bool force{false};
  std::string templateName;
  std::string passthroughFlags;
  std::filesystem::path destinationRoot;
};

class IScaffoldEnvironment {
 public:
  virtual ~IScaffoldEnvironment() = default;

  virtual std::filesystem::path currentPath() const = 0;
  virtual bool pathExists(const std::filesystem::path& path) const = 0;
  virtual bool createDirectories(const std::filesystem::path& path) = 0;
  virtual bool writeTextFile(const std::filesystem::path& path,
                             const std::string& content) = 0;
  virtual bool isToolAvailable(
      const std::vector<std::string>& probeCommands) const = 0;
  virtual int runCommand(const std::filesystem::path& workingDirectory,
                         const std::string& command,
                         const ScaffoldOptions& options) = 0;
};

class DefaultScaffoldEnvironment final : public IScaffoldEnvironment {
 public:
  std::filesystem::path currentPath() const override;
  bool pathExists(const std::filesystem::path& path) const override;
  bool createDirectories(const std::filesystem::path& path) override;
  bool writeTextFile(const std::filesystem::path& path,
                     const std::string& content) override;
  bool isToolAvailable(
      const std::vector<std::string>& probeCommands) const override;
  int runCommand(const std::filesystem::path& workingDirectory,
                 const std::string& command,
                 const ScaffoldOptions& options) override;
};

class ScaffoldBase {
 public:
  explicit ScaffoldBase(IScaffoldEnvironment& environment);
  virtual ~ScaffoldBase() = default;

  virtual void generate(const std::string& name,
                        const ScaffoldOptions& options) = 0;

  int lastExitCode() const noexcept;

 protected:
  IScaffoldEnvironment& environment() noexcept;
  const IScaffoldEnvironment& environment() const noexcept;

  bool ensureTargetAvailable(const std::filesystem::path& targetDirectory,
                             const ScaffoldOptions& options);
  bool ensureTool(const std::string& toolDisplayName,
                  const std::vector<std::string>& probeCommands);
  bool writeUltraConfig(const std::filesystem::path& projectRoot,
                        const std::string& moduleName,
                        const std::string& moduleType,
                        const ScaffoldOptions& options);
  void printInitStart(const std::string& projectName) const;
  void printEnterInstruction(const std::string& projectName) const;
  int run(const std::filesystem::path& workingDirectory,
          const std::string& command,
          const ScaffoldOptions& options);

  void fail(const std::string& message);
  void succeed();

  static std::string quoteArg(const std::string& value);

 private:
  IScaffoldEnvironment& m_environment;
  int m_lastExitCode{0};
};

}  // namespace ultra::scaffolds
