// Unpacking a release zip: stored and deflated entries, as GitHub's builds and Windows' own zips use.
#pragma once

#include <filesystem>
#include <string>

namespace kameo::launcher {

// Extracts every file of `zip` under `folder`; entries that would land outside it are refused. False with `error` set on any failure.
bool ExtractZip(const std::filesystem::path& zip, const std::filesystem::path& folder, std::string& error);

}  // namespace kameo::launcher
