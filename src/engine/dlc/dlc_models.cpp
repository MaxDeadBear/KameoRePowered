// DLC model files on the host: naming, the content folder they live in, and keeping each package's KameoDLCList.txt in step with them.

#include "engine/dlc/dlc_models.h"

#include <fstream>
#include <set>

bool IsKameoDlcModelName(const std::string& filename) {
  return filename.rfind("007183BF_", 0) == 0 &&
         filename.size() > 13 &&
         filename.substr(filename.size() - 4) == ".mdl";
}

std::filesystem::path KameoActiveDlcPath(const std::filesystem::path& user_data_root) {
  return user_data_root / "0000000000000000" / "4D5307D2" / "00000002";
}

void SyncKameoDlcListForCustomModels(const std::filesystem::path& user_data_root) {
  const auto dlc_root = KameoActiveDlcPath(user_data_root);
  if (!std::filesystem::exists(dlc_root)) {
    return;
  }

  for (const auto& pkg_entry : std::filesystem::directory_iterator(dlc_root)) {
    if (!pkg_entry.is_directory()) {
      continue;
    }
    const auto& pkg_dir = pkg_entry.path();

    const auto list_path = pkg_dir / "KameoDLCList.txt";
    if (!std::filesystem::exists(list_path)) {
      continue;
    }

    std::ifstream in(list_path);
    if (!in) {
      continue;
    }

    std::set<std::string> listed;
    std::string line;
    bool list_ended_with_newline = true;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!line.empty()) {
        listed.insert(line);
      }
      list_ended_with_newline = false;
    }
    if (in.eof()) {
      in.clear();
      in.seekg(0, std::ios::end);
      const auto size = in.tellg();
      if (size > 0) {
        in.seekg(-1, std::ios::end);
        char last = '\0';
        in.get(last);
        list_ended_with_newline = last == '\n';
      }
    }

    std::ofstream out(list_path, std::ios::app);
    if (!out) {
      continue;
    }

    bool wrote_any = false;
    for (const auto& entry : std::filesystem::directory_iterator(pkg_dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }

      const std::string filename = entry.path().filename().string();
      if (!IsKameoDlcModelName(filename) || listed.count(filename) != 0) {
        continue;
      }

      if (!list_ended_with_newline || wrote_any) {
        out << '\n';
      }
      out << filename;
      listed.insert(filename);
      wrote_any = true;
    }
  }
}
