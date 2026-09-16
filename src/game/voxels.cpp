#include "game/voxels.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

#include "game/kvx.h"

namespace rl::game {

namespace fs = std::filesystem;

std::optional<fs::path> VoxelModels::findPack(const std::optional<fs::path>& explicitDir, const std::string& prefDir, const std::string& baseDir) {
    auto hasKvx = [](const fs::path& p) {
        std::error_code ec;
        if (!fs::is_directory(p, ec)) return false;
        for (const auto& e : fs::directory_iterator(p, ec)) {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".kvx") return true;
        }
        return false;
    };
    std::vector<fs::path> candidates;
    if (explicitDir) candidates.push_back(*explicitDir);
    if (const char* env = std::getenv("REDLINE_VOXELS")) candidates.emplace_back(env);
    // The pack bundled with the game (MIT licensed): install layouts and the source tree.
    if (!baseDir.empty()) {
        fs::path base(baseDir);
        candidates.push_back(base / "voxels");                                   // Windows zip / installer
        candidates.push_back(base / ".." / "share" / "redline" / "voxel-doom");  // Linux: bin/../share
        candidates.push_back(base / "assets" / "voxel-doom");
        candidates.push_back(base / ".." / "assets" / "voxel-doom");             // build/ next to the checkout
    }
    candidates.emplace_back("assets/voxel-doom");
    candidates.emplace_back("/usr/share/redline/voxel-doom");
    candidates.emplace_back("/usr/local/share/redline/voxel-doom");
    candidates.emplace_back("voxels");
    if (!prefDir.empty()) candidates.emplace_back(fs::path(prefDir) / "voxels");
    if (const char* home = std::getenv("HOME")) {
        candidates.emplace_back(fs::path(home) / "Downloads" / "doom-voxel-models" / "voxel-doom-kvx" / "kvx");
        candidates.emplace_back(fs::path(home) / "Downloads" / "doom-voxel-models" / "voxel-doom-kvx");
    }
    for (const fs::path& c : candidates) {
        if (hasKvx(c)) return c;
        if (hasKvx(c / "kvx")) return c / "kvx";
        if (hasKvx(c / "voxels")) return c / "voxels";
    }
    return std::nullopt;
}

bool VoxelModels::init(render::Renderer& renderer, const fs::path& dir) {
    renderer_ = &renderer;
    dir_ = dir;
    dirName_ = dir.string();
    defs_.clear();
    cache_.clear();
    // VOXELDEF lives next to the kvx folder in the pack, or inside it.
    for (const fs::path& p : {dir / "VOXELDEF.txt", dir.parent_path() / "VOXELDEF.txt", dir / "voxeldef.txt"}) {
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        parseVoxelDef(text);
        break;
    }
    // Any .kvx without a definition is still usable under its own name.
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string stem = e.path().stem().string(), ext = e.path().extension().string();
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".kvx" && !defs_.count(stem)) defs_[stem] = Def{stem, 1.f, 90.f};
    }
    std::fprintf(stderr, "[voxels] %s: %zu models\n", dirName_.c_str(), defs_.size());
    return !defs_.empty();
}

