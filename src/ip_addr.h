#pragma once

// IPv4/IPv6 text parsing into raw sockaddr storage.
//
// Deliberately uses inet_pton() only -- never getaddrinfo() or any resolver
// path -- so parsing a client-supplied "ip" value can never trigger a DNS
// lookup or any other outbound network activity.

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <string>

namespace nshgeoip
{

struct ParsedAddr
{
    sockaddr_storage storage{};
    socklen_t len = 0;
    int family = AF_UNSPEC;

    const sockaddr *sockaddr_ptr() const
    {
        return reinterpret_cast<const sockaddr *>(&storage);
    }
};

// Returns true and fills `out` if `text` is a syntactically valid IPv4 or
// IPv6 address literal. No hostname resolution is ever attempted.
bool parse_ip_address(const std::string &text, ParsedAddr &out);

} // namespace nshgeoip
