#pragma once

#include "ImpactTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <queue>
#include <set>
#include <vector>

namespace ultra::engine::impact {

class ImpactGraphTraversal {
 public:
  template <typename NodeT>
  struct Request {
    std::vector<NodeT> startNodes;
    TraversalDirection direction{TraversalDirection::Forward};
    std::size_t maxDepth{1};
    std::size_t maxNodes{64};
    bool includeStartNodes{true};
    std::vector<NodeT> allowedNodes;
  };

  template <typename NodeT>
  struct Result {
    std::vector<NodeT> orderedNodes;
    std::map<NodeT, std::size_t> depthByNode;
  };

  template <typename NodeT>
  static Result<NodeT> traverse(
      const Request<NodeT>& request,
      const std::map<NodeT, std::vector<NodeT>>& forwardAdj,
      const std::map<NodeT, std::vector<NodeT>>& reverseAdj);
};

}  // namespace ultra::engine::impact