#pragma once

#include "slash/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

// What a TLS connection trusts and shows, as Godot's TLSOptions: for wss:// WebSockets.
namespace slash::net {

struct TlsOptions {
    enum class Mode : std::uint8_t { Client, ClientUnsafe, Server };

    Mode mode = Mode::Client;
    // A client's: the certificates (PEM) that may sign the server's, "" for the system's
    // (platform::system_ca_certificates, or the platform's own check on Apple platforms), and the
    // name the server's certificate must carry, "" for the host the URL names.
    std::string trusted_chain;
    std::string common_name;
    // A server's: its private key and its certificate chain, PEM.
    std::string private_key;
    std::string certificate;

    // A client that checks the server's certificate, as TLSOptions.client.
    static TlsOptions client(std::string trusted_chain = {}, std::string common_name = {}) {
        TlsOptions options;
        options.trusted_chain = std::move(trusted_chain);
        options.common_name = std::move(common_name);
        return options;
    }
    // A client that takes any certificate, for testing only, as TLSOptions.client_unsafe.
    static TlsOptions client_unsafe() {
        TlsOptions options;
        options.mode = Mode::ClientUnsafe;
        return options;
    }
    // A server with its key and certificate, as TLSOptions.server.
    static TlsOptions server(std::string private_key, std::string certificate) {
        TlsOptions options;
        options.mode = Mode::Server;
        options.private_key = std::move(private_key);
        options.certificate = std::move(certificate);
        return options;
    }
};

// A certificate that is its own issuer, and its private key, as PEM.
struct SelfSignedCertificate {
    std::string private_key;
    std::string certificate;
};
// Makes one, as Godot's Crypto.generate_self_signed_certificate, with an elliptic curve key
// (P-256) where Godot's is RSA: for tests and servers on a local network, whose clients pass its
// certificate as their trusted chain. Dates are "YYYYMMDDhhmmss"; a client checks the name
// against the CN (common name). Fails where this build has no TLS (the web).
Result<SelfSignedCertificate>
generate_self_signed_certificate(std::string_view issuer_name = "CN=myserver,O=myorganisation,C=IT",
                                 std::string_view not_before = "20140101000000",
                                 std::string_view not_after = "20340101000000");

} // namespace slash::net
