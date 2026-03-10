#include "ParallelScanner.h"

#include "../../runtime/CPUGovernor.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace ultra::engine::parallel {

namespace {

struct FileTask {
  std::size_t index{0U};
  ai::FileRecord record;
  std::filesystem::path absolutePath;
};

class FileTaskQueue {
 public:
  void push(FileTask task) {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(task));
  }

  bool pop(FileTask& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (nextIndex_ >= tasks_.size()) {
      return false;
    }
    task = std::move(tasks_[nextIndex_]);
    ++nextIndex_;
    return true;
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<FileTask> tasks_;
  std::size_t nextIndex_{0U};
};

struct ThreadLocalBuffer {
  std::vector<std::pair<std::size_t, ai::Sha256Hash>> hashesByIndex;
  std::vector<ai::SymbolRecord> symbols;
  std::vector<ai::FileDependencyEdge> fileEdges;
  std::vector<std::pair<std::uint32_t, std::vector<ai::SemanticSymbolDependency>>>
      semanticDepsByFileId;
};

bool semanticDependencyLess(const ai::SemanticSymbolDependency& left,
                            const ai::SemanticSymbolDependency& right) {
  return std::tie(left.fromSymbol, left.toSymbol, left.lineNumber) <
         std::tie(right.fromSymbol, right.toSymbol, right.lineNumber);
}

bool semanticDependencyEqual(const ai::SemanticSymbolDependency& left,
                             const ai::SemanticSymbolDependency& right) {
  return left.fromSymbol == right.fromSymbol &&
         left.toSymbol == right.toSymbol && left.lineNumber == right.lineNumber;
}

}  // namespace

ParallelScanner::ParallelScanner(std::filesystem::path projectRoot)
    : projectRoot_(
          std::filesystem::absolute(std::move(projectRoot)).lexically_normal()) {}

