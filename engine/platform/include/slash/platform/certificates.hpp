#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The certificates the operating system trusts to sign servers', for TLS connections, as Godot's
// OS::get_system_ca_certificates.
namespace slash::platform {

// The system's root certificates as PEM text: SSL_CERT_FILE's when it names a file, else Windows'
// root store, the bundle Linux distributions keep (Debian's, Fedora's, openSUSE's, Alpine's) or
// Android's roots. Empty on Apple platforms, whose trust system_trusts asks, and the web.
[[nodiscard]] std::string system_ca_certificates();

// Whether the platform trusts a server's certificate chain (each certificate DER, the server's
// own first) for host, by its own trust settings, as Safari would. Only Apple platforms answer;
// elsewhere false.
[[nodiscard]] bool system_trusts(std::span<const std::span<const std::uint8_t>> chain,
                                 std::string_view host);

// Whether system_trusts answers here.
[[nodiscard]] bool system_trust_available();

} // namespace slash::platform
