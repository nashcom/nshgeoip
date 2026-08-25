// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "text_util.h"

#include <cstdio>

namespace nshgeoip
{

std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);

    for (unsigned char c : in)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += static_cast<char>(c);
            }
        }
    }

    return out;
}

std::string sanitize_header_value(const std::string &in)
{
    std::string out;
    out.reserve(in.size());

    for (unsigned char c : in)
    {
        // Reject CR, LF, and other control/DEL bytes outright; a plain
        // space (0x20) is the only "whitespace-like" byte allowed through.
        if (c == '\r' || c == '\n' || (c < 0x20) || c == 0x7f)
        {
            continue;
        }
        out += static_cast<char>(c);
    }

    return out;
}

} // namespace nshgeoip