bool ParallelScanner::runFullScan(const DependencyResolver& dependencyResolver,
                                  ParallelScanResult& output,
                                  std::string& error) const {
  output = ParallelScanResult{};
  if (!dependencyResolver) {
    error = "Dependency resolver is required for parallel semantic scan.";
    return false;
  }

  const std::vector<ai::DiscoveredFile> discovered =
      ai::FileRegistry::discoverProjectFiles(projectRoot_);
  output.files = ai::FileRegistry::deriveRecords(discovered);
  if (output.files.empty()) {
    return true;
  }

  std::map<std::string, ai::DiscoveredFile> discoveredByPath;
  for (const ai::DiscoveredFile& file : discovered) {
    discoveredByPath[file.relativePath] = file;
  }

  const std::map<std::string, ai::FileRecord> filesByPath =
      ai::FileRegistry::mapByPath(output.files);

  FileTaskQueue queue;
  for (std::size_t index = 0U; index < output.files.size(); ++index) {
    const ai::FileRecord& record = output.files[index];
    const auto discoveredIt = discoveredByPath.find(record.path);
    if (discoveredIt == discoveredByPath.end()) {
      continue;
    }
    FileTask task;
    task.index = index;
    task.record = record;
    task.absolutePath = discoveredIt->second.absolutePath;
    queue.push(std::move(task));
  }

  if (queue.size() == 0U) {
    return true;
  }

  unsigned int maxThreads = std::thread::hardware_concurrency();
  if (maxThreads == 0U) {
    maxThreads = 4U;
  }
  std::size_t threadCount = runtime::CPUGovernor::instance().recommendedThreadCount(
      static_cast<std::size_t>(maxThreads));
  threadCount = std::max<std::size_t>(1U, std::min(threadCount, queue.size()));

  std::vector<ThreadLocalBuffer> threadLocal(threadCount);
  std::atomic<bool> hasError{false};
  std::mutex errorMutex;
  std::string firstError;

  const auto reportError = [&](std::string message) {
    hasError.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(errorMutex);
    if (firstError.empty()) {
      firstError = std::move(message);
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(threadCount);
  for (std::size_t workerIndex = 0U; workerIndex < threadCount; ++workerIndex) {
    workers.emplace_back([&, workerIndex]() {
      ThreadLocalBuffer& local = threadLocal[workerIndex];
      FileTask task;
      while (!hasError.load(std::memory_order_relaxed) && queue.pop(task)) {
        ai::Sha256Hash hash{};
        std::string localError;
        if (!ai::sha256OfFile(task.absolutePath, hash, localError)) {
          reportError(localError);
          return;
        }

        ai::SemanticParseResult semantic =
            ai::SemanticExtractor::extract(task.absolutePath, task.record.language);

        std::vector<ai::SymbolRecord> fileSymbols;
        if (!ai::SymbolTable::buildFromExtracted(task.record.fileId,
                                                 semantic.symbols,
                                                 fileSymbols,
                                                 localError)) {
          reportError(localError);
          return;
        }

        local.hashesByIndex.push_back({task.index, hash});
        local.symbols.insert(local.symbols.end(), fileSymbols.begin(), fileSymbols.end());
        local.semanticDepsByFileId.push_back(
            {task.record.fileId, std::move(semantic.symbolDependencies)});

        for (const std::string& reference : semantic.dependencyReferences) {
          std::string resolvedPath;
          if (!dependencyResolver(task.record.path, reference, filesByPath, resolvedPath)) {
            continue;
          }
          const auto targetIt = filesByPath.find(resolvedPath);
          if (targetIt == filesByPath.end()) {
            continue;
          }
          ai::FileDependencyEdge edge;
          edge.fromFileId = task.record.fileId;
          edge.toFileId = targetIt->second.fileId;
          local.fileEdges.push_back(edge);
        }
      }
    });
  }

  for (std::thread& worker : workers) {
    worker.join();
  }

  if (hasError.load(std::memory_order_relaxed)) {
    error = firstError.empty() ? "Parallel semantic scan failed." : firstError;
    return false;
  }

  for (const ThreadLocalBuffer& local : threadLocal) {
    for (const auto& [index, hash] : local.hashesByIndex) {
      if (index >= output.files.size()) {
        continue;
      }
      output.files[index].hash = hash;
    }
  }

  for (const ai::FileRecord& file : output.files) {
    if (file.hash == ai::zeroHash()) {
      error = "Parallel semantic scan failed to hash file: " + file.path;
      return false;
    }
  }

  std::size_t totalSymbols = 0U;
  std::size_t totalFileEdges = 0U;
  for (const ThreadLocalBuffer& local : threadLocal) {
    totalSymbols += local.symbols.size();
    totalFileEdges += local.fileEdges.size();
  }

  output.symbols.clear();
  output.symbols.reserve(totalSymbols);
  output.deps.fileEdges.clear();
  output.deps.fileEdges.reserve(totalFileEdges);

  for (ThreadLocalBuffer& local : threadLocal) {
    output.symbols.insert(output.symbols.end(),
                          std::make_move_iterator(local.symbols.begin()),
                          std::make_move_iterator(local.symbols.end()));
    output.deps.fileEdges.insert(output.deps.fileEdges.end(),
                                 std::make_move_iterator(local.fileEdges.begin()),
                                 std::make_move_iterator(local.fileEdges.end()));
    for (auto& [fileId, dependencies] : local.semanticDepsByFileId) {
      auto& target = output.semanticSymbolDepsByFileId[fileId];
      target.insert(target.end(), std::make_move_iterator(dependencies.begin()),
                    std::make_move_iterator(dependencies.end()));
    }
  }

  ai::SymbolTable::sortDeterministic(output.symbols);
  ai::DependencyTable::sortAndDedupe(output.deps);

  for (auto& [fileId, dependencies] : output.semanticSymbolDepsByFileId) {
    (void)fileId;
    std::sort(dependencies.begin(), dependencies.end(), semanticDependencyLess);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end(),
                                   semanticDependencyEqual),
                       dependencies.end());
  }

  const std::map<std::uint32_t, std::vector<ai::SymbolRecord>> symbolsByFileId =
      ai::SymbolTable::groupByFileId(output.symbols);
  output.deps.symbolEdges.clear();
  const std::vector<ai::SymbolDependencyEdge> fromFileEdges =
      ai::DependencyTable::buildSymbolEdgesFromFileEdges(output.deps.fileEdges,
                                                         symbolsByFileId);
  const std::vector<ai::SymbolDependencyEdge> fromSemanticEdges =
      ai::DependencyTable::buildSymbolEdgesFromSemanticDependencies(
          output.semanticSymbolDepsByFileId, symbolsByFileId);
  output.deps.symbolEdges.insert(output.deps.symbolEdges.end(),
                                 fromFileEdges.begin(), fromFileEdges.end());
  output.deps.symbolEdges.insert(output.deps.symbolEdges.end(),
                                 fromSemanticEdges.begin(),
                                 fromSemanticEdges.end());
  ai::DependencyTable::sortAndDedupe(output.deps);

  return true;
}

}  // namespace ultra::engine::parallel
