// dk_shaders <shadercache dir> <out dir>: every cached SPIR-V module through dkglsl::FromSpirv and uam. Writes
// <hash>.glsl and <hash>.dksh (or <hash>.err) to the out dir, and a report: how many converted and compiled, each
// failure and every uam warning, with the time per shader.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include "dk_glsl.h"
#include "uam_api.h"

namespace fs = std::filesystem;

namespace {

std::vector<uint32_t> ReadSpirv(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    std::vector<uint32_t> words(size_t(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(words.data()), std::streamsize(words.size() * 4));
    return words;
}

void Write(const fs::path& p, const void* data, size_t size) {
    std::ofstream f(p, std::ios::binary);
    f.write(static_cast<const char*>(data), std::streamsize(size));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: dk_shaders <shadercache dir> <out dir>\n");
        return 2;
    }
    fs::path in = argv[1], out = argv[2];
    fs::create_directories(out);
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(in))
        if (e.path().extension() == ".spv") files.push_back(e.path());
    std::sort(files.begin(), files.end());

    uam::init(true);
    int vertex = 0, pixel = 0, converted = 0, compiled = 0, warned = 0;
    std::vector<double> ms;
    std::map<std::string, int> warnings;
    for (const fs::path& p : files) {
        std::string name = p.stem().string();
        dkglsl::Glsl g = dkglsl::FromSpirv(ReadSpirv(p));
        if (g.text.empty()) {
            std::printf("FAIL convert %s: %s\n", name.c_str(), g.error.c_str());
            Write(out / (name + ".err"), g.error.data(), g.error.size());
            continue;
        }
        converted++;
        (g.stage == dkglsl::Stage::Vertex ? vertex : pixel)++;
        Write(out / (name + ".glsl"), g.text.data(), g.text.size());
        auto t0 = std::chrono::steady_clock::now();
        uam::Result r = uam::compile(g.stage == dkglsl::Stage::Vertex ? uam::Stage::Vertex : uam::Stage::Fragment,
                                     g.text.c_str());
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        if (!r.ok) {
            std::printf("FAIL uam %s (%s):\n%s\n", name.c_str(), g.stage == dkglsl::Stage::Vertex ? "vertex" : "pixel",
                        r.log.c_str());
            Write(out / (name + ".err"), r.log.data(), r.log.size());
            continue;
        }
        compiled++;
        Write(out / (name + ".dksh"), r.dksh.data(), r.dksh.size());
        if (!r.log.empty()) {
            warned++;
            std::printf("warn %s: %s\n", name.c_str(), r.log.c_str());
            for (size_t at = 0, end; at < r.log.size(); at = end + 1) {
                end = r.log.find('\n', at);
                if (end == std::string::npos) end = r.log.size();
                std::string line = r.log.substr(at, end - at);
                if (!line.empty()) warnings[line.substr(0, 90)]++;
            }
        }
    }
    std::sort(ms.begin(), ms.end());
    std::printf("\n%zu SPIR-V modules: %d converted (%d vertex, %d pixel), %d compiled by uam, %d with warnings\n",
                files.size(), converted, vertex, pixel, compiled, warned);
    if (!ms.empty())
        std::printf("uam time per shader: p50 %.1f ms, p90 %.1f ms, max %.1f ms\n", ms[ms.size() / 2],
                    ms[ms.size() * 9 / 10], ms.back());
    for (const auto& [w, n] : warnings) std::printf("  %4d x %s\n", n, w.c_str());
    return converted == int(files.size()) && compiled == converted ? 0 : 1;
}
