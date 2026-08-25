// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "http.h"

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <sstream>

#include "text_util.h"

namespace nshgeoip
{

namespace
{

bool percent_decode(const std::string &in, std::string &out)
{
    out.clear();
    out.reserve(in.size());

    auto hex_val = [](char h) -> int {
        if (h >= '0' && h <= '9')
            return h - '0';
        if (h >= 'a' && h <= 'f')
            return h - 'a' + 10;
        if (h >= 'A' && h <= 'F')
            return h - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < in.size(); ++i)
    {
        char c = in[i];
        if (c == '+')
        {
            out += ' ';
            continue;
        }
        if (c == '%')
        {
            if (i + 2 >= in.size())
            {
                return false;
            }
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi < 0 || lo < 0)
            {
                return false;
            }
            char decoded = static_cast<char>((hi << 4) | lo);
            if (decoded == '\0')
            {
                return false; // reject embedded NUL
            }
            out += decoded;
            i += 2;
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
        {
            return false; // raw control chars / unencoded space are invalid
        }
        out += c;
    }
    return true;
}

std::string trim_spaces(const std::string &s)
{
    size_t start = s.find_first_not_of(' ');
    if (start == std::string::npos)
    {
        return "";
    }
    size_t end = s.find_last_not_of(' ');
    return s.substr(start, end - start + 1);
}

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

ReadResult read_http_head(int fd, std::size_t max_bytes, int timeout_seconds, std::string &out)
{
    out.clear();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    char buf[4096];

    for (;;)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return ReadResult::Timeout;
        }
        int remaining_ms =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, remaining_ms);
        if (pr == 0)
        {
            return ReadResult::Timeout;
        }
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return ReadResult::IoError;
        }

        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0)
        {
            return ReadResult::ConnectionClosed;
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return ReadResult::IoError;
        }

        out.append(buf, static_cast<size_t>(n));
        if (out.size() > max_bytes)
        {
            return ReadResult::TooLarge;
        }
        if (out.find("\r\n\r\n") != std::string::npos)
        {
            return ReadResult::Ok;
        }
    }
}

bool parse_http_request(const std::string &raw, HttpRequest &req)
{
    size_t line_end = raw.find("\r\n");
    if (line_end == std::string::npos)
    {
        return false;
    }
    std::string line = raw.substr(0, line_end);

    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos)
    {
        return false;
    }
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos)
    {
        return false;
    }

    std::string method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string version = line.substr(sp2 + 1);

    if (version != "HTTP/1.1" && version != "HTTP/1.0")
    {
        return false;
    }
    if (method.empty() || target.empty() || target[0] != '/')
    {
        return false;
    }
    for (char c : method)
    {
        if (!std::isupper(static_cast<unsigned char>(c)))
        {
            return false;
        }
    }

    req.method = method;
    req.accept.clear();

    // Scan the remaining header lines up to the blank line that ends them,
    // keeping only Accept -- everything else is intentionally discarded
    // (nshgeoip has no use for it, and this is not a general header parser).
    // A missing/malformed blank-line terminator here just means Accept
    // stays unset; read_http_head() already guarantees the buffer as a
    // whole contains "\r\n\r\n" before parse_http_request() is ever called.
    size_t header_pos = line_end + 2;
    while (header_pos < raw.size())
    {
        size_t next = raw.find("\r\n", header_pos);
        if (next == std::string::npos || next == header_pos)
        {
            break; // incomplete data, or the blank line ending headers
        }
        std::string header_line = raw.substr(header_pos, next - header_pos);
        size_t colon = header_line.find(':');
        if (colon != std::string::npos)
        {
            std::string name = to_lower(trim_spaces(header_line.substr(0, colon)));
            if (name == "accept")
            {
                req.accept = trim_spaces(header_line.substr(colon + 1));
            }
        }
        header_pos = next + 2;
    }

    size_t qpos = target.find('?');
    std::string path_enc = (qpos == std::string::npos) ? target : target.substr(0, qpos);
    std::string query_enc = (qpos == std::string::npos) ? "" : target.substr(qpos + 1);

    if (!percent_decode(path_enc, req.path))
    {
        return false;
    }

    req.query.clear();
    size_t start = 0;
    while (!query_enc.empty() && start <= query_enc.size())
    {
        size_t amp = query_enc.find('&', start);
        std::string pair = (amp == std::string::npos) ? query_enc.substr(start) : query_enc.substr(start, amp - start);
        if (!pair.empty())
        {
            size_t eq = pair.find('=');
            std::string k_enc = (eq == std::string::npos) ? pair : pair.substr(0, eq);
            std::string v_enc = (eq == std::string::npos) ? "" : pair.substr(eq + 1);
            std::string k, v;
            if (!percent_decode(k_enc, k) || !percent_decode(v_enc, v))
            {
                return false;
            }
            req.query.emplace_back(std::move(k), std::move(v));
        }
        if (amp == std::string::npos)
        {
            break;
        }
        start = amp + 1;
    }

    return true;
}

