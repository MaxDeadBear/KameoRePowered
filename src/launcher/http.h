// HTTPS GETs through WinHTTP, for the release list and the build downloads.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace kameo::launcher::http {

// Called as bytes arrive with the running total and the full size (0 when the server does not say); returning false cancels.
using Progress = std::function<bool(uint64_t done, uint64_t total)>;

// Fetches `url` (https only, redirects followed) into `body`; false with `error` set on any failure or a status other than 200.
bool Get(const std::string& url, std::string& body, std::string& error, const char* accept = nullptr);
// The same into a file, written as it arrives.
bool Download(const std::string& url, const std::filesystem::path& file, const Progress& progress, std::string& error);

}  // namespace kameo::launcher::http
