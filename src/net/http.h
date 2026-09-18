#pragma once
// Blocking HTTPS requests for the online service: libcurl on Linux, WinHTTP
// on Windows (no extra DLLs to ship). Call from a worker thread; see
// game/online.h for the queue the game uses.
#include <string>
#include <vector>

namespace rl::net {

struct HttpResponse {
    bool ok = false;        // the request completed (status may still be an error code)
    int status = 0;         // HTTP status, 0 when the transport failed
    std::string body;
    std::string error;      // transport error text when !ok
};

// Whether this build can make requests at all (false: no HTTP library at build time).
bool httpAvailable();

// `headers` are full "Name: value" lines. Both time out after `timeoutSeconds`.
HttpResponse httpGet(const std::string& url, const std::vector<std::string>& headers = {}, int timeoutSeconds = 15);
HttpResponse httpPost(const std::string& url, const std::string& body, const std::vector<std::string>& headers = {}, int timeoutSeconds = 20);

}  // namespace rl::net