// VOXELDEF grammar (GZDoom): `sprite[, sprite...] = "file" [{ Option = value ... }]`,
// with // and /* */ comments.
void VoxelModels::parseVoxelDef(const std::string& raw) {
    std::string text;
    text.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        if (raw.compare(i, 2, "/*") == 0) { size_t e = raw.find("*/", i + 2); i = e == std::string::npos ? raw.size() : e + 2; continue; }
        if (raw.compare(i, 2, "//") == 0) { size_t e = raw.find('\n', i); i = e == std::string::npos ? raw.size() : e; continue; }
        text.push_back(raw[i++]);
    }
    size_t pos = 0;
    auto skipWs = [&]() { while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos; };
    auto ident = [&]() {
        skipWs();
        size_t s = pos;
        while (pos < text.size() && (std::isalnum(static_cast<unsigned char>(text[pos])) || text[pos] == '_' || text[pos] == '.' || text[pos] == '-' || text[pos] == '+')) ++pos;
        return text.substr(s, pos - s);
    };
    while (true) {
        std::vector<std::string> names;
        std::string n = ident();
        if (n.empty()) break;
        names.push_back(n);
        skipWs();
        while (pos < text.size() && text[pos] == ',') { ++pos; names.push_back(ident()); skipWs(); }
        if (pos >= text.size() || text[pos] != '=') break;
        ++pos;
        skipWs();
        std::string file;
        if (pos < text.size() && text[pos] == '"') {
            size_t e = text.find('"', pos + 1);
            if (e == std::string::npos) break;
            file = text.substr(pos + 1, e - pos - 1);
            pos = e + 1;
        } else {
            file = ident();
        }
        Def def;
        def.file = file;
        skipWs();
        if (pos < text.size() && text[pos] == '{') {
            size_t e = text.find('}', pos);
            std::string body = text.substr(pos + 1, e == std::string::npos ? std::string::npos : e - pos - 1);
            pos = e == std::string::npos ? text.size() : e + 1;
            std::istringstream ss(body);
            std::string tok;
            while (ss >> tok) {
                std::string key = tok, val;
                size_t eq = key.find('=');
                if (eq != std::string::npos) { val = key.substr(eq + 1); key = key.substr(0, eq); }
                if (val.empty()) { std::string t2; if (ss >> t2) { if (t2 == "=") ss >> val; else val = t2.front() == '=' ? t2.substr(1) : t2; } }
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (key == "angleoffset") def.angleOffset = static_cast<float>(std::atof(val.c_str()));
                else if (key == "scale") def.scale = static_cast<float>(std::atof(val.c_str()));
            }
        }
        for (std::string nm : names) {
            std::transform(nm.begin(), nm.end(), nm.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!nm.empty()) defs_[nm] = def;
        }
    }
}

int VoxelModels::addPack(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    int added = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string stem = e.path().stem().string(), ext = e.path().extension().string();
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".kvx" || defs_.count(stem)) continue;
        Def d;
        d.file = stem;
        d.dir = dir;
        defs_[stem] = d;
        cache_.erase(stem);
        ++added;
    }
    return added;
}

const VoxelModel* VoxelModels::get(const std::string& atlasKey) {
    if (!renderer_ || defs_.empty()) return nullptr;
    // "TROO_A" -> "trooa"; anything else (procedural keys) has no voxel.
    if (atlasKey.size() != 6 || atlasKey[4] != '_') return nullptr;
    std::string name;
    for (size_t i = 0; i < atlasKey.size(); ++i) if (i != 4) name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(atlasKey[i]))));
    auto it = cache_.find(name);
    if (it != cache_.end()) return it->second.get();
    auto d = defs_.find(name);
    if (d == defs_.end()) { cache_[name] = nullptr; return nullptr; }
    std::string file = d->second.file;
    const fs::path& base = d->second.dir.empty() ? dir_ : d->second.dir;
    fs::path p = base / (file + ".kvx");
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        std::string upper = file;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        p = base / (upper + ".KVX");
        if (!fs::exists(p, ec)) p = base / (upper + ".kvx");
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) { std::fprintf(stderr, "[voxels] missing %s\n", p.string().c_str()); cache_[name] = nullptr; return nullptr; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    VoxelMeshData mesh;
    std::string err;
    if (!loadKvx(bytes, mesh, &err) || mesh.idx.empty()) {
        std::fprintf(stderr, "[voxels] %s: %s\n", p.string().c_str(), err.empty() ? "empty" : err.c_str());
        cache_[name] = nullptr;
        return nullptr;
    }
    auto m = std::make_unique<VoxelModel>();
    m->mesh = renderer_->createMesh(mesh.verts, mesh.idx);
    m->pivot = mesh.pivot;
    m->scale = d->second.scale;
    m->angleOffset = d->second.angleOffset;
    m->sizeX = mesh.sizeX; m->sizeY = mesh.sizeY; m->sizeZ = mesh.sizeZ;
    m->quads = mesh.quads();
    ++loaded_;
    bytes_ += mesh.verts.size() * sizeof(render::MeshVertex) + mesh.idx.size() * sizeof(uint32_t);
    const VoxelModel* out = m.get();
    cache_[name] = std::move(m);
    return out;
}

}  // namespace rl::game
