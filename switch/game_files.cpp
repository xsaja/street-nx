// The game on the Switch: its extracted files (default.xbe with its folders) or a disc image (.iso) in
// sdmc:/switch/street-nx/game. As MVP's Windows host (app/game_files.cpp), a copy is accepted by the XBE certificate's
// title ID 45410072, the whole-file SHA-1 only logged. The image check and the stat calls are BLACK-NX-PORT's.
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <fstream>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

namespace app {

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kTitleId = 0x45410072u;

// the game's folder beside the program, and an assets folder in it (a copy of the Windows package's layout)
std::vector<fs::path> AssetFolders() { return {ExeDir() / "game", ExeDir() / "game" / "assets"}; }

// the XBE title identity without its whole-file hash: certificate keys, media and region flags and other per-copy
// metadata are ignored (app/game_files.cpp)
uint32_t ReadXbeTitleId(const std::vector<uint8_t>& xbe) {
    if (xbe.size() < 0x11C || xbe[0] != 'X' || xbe[1] != 'B' || xbe[2] != 'E' || xbe[3] != 'H') return 0;
    auto u32 = [&](size_t off) {
        return uint32_t(xbe[off]) | (uint32_t(xbe[off + 1]) << 8) | (uint32_t(xbe[off + 2]) << 16) |
               (uint32_t(xbe[off + 3]) << 24);
    };
    const uint32_t base = u32(0x104), address = u32(0x118);
    if (address < base) return 0;
    const size_t cert = size_t(address - base);
    if (cert < 0x11C || cert > xbe.size() || xbe.size() - cert < 0x5C) return 0;
    const uint32_t length = u32(cert);
    if (length < 0x5C || length > xbe.size() - cert) return 0;
    return u32(cert + 8);
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

// an image cut short (a copy that stopped) still opens, its folders being near the start; the game then fails a
// read later. Its files are checked against its size, for the log
void CheckImage(const kernel::Disc& disc, const fs::path& iso) {
    // libstdc++'s file_size fails on the card: stat
    struct stat st;
    if (::stat(iso.c_str(), &st)) {
        LOGE("game: %s: its size cannot be read", iso.string().c_str());
        return;
    }
    uint64_t size = uint64_t(st.st_size);
    uint64_t end = 0;
    size_t files = 0, past = 0;
    std::string first;
    std::vector<std::pair<const kernel::Disc::Entry*, std::string>> todo{{disc.Find(std::string_view()), ""}};
    while (!todo.empty()) {
        auto [dir, path] = todo.back();
        todo.pop_back();
        if (!dir) continue;
        for (const kernel::Disc::Entry& e : dir->children) {
            std::string name = path.empty() ? e.name : path + "\\" + e.name;
            if (e.dir) {
                todo.push_back({&e, name});
                continue;
            }
            files++;
            uint64_t file_end = e.offset + e.size;
            end = std::max(end, file_end);
            if (file_end > size && past++ == 0) first = name;
        }
    }
    LOGR("Game: image %llu bytes, %zu files up to byte %llu", (unsigned long long)size, files, (unsigned long long)end);
    if (past)
        LOGE("game: %s is cut short: %zu of its files end past its %llu bytes, %s the first; copy the image again",
             iso.string().c_str(), past, (unsigned long long)size, first.c_str());
}

}  // namespace

// the extracted files before any image, the first copy with the title ID wins; false when there is none (main.cpp
// tells the player)
bool FindGame(GameFiles& game) {
    auto consider = [&](const fs::path& assets, const fs::path& name, std::vector<uint8_t> xbe,
                        std::unique_ptr<kernel::Disc> disc) {
        std::string sha = Sha1(xbe);
        const uint32_t title = ReadXbeTitleId(xbe);
        const bool match = title == kTitleId;
        LOGR("Game: %s in %s, title %08X, sha1 %s%s", IMAGE_XBE, name.string().c_str(), title, sha.c_str(),
             match ? ", accepted by title ID" : ", wrong title or invalid XBE");
        if (!match) return false;
        game = {assets, std::move(xbe)};
        kernel::MountDisc(std::move(disc));
        return true;
    };

    std::error_code ec;
    for (const fs::path& assets : AssetFolders()) {
        if (!fs::is_directory(assets, ec)) continue;
        std::vector<uint8_t> xbe;
        if (ReadHostFile(assets / IMAGE_XBE, xbe) && consider(assets, assets, std::move(xbe), nullptr)) return true;
        std::vector<fs::path> isos;
        for (auto& e : fs::directory_iterator(assets, ec)) {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
        std::sort(isos.begin(), isos.end());
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
            CheckImage(*disc, iso);
            if (consider(assets, iso, std::move(xbe), std::move(disc))) return true;
        }
    }
    LOGE("game: no %s with title %08X in %s", IMAGE_XBE, kTitleId, (ExeDir() / "game").string().c_str());
    return false;
}

}  // namespace app
