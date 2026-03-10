#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ultra::cli {

class CommandRouter {
 public:
  using Handler = std::function<void(const std::vector<std::string>&)>;

  void registerCommand(const std::string& name, Handler handler);
  bool execute(const std::string& name,
               const std::vector<std::string>& args) const;
  bool hasCommand(const std::string& name) const;

 private:
  std::unordered_map<std::string, Handler> m_handlers;
};

}  // namespace ultra::cli
