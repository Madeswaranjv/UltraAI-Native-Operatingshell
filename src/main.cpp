#include "cli/CLIEngine.h"
#include "cli/CommandRouter.h"
#include "core/Logger.h"
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include<cmath>
int main(int argc, char* argv[]) {
  try {
    // Phase 1 example:
    //   ultra build --release --verbose 
    // This is parsed in CLIEngine and dispatched through UniversalCLI adapters.
    ultra::cli::CommandRouter router; //a new command is added
    ultra::cli::CLIEngine engine(router);
    return engine.run(argc, argv);
  } catch (const std::exception& e) {
    ultra::core::Logger::error(std::string("Fatal error: ") + e.what());
    return 1;
  } catch (...) {
    ultra::core::Logger::error("Fatal error: unknown exception");
    return 1;
  }
}
