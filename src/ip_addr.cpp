// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "ip_addr.h"

#include <arpa/inet.h>
#include <cstring>

namespace nshgeoip
{

namespace
{

// inet_pton() rejects leading/trailing junk on the string it is given, but
// it happily accepts embedded NUL-terminated garbage only up to the first
// NUL -- std::string::c_str() is fine here since query-string decoding
// already rejects raw NUL bytes before this is ever called. Reject anything
// implausibly long up front so we never hand inet_pton() an oversized
// buffer for no reason.
constexpr size_t kMaxAddrTextLen = 45; // longest valid IPv6 literal

} // namespace

bool parse_ip_address(const std::string &text, ParsedAddr &out)
{
    if (text.empty() || text.size() > kMaxAddrTextLen)
    {
        return false;
    }

    in_addr addr4{};
    if (inet_pton(AF_INET, text.c_str(), &addr4) == 1)
    {
        auto *sin = reinterpret_cast<sockaddr_in *>(&out.storage);
        std::memset(&out.storage, 0, sizeof(out.storage));
        sin->sin_family = AF_INET;
        sin->sin_addr = addr4;
        out.len = sizeof(sockaddr_in);
        out.family = AF_INET;
        return true;
    }

    in6_addr addr6{};
    if (inet_pton(AF_INET6, text.c_str(), &addr6) == 1)
    {
        auto *sin6 = reinterpret_cast<sockaddr_in6 *>(&out.storage);
        std::memset(&out.storage, 0, sizeof(out.storage));
        sin6->sin6_family = AF_INET6;
        sin6->sin6_addr = addr6;
        out.len = sizeof(sockaddr_in6);
        out.family = AF_INET6;
        return true;
    }

    return false;
}

} // namespace nshgeoip
