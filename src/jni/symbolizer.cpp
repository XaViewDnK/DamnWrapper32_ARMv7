#include "symbolizer.h"

#include <elf.h>
#include <link.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define SYM_MAX_ENTRIES 640
#define SYM_MAX_FILES   1024
#define SYM_MAX_DIRS    256
#define SYM_LIB_NAME    "libDamnWrapper32.so"

struct SymEntry {
    uint32_t key;          // адрес в системе координат линковки
    bool     resolved;
    char     text[192];    // " >>> src/jni/main.cpp:1303 (Func)"
};

static SymEntry g_ents[SYM_MAX_ENTRIES];
static int      g_entCount = 0;
static int      g_sorted[SYM_MAX_ENTRIES];
static int      g_pending = 0;

static bool     g_inited = false;
static bool     g_ready = false;
static char     g_status[512] = "символайзер не инициализирован";

static const uint8_t* g_map = nullptr;
static size_t         g_mapSize = 0;
static const uint8_t* g_dbgLine = nullptr;
static size_t         g_dbgLineSize = 0;
static const uint8_t* g_symtab = nullptr;
static size_t         g_symtabSize = 0;
static const char*    g_strtab = nullptr;
static size_t         g_strtabSize = 0;

static uintptr_t g_bias = 0, g_lo = 0, g_hi = 0;
static uint8_t   g_memBuildId[32]; static int g_memBuildIdLen = 0;

// файловая таблица текущего CU
static const char* g_files[SYM_MAX_FILES];
static int         g_fileDir[SYM_MAX_FILES];
static int         g_fileCount = 0;
static const char* g_dirs[SYM_MAX_DIRS];
static int         g_dirCount = 0;

// ------------------------------------------------------------------
// поиск собственного модуля в памяти
// ------------------------------------------------------------------
struct SelfInfo { uintptr_t probe, bias, lo, hi; const ElfW(Phdr)* phdr; int phnum; bool found; };

static int SelfPhdrCallback(struct dl_phdr_info* info, size_t, void* data) {
    SelfInfo* self = (SelfInfo*)data;
    uintptr_t lo = 0, hi = 0; bool any = false, hit = false;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        uintptr_t s = info->dlpi_addr + ph->p_vaddr;
        uintptr_t e = s + ph->p_memsz;
        if (!any || s < lo) lo = s;
        if (!any || e > hi) hi = e;
        any = true;
        if (self->probe >= s && self->probe < e) hit = true;
    }
    if (!hit) return 0;
    self->bias = info->dlpi_addr; self->lo = lo; self->hi = hi;
    self->phdr = info->dlpi_phdr; self->phnum = info->dlpi_phnum;
    self->found = true;
    return 1;
}

static int ReadBuildIdNote(const uint8_t* note, size_t size, uint8_t* out, int outMax) {
    const uint8_t* p = note; const uint8_t* end = note + size;
    while (p + 12 <= end) {
        uint32_t namesz, descsz, type;
        memcpy(&namesz, p, 4); memcpy(&descsz, p + 4, 4); memcpy(&type, p + 8, 4);
        const uint8_t* name = p + 12;
        const uint8_t* desc = name + ((namesz + 3) & ~3u);
        const uint8_t* next = desc + ((descsz + 3) & ~3u);
        if (next > end || next <= p) break;
        if (type == NT_GNU_BUILD_ID && namesz == 4 && memcmp(name, "GNU", 4) == 0) {
            int n = (int)descsz; if (n > outMax) n = outMax;
            memcpy(out, desc, n);
            return n;
        }
        p = next;
    }
    return 0;
}

