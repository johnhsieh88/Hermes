#pragma once
// Minimal blocking HTTP/1.1 JSON POST (POSIX sockets, no deps) used by the Memory facade to
// reach the local mem0 sidecar. Returns the response body, or "" on ANY failure (sidecar down,
// connect refused, timeout) so memory degrades gracefully — the agent runs fine without it.
#include <arpa/inet.h>
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace hermes {

inline std::string http_post_json(const std::string& host, int port,
                                  const std::string& path, const std::string& body) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) <= 0) { close(fd); return ""; }
    timeval tv{2, 0};                                            // 2 s connect/IO cap — never block the loop long
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { close(fd); return ""; }

    const std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host +
                            "\r\nContent-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    if (write(fd, req.data(), req.size()) < 0) { close(fd); return ""; }

    std::string resp;
    char buf[2048];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) resp.append(buf, static_cast<size_t>(n));
    close(fd);
    const size_t h = resp.find("\r\n\r\n");                      // strip headers → body
    return h == std::string::npos ? std::string() : resp.substr(h + 4);
}

} // namespace hermes
