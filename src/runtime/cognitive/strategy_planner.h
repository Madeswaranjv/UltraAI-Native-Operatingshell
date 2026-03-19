#pragma once

#include "ultra_loop.h"
#include "../intent/Intent.h"
#include "../intent/Strategy.h"
#include "../model/i_model.h"

#include <optional>

namespace ultra::runtime::cognitive {

class StrategyPlanner {
 public:
  [[nodiscard]] intent::Strategy generate(const intent::Intent& intentValue) const;
  void setModel(model::IModel* model) noexcept;

 private:
  [[nodiscard]] intent::Strategy generateDeterministic(
      const intent::Intent& intentValue) const;
  [[nodiscard]] std::optional<intent::Strategy> tryGenerateWithModel(
      const intent::Intent& intentValue) const;

  model::IModel* model_{nullptr};
};

class StrategyPlanningStage final : public IStrategyStage {
 public:
  StrategyPlanningStage() = default;
  explicit StrategyPlanningStage(StrategyPlanner planner);

  void setModel(model::IModel* model) noexcept;
  [[nodiscard]] StageResult run(UltraLoopFrame& frame) override;

 private:
  StrategyPlanner planner_{};
};

}  // namespace ultra::runtime::cognitive
