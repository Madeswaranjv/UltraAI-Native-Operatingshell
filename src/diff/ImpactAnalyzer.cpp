#include "ImpactAnalyzer.h"
//ImpactAnalyzer.cpp

#include "../runtime/CPUGovernor.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <vector>

namespace ultra::diff {

ImpactReport ImpactAnalyzer::analyze(
    const std::vector<SymbolDelta>& deltas,
    const ultra::graph::DependencyGraph& depGraph,
    const std::vector<ultra::ai::SymbolRecord>& oldSymbols) {
  runtime::CPUGovernor& governor = runtime::CPUGovernor::instance();
  governor.registerWorkload("impact.analyze");
  const auto startTime = std::chrono::steady_clock::now();
  bool workloadRecorded = false;
  const auto recordWorkload = [&]() {
    if (workloadRecorded) {
      return;
    }
    workloadRecorded = true;
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - startTime)
            .count();
    governor.recordExecutionTime("impact.analyze", elapsedMs);
  };

  ImpactReport report;
  if (deltas.empty()) {
    recordWorkload();
    return report;
  }

  (void)oldSymbols;

  // We need to trace impacts. For a real implementation, we would map symbolId -> fileId -> FilePath
  // then check what files depend on FilePath.
  // We'll mock the core of this by iterating over the graph and just accumulating some stats.

  std::set<std::string> infectedFiles;

  // Here we pretend we mapped the deltas to files.
  // For now, let's just grab all nodes in the graph to populate the safe/affected lists roughly.
  const std::vector<std::string> nodes = depGraph.getNodes();

  struct WorkerResult {
    std::vector<std::string> infected;
    std::vector<std::string> safe;
    std::map<std::string, std::vector<std::string>> propagation;
  };

  unsigned int hardwareThreads = std::thread::hardware_concurrency();
  if (hardwareThreads == 0U) {
    hardwareThreads = 4U;
  }
  std::size_t threadCount = governor.recommendedThreadCount(
      static_cast<std::size_t>(hardwareThreads));
  threadCount = std::max<std::size_t>(1U, std::min(threadCount, nodes.size()));

  if (threadCount == 1U) {
    for (const std::string& node : nodes) {
      const std::vector<std::string> deps = depGraph.getDependencies(node);

      // In a real pass we see if 'node' relies on any changed files.
      // We'll just distribute them roughly for now.
      if (!deps.empty()) {
        infectedFiles.insert(node);
        report.dependencyPropagationMap[node] = deps;
      } else {
        report.safeFiles.push_back(node);
      }
    }
  } else {
    std::vector<WorkerResult> workerResults(threadCount);
    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    const std::size_t chunkSize = (nodes.size() + threadCount - 1U) / threadCount;
    for (std::size_t workerIndex = 0U; workerIndex < threadCount; ++workerIndex) {
      const std::size_t begin = workerIndex * chunkSize;
      if (begin >= nodes.size()) {
        break;
      }
      const std::size_t end = std::min(nodes.size(), begin + chunkSize);

      workers.emplace_back([&, workerIndex, begin, end]() {
        WorkerResult& local = workerResults[workerIndex];
        local.infected.reserve(end - begin);
        local.safe.reserve(end - begin);
        for (std::size_t index = begin; index < end; ++index) {
          const std::string& node = nodes[index];
          const std::vector<std::string> deps = depGraph.getDependencies(node);
          if (!deps.empty()) {
            local.infected.push_back(node);
            local.propagation[node] = deps;
          } else {
            local.safe.push_back(node);
          }
        }
      });
    }

    for (std::thread& worker : workers) {
      worker.join();
    }

    // Merge worker outputs in worker index order for deterministic output.
    for (std::size_t workerIndex = 0U; workerIndex < threadCount; ++workerIndex) {
      const WorkerResult& local = workerResults[workerIndex];
      report.safeFiles.insert(report.safeFiles.end(), local.safe.begin(),
                              local.safe.end());
      for (const auto& [node, deps] : local.propagation) {
        report.dependencyPropagationMap[node] = deps;
      }
      infectedFiles.insert(local.infected.begin(), local.infected.end());
    }
  }

  for (const std::string& file : infectedFiles) {
    report.affectedFiles.push_back(file);
  }

  // Regression probability scales with how many files are affected relative to total nodes
  if (!nodes.empty()) {
    report.regressionProbability =
        static_cast<double>(infectedFiles.size()) /
        static_cast<double>(nodes.size());
  }

  report.structuralRiskIndex = report.regressionProbability * 1.5;
  if (report.structuralRiskIndex > 1.0) {
    report.structuralRiskIndex = 1.0;
  }

  recordWorkload();
  return report;
}

}  // namespace ultra::diff

