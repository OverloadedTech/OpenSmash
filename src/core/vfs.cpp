#include "core/vfs.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <cstring>
#include <miniz.h>
#include <sys/stat.h>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace sh {

static bool endsWith(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

struct Vfs::Impl {
    enum Kind { None, Zip, Dir } kind = None;
    std::string root;
    mz_zip_archive zip{};
    std::mutex zipMutex; // one miniz reader = one FILE* (seek + read): reads from several threads must not interleave
    std::map<std::string, int> zipIndex;         // logical name -> zip file index
    std::map<std::string, std::string> dirIndex; // logical name -> real relative path

    ~Impl() {
        if (kind == Zip) mz_zip_reader_end(&zip);
    }

    // Recursive folder scan. Symlinks and junctions are followed so folders assembled from links work, but a link back
    // to a directory already on the current path is skipped. Depth and entry count are capped (game data is at most
    // 6 deep); running out of budget returns false, since that can't be a Smash Hit asset folder.
    static constexpr int kMaxDepth = 16;
#if defined(_WIN32)
    struct DirId {}; // link loops are not tracked on Windows, the depth cap ends them
#else
    struct DirId {
        dev_t dev;
        ino_t ino;
        bool operator==(const DirId& o) const { return dev == o.dev && ino == o.ino; }
    };
#endif
    static bool walk(const std::string& base, const std::string& rel, std::vector<std::string>& out, size_t& budget,
                     int depth, std::vector<DirId>& chain) {
        if (depth > kMaxDepth) return true;
#if defined(_WIN32)
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((base + (rel.empty() ? "" : "/" + rel) + "/*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return true;
        bool ok = true;
        do {
            std::string n = fd.cFileName;
            if (n == "." || n == "..") continue;
            if (budget == 0) {
                ok = false;
                break;
            }
            --budget;
            std::string r = rel.empty() ? n : rel + "/" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!walk(base, r, out, budget, depth + 1, chain)) {
                    ok = false;
                    break;
                }
            } else out.push_back(r);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return ok;
#else
        std::string dir = base + (rel.empty() ? "" : "/" + rel);
        DIR* d = opendir(dir.c_str());
        if (!d) return true;
        bool ok = true;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            if (budget == 0) {
                ok = false;
                break;
            }
            --budget;
            std::string r = rel.empty() ? n : rel + "/" + n;
            struct stat st;
            if (stat((base + "/" + r).c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                DirId id{st.st_dev, st.st_ino};
                if (std::find(chain.begin(), chain.end(), id) != chain.end()) continue; // symlink back to an ancestor
                chain.push_back(id);
                ok = walk(base, r, out, budget, depth + 1, chain);
                chain.pop_back();
                if (!ok) break;
            } else if (S_ISREG(st.st_mode)) out.push_back(r); // no FIFOs/devices: a read would block
        }
        closedir(d);
        return ok;
#endif
    }
};

Vfs::Vfs() : d(new Impl) {}
Vfs::~Vfs() = default;
bool Vfs::mounted() const { return d->kind != Impl::None; }

static std::string logical(std::string n) {
    if (endsWith(n, ".mp3")) {
        // strip only the APK's disguise suffix: "<name>.<ext>.mp3"
        std::string inner = n.substr(0, n.size() - 4);
        if (inner.find('.') != std::string::npos) n = inner;
    }
    return n;
}

bool Vfs::mount(const std::string& path, std::string* err) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        if (err) *err = "cannot stat " + path;
        return false;
    }
    if (st.st_mode & S_IFDIR) {
        d->kind = Impl::Dir;
        d->root = path;
        std::vector<std::string> files;
        size_t budget = 200000; // the APK holds about 2,400 files
#if defined(_WIN32)
        std::vector<Impl::DirId> chain;
#else
        std::vector<Impl::DirId> chain{{st.st_dev, st.st_ino}};
#endif
        const bool walked = Impl::walk(path, "", files, budget, 0, chain);
        if (!walked) {
            if (err) *err = "folder is too large or loops back on itself (not a Smash Hit asset folder): " + path;
            d->kind = Impl::None;
            return false;
        }
        for (auto& f : files) d->dirIndex[logical(f)] = f;
        if (!d->dirIndex.count("game.xml") && d->dirIndex.count("assets/game.xml")) {
            // a plainly unzipped APK (AndroidManifest.xml, META-INF/, assets/...): use the assets/ subtree like the zip
            // branch; the stored relative path keeps its assets/ prefix, so read() still joins it with root
            d->dirIndex.clear();
            for (auto& f : files)
                if (f.rfind("assets/", 0) == 0 && f.rfind("assets/dexopt/", 0) != 0)
                    d->dirIndex[logical(f.substr(7))] = f;
        }
        if (d->dirIndex.empty()) {
            if (err) *err = "directory is empty";
            d->kind = Impl::None;
            return false;
        }
        return true;
    }
    memset(&d->zip, 0, sizeof d->zip);
    if (!mz_zip_reader_init_file(&d->zip, path.c_str(), 0)) {
        if (err) *err = "not a valid zip/apk: " + path;
        return false;
    }
    d->kind = Impl::Zip;
    mz_uint n = mz_zip_reader_get_num_files(&d->zip);
    for (mz_uint i = 0; i < n; i++) {
        if (mz_zip_reader_is_file_a_directory(&d->zip, i)) continue;
        char name[1024];
        mz_zip_reader_get_filename(&d->zip, i, name, sizeof name);
        std::string s = name;
        if (s.rfind("assets/", 0) != 0) continue;
        if (s.rfind("assets/dexopt/", 0) == 0) continue;
        d->zipIndex[logical(s.substr(7))] = (int)i;
    }
    if (d->zipIndex.empty()) {
        if (err) *err = "no assets/ in archive";
        mz_zip_reader_end(&d->zip);
        d->kind = Impl::None;
        return false;
    }
    return true;
}

bool Vfs::exists(const std::string& name) const {
    if (d->kind == Impl::Zip) return d->zipIndex.count(name) != 0;
    if (d->kind == Impl::Dir) return d->dirIndex.count(name) != 0;
    return false;
}

bool Vfs::read(const std::string& name, std::vector<uint8_t>& out) const {
    out.clear();
    if (d->kind == Impl::Zip) {
        auto it = d->zipIndex.find(name);
        if (it == d->zipIndex.end()) return false;
        std::lock_guard<std::mutex> lk(d->zipMutex); // index lookups above are read-only after mount
        mz_zip_archive_file_stat fs;
        if (!mz_zip_reader_file_stat(&d->zip, it->second, &fs)) return false;
        out.resize((size_t)fs.m_uncomp_size);
        if (fs.m_uncomp_size == 0) return true;
        return mz_zip_reader_extract_to_mem(&d->zip, it->second, out.data(), out.size(), 0) != 0;
    }
    if (d->kind == Impl::Dir) {
        auto it = d->dirIndex.find(name);
        if (it == d->dirIndex.end()) return false;
        std::ifstream f(d->root + "/" + it->second, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }
    return false;
}

std::string Vfs::readText(const std::string& name, bool* ok) const {
    std::vector<uint8_t> b;
    bool r = read(name, b);
    if (ok) *ok = r;
    return std::string(b.begin(), b.end());
}

std::vector<std::string> Vfs::list(const std::string& prefix) const {
    std::vector<std::string> r;
    auto scan = [&](const auto& m) {
        for (auto& kv : m)
            if (kv.first.compare(0, prefix.size(), prefix) == 0) r.push_back(kv.first);
    };
    if (d->kind == Impl::Zip) scan(d->zipIndex);
    else if (d->kind == Impl::Dir) scan(d->dirIndex);
    std::sort(r.begin(), r.end());
    return r;
}

bool hasGameData(const Vfs& v, std::string* missing) {
    for (const char* f : {"game.xml", "materials.xml", "shaders/room.glsl"}) // Content + the renderer's first shader
        if (!v.exists(f)) {
            if (missing) *missing = f;
            return false;
        }
    return true;
}

bool mountGame(Vfs& v, const std::string& path, std::string* err) {
    std::string e;
    if (!v.mount(path, &e)) {
        if (err) *err = "Cannot open assets '" + path + "': " + e;
        return false;
    }
    if (!hasGameData(v, &e)) {
        if (err)
            *err = "'" + path + "' does not contain Smash Hit data (" + e +
                   " missing): pass the Smash Hit .apk or the folder it was extracted to";
        return false;
    }
    return true;
}

} // namespace sh
