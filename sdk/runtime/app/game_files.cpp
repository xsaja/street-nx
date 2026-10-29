// Find extracted files or an ISO for this game. Match title identity, not per-copy metadata.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <cstdint>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"


namespace app {

namespace {

namespace fs = std::filesystem;

// Read the XBE title identity without using its whole-file hash. Certificate
// keys, media flags, region flags and other per-copy metadata are ignored.
uint32_t ReadXbeTitleId(const std::vector<uint8_t>& xbe) {
    if (xbe.size() < 0x11C || xbe[0] != 'X' || xbe[1] != 'B' ||
        xbe[2] != 'E' || xbe[3] != 'H') return 0;
    auto u32 = [&](size_t off) {
        return uint32_t(xbe[off]) | (uint32_t(xbe[off + 1]) << 8) |
               (uint32_t(xbe[off + 2]) << 16) | (uint32_t(xbe[off + 3]) << 24);
    };
    const uint32_t base = u32(0x104), address = u32(0x118);
    if (address < base) return 0;
    const size_t cert = size_t(address - base);
    if (cert < 0x11C || cert > xbe.size() || xbe.size() - cert < 0x5C) return 0;
    const uint32_t length = u32(cert);
    if (length < 0x5C || length > xbe.size() - cert) return 0;
    return u32(cert + 8);
}

// assets beside the exe or in any folder above it (the build output)
fs::path FindAssets() {
    auto dir = ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

}  // namespace

bool FindGame(GameFiles& game) {
    fs::path assets = FindAssets();
    std::vector<fs::path> isos;
    std::error_code ec;
    if (!assets.empty())
        for (auto& e : fs::directory_iterator(assets, ec)) {
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
            if (ext == L".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
    std::sort(isos.begin(), isos.end());

    // Prefer extracted files, then the first ISO with the expected title ID.
    fs::path other_name;
    auto consider = [&](const fs::path& name, std::vector<uint8_t> xbe, std::unique_ptr<kernel::Disc> disc) {
        std::string sha = Sha1(xbe);
        const uint32_t title = ReadXbeTitleId(xbe);
        const bool match = title == 0x45410072u;
        LOGR("Game: %s in %s, title %08X, sha1 %s%s", IMAGE_XBE, name.string().c_str(),
             title, sha.c_str(), match ? ", accepted by title ID" : ", wrong title or invalid XBE");
        if (match) {
            game = {assets, std::move(xbe)};
            kernel::MountDisc(std::move(disc));
            return true;
        }
        if (other_name.empty()) {
            other_name = name;
        }
        return false;
    };

    std::vector<uint8_t> xbe;
    if (!assets.empty() && ReadHostFile(assets / IMAGE_XBE, xbe) && consider(assets, std::move(xbe), nullptr)) return true;
    for (const fs::path& iso : isos) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string error;
        if (!disc->Open(iso, error)) {
            LOGW("game: %s: %s", iso.string().c_str(), error.c_str());
            continue;
        }
        if (!disc->Load(IMAGE_XBE, xbe)) {
            LOGW("game: %s has no %s", iso.string().c_str(), IMAGE_XBE);
            continue;
        }
        if (consider(iso, std::move(xbe), std::move(disc))) return true;
    }

    if (other_name.empty()) {
        MessageBoxW(nullptr, kGame.missing, kGame.short_title, MB_ICONERROR);
        return false;
    }
    std::wstring text = other_name.filename().wstring() +
        L" does not contain a valid XBE for this game.\n\nPlease provide the correct original Xbox game files.";
    MessageBoxW(nullptr, text.c_str(), kGame.short_title, MB_ICONERROR);
    return false;
}

}  // namespace app
