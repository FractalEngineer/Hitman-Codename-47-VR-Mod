#pragma once

#include <string>
#include <vector>

namespace h47vr {

struct PinCheck {
    std::string relativePath;
    std::string expected;
    std::string actual;
    bool present = false;
    bool ok = false;
};

struct FingerprintReport {
    std::string appId;
    std::string buildId;
    std::vector<PinCheck> checks;
    bool allPresent = true;
    bool allMatch = true;

    int MissingCount() const;
    int MismatchedCount() const;
};

// Upper-case hex SHA-256 of a file. Returns false when the file cannot be read.
bool Sha256File(const std::wstring& path, std::string& outHex);

// Compares every pinned module under gameDirectory against the build pins
// compiled into this module.
FingerprintReport VerifyInstall(const std::wstring& gameDirectory);

// Multi-line human-readable summary, one line per module.
std::string FormatReport(const FingerprintReport& report);

}  // namespace h47vr