// ------------------------------------------------------------------
// разбор ELF-файла с отладочной информацией
// ------------------------------------------------------------------
static bool TryOpenDebugFile(const char* path, char* why, size_t whySize) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(why, whySize, "нет файла"); return false; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf32_Ehdr)) {
        close(fd); snprintf(why, whySize, "не читается"); return false;
    }
    size_t size = (size_t)st.st_size;
    void* m = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { snprintf(why, whySize, "mmap не удался"); return false; }

    const uint8_t* base = (const uint8_t*)m;
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)base;
    const uint8_t* dbgLine = nullptr; size_t dbgLineSize = 0;
    const uint8_t* symtab = nullptr; size_t symtabSize = 0;
    const char* strtab = nullptr; size_t strtabSize = 0;
    const uint8_t* noteBid = nullptr; size_t noteBidSize = 0;

    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32) {
        munmap(m, size); snprintf(why, whySize, "не 32-битный ELF"); return false;
    }
    size_t shEnd = (size_t)eh->e_shoff + (size_t)eh->e_shnum * eh->e_shentsize;
    if (eh->e_shoff == 0 || shEnd > size || eh->e_shstrndx >= eh->e_shnum) {
        munmap(m, size); snprintf(why, whySize, "нет таблицы секций"); return false;
    }
    const Elf32_Shdr* sh = (const Elf32_Shdr*)(base + eh->e_shoff);
    if ((size_t)sh[eh->e_shstrndx].sh_offset + sh[eh->e_shstrndx].sh_size > size) {
        munmap(m, size); snprintf(why, whySize, "битая таблица имён секций"); return false;
    }
    const char* shstr = (const char*)(base + sh[eh->e_shstrndx].sh_offset);
    int strtabIdx = -1;

    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_NOBITS) continue;
        if ((size_t)sh[i].sh_offset + sh[i].sh_size > size) continue;
        const char* nm = shstr + sh[i].sh_name;
        const uint8_t* data = base + sh[i].sh_offset;
        if (strcmp(nm, ".debug_line") == 0)            { dbgLine = data; dbgLineSize = sh[i].sh_size; }
        else if (strcmp(nm, ".note.gnu.build-id") == 0) { noteBid = data; noteBidSize = sh[i].sh_size; }
        else if (sh[i].sh_type == SHT_SYMTAB)          { symtab = data; symtabSize = sh[i].sh_size; strtabIdx = (int)sh[i].sh_link; }
    }
    if (strtabIdx > 0 && strtabIdx < eh->e_shnum && (size_t)sh[strtabIdx].sh_offset + sh[strtabIdx].sh_size <= size) {
        strtab = (const char*)(base + sh[strtabIdx].sh_offset);
        strtabSize = sh[strtabIdx].sh_size;
    }

    if (!dbgLine || dbgLineSize == 0) {
        munmap(m, size); snprintf(why, whySize, "нет .debug_line (библиотека обрезана)"); return false;
    }
    if (g_memBuildIdLen > 0) {
        uint8_t fileId[32]; int n = noteBid ? ReadBuildIdNote(noteBid, noteBidSize, fileId, sizeof(fileId)) : 0;
        if (n != g_memBuildIdLen || memcmp(fileId, g_memBuildId, n) != 0) {
            munmap(m, size);
            snprintf(why, whySize, "build-id не совпал с загруженной библиотекой");
            return false;
        }
    }

    g_map = base; g_mapSize = size;
    g_dbgLine = dbgLine; g_dbgLineSize = dbgLineSize;
    g_symtab = symtab; g_symtabSize = symtabSize;
    g_strtab = strtab; g_strtabSize = strtabSize;
    return true;
}

bool SymInit() {
    if (g_inited) return g_ready;
    g_inited = true;

    SelfInfo self; memset(&self, 0, sizeof(self));
    self.probe = (uintptr_t)&SymInit;
    dl_iterate_phdr(SelfPhdrCallback, &self);
    if (!self.found) { snprintf(g_status, sizeof(g_status), "не найден собственный модуль в памяти"); return false; }
    g_bias = self.bias; g_lo = self.lo; g_hi = self.hi;

    for (int i = 0; i < self.phnum; i++) {
        if (self.phdr[i].p_type != PT_NOTE) continue;
        g_memBuildIdLen = ReadBuildIdNote((const uint8_t*)(g_bias + self.phdr[i].p_vaddr),
                                          self.phdr[i].p_memsz, g_memBuildId, sizeof(g_memBuildId));
        if (g_memBuildIdLen > 0) break;
    }

    char self_path[512] = {0};
    Dl_info di;
    if (dladdr((void*)&SymInit, &di) && di.dli_fname) snprintf(self_path, sizeof(self_path), "%s", di.dli_fname);

    const char* candidates[] = {
        self_path,
        "/storage/emulated/0/IDE_Logic/DamnWrapper32_ARMv7/obj/local/armeabi-v7a/" SYM_LIB_NAME,
        "/sdcard/IDE_Logic/DamnWrapper32_ARMv7/obj/local/armeabi-v7a/" SYM_LIB_NAME,
    };
    char report[512] = {0}; int rlen = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (!candidates[i] || candidates[i][0] == '\0') continue;
        char why[128] = {0};
        if (TryOpenDebugFile(candidates[i], why, sizeof(why))) {
            g_ready = true;
            snprintf(g_status, sizeof(g_status), "строки исходников: %s (.debug_line %u КБ%s)",
                     candidates[i], (unsigned)(g_dbgLineSize / 1024), g_symtab ? ", .symtab есть" : "");
            return true;
        }
        rlen += snprintf(report + rlen, sizeof(report) - rlen, "%s%s: %s",
                         rlen ? "; " : "", candidates[i], why);
        if (rlen >= (int)sizeof(report) - 1) break;
    }
    snprintf(g_status, sizeof(g_status), "строк исходников НЕТ (%s)", report);
    return false;
}

