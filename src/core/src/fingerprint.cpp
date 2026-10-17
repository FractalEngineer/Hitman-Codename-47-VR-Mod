#include "h47vr/fingerprint.hpp"

#include "h47vr/paths.hpp"
#include "h47vr/pins_generated.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdio>

#pragma comment(lib, "bcrypt.lib")

namespace h47vr {
namespace {

constexpr std::size_t kSha256Bytes = 32;

std::string ToUpperHex(const unsigned char* bytes, std::size_t count) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0x0F]);
    }
    return out;
}

}  // namespace

bool Sha256File(const std::wstring& path, std::string& outHex) {
    outHex.clear();

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    bool ok = false;

    do {
        if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            break;
        }

        DWORD objectSize = 0;
        DWORD written = 0;
        if (::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<PUCHAR>(&objectSize),
                                sizeof(objectSize), &written, 0) < 0) {
            break;
        }
        object.resize(objectSize);

        if (::BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
                               nullptr, 0, 0) < 0) {
            break;
        }

        std::array<unsigned char, 64 * 1024> buffer{};
        for (;;) {
            DWORD read = 0;
            if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                            &read, nullptr)) {
                break;
            }
            if (read == 0) {
                ok = true;
                break;
            }
            if (::BCryptHashData(hash, buffer.data(), read, 0) < 0) {
                break;
            }
        }
        if (!ok) {
            break;
        }

        std::array<unsigned char, kSha256Bytes> digest{};
        if (::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
            ok = false;
            break;
        }
        outHex = ToUpperHex(digest.data(), digest.size());
    } while (false);

    if (hash != nullptr) {
        ::BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    ::CloseHandle(file);

    if (!ok) {
        outHex.clear();
    }
    return ok;
}

int FingerprintReport::MissingCount() const {
    int count = 0;
    for (const PinCheck& check : checks) {
        if (!check.present) {
            ++count;
        }
    }
    return count;
}

int FingerprintReport::MismatchedCount() const {
    int count = 0;
    for (const PinCheck& check : checks) {
        if (check.present && !check.ok) {
            ++count;
        }
    }
    return count;
}

FingerprintReport VerifyInstall(const std::wstring& gameDirectory) {
    FingerprintReport report;
    report.appId = pins::kAppId;
    report.buildId = pins::kBuildId;
    report.checks.reserve(pins::kModuleCount);

    for (std::size_t i = 0; i < pins::kModuleCount; ++i) {
        const pins::ModulePin& pin = pins::kModules[i];

        PinCheck check;
        check.relativePath = pin.relativePath;
        check.expected = pin.sha256;

        std::wstring relative = Widen(pin.relativePath);
        for (wchar_t& ch : relative) {
            if (ch == L'/') {
                ch = L'\\';
            }
        }

        const std::wstring full = JoinPath(gameDirectory, relative);
        check.present = FileExists(full);
        if (check.present) {
            check.ok = Sha256File(full, check.actual) && check.actual == check.expected;
        }

        report.allPresent = report.allPresent && check.present;
        report.allMatch = report.allMatch && check.ok;
        report.checks.push_back(std::move(check));
    }

    return report;
}

std::string FormatReport(const FingerprintReport& report) {
    std::string out;
    char line[512]{};

    ::_snprintf_s(line, sizeof(line), _TRUNCATE,
                  "expected build appid %s buildid %s\n",
                  report.appId.c_str(), report.buildId.c_str());
    out += line;

    for (const PinCheck& check : report.checks) {
        const char* mark = check.ok ? "OK  " : (check.present ? "FAIL" : "MISS");
        ::_snprintf_s(line, sizeof(line), _TRUNCATE, "  [%s] %s\n", mark,
                      check.relativePath.c_str());
        out += line;
        if (check.present && !check.ok) {
            ::_snprintf_s(line, sizeof(line), _TRUNCATE, "         expected %s\n         actual   %s\n",
                          check.expected.c_str(),
                          check.actual.empty() ? "(unreadable)" : check.actual.c_str());
            out += line;
        }
    }

    ::_snprintf_s(line, sizeof(line), _TRUNCATE,
                  "result: %s (%d missing, %d mismatched)\n",
                  report.allMatch ? "SUPPORTED BUILD" : "UNSUPPORTED BUILD",
                  report.MissingCount(), report.MismatchedCount());
    out += line;
    return out;
}

}  // namespace h47vr
