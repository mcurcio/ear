// Private to ear_server: what the control port accepts from a request (docs/design/batch2.md
// sections 4.6 and 6). Pure functions of the request's text; control_port.cpp applies them.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ear::server {

/// A Host header's name is an IP literal (IPv4 dotted quad, or IPv6 in brackets), "localhost",
/// or one of `allowedHosts`, compared case-insensitively. Anything else, an absent Host
/// included, is refused: a DNS name the operator did not give cannot reach the port (DNS
/// rebinding; Vite's server.allowedHosts and Transmission's rpc-host-whitelist do the same).
bool hostAllowed(std::string_view host, const std::vector<std::string>& allowedHosts);

/// A WebSocket upgrade's Origin: absent, or its host[:port] equal to the Host header
/// (case-insensitive), so another site's page cannot drive the port from a browser.
bool originAllowed(std::optional<std::string_view> origin, std::string_view host);

/// The file a request target names under `root`: its query and fragment dropped, "/" and every
/// target ending in "/" naming index.html. None for a target that does not start with "/" or
/// has a ".." segment: no target leaves the root.
std::optional<std::filesystem::path> fileUnder(const std::filesystem::path& root, std::string_view target);

/// The Content-Type of a served file, by extension.
std::string_view contentType(const std::filesystem::path& file);

} // namespace ear::server