const char* SymStatus() { return g_status; }
bool SymReady() { return g_ready; }
bool SymOwnsAddress(uintptr_t a) { return g_lo != 0 && a >= g_lo && a < g_hi; }

// ------------------------------------------------------------------
// имя функции из .symtab
// ------------------------------------------------------------------
static const char* SymFuncName(uint32_t linkAddr, uint32_t* offOut) {
    if (!g_symtab || !g_strtab) return nullptr;
    size_t count = g_symtabSize / sizeof(Elf32_Sym);
    const Elf32_Sym* syms = (const Elf32_Sym*)g_symtab;
    const char* best = nullptr; uint32_t bestAddr = 0;
    for (size_t i = 0; i < count; i++) {
        if (ELF32_ST_TYPE(syms[i].st_info) != STT_FUNC) continue;
        uint32_t v = syms[i].st_value & ~1u;
        if (v == 0 || v > linkAddr) continue;
        uint32_t sz = syms[i].st_size ? syms[i].st_size : 0x1000;
        if (linkAddr >= v + sz) continue;
        if (v >= bestAddr && syms[i].st_name < g_strtabSize) {
            bestAddr = v; best = g_strtab + syms[i].st_name;
        }
    }
    if (best && offOut) *offOut = linkAddr - bestAddr;
    return best;
}

// ------------------------------------------------------------------
// чтение примитивов DWARF
// ------------------------------------------------------------------
static inline uint8_t rd8(const uint8_t*& p, const uint8_t* e) { return p < e ? *p++ : 0; }
static inline uint16_t rd16(const uint8_t*& p, const uint8_t* e) {
    if (p + 2 > e) { p = e; return 0; }
    uint16_t v; memcpy(&v, p, 2); p += 2; return v;
}
static inline uint32_t rd32(const uint8_t*& p, const uint8_t* e) {
    if (p + 4 > e) { p = e; return 0; }
    uint32_t v; memcpy(&v, p, 4); p += 4; return v;
}
static uint64_t rdULEB(const uint8_t*& p, const uint8_t* e) {
    uint64_t r = 0; int s = 0;
    while (p < e) { uint8_t b = *p++; if (s < 64) r |= (uint64_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) break; }
    return r;
}
static int64_t rdSLEB(const uint8_t*& p, const uint8_t* e) {
    int64_t r = 0; int s = 0; uint8_t b = 0;
    while (p < e) { b = *p++; if (s < 64) r |= (int64_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) break; }
    if (s < 64 && (b & 0x40)) r |= -((int64_t)1 << s);
    return r;
}
static const char* rdStr(const uint8_t*& p, const uint8_t* e) {
    const uint8_t* z = (const uint8_t*)memchr(p, 0, (size_t)(e - p));
    if (!z) { p = e; return nullptr; }
    const char* s = (const char*)p; p = z + 1; return s;
}

// ------------------------------------------------------------------
// пакет запросов
// ------------------------------------------------------------------
static inline uint32_t MakeKey(uintptr_t runtimeAddr, bool isReturn) {
    uintptr_t a = runtimeAddr & ~1u;
    if (isReturn && a >= g_lo + 2) a -= 2;
    return (uint32_t)(a - g_bias);
}

void SymBatchReset() { g_entCount = 0; g_pending = 0; }

