#include "net/http.h"

#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

namespace rl::net {

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

HttpResponse request(const std::string& method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, int timeoutSeconds) {
    HttpResponse r;
    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { r.error = "bad url"; return r; }
    HINTERNET session = WinHttpOpen(L"REDLINE/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { r.error = "WinHttpOpen failed"; return r; }
    const int ms = timeoutSeconds * 1000;
    WinHttpSetTimeouts(session, ms, ms, ms, ms);
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) { r.error = "connect failed"; WinHttpCloseHandle(session); return r; }
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET req = WinHttpOpenRequest(conn, widen(method).c_str(), path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { r.error = "open request failed"; WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return r; }
    std::wstring hdr;
    for (const std::string& h : headers) { hdr += widen(h); hdr += L"\r\n"; }
    const BOOL sent = WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), hdr.empty() ? 0 : static_cast<DWORD>(-1),
                                         body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (sent && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        r.status = static_cast<int>(status);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &got)) break;
            r.body.append(chunk.data(), got);
        }
        r.ok = true;
    } else {
        r.error = "request failed (" + std::to_string(GetLastError()) + ")";
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return r;
}
}  // namespace

bool httpAvailable() { return true; }
HttpResponse httpGet(const std::string& url, const std::vector<std::string>& headers, int timeoutSeconds) { return request("GET", url, "", headers, timeoutSeconds); }
HttpResponse httpPost(const std::string& url, const std::string& body, const std::vector<std::string>& headers, int timeoutSeconds) { return request("POST", url, body, headers, timeoutSeconds); }

}  // namespace rl::net

#elif defined(REDLINE_HAS_CURL)
#include <curl/curl.h>

namespace rl::net {

namespace {
size_t sink(char* ptr, size_t size, size_t nmemb, void* user) {
    static_cast<std::string*>(user)->append(ptr, size * nmemb);
    return size * nmemb;
}

HttpResponse request(const std::string& url, const std::string* body, const std::vector<std::string>& headers, int timeoutSeconds) {
    HttpResponse r;
    static bool inited = false;
    if (!inited) { curl_global_init(CURL_GLOBAL_DEFAULT); inited = true; }
    CURL* c = curl_easy_init();
    if (!c) { r.error = "curl init failed"; return r; }
    curl_slist* list = nullptr;
    for (const std::string& h : headers) list = curl_slist_append(list, h.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "REDLINE/1.0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeoutSeconds));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    if (list) curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    if (body) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
    }
    const CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        r.status = static_cast<int>(status);
        r.ok = true;
    } else {
        r.error = curl_easy_strerror(rc);
    }
    if (list) curl_slist_free_all(list);
    curl_easy_cleanup(c);
    return r;
}
}  // namespace

bool httpAvailable() { return true; }
HttpResponse httpGet(const std::string& url, const std::vector<std::string>& headers, int timeoutSeconds) { return request(url, nullptr, headers, timeoutSeconds); }
HttpResponse httpPost(const std::string& url, const std::string& body, const std::vector<std::string>& headers, int timeoutSeconds) { return request(url, &body, headers, timeoutSeconds); }

}  // namespace rl::net

#else

namespace rl::net {
bool httpAvailable() { return false; }
HttpResponse httpGet(const std::string&, const std::vector<std::string>&, int) { HttpResponse r; r.error = "built without HTTP support"; return r; }
HttpResponse httpPost(const std::string&, const std::string&, const std::vector<std::string>&, int) { HttpResponse r; r.error = "built without HTTP support"; return r; }
}  // namespace rl::net

#endif
