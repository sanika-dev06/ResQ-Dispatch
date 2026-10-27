// paths.hpp - find the project folder (the one containing db/ and public/) no matter
// where the program is started from: a terminal, VS Code's debugger, or by double-click.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace paths {

inline bool isProjectRoot(const std::filesystem::path& p) {
  std::error_code ec;
  return std::filesystem::exists(p / "db" / "schema.sql", ec) && std::filesystem::exists(p / "public" / "index.html", ec);
}

// 1. an explicit RESQ_ROOT wins;  2. otherwise walk upwards from the current folder and from the
// executable's folder (build/Release/resq_server.exe -> build/Release -> build -> project).
inline std::string findProjectRoot(const char* argv0, const std::string& explicitRoot) {
  namespace fs = std::filesystem;
  if (!explicitRoot.empty()) return explicitRoot;
  std::error_code ec;
  std::vector<fs::path> starts;
  starts.push_back(fs::current_path(ec));
  if (argv0 && *argv0) starts.push_back(fs::absolute(fs::path(argv0), ec).parent_path());
  for (const fs::path& start : starts) {
    fs::path p = start;
    for (int i = 0; i < 6 && !p.empty(); ++i) {
      if (isProjectRoot(p)) return p.string();
      fs::path up = p.parent_path();
      if (up == p) break;
      p = up;
    }
  }
  return ".";
}

}  // namespace paths