void SymBatchAdd(uintptr_t runtimeAddr, bool isReturn) {
    if (!g_ready || !SymOwnsAddress(runtimeAddr & ~1u)) return;
    uint32_t key = MakeKey(runtimeAddr, isReturn);
    for (int i = 0; i < g_entCount; i++) if (g_ents[i].key == key) return;
    if (g_entCount >= SYM_MAX_ENTRIES) return;
    g_ents[g_entCount].key = key;
    g_ents[g_entCount].resolved = false;
    g_ents[g_entCount].text[0] = '\0';
    g_entCount++; g_pending++;
}

static void BuildSortedIndex() {
    for (int i = 0; i < g_entCount; i++) g_sorted[i] = i;
    for (int i = 1; i < g_entCount; i++) {          // вставками: записей мало
        int cur = g_sorted[i]; int j = i - 1;
        while (j >= 0 && g_ents[g_sorted[j]].key > g_ents[cur].key) { g_sorted[j + 1] = g_sorted[j]; j--; }
        g_sorted[j + 1] = cur;
    }
}

static int LowerBound(uint32_t key) {
    int lo = 0, hi = g_entCount;
    while (lo < hi) { int mid = (lo + hi) / 2; if (g_ents[g_sorted[mid]].key < key) lo = mid + 1; else hi = mid; }
    return lo;
}

static void ComposeFilePath(int fileIdx, char* out, size_t outSize) {
    out[0] = '\0';
    if (fileIdx < 0 || fileIdx >= g_fileCount || !g_files[fileIdx]) { snprintf(out, outSize, "?"); return; }
    const char* name = g_files[fileIdx];
    char full[512];
    int dir = g_fileDir[fileIdx];
    if (name[0] == '/' || dir <= 0 || dir >= g_dirCount || !g_dirs[dir]) snprintf(full, sizeof(full), "%s", name);
    else snprintf(full, sizeof(full), "%s/%s", g_dirs[dir], name);

    const char* m = strstr(full, "/src/");
    if (m) { snprintf(out, outSize, "%s", m + 1); return; }
    if (strncmp(full, "src/", 4) == 0) { snprintf(out, outSize, "%s", full); return; }
    const char* slash = strrchr(full, '/');
    snprintf(out, outSize, "%s", slash ? slash + 1 : full);
}

static void AssignRange(uint32_t from, uint32_t to, int fileIdx, int line) {
    if (to <= from || g_pending == 0) return;
    for (int i = LowerBound(from); i < g_entCount; i++) {
        SymEntry& e = g_ents[g_sorted[i]];
        if (e.key >= to) break;
        if (e.resolved) continue;
        char path[256]; ComposeFilePath(fileIdx, path, sizeof(path));
        uint32_t off = 0; const char* fn = SymFuncName(e.key, &off);
        if (fn) snprintf(e.text, sizeof(e.text), " >>> %s:%d (%s+0x%x)", path, line, fn, off);
        else    snprintf(e.text, sizeof(e.text), " >>> %s:%d", path, line);
        e.resolved = true; g_pending--;
    }
}

