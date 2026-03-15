#include "CommandRouter.h"
//E:\Projects\Ultra\src\cli\CommandRouter.cpp
namespace ultra::cli {

void CommandRouter::registerCommand(const std::string& name, Handler handler) {
  // Command registration is intentionally overwrite-friendly to support
  // iterative CLI composition during startup.
  m_handlers[name] = std::move(handler);
}

bool CommandRouter::execute(const std::string& name,
                            const std::vector<std::string>& args) const {
  auto it = m_handlers.find(name);
  if (it == m_handlers.end()) return false;
  it->second(args);
  return true;
}

bool CommandRouter::hasCommand(const std::string& name) const {
  return m_handlers.find(name) != m_handlers.end();
}

}  // namespace ultra::cli