bool find_query_param(const HttpRequest &req, const std::string &key, std::string &value)
{
    for (const auto &kv : req.query)
    {
        if (kv.first == key)
        {
            value = kv.second;
            return true;
        }
    }
    return false;
}

ResponseFormat negotiate_format(const std::string &accept_header)
{
    std::string lower = to_lower(accept_header);
    if (lower.find("application/json") != std::string::npos)
    {
        return ResponseFormat::Json;
    }
    if (lower.find("text/plain") != std::string::npos)
    {
        return ResponseFormat::Text;
    }
    return ResponseFormat::Json;
}

bool accept_wants_json(const std::string &accept_header)
{
    return to_lower(accept_header).find("application/json") != std::string::npos;
}

std::string build_http_response(const HttpResponse &resp, bool include_body)
{
    const char *reason = "Unknown";
    switch (resp.status)
    {
    case 200:
        reason = "OK";
        break;
    case 400:
        reason = "Bad Request";
        break;
    case 404:
        reason = "Not Found";
        break;
    case 405:
        reason = "Method Not Allowed";
        break;
    case 500:
        reason = "Internal Server Error";
        break;
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status << " " << reason << "\r\n";
    // Content-Type describes the body's representation, so it's left off
    // when there is no body to describe (include_body=false, i.e. HEAD).
    if (include_body)
    {
        const char *content_type = "text/plain; charset=utf-8";
        switch (resp.format)
        {
        case ResponseFormat::Json:
            content_type = "application/json";
            break;
        case ResponseFormat::Text:
            content_type = "text/plain; charset=utf-8";
            break;
        case ResponseFormat::Prometheus:
            // version=0.0.4 is the Prometheus text exposition format
            // scrapers expect in Content-Type, not an nshgeoip version.
            content_type = "text/plain; version=0.0.4; charset=utf-8";
            break;
        }
        oss << "Content-Type: " << content_type << "\r\n";
    }
    // Content-Length still reflects the real body size even for HEAD: it
    // tells the client how large the body would have been, which is the
    // one part of HEAD's headers that's still meaningful without a body.
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    oss << "Connection: close\r\n";
    for (const auto &h : resp.headers)
    {
        oss << h.first << ": " << h.second << "\r\n";
    }
    oss << "\r\n";
    if (include_body)
    {
        oss << resp.body;
    }
    return oss.str();
}

HttpResponse make_error_response(int status, const std::string &message, ResponseFormat format)
{
    HttpResponse resp;
    resp.status = status;
    resp.format = format;
    if (format == ResponseFormat::Json)
    {
        resp.body = "{\"error\":\"" + json_escape(message) + "\"}";
    }
    else
    {
        resp.body = "error=" + message + "\n";
    }
    return resp;
}

} // namespace nshgeoip