void SymBatchResolve() {
    if (!g_ready || g_pending == 0) return;
    BuildSortedIndex();
    const uint8_t* p = g_dbgLine;
    const uint8_t* fileEnd = g_dbgLine + g_dbgLineSize;

    while (p + 10 <= fileEnd && g_pending > 0) {
        const uint8_t* unitStart = p;
        uint32_t unitLen = rd32(p, fileEnd);
        if (unitLen < 4 || unitLen == 0xffffffffu) break;                 // 64-битный DWARF не поддерживаем
        const uint8_t* unitEnd = unitStart + 4 + unitLen;
        if (unitEnd > fileEnd || unitEnd <= unitStart) break;

        uint16_t ver = rd16(p, unitEnd);
        if (ver < 2 || ver > 4) { p = unitEnd; continue; }                // DWARF5 таблицы пропускаем
        uint32_t hdrLen = rd32(p, unitEnd);
        const uint8_t* prog = p + hdrLen;
        uint8_t minInst = rd8(p, unitEnd);
        if (minInst == 0) minInst = 1;
        if (ver >= 4) rd8(p, unitEnd);                                    // maximum_operations_per_instruction
        rd8(p, unitEnd);                                                  // default_is_stmt
        int8_t lineBase = (int8_t)rd8(p, unitEnd);
        uint8_t lineRange = rd8(p, unitEnd);
        uint8_t opcodeBase = rd8(p, unitEnd);
        if (lineRange == 0) { p = unitEnd; continue; }
        uint8_t stdLens[256]; memset(stdLens, 0, sizeof(stdLens));
        for (int i = 1; i < opcodeBase; i++) stdLens[i] = rd8(p, unitEnd);

        g_dirCount = 1; g_dirs[0] = nullptr;
        while (p < unitEnd) {
            const char* d = rdStr(p, unitEnd);
            if (!d || d[0] == '\0') break;
            if (g_dirCount < SYM_MAX_DIRS) g_dirs[g_dirCount++] = d;
        }
        g_fileCount = 1; g_files[0] = nullptr; g_fileDir[0] = 0;
        while (p < unitEnd) {
            const char* f = rdStr(p, unitEnd);
            if (!f || f[0] == '\0') break;
            uint32_t dirIdx = (uint32_t)rdULEB(p, unitEnd);
            rdULEB(p, unitEnd); rdULEB(p, unitEnd);                       // mtime, length
            if (g_fileCount < SYM_MAX_FILES) { g_files[g_fileCount] = f; g_fileDir[g_fileCount] = (int)dirIdx; g_fileCount++; }
        }

        if (prog < p || prog > unitEnd) { p = unitEnd; continue; }
        p = prog;

        uint32_t address = 0; int file = 1; int line = 1;
        bool haveRow = false; uint32_t prevAddr = 0; int prevFile = 1, prevLine = 1;

        while (p < unitEnd && g_pending > 0) {
            uint8_t op = rd8(p, unitEnd);
            bool emit = false, endSeq = false;

            if (op >= opcodeBase) {
                uint8_t adj = op - opcodeBase;
                address += (adj / lineRange) * minInst;
                line += lineBase + (adj % lineRange);
                emit = true;
            } else if (op == 0) {
                uint32_t len = (uint32_t)rdULEB(p, unitEnd);
                const uint8_t* next = p + len;
                if (next > unitEnd) { p = unitEnd; break; }
                uint8_t sub = rd8(p, next);
                if (sub == 1) { emit = true; endSeq = true; }
                else if (sub == 2) { address = rd32(p, next); }
                else if (sub == 3) {
                    const char* f = rdStr(p, next);
                    uint32_t dirIdx = (uint32_t)rdULEB(p, next);
                    rdULEB(p, next); rdULEB(p, next);
                    if (f && g_fileCount < SYM_MAX_FILES) { g_files[g_fileCount] = f; g_fileDir[g_fileCount] = (int)dirIdx; g_fileCount++; }
                }
                p = next;
            } else {
                switch (op) {
                    case 1: emit = true; break;                                        // copy
                    case 2: address += (uint32_t)rdULEB(p, unitEnd) * minInst; break;  // advance_pc
                    case 3: line += (int)rdSLEB(p, unitEnd); break;                    // advance_line
                    case 4: file = (int)rdULEB(p, unitEnd); break;                     // set_file
                    case 5: rdULEB(p, unitEnd); break;                                 // set_column
                    case 6: case 7: break;                                             // negate_stmt, basic_block
                    case 8: address += ((255 - opcodeBase) / lineRange) * minInst; break; // const_add_pc
                    case 9: address += rd16(p, unitEnd); break;                        // fixed_advance_pc
                    case 10: case 11: break;                                           // prologue_end, epilogue_begin
                    case 12: rdULEB(p, unitEnd); break;                                // set_isa
                    default: for (int i = 0; i < stdLens[op]; i++) rdULEB(p, unitEnd); break;
                }
            }

            if (emit) {
                if (haveRow && address > prevAddr) AssignRange(prevAddr, address, prevFile, prevLine);
                if (endSeq) {
                    haveRow = false; address = 0; file = 1; line = 1;
                } else {
                    haveRow = true; prevAddr = address; prevFile = file; prevLine = line;
                }
            }
        }
        p = unitEnd;
    }
}

const char* SymSuffix(uintptr_t runtimeAddr, bool isReturn) {
    if (!g_ready || !SymOwnsAddress(runtimeAddr & ~1u)) return "";
    uint32_t key = MakeKey(runtimeAddr, isReturn);
    for (int i = 0; i < g_entCount; i++) {
        if (g_ents[i].key == key && g_ents[i].resolved) return g_ents[i].text;
    }
    return "";
}
