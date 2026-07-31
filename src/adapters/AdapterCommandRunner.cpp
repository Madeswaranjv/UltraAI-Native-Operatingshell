#include "AdapterCommandRunner.h"
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
// POSIX placeholder
#endif

namespace ultra::adapters {

namespace {

std::string normalizedPath(const std::filesystem::path& path) {
  std::string value = path.lexically_normal().generic_string();
  const bool isWindowsDriveRoot =
      value.size() == 3 && std::isalpha(static_cast<unsigned char>(value[0])) &&
      value[1] == ':' && value[2] == '/';
  if (!isWindowsDriveRoot && value.size() > 1 && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

}  // namespace

bool isToolAvailable(const std::string& toolName) {
  if (toolName.empty()) {
    return false;
  }

#ifdef _WIN32
  const std::string probe = "cmd.exe /c " + toolName + " --version >nul 2>&1";
#else
  const std::string probe = toolName + " --version > /dev/null 2>&1";
#endif
  const int code = std::system(probe.c_str());
  return code == 0;
}

int runCommand(const std::filesystem::path& workingDirectory,
               const std::string& command,
               const ultra::cli::CommandOptions& options,
               std::string* out_captured_output) {
  std::cout << "[exec] (" << normalizedPath(workingDirectory) << ") " << command
            << '\n';

  if (options.dryRun) {
    return 0;
  }

#ifdef _WIN32
  HANDLE hReadPipe = NULL;
  HANDLE hWritePipe = NULL;
  
  SECURITY_ATTRIBUTES saAttr;
  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = NULL;

  if (out_captured_output) {
      if (!CreatePipe(&hReadPipe, &hWritePipe, &saAttr, 0)) {
          std::cout << "[ERROR] Failed to create pipes for stdout/stderr.\n";
          return 1;
      }
      SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
  }

  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  
  if (out_captured_output) {
      si.hStdError = hWritePipe;
      si.hStdOutput = hWritePipe;
      si.dwFlags |= STARTF_USESTDHANDLES;
  }

  ZeroMemory(&pi, sizeof(pi));

  std::wstring wCmd = L"cmd.exe /c \"" + std::wstring(command.begin(), command.end()) + L"\"";
  std::wstring wCwd = workingDirectory.wstring();

  std::vector<wchar_t> cmdBuffer(wCmd.begin(), wCmd.end());
  cmdBuffer.push_back(0);

  BOOL success = CreateProcessW(
      NULL,
      cmdBuffer.data(),
      NULL,
      NULL,
      out_captured_output ? TRUE : FALSE,
      CREATE_NO_WINDOW,
      NULL,
      wCwd.c_str(),
      &si,
      &pi
  );

  if (out_captured_output) {
      CloseHandle(hWritePipe);
  }

  if (!success) {
      if (out_captured_output) {
          CloseHandle(hReadPipe);
      }
      std::cout << "[ERROR] Failed to create process for: " << command << '\n';
      return 1;
  }

  if (out_captured_output) {
      std::string output;
      char buffer[4096];
      DWORD bytesRead;
      while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead != 0) {
          output.append(buffer, bytesRead);
      }
      *out_captured_output = output;
      
      if (!options.jsonOutput && !options.dryRun) {
          std::cout << *out_captured_output << "\n";
      }
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  if (out_captured_output) {
      CloseHandle(hReadPipe);
  }

  if (exitCode != 0) {
      std::cout << "[ERROR] Command failed (exit code " << exitCode << ").\n";
  }

  return static_cast<int>(exitCode);

#else
  // Fallback for non-Windows using chdir logic since this runs on user workspace only for now.
  // Properly this should use posix_spawn, but as per requirements, Windows is first priority
  // and we'll leave the POSIX as a shim for now as we don't have access to OS-specific APIs.
  std::error_code ec;
  const std::filesystem::path originalDirectory = std::filesystem::current_path(ec);
  std::filesystem::current_path(workingDirectory, ec);
  
  std::string actualCommand = command;
  std::filesystem::path tempOutputPath;
  if (out_captured_output) {
    tempOutputPath = std::filesystem::temp_directory_path(ec) / 
        ("ultra_cmd_capture_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
    actualCommand += " > \"" + tempOutputPath.string() + "\" 2>&1";
  }

  const int code = std::system(actualCommand.c_str());

  if (out_captured_output) {
    std::ifstream input(tempOutputPath, std::ios::binary);
    if (input) {
        std::ostringstream stream;
        stream << input.rdbuf();
        *out_captured_output = stream.str();
    }
    std::error_code removeError;
    std::filesystem::remove(tempOutputPath, removeError);
    if (!options.jsonOutput && !options.dryRun) {
        std::cout << *out_captured_output << "\n";
    }
  }

  std::error_code restoreEc;
  std::filesystem::current_path(originalDirectory, restoreEc);

  if (code != 0) {
    std::cout << "[ERROR] Command failed (exit code " << code << ").\n";
  }

  return code;
#endif
}

}  // namespace ultra::adapters