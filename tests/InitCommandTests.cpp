#include <gtest/gtest.h>
#include "cli/InitCommand.h"
#include "scaffolds/ScaffoldBase.h"
#include <filesystem>
#include <string>
#include <vector>

using namespace ultra;

class FakeScaffoldEnvironment final : public scaffolds::IScaffoldEnvironment {
public:
    std::filesystem::path cwd{"E:/Projects/Ultra"};
    mutable std::vector<std::vector<std::string>> toolChecks;
    std::vector<std::pair<std::filesystem::path, std::string>> runCalls;
    std::vector<std::filesystem::path> createdDirectories;
    std::vector<std::pair<std::filesystem::path, std::string>> writtenFiles;
    std::vector<std::filesystem::path> existingPaths;
    bool toolsAvailable{true};
    int commandResult{0};

    std::filesystem::path currentPath() const override { return cwd; }

    bool pathExists(const std::filesystem::path& path) const override {
        for (const auto& existing : existingPaths) {
            if (existing.lexically_normal() == path.lexically_normal())
                return true;
        }
        return false;
    }

    bool createDirectories(const std::filesystem::path& path) override {
        createdDirectories.push_back(path);
        return true;
    }

    bool writeTextFile(const std::filesystem::path& path,
                       const std::string& content) override {
        writtenFiles.emplace_back(path, content);
        return true;
    }

    bool isToolAvailable(const std::vector<std::string>& probeCommands) const override {
        toolChecks.push_back(probeCommands);
        return toolsAvailable;
    }

    int runCommand(const std::filesystem::path& workingDirectory,
                   const std::string& command,
                   const scaffolds::ScaffoldOptions&) override {
        runCalls.emplace_back(workingDirectory, command);
        return commandResult;
    }
};

static bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

TEST(InitCommand, ReactInit) {
    FakeScaffoldEnvironment env;
    cli::InitCommand init(env);

    int code = init.execute({"react", "sampleApp"});

    ASSERT_EQ(code, 0);
    ASSERT_EQ(env.runCalls.size(), 1);
    EXPECT_TRUE(contains(env.runCalls[0].second, "create-vite"));
}

TEST(InitCommand, DjangoInit) {
    FakeScaffoldEnvironment env;
    cli::InitCommand init(env);

    int code = init.execute({"django", "sampleProject"});

    ASSERT_EQ(code, 0);
    ASSERT_EQ(env.runCalls.size(), 1);
    EXPECT_TRUE(contains(env.runCalls[0].second, "django-admin"));
}

TEST(InitCommand, PythonInit) {
    FakeScaffoldEnvironment env;
    cli::InitCommand init(env);

    int code = init.execute({"python", "toolish"});

    ASSERT_EQ(code, 0);
    ASSERT_EQ(env.createdDirectories.size(), 1);
}