#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <optional>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

struct ConsolePause {
    ~ConsolePause() {
        DWORD pids[2];
        if (GetConsoleProcessList(pids, 2) <= 1) {
            std::cout << "\nPress Enter to close..." << std::flush;
            std::string line; std::getline(std::cin, line);
        }
    }
};
#else
struct ConsolePause {};
#endif

namespace fs = std::filesystem;

struct Options {
    std::string path;
    enum Mode { Shrink, Null } mode = Shrink;
    std::string tag = "reach_moa_statue";
    std::string forceGame;
    bool recursive = false;
    bool dryRun    = false;
    bool backup    = true;
    bool assumeYes = false;
    bool quiet     = false;
};
struct Palette {
    uint32_t paletteField;
    uint32_t placementField;
    uint32_t placementStride;
    const char* group;
};
struct GameProfile {
    const char* name;
    const char* id;
    uint32_t    expandMagic;
    Palette     scenery;
    Palette     crates;
};

static const GameProfile PROFILES[] = {

    { "Halo Reach (MCC)",  "reach", 0x50000000u, {0x108, 0x0FC, 0xDC, "scen"}, {0x60C, 0x600, 0xD8, "bloc"} },
    { "Halo 3 (MCC)",      "halo3", 0x00000000u, {0x0C0, 0x0B4, 0xB4, "scen"}, {0x5C8, 0x5BC, 0xB0, "bloc"} },
    { "Halo 3 ODST (MCC)", "odst",  0x00000000u, {0x0DC, 0x0D0, 0xB4, "scen"}, {0x608, 0x5FC, 0xB0, "bloc"} },
    { "Halo 4 (MCC)",      "halo4", 0x4FFF0000u, {0x15C, 0x150, 0x17C,"scen"}, {0x644, 0x638, 0x178,"bloc"} },
};

static const uint32_t HEAD_MAGIC = 0x68656164; // head

class Cache {
public:
    std::vector<uint8_t> d;

    explicit Cache(std::vector<uint8_t> bytes) : d(std::move(bytes)) {}

    size_t size() const { return d.size(); }
    void need(size_t off, size_t n) const {
        if (off > d.size() || n > d.size() - off)
            throw std::runtime_error("read out of bounds");
    }
    uint16_t u16(size_t o) const { need(o,2); return (uint16_t)(d[o] | (d[o+1]<<8)); }
    int16_t  i16(size_t o) const { return (int16_t)u16(o); }
    uint32_t u32(size_t o) const { need(o,4); return (uint32_t)(d[o] | (d[o+1]<<8) | (d[o+2]<<16) | (uint32_t(d[o+3])<<24)); }
    int32_t  i32(size_t o) const { return (int32_t)u32(o); }
    uint64_t u64(size_t o) const { return (uint64_t)u32(o) | ((uint64_t)u32(o+4) << 32); }
    std::array<uint8_t,4> r4(size_t o) const { need(o,4); return { d[o], d[o+1], d[o+2], d[o+3] }; }
    std::string cstr(size_t o) const {
        std::string s; while (o < d.size() && d[o]) s.push_back((char)d[o++]); return s;
    }
    void w16(size_t o, uint16_t v) { need(o,2); d[o]=(uint8_t)v; d[o+1]=(uint8_t)(v>>8); }
    void w32(size_t o, uint32_t v) { need(o,4); d[o]=(uint8_t)v; d[o+1]=(uint8_t)(v>>8); d[o+2]=(uint8_t)(v>>16); d[o+3]=(uint8_t)(v>>24); }

    size_t find4(const char* pat, size_t from=0) const {
        for (size_t i = from; i + 4 <= d.size(); ++i)
            if (d[i]==(uint8_t)pat[0] && d[i+1]==(uint8_t)pat[1] && d[i+2]==(uint8_t)pat[2] && d[i+3]==(uint8_t)pat[3])
                return i;
        return (size_t)-1;
    }
};

static std::array<uint8_t,4> fourccLE(const char* s) { // scen
    return { (uint8_t)s[3], (uint8_t)s[2], (uint8_t)s[1], (uint8_t)s[0] };
}

class Scenario {
public:
    Cache& c;
    // index header/tag table
    size_t   groupFileOff = 0;
    uint32_t nGroups = 0, nTags = 0;
    int64_t  K = 0; // meta virtual-addr -> file-offset delta
    size_t   tagTableOff = 0;
    // tag names
    size_t   nameBlob = 0, nameIdx = 0;
    uint32_t fileCount = 0;
    // resolved game
    const GameProfile* profile = nullptr;
    uint32_t expandMagic = 0;
    size_t   scnrMeta = 0;

    explicit Scenario(Cache& cache) : c(cache) {}

    uint64_t expand(uint32_t p) const {
        return (p == 0 || p == 0xFFFFFFFFu) ? p : (((uint64_t)p << 2) + expandMagic);
    }
    // meta virtual address -> file offset
    std::optional<size_t> convAddr(uint64_t addr) const {
        int64_t off = (int64_t)addr + K;
        if (off < 0 || (uint64_t)off >= c.size()) return std::nullopt;
        return (size_t)off;
    }
    // compressed per-tag/tag-block pointer -> file offset
    std::optional<size_t> metaOff(uint32_t ptr) const {
        if (ptr == 0 || ptr == 0xFFFFFFFFu) return std::nullopt;
        return convAddr(expand(ptr));
    }

    std::string loadHeader() {
        if (c.size() < 0x5000) return "file too small to be a cache";
        if (c.u32(0) != HEAD_MAGIC) return "not an uncompressed 'head' cache (compressed or unsupported)";
        if (c.u32(0x8) != c.size())
            return "header file-length != on-disk size (map is compressed or corrupt)";

        uint32_t mask0    = c.u32(0x4CC);
        fileCount         = c.u32(0x20);
        uint32_t fileData = c.u32(0x24);
        uint32_t fileIdx  = c.u32(0x2C);
        nameBlob = (size_t)((fileData + mask0) & 0xFFFFFFFFu);
        nameIdx  = (size_t)((fileIdx  + mask0) & 0xFFFFFFFFu);
        if (nameBlob >= c.size() || nameIdx >= c.size())
            return "tag-name tables out of range (unexpected header layout)";

        if (!findIndexHeader()) return "could not locate/validate tag index header";
        return "";
    }

    bool findIndexHeader() {
        const std::array<uint8_t,4> rncs = fourccLE("scnr"); // group table entry for scnr
        size_t from = 0;
        for (;;) {
            size_t m = c.find4("sgat", from);
            if (m == (size_t)-1) break;
            from = m + 1;
            for (size_t delta : {size_t(0x48), size_t(0x44), size_t(0x40), size_t(0x4C)}) {
                if (m < delta) continue;
                size_t ih = m - delta;
                if (ih + 0x20 > c.size()) continue;
                uint32_t ng = c.u32(ih), nt = c.u32(ih + 0x10);
                if (ng < 1 || ng > 8192 || nt < 1 || nt > 2000000) continue;
                if ((size_t)ng * 16 > ih) continue;
                size_t gfo = ih - (size_t)ng * 16;
                uint64_t grpA = c.u64(ih + 0x8), tagA = c.u64(ih + 0x18);
                int64_t k = (int64_t)gfo - (int64_t)grpA;
                int64_t tagOff = (int64_t)tagA + k;
                if (tagOff < 0 || (uint64_t)tagOff >= c.size()) continue;
                if (gfo + (size_t)ng * 16 > c.size()) continue;
                bool hasScnr = false;
                for (uint32_t g = 0; g < ng; ++g) { if (c.r4(gfo + (size_t)g*16) == rncs) { hasScnr = true; break; } }
                if (!hasScnr) continue;
                nGroups = ng; nTags = nt; groupFileOff = gfo; K = k;
                tagTableOff = (size_t)tagOff;
                return true;
            }
        }
        return false;
    }

    // tag table/groups/names
    struct TagEntry { int16_t groupIndex; uint16_t salt; uint32_t memAddr; };
    TagEntry tag(uint32_t i) const {
        size_t b = tagTableOff + (size_t)i * 8;
        return { c.i16(b), c.u16(b + 2), c.u32(b + 4) };
    }
    std::array<uint8_t,4> groupMagic(int16_t gi) const {
        if (gi < 0 || (uint32_t)gi >= nGroups) return {0,0,0,0};
        return c.r4(groupFileOff + (size_t)gi * 16);
    }
    int groupIndexOf(const char* fourcc) const {
        auto want = fourccLE(fourcc);
        for (uint32_t g = 0; g < nGroups; ++g) if (c.r4(groupFileOff + (size_t)g*16) == want) return (int)g;
        return -1;
    }
    std::string tagName(uint32_t row) const {
        if (row >= fileCount) return {};
        int32_t o = c.i32(nameIdx + (size_t)row * 4);
        if (o < 0) return {};
        size_t at = nameBlob + (size_t)o;
        if (at >= c.size()) return {};
        return c.cstr(at);
    }
    bool nameEndsWith(uint32_t row, const std::string& suffix) const {
        std::string n = tagName(row);
        return n.size() >= suffix.size() && n.compare(n.size()-suffix.size(), suffix.size(), suffix) == 0;
    }

    std::optional<uint32_t> findScnrRow() const {
        int gi = groupIndexOf("scnr");
        if (gi < 0) return std::nullopt;
        for (uint32_t i = 0; i < nTags; ++i) if (tag(i).groupIndex == gi) return i;
        return std::nullopt;
    }

    struct Block { uint32_t count; uint32_t ptr; };
    Block readBlock(size_t metaBase, uint32_t field) const {
        return { c.u32(metaBase + field), c.u32(metaBase + field + 4) };
    }

    bool paletteLooksValid(size_t scnrBase, const Palette& p, uint32_t& count) const {
        Block b = readBlock(scnrBase, p.paletteField);
        count = b.count;
        if (b.count == 0) return true; // empty palette is fine
        if (b.count > 8192) return false;
        auto base = metaOff(b.ptr);
        if (!base) return false;
        if (*base + (size_t)b.count * 0x10 > c.size()) return false;
        auto wantGroup = fourccLE(p.group);
        for (uint32_t k = 0; k < b.count; ++k) {
            uint32_t dat = c.u32(*base + (size_t)k*0x10 + 0xC);
            if (dat == 0 || dat == 0xFFFFFFFFu) continue;
            uint32_t row = dat & 0xFFFF;
            if (row >= nTags) return false;
            if (groupMagic(tag(row).groupIndex) != wantGroup) return false;
        }
        return true;
    }

    std::string detectGame(const std::string& force) {
        auto scnrRow = findScnrRow();
        if (!scnrRow) return "no scenario (scnr) tag found";
        uint32_t scnrMem = tag(*scnrRow).memAddr;

        for (const auto& prof : PROFILES) {
            if (!force.empty() && force != prof.id) continue;
            expandMagic = prof.expandMagic;
            auto meta = metaOff(scnrMem);
            if (!meta) continue;
            uint32_t sc = 0, cr = 0;
            if (!paletteLooksValid(*meta, prof.scenery, sc)) continue;
            if (!paletteLooksValid(*meta, prof.crates,  cr)) continue;
            if (sc == 0 && cr == 0 && force.empty()) continue;
            profile = &prof; scnrMeta = *meta;
            return "";
        }
        return force.empty() ? "could not determine game/scnr layout (unknown MCC build?)" : "forced game layout did not validate against this map";
    }
};

struct PaletteResult {
    std::string palette; // scenery/crates
    uint32_t oldCount = 0, newCount = 0;
    std::vector<uint32_t> moaIndices;
    uint32_t placementsNeutralized = 0;
    bool shrunk = false;
};

struct EditReport {
    std::vector<PaletteResult> palettes;
    bool changed() const { for (auto& p : palettes) if (!p.moaIndices.empty()) return true; return false; }
};

static EditReport applyEdit(Scenario& s, Options::Mode mode, const std::string& tagSuffix) {
    EditReport rep;
    const GameProfile& g = *s.profile;
    for (const Palette* pal : { &g.scenery, &g.crates }) {
        PaletteResult r; r.palette = (pal == &g.scenery) ? "scenery" : "crates";
        auto blk = s.readBlock(s.scnrMeta, pal->paletteField);
        r.oldCount = r.newCount = blk.count;
        if (blk.count == 0 || blk.count > 8192) { continue; }
        auto base = s.metaOff(blk.ptr);
        if (!base) continue;

        // locate moa entries
        for (uint32_t k = 0; k < blk.count; ++k) {
            uint32_t dat = s.c.u32(*base + (size_t)k*0x10 + 0xC);
            if (dat == 0 || dat == 0xFFFFFFFFu) continue;
            if (s.nameEndsWith(dat & 0xFFFF, tagSuffix)) r.moaIndices.push_back(k);
        }
        if (r.moaIndices.empty()) continue;

        bool doShrink = (mode == Options::Shrink);
        if (doShrink) {
            uint32_t need = blk.count - (uint32_t)r.moaIndices.size();
            for (size_t j = 0; j < r.moaIndices.size(); ++j)
                if (r.moaIndices[j] != need + (uint32_t)j) { doShrink = false; break; }
        }

        if (!doShrink) {
            // null each moa tag_reference to 0xFFFFFFFF (group magic + datum index).
            for (uint32_t k : r.moaIndices) {
                size_t e = *base + (size_t)k*0x10;
                s.c.w32(e + 0x0, 0xFFFFFFFFu);
                s.c.w32(e + 0x4, 0);
                s.c.w32(e + 0x8, 0);
                s.c.w32(e + 0xC, 0xFFFFFFFFu);
            }
        } else {
            uint32_t kmin = r.moaIndices.front();
            auto pb = s.readBlock(s.scnrMeta, pal->placementField);
            auto pbase = s.metaOff(pb.ptr);
            if (pbase && pb.count > 0 && pb.count < 500000 &&
                *pbase + (size_t)pb.count * pal->placementStride <= s.c.size()) {
                for (uint32_t j = 0; j < pb.count; ++j) {
                    size_t po = *pbase + (size_t)j * pal->placementStride; // palette index int16 @ +0
                    int16_t pi = s.c.i16(po);
                    if (pi >= (int16_t)kmin && pi < (int16_t)blk.count) { // referenced a removed entry
                        s.c.w16(po, 0xFFFF);
                        r.placementsNeutralized++;
                    }
                }
            }
            for (uint32_t k : r.moaIndices) { // zero removed tail slots
                size_t e = *base + (size_t)k*0x10;
                for (int b = 0; b < 16; ++b) s.c.d[e + b] = 0;
            }
            r.newCount = blk.count - (uint32_t)r.moaIndices.size();
            s.c.w32(s.scnrMeta + pal->paletteField, r.newCount); // decrement count
            r.shrunk = true;
        }
        rep.palettes.push_back(std::move(r));
    }
    return rep;
}

static std::vector<std::string> verifyEdited(Cache& edited, const std::string& forceGame, const std::string& tagSuffix) {
    std::vector<std::string> issues;
    Scenario s(edited);
    std::string err = s.loadHeader(); if (!err.empty()) { issues.push_back("re-parse header: " + err); return issues; }
    err = s.detectGame(forceGame);    if (!err.empty()) { issues.push_back("re-parse detect: " + err); return issues; }
    const GameProfile& g = *s.profile;
    for (const Palette* pal : { &g.scenery, &g.crates }) {
        std::string pname = (pal == &g.scenery) ? "scenery" : "crates";
        auto blk = s.readBlock(s.scnrMeta, pal->paletteField);
        if (blk.count == 0 || blk.count > 8192) continue;
        auto base = s.metaOff(blk.ptr);
        if (!base) continue;
        for (uint32_t k = 0; k < blk.count; ++k) {
            uint32_t dat = edited.u32(*base + (size_t)k*0x10 + 0xC);
            if (dat == 0 || dat == 0xFFFFFFFFu) continue;
            if (s.nameEndsWith(dat & 0xFFFF, tagSuffix))
                issues.push_back(pname + ": moa still referenced at palette index " + std::to_string(k));
        }
        auto pb = s.readBlock(s.scnrMeta, pal->placementField);
        auto pbase = s.metaOff(pb.ptr);
        if (pbase && pb.count > 0 && pb.count < 500000 &&
            *pbase + (size_t)pb.count * pal->placementStride <= edited.size()) {
            uint32_t oob = 0;
            for (uint32_t j = 0; j < pb.count; ++j) {
                int16_t pi = edited.i16(*pbase + (size_t)j * pal->placementStride);
                if (pi >= 0 && (uint32_t)pi >= blk.count) oob++;
            }
            if (oob) issues.push_back(pname + ": " + std::to_string(oob) + " placement(s) index past palette count " + std::to_string(blk.count));
        }
    }
    return issues;
}

static std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    f.seekg(0, std::ios::end); std::streamoff n = f.tellg(); f.seekg(0);
    std::vector<uint8_t> v((size_t)n);
    if (n && !f.read((char*)v.data(), n)) throw std::runtime_error("read failed " + p.string());
    return v;
}
static void writeFile(const fs::path& p, const std::vector<uint8_t>& v) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + p.string());
    if (!v.empty()) f.write((const char*)v.data(), (std::streamsize)v.size());
    if (!f) throw std::runtime_error("write failed " + p.string());
}
static fs::path backupPath(const fs::path& p) {
    fs::path b = p; b += ".bak";
    int n = 1; while (fs::exists(b)) { b = p; b += ".bak." + std::to_string(n++); }
    return b;
}

struct Summary { int scanned=0, edited=0, skipped=0, errored=0; };

static bool processMap(const fs::path& p, const Options& opt, Summary& sum) {
    sum.scanned++;
    auto say = [&](const std::string& s){ if (!opt.quiet) std::cout << s << "\n"; };
    try {
        Cache cache(readFile(p));
        Scenario s(cache);
        std::string err = s.loadHeader();
        if (!err.empty()) { say("  [skip] " + p.filename().string() + " : " + err); sum.skipped++; return false; }
        err = s.detectGame(opt.forceGame);
        if (!err.empty()) { say("  [skip] " + p.filename().string() + " : " + err); sum.skipped++; return false; }

        EditReport plan;
        {
            Cache probe = cache;                 // copy
            Scenario ps(probe); ps.loadHeader(); ps.detectGame(opt.forceGame);
            plan = applyEdit(ps, opt.mode, opt.tag);
        }
        if (!plan.changed()) { say("  [ ok ] " + p.filename().string() + " (" + s.profile->name + ") : no " + opt.tag + " found"); sum.skipped++; return false; }

        std::string desc = "  [edit] " + p.filename().string() + " (" + s.profile->name + ") :";
        for (auto& r : plan.palettes) {
            if (r.moaIndices.empty()) continue;
            desc += " " + r.palette + "[";
            for (size_t i=0;i<r.moaIndices.size();++i) desc += (i?",":"") + std::to_string(r.moaIndices[i]);
            desc += "]";
            if (r.shrunk) desc += " shrink " + std::to_string(r.oldCount) + "->" + std::to_string(r.newCount)
                                + " (-" + std::to_string(r.placementsNeutralized) + " placements)";
            else          desc += " null(" + std::to_string(r.moaIndices.size()) + ")";
        }
        say(desc);

        if (opt.dryRun) { sum.edited++; return true; }

        EditReport rep = applyEdit(s, opt.mode, opt.tag);
        auto issues = verifyEdited(cache, opt.forceGame, opt.tag);
        if (!issues.empty()) {
            say("  [FAIL] " + p.filename().string() + " : verification failed, NOT written:");
            for (auto& i : issues) say("         - " + i);
            sum.errored++; return false;
        }
        if (opt.backup) {
            fs::path b = backupPath(p);
            fs::copy_file(p, b);
            say("         backup -> " + b.filename().string());
        }
        writeFile(p, cache.d);
        say("         written, verified clean.");
        sum.edited++;
        return true;
    } catch (const std::exception& e) {
        say("  [ERR ] " + p.filename().string() + " : " + e.what());
        sum.errored++; return false;
    }
}

static void usage() {
    std::cout <<
    "Usage: kill-all-moa <file.map | folder> [more paths...] [options]\n"
    "       (or drag one or more .map files onto the .exe)\n\n"
    "Options:\n"
    "  -m, --mode <shrink|null>  shrink (default): remove palette entry, shrink the\n"
    "                            block and neutralize placements; null: just null the\n"
    "                            palette reference (safest, count unchanged)\n"
    "      --null                alias for --mode null\n"
    "      --game <id>           force layout: reach|halo3|odst|halo4 (default: auto)\n"
    "  -r, --recursive           recurse into subfolders when given a folder\n"
    "  -n, --dry-run             analyze and report only; write nothing\n"
    "      --no-backup           do not create a .bak backup before writing\n"
    "  -y, --yes                 do not prompt for confirmation before writing\n"
    "  -q, --quiet               only print the final summary\n"
}

int main(int argc, char** argv) {
    ConsolePause keepWindowOpen;
    Options opt;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i+1 >= argc) { std::cerr << "missing value for " << what << "\n"; std::exit(2); }
            return argv[++i];
        };
        if      (a=="-h"||a=="--help") { usage(); return 0; }
        else if (a=="-m"||a=="--mode") { std::string v=next("--mode"); opt.mode = (v=="null")?Options::Null:Options::Shrink; if(v!="null"&&v!="shrink"){std::cerr<<"bad --mode\n";return 2;} }
        else if (a=="--null")          opt.mode = Options::Null;
        else if (a=="--tag")           opt.tag = next("--tag");
        else if (a=="--game")          opt.forceGame = next("--game");
        else if (a=="-r"||a=="--recursive") opt.recursive = true;
        else if (a=="-n"||a=="--dry-run")   opt.dryRun = true;
        else if (a=="--no-backup")          opt.backup = false;
        else if (a=="-y"||a=="--yes")       opt.assumeYes = true;
        else if (a=="-q"||a=="--quiet")     opt.quiet = true;
        else if (!a.empty() && a[0]=='-') { std::cerr << "unknown option: " << a << "\n"; return 2; }
        else pos.push_back(a);
    }
    if (pos.empty()) { usage(); return 2; }
    if (!opt.forceGame.empty()) {
        bool ok=false; for (auto& p:PROFILES) if (opt.forceGame==p.id) ok=true;
        if (!ok) { std::cerr << "unknown --game '" << opt.forceGame << "' (reach|halo3|odst|halo4)\n"; return 2; }
    }

    std::vector<fs::path> maps;
    for (const auto& ps : pos) {
        fs::path root = ps;
        if (!fs::exists(root)) { std::cerr << "path not found: " << root.string() << "\n"; continue; }
        if (fs::is_directory(root)) {
            auto add = [&](const fs::directory_entry& e){
                if (e.is_regular_file() && e.path().extension() == ".map") maps.push_back(e.path());
            };
            if (opt.recursive) for (auto& e : fs::recursive_directory_iterator(root)) add(e);
            else               for (auto& e : fs::directory_iterator(root)) add(e);
        } else if (root.extension() == ".map") {
            maps.push_back(root);
        } else {
            std::cerr << "not a .map file, ignoring: " << root.string() << "\n";
        }
    }
    std::sort(maps.begin(), maps.end());
    maps.erase(std::unique(maps.begin(), maps.end()), maps.end());
    if (maps.empty()) { std::cerr << "no .map files found\n"; return 1; }

    std::cout << "kill-all-moa: " << maps.size() << " map(s), mode=" << (opt.mode==Options::Shrink?"shrink":"null")
              << ", tag='" << opt.tag << "'"
              << (opt.forceGame.empty()?"":(", game="+opt.forceGame))
              << (opt.dryRun?"  [DRY RUN]":"") << "\n";

    if (!opt.dryRun && !opt.assumeYes) {
        std::cout << "This will modify .map files in place (backups created). Continue? [y/N] " << std::flush;
        std::string line; std::getline(std::cin, line);
        if (line.empty() || (line[0]!='y' && line[0]!='Y')) { std::cout << "aborted.\n"; return 0; }
    }

    Summary sum;
    for (auto& m : maps) processMap(m, opt, sum);

    std::cout << "\nSummary: scanned " << sum.scanned
              << ", " << (opt.dryRun?"would edit ":"edited ") << sum.edited
              << ", skipped " << sum.skipped
              << ", errors " << sum.errored << "\n";
    return sum.errored ? 1 : 0;
}
