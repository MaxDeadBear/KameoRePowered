#include "launcher/http.h"

#include <windows.h>

#include <winhttp.h>

#include <fstream>
#include <vector>

namespace kameo::launcher::http {
namespace {

struct Handle {
  HINTERNET value = nullptr;
  Handle() = default;
  explicit Handle(HINTERNET h) : value(h) {}
  ~Handle() {
    if (value) ::WinHttpCloseHandle(value);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  explicit operator bool() const { return value != nullptr; }
};

std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring wide(size_t(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), length);
  return wide;
}

std::string LastError(const char* what) { return std::string(what) + " failed (error " + std::to_string(::GetLastError()) + ")"; }

// Opens the request and reads the status and length; the body is then pulled with WinHttpReadData.
bool Open(const std::string& url, const char* accept, Handle& session, Handle& connection, Handle& request, uint64_t& length, std::string& error) {
  const std::wstring wide = Widen(url);
  URL_COMPONENTS parts = {sizeof(parts)};
  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = DWORD(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = DWORD(std::size(path));
  wchar_t extra[2048] = {};
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = DWORD(std::size(extra));
  if (!::WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
    error = "not an https address: " + url;
    return false;
  }
  session.value = ::WinHttpOpen(L"KameoLauncher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    error = LastError("WinHttpOpen");
    return false;
  }
  connection.value = ::WinHttpConnect(session.value, host, parts.nPort, 0);
  if (!connection) {
    error = LastError("connecting to the server");
    return false;
  }
  const std::wstring object = std::wstring(path) + extra;
  request.value = ::WinHttpOpenRequest(connection.value, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request) {
    error = LastError("opening the request");
    return false;
  }
  // GitHub's release downloads redirect to its storage host, which WinHTTP follows for https to https.
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  ::WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
  std::wstring headers;
  if (accept) headers = L"Accept: " + Widen(accept) + L"\r\n";
  if (!::WinHttpSendRequest(request.value, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(request.value, nullptr)) {
    error = LastError("contacting the server");
    return false;
  }
  DWORD status = 0;
  DWORD size = sizeof(status);
  ::WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    error = "the server answered " + std::to_string(status) + (status == 403 ? " (GitHub's hourly limit for unsigned requests may be used up; try again later)" : "");
    return false;
  }
  length = 0;
  wchar_t length_text[32] = {};
  size = sizeof(length_text);
  if (::WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_text, &size, WINHTTP_NO_HEADER_INDEX)) length = _wcstoui64(length_text, nullptr, 10);
  return true;
}

template <typename Sink>
bool ReadBody(HINTERNET request, uint64_t length, const Progress& progress, Sink sink, std::string& error) {
  std::vector<char> buffer(1 << 16);
  uint64_t done = 0;
  while (true) {
    DWORD read = 0;
    if (!::WinHttpReadData(request, buffer.data(), DWORD(buffer.size()), &read)) {
      error = LastError("downloading");
      return false;
    }
    if (read == 0) break;
    if (!sink(buffer.data(), read)) {
      error = "could not write the download to disk";
      return false;
    }
    done += read;
    if (progress && !progress(done, length)) {
      error = "cancelled";
      return false;
    }
  }
  if (length && done != length) {
    error = "the download ended early (" + std::to_string(done) + " of " + std::to_string(length) + " bytes)";
    return false;
  }
  return true;
}

}  // namespace

bool Get(const std::string& url, std::string& body, std::string& error, const char* accept) {
  Handle session, connection, request;
  uint64_t length = 0;
  if (!Open(url, accept, session, connection, request, length, error)) return false;
  body.clear();
  return ReadBody(request.value, length, nullptr, [&](const char* data, DWORD size) {
    body.append(data, size);
    return true;
  }, error);
}

bool Download(const std::string& url, const std::filesystem::path& file, const Progress& progress, std::string& error) {
  Handle session, connection, request;
  uint64_t length = 0;
  if (!Open(url, nullptr, session, connection, request, length, error)) return false;
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "could not create " + file.string();
    return false;
  }
  const bool ok = ReadBody(request.value, length, progress, [&](const char* data, DWORD size) {
    out.write(data, size);
    return bool(out);
  }, error);
  out.close();
  if (!ok || !out) {
    std::error_code ec;
    std::filesystem::remove(file, ec);
    if (ok) error = "could not finish writing " + file.string();
    return false;
  }
  return true;
}

}  // namespace kameo::launcher::http
