#pragma once
#include <string>

namespace ModularInput {
inline bool Clean(const std::string& value, size_t limit) {
    if (value.size() > limit)
        return false;
    for (unsigned char c : value)
        if (c < 0x20 || c == 0x7f)
            return false;
    return true;
}
// The upstream WebSocket URL parser does not support IPv6 literals/userinfo.
inline bool HaUrl(const std::string& url, bool external) {
    if (url.empty())
        return true;
    if (!Clean(url, 256))
        return false;
    size_t start;
    if (url.rfind("https://", 0) == 0)
        start = 8;
    else if (url.rfind("wss://", 0) == 0)
        start = 6;
    else if (!external && url.rfind("http://", 0) == 0)
        start = 7;
    else if (!external && url.rfind("ws://", 0) == 0)
        start = 5;
    else
        return false;
    auto end = url.find('/', start);
    auto authority = url.substr(start, end - start);
    auto colon = authority.find(':');
    auto host = authority.substr(0, colon);
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.')
        return false;
    for (unsigned char c : host) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.'))
            return false;
    }
    if (colon != std::string::npos) {
        auto port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5)
            return false;
        unsigned number = 0;
        for (unsigned char c : port) {
            if (c < '0' || c > '9')
                return false;
            number = number * 10 + c - '0';
        }
        if (number == 0 || number > 65535)
            return false;
    }
    if (end != std::string::npos) {
        for (unsigned char c : url.substr(end)) {
            if (c <= 0x20 || c == '@' || c == '?' || c == '#' || c == ':' || c == '\\')
                return false;
        }
    }
    return true;
}
inline bool Cookie(const std::string& cookies, const std::string& session) {
    if (session.empty())
        return false;
    size_t pos = 0;
    while (pos < cookies.size()) {
        auto end = cookies.find(';', pos);
        if (end == std::string::npos)
            end = cookies.size();
        while (pos < end && (cookies[pos] == ' ' || cookies[pos] == '\t'))
            ++pos;
        if (cookies.substr(pos, end - pos) == "s=" + session)
            return true;
        pos = end + 1;
    }
    return false;
}
}  // namespace ModularInput
