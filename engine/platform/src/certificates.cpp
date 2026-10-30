#include "slash/platform/certificates.hpp"

#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <array>
#include <cstddef>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <wincrypt.h>
// clang-format on
#endif

namespace slash::platform {
namespace {

#if defined(_WIN32)
// A DER certificate as PEM text.
std::string pem_of(const std::uint8_t* der, std::size_t size) {
    static constexpr std::string_view kDigits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string base64;
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t a = der[i];
        const std::uint32_t b = i + 1 < size ? der[i + 1] : 0U;
        const std::uint32_t c = i + 2 < size ? der[i + 2] : 0U;
        const std::uint32_t triple = (a << 16U) | (b << 8U) | c;
        base64 += kDigits[(triple >> 18U) & 63U];
        base64 += kDigits[(triple >> 12U) & 63U];
        base64 += i + 1 < size ? kDigits[(triple >> 6U) & 63U] : '=';
        base64 += i + 2 < size ? kDigits[triple & 63U] : '=';
    }
    std::string pem = "-----BEGIN CERTIFICATE-----\n";
    for (std::size_t i = 0; i < base64.size(); i += 64) {
        pem += base64.substr(i, 64) + "\n";
    }
    return pem + "-----END CERTIFICATE-----\n";
}
#endif

} // namespace

std::string system_ca_certificates() {
    if (const std::string file = environment_variable("SSL_CERT_FILE"); !file.empty()) {
        if (auto text = read_file(file)) {
            return std::move(text).value();
        }
    }
#if defined(_WIN32)
    // As Godot's OS_Windows: the root store's certificates.
    std::string pem;
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (store == nullptr) {
        return pem;
    }
    for (PCCERT_CONTEXT certificate = CertEnumCertificatesInStore(store, nullptr);
         certificate != nullptr; certificate = CertEnumCertificatesInStore(store, certificate)) {
        pem += pem_of(certificate->pbCertEncoded, certificate->cbCertEncoded);
    }
    CertCloseStore(store, 0);
    return pem;
#elif defined(__ANDROID__)
    // Where Android keeps its roots, a PEM file each: the updatable module's since Android 14,
    // then the system's.
    for (const char* dir :
         {"/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts"}) {
        auto files = list_files(dir);
        if (!files || files.value().empty()) {
            continue;
        }
        std::string pem;
        for (const std::string& name : files.value()) {
            if (auto text = read_file(std::string(dir) + "/" + name)) {
                pem += text.value();
                pem += "\n";
            }
        }
        return pem;
    }
    return {};
#elif defined(__linux__)
    // As Godot's OS_LinuxBSD: the first bundle the distribution keeps.
    static constexpr std::array<const char*, 6> kBundles{
        "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch
        "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                            // openSUSE
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS
        "/etc/ssl/cert.pem",                                 // Alpine
        "/etc/pki/tls/cacert.pem",                           // OpenELEC
    };
    for (const char* bundle : kBundles) {
        if (auto text = read_file(bundle); text && !text.value().empty()) {
            return std::move(text).value();
        }
    }
    return {};
#else
    return {};
#endif
}

bool system_trust_available() {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

bool system_trusts([[maybe_unused]] std::span<const std::span<const std::uint8_t>> chain,
                   [[maybe_unused]] std::string_view host) {
#if defined(__APPLE__)
    CFMutableArrayRef certificates = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    for (const std::span<const std::uint8_t> der : chain) {
        CFDataRef data = CFDataCreate(nullptr, der.data(), static_cast<CFIndex>(der.size()));
        SecCertificateRef certificate = SecCertificateCreateWithData(nullptr, data);
        CFRelease(data);
        if (certificate != nullptr) {
            CFArrayAppendValue(certificates, certificate);
            CFRelease(certificate);
        }
    }
    const std::string text(host);
    CFStringRef name = CFStringCreateWithCString(nullptr, text.c_str(), kCFStringEncodingUTF8);
    SecPolicyRef policy = SecPolicyCreateSSL(true, name);
    SecTrustRef trust = nullptr;
    const bool trusted =
        CFArrayGetCount(certificates) > 0 &&
        SecTrustCreateWithCertificates(certificates, policy, &trust) == errSecSuccess &&
        SecTrustEvaluateWithError(trust, nullptr);
    if (trust != nullptr) {
        CFRelease(trust);
    }
    CFRelease(policy);
    CFRelease(name);
    CFRelease(certificates);
    return trusted;
#else
    return false;
#endif
}

} // namespace slash::platform
