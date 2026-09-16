#include "ipavfs.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <string>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

// ============================================================================
// Оглавление архива
// ============================================================================

struct IpaEntry {
    uint64_t lho = 0;
    uint64_t dataOff = 0;
    uint64_t csize = 0;
    uint64_t usize = 0;
    uint16_t method = 0;
    bool isDir = false;
    bool dataOffResolved = false;
};

struct DirChild {
    std::string name;
    bool isDir;
};

static int g_ipaFd = -1;
static std::string g_ipaPath;
static std::string g_bundleVDir;
static std::map<std::string, IpaEntry> g_entries;
static std::map<std::string, std::vector<DirChild>> g_dirs;
static pthread_mutex_t g_ipaMutex = PTHREAD_MUTEX_INITIALIZER;

static uint16_t Rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t Rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t Rd64(const unsigned char* p) {
    return (uint64_t)Rd32(p) | ((uint64_t)Rd32(p + 4) << 32);
}

bool IpaIsMounted() { return g_ipaFd >= 0; }

// "/ipa/Payload/Game.app/a.png" -> "Payload/Game.app/a.png"
static bool ToRel(const std::string& vpath, std::string& rel) {
    if (!IpaIsVirtualPath(vpath.c_str())) return false;
    rel = vpath.substr(sizeof(IPA_MOUNT_PREFIX) - 1);
    std::string out;
    out.reserve(rel.size());
    for (size_t i = 0; i < rel.size();) {
        if (rel[i] == '/') {
            while (i < rel.size() && rel[i] == '/') i++;
            if (i + 1 < rel.size() && rel[i] == '.' && rel[i + 1] == '/') { i += 2; continue; }
            if (i + 1 == rel.size() && rel[i] == '.') { i++; continue; }
            if (!out.empty()) out += '/';
            continue;
        }
        out += rel[i++];
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    rel = out;
    return true;
}

static void RegisterDirs(const std::string& rel, bool isDir) {
    std::string cur = rel;
    bool childIsDir = isDir;
    while (true) {
        size_t slash = cur.find_last_of('/');
        std::string parent = (slash == std::string::npos) ? std::string() : cur.substr(0, slash);
        std::string name = (slash == std::string::npos) ? cur : cur.substr(slash + 1);
        if (name.empty()) break;
        auto& kids = g_dirs[parent];
        bool found = false;
        for (auto& k : kids) {
            if (k.name == name) { found = true; break; }
        }
        if (!found) kids.push_back({name, childIsDir});
        if (parent.empty()) break;
        cur = parent;
        childIsDir = true;
    }
}

// Смещение данных известно только из локального заголовка: длины имени и extra
// в нём и в центральном каталоге не обязаны совпадать.
static bool ResolveDataOff(IpaEntry& e) {
    if (e.dataOffResolved) return true;
    unsigned char lh[30];
    if (pread64(g_ipaFd, lh, 30, (off64_t)e.lho) != 30) return false;
    if (Rd32(lh) != 0x04034b50) return false;
    e.dataOff = e.lho + 30 + Rd16(lh + 26) + Rd16(lh + 28);
    e.dataOffResolved = true;
    return true;
}

static bool ParseCentralDirectory(const unsigned char* cd, size_t cdSize, uint64_t expectEntries) {
    size_t p = 0;
    uint64_t seen = 0;
    while (p + 46 <= cdSize) {
        if (Rd32(cd + p) != 0x02014b50) break;
        uint16_t method = Rd16(cd + p + 10);
        uint64_t csize = Rd32(cd + p + 20);
        uint64_t usize = Rd32(cd + p + 24);
        uint16_t nlen = Rd16(cd + p + 28);
        uint16_t elen = Rd16(cd + p + 30);
        uint16_t clen = Rd16(cd + p + 32);
        uint64_t lho = Rd32(cd + p + 42);
        if (p + 46 + nlen + elen + clen > cdSize) break;
        std::string name((const char*)cd + p + 46, nlen);

        // ZIP64: настоящие значения лежат в extra-поле 0x0001 в том же порядке,
        // в каком соответствующие 32-битные поля равны 0xFFFFFFFF.
        const unsigned char* ex = cd + p + 46 + nlen;
        size_t exLeft = elen;
        while (exLeft >= 4) {
            uint16_t id = Rd16(ex), sz = Rd16(ex + 2);
            if (sz + 4u > exLeft) break;
            if (id == 0x0001) {
                const unsigned char* q = ex + 4;
                size_t left = sz;
                if (usize == 0xFFFFFFFFu && left >= 8) { usize = Rd64(q); q += 8; left -= 8; }
                if (csize == 0xFFFFFFFFu && left >= 8) { csize = Rd64(q); q += 8; left -= 8; }
                if (lho == 0xFFFFFFFFu && left >= 8) { lho = Rd64(q); q += 8; left -= 8; }
                break;
            }
            ex += 4 + sz;
            exLeft -= 4 + sz;
        }

        p += 46 + nlen + elen + clen;
        seen++;
        if (name.empty()) continue;

        bool isDir = name.back() == '/';
        while (!name.empty() && name.back() == '/') name.pop_back();
        if (name.empty()) continue;

        IpaEntry e;
        e.lho = lho;
        e.csize = csize;
        e.usize = usize;
        e.method = method;
        e.isDir = isDir;
        g_entries[name] = e;
        RegisterDirs(name, isDir);
    }
    (void)expectEntries;
    return !g_entries.empty();
}

bool IpaMount(const std::string& ipaPath, std::string& outBundleDir, std::string& outError) {
    IpaUnmount();

    g_ipaFd = open(ipaPath.c_str(), O_RDONLY);
    if (g_ipaFd < 0) { outError = "не удалось открыть " + ipaPath + ": " + strerror(errno); return false; }
    g_ipaPath = ipaPath;

    off64_t size = lseek64(g_ipaFd, 0, SEEK_END);
    if (size < 22) { outError = "файл слишком мал для zip"; IpaUnmount(); return false; }

    size_t tail = (size_t)std::min<off64_t>(size, 66560);
    std::vector<unsigned char> t(tail);
    if (pread64(g_ipaFd, t.data(), tail, size - (off64_t)tail) != (ssize_t)tail) {
        outError = "не прочитался хвост архива"; IpaUnmount(); return false;
    }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--) {
        if (Rd32(t.data() + i) == 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) { outError = "не найден EOCD — это не zip/.ipa"; IpaUnmount(); return false; }

    uint64_t nent = Rd16(t.data() + eocd + 10);
    uint64_t cdSize = Rd32(t.data() + eocd + 12);
    uint64_t cdOff = Rd32(t.data() + eocd + 16);

    if (nent == 0xFFFFu || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) {
        long loc = -1;
        for (long i = eocd - 20; i >= 0; i--) {
            if (Rd32(t.data() + i) == 0x07064b50) { loc = i; break; }
        }
        if (loc >= 0) {
            uint64_t eocd64Off = Rd64(t.data() + loc + 8);
            unsigned char e64[56];
            if (pread64(g_ipaFd, e64, sizeof(e64), (off64_t)eocd64Off) == (ssize_t)sizeof(e64) &&
                Rd32(e64) == 0x06064b50) {
                nent = Rd64(e64 + 32);
                cdSize = Rd64(e64 + 40);
                cdOff = Rd64(e64 + 48);
            }
        }
    }
    if (cdSize == 0 || cdSize > (uint64_t)size) { outError = "битый центральный каталог"; IpaUnmount(); return false; }

    std::vector<unsigned char> cd((size_t)cdSize);
    if (pread64(g_ipaFd, cd.data(), (size_t)cdSize, (off64_t)cdOff) != (ssize_t)cdSize) {
        outError = "не прочитался центральный каталог"; IpaUnmount(); return false;
    }
    if (!ParseCentralDirectory(cd.data(), (size_t)cdSize, nent)) {
        outError = "в архиве нет ни одной записи"; IpaUnmount(); return false;
    }

    // Ищем Payload/<Что-то>.app
    std::string appDir;
    auto it = g_dirs.find("Payload");
    if (it != g_dirs.end()) {
        for (auto& k : it->second) {
            if (k.name.size() > 4 && k.name.compare(k.name.size() - 4, 4, ".app") == 0) {
                appDir = "Payload/" + k.name;
                break;
            }
        }
    }
    if (appDir.empty()) {
        outError = "в архиве нет Payload/<Имя>.app";
        IpaUnmount();
        return false;
    }

    g_bundleVDir = std::string(IPA_MOUNT_PREFIX) + "/" + appDir;
    outBundleDir = g_bundleVDir;
    return true;
}

void IpaUnmount() {
    if (g_ipaFd >= 0) close(g_ipaFd);
    g_ipaFd = -1;
    g_ipaPath.clear();
    g_entries.clear();
    g_dirs.clear();
    g_bundleVDir.clear();
}

// Для тех потребителей, что умеют только настоящий файл (MediaExtractor, MediaPlayer):
// у STORED-записи байты лежат в архиве непрерывно, поэтому вместо распаковки во времянку
// достаточно отдать им сам архив плюс срез. Для сжатой записи вернём false.
bool IpaGetStoredRange(const std::string& vpath, std::string& outArchive,
                       int64_t& outOffset, int64_t& outLength) {
    std::string rel;
    if (!ToRel(vpath, rel)) return false;
    pthread_mutex_lock(&g_ipaMutex);
    bool ok = false;
    auto it = g_entries.find(rel);
    if (it != g_entries.end() && !it->second.isDir && it->second.method == 0 &&
        ResolveDataOff(it->second)) {
        outArchive = g_ipaPath;
        outOffset = (int64_t)it->second.dataOff;
        outLength = (int64_t)it->second.usize;
        ok = true;
    }
    pthread_mutex_unlock(&g_ipaMutex);
    return ok;
}

// ============================================================================
// Поток чтения записи
// ============================================================================

struct IpaStream {
    IpaEntry e;
    uint64_t pos = 0;
    z_stream zs;
    bool zsInit = false;
    uint64_t zsOut = 0;
    uint64_t zsIn = 0;
    unsigned char inbuf[32768];
};

static void StreamResetInflate(IpaStream* s) {
    if (s->zsInit) inflateEnd(&s->zs);
    memset(&s->zs, 0, sizeof(s->zs));
    inflateInit2(&s->zs, -15);
    s->zsInit = true;
    s->zsIn = 0;
    s->zsOut = 0;
}

static ssize_t StreamInflateInto(IpaStream* s, void* buf, size_t n) {
    s->zs.next_out = (Bytef*)buf;
    s->zs.avail_out = (uInt)n;
    while (s->zs.avail_out > 0) {
        if (s->zs.avail_in == 0) {
            if (s->zsIn >= s->e.csize) break;
            size_t want = (size_t)std::min<uint64_t>(sizeof(s->inbuf), s->e.csize - s->zsIn);
            ssize_t r = pread64(g_ipaFd, s->inbuf, want, (off64_t)(s->e.dataOff + s->zsIn));
            if (r <= 0) break;
            s->zsIn += (uint64_t)r;
            s->zs.next_in = s->inbuf;
            s->zs.avail_in = (uInt)r;
        }
        int rc = inflate(&s->zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_BUF_ERROR) break;
        if (rc == Z_BUF_ERROR && s->zs.avail_in == 0 && s->zsIn >= s->e.csize) break;
    }
    size_t produced = n - s->zs.avail_out;
    s->zsOut += produced;
    return (ssize_t)produced;
}

static ssize_t StreamRead(IpaStream* s, void* buf, size_t n) {
    if (s->pos >= s->e.usize) return 0;
    if ((uint64_t)n > s->e.usize - s->pos) n = (size_t)(s->e.usize - s->pos);
    if (n == 0) return 0;

    if (s->e.method == 0) {
        ssize_t r = pread64(g_ipaFd, buf, n, (off64_t)(s->e.dataOff + s->pos));
        if (r > 0) s->pos += (uint64_t)r;
        return r;
    }

    // Deflate читается только вперёд, поэтому шаг назад означает повтор с начала записи.
    if (!s->zsInit || s->pos < s->zsOut) StreamResetInflate(s);
    unsigned char skip[16384];
    while (s->zsOut < s->pos) {
        size_t want = (size_t)std::min<uint64_t>(sizeof(skip), s->pos - s->zsOut);
        if (StreamInflateInto(s, skip, want) <= 0) return -1;
    }
    ssize_t got = StreamInflateInto(s, buf, n);
    if (got > 0) s->pos += (uint64_t)got;
    return got;
}

static IpaStream* StreamOpen(const std::string& vpath) {
    std::string rel;
    if (!ToRel(vpath, rel)) return nullptr;
    pthread_mutex_lock(&g_ipaMutex);
    auto it = g_entries.find(rel);
    if (it == g_entries.end() || it->second.isDir || !ResolveDataOff(it->second)) {
        pthread_mutex_unlock(&g_ipaMutex);
        return nullptr;
    }
    IpaStream* s = new IpaStream();
    s->e = it->second;
    pthread_mutex_unlock(&g_ipaMutex);
    return s;
}

static void StreamClose(IpaStream* s) {
    if (!s) return;
    if (s->zsInit) inflateEnd(&s->zs);
    delete s;
}

// ============================================================================
// Запросы по путям
// ============================================================================

bool IpaExists(const std::string& vpath) {
    std::string rel;
    if (!ToRel(vpath, rel)) return false;
    if (rel.empty()) return true;
    pthread_mutex_lock(&g_ipaMutex);
    bool ok = g_entries.count(rel) > 0 || g_dirs.count(rel) > 0;
    pthread_mutex_unlock(&g_ipaMutex);
    return ok;
}

bool IpaStat(const std::string& vpath, struct stat* st) {
    std::string rel;
    if (!ToRel(vpath, rel)) return false;
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_blksize = 4096;

    pthread_mutex_lock(&g_ipaMutex);
    bool isDir = rel.empty() || g_dirs.count(rel) > 0;
    auto it = g_entries.find(rel);
    bool isFile = (it != g_entries.end() && !it->second.isDir);
    uint64_t sz = isFile ? it->second.usize : 0;
    pthread_mutex_unlock(&g_ipaMutex);

    if (isFile) {
        st->st_mode = S_IFREG | 0444;
        st->st_size = (off_t)sz;
        st->st_blocks = (blkcnt_t)((sz + 511) / 512);
        return true;
    }
    if (isDir) {
        st->st_mode = S_IFDIR | 0555;
        st->st_size = 4096;
        return true;
    }
    return false;
}

bool IpaReadAll(const std::string& vpath, std::vector<unsigned char>& out) {
    IpaStream* s = StreamOpen(vpath);
    if (!s) return false;
    out.resize((size_t)s->e.usize);
    size_t done = 0;
    while (done < out.size()) {
        ssize_t r = StreamRead(s, out.data() + done, out.size() - done);
        if (r <= 0) break;
        done += (size_t)r;
    }
    StreamClose(s);
    if (done != out.size()) { out.clear(); return false; }
    return true;
}

// ============================================================================
// FILE* через funopen
// ============================================================================

static int FnRead(void* cookie, char* buf, int n) {
    if (n < 0) return -1;
    return (int)StreamRead((IpaStream*)cookie, buf, (size_t)n);
}
static int FnWrite(void* cookie, const char* buf, int n) {
    (void)cookie; (void)buf; (void)n;
    errno = EBADF;
    return -1;
}
static fpos_t FnSeek(void* cookie, fpos_t off, int whence) {
    IpaStream* s = (IpaStream*)cookie;
    int64_t base = 0;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (int64_t)s->pos;
    else if (whence == SEEK_END) base = (int64_t)s->e.usize;
    else { errno = EINVAL; return (fpos_t)-1; }
    int64_t np = base + (int64_t)off;
    if (np < 0) { errno = EINVAL; return (fpos_t)-1; }
    s->pos = (uint64_t)np;
    return (fpos_t)np;
}
static int FnClose(void* cookie) {
    StreamClose((IpaStream*)cookie);
    return 0;
}

FILE* IpaFopen(const std::string& vpath) {
    IpaStream* s = StreamOpen(vpath);
    if (!s) return nullptr;
    FILE* f = funopen(s, FnRead, FnWrite, FnSeek, FnClose);
    if (!f) { StreamClose(s); return nullptr; }
    return f;
}

// ============================================================================
// Виртуальные дескрипторы
// ============================================================================

static std::map<int, IpaStream*> g_vfds;
static int g_nextVfd = IPA_FD_BASE;
static pthread_mutex_t g_vfdMutex = PTHREAD_MUTEX_INITIALIZER;

static IpaStream* VfdGet(int fd) {
    pthread_mutex_lock(&g_vfdMutex);
    auto it = g_vfds.find(fd);
    IpaStream* s = (it == g_vfds.end()) ? nullptr : it->second;
    pthread_mutex_unlock(&g_vfdMutex);
    return s;
}

int IpaOpenFd(const std::string& vpath) {
    IpaStream* s = StreamOpen(vpath);
    if (!s) return -1;
    pthread_mutex_lock(&g_vfdMutex);
    int fd = g_nextVfd++;
    g_vfds[fd] = s;
    pthread_mutex_unlock(&g_vfdMutex);
    return fd;
}

// read()/pread() на обычном файле отдают меньше запрошенного только на EOF. Внутри же
// архива данные приходят порциями: inflate отдаёт столько, сколько влезло в окно, а
// pread64 по FUSE тоже вправе вернуть хвост короче. Игра рассчитывает на семантику
// обычного файла, поэтому дочитываем до конца сами.
long g_ipaShortReadFixups = 0;

static ssize_t StreamReadFull(IpaStream* s, void* buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = StreamRead(s, (char*)buf + done, n - done);
        if (r < 0) return done ? (ssize_t)done : -1;
        if (r == 0) break;
        done += (size_t)r;
        if (done < n) g_ipaShortReadFixups++;
    }
    return (ssize_t)done;
}

ssize_t IpaRead(int fd, void* buf, size_t n) {
    IpaStream* s = VfdGet(fd);
    if (!s) { errno = EBADF; return -1; }
    return StreamReadFull(s, buf, n);
}

ssize_t IpaPread(int fd, void* buf, size_t n, off64_t off) {
    IpaStream* s = VfdGet(fd);
    if (!s) { errno = EBADF; return -1; }
    uint64_t save = s->pos;
    s->pos = (uint64_t)off;
    ssize_t r = StreamReadFull(s, buf, n);
    s->pos = save;
    return r;
}

off_t IpaLseek(int fd, off_t off, int whence) {
    IpaStream* s = VfdGet(fd);
    if (!s) { errno = EBADF; return (off_t)-1; }
    int64_t base = 0;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (int64_t)s->pos;
    else if (whence == SEEK_END) base = (int64_t)s->e.usize;
    else { errno = EINVAL; return (off_t)-1; }
    int64_t np = base + (int64_t)off;
    if (np < 0) { errno = EINVAL; return (off_t)-1; }
    s->pos = (uint64_t)np;
    return (off_t)np;
}

int IpaCloseFd(int fd) {
    pthread_mutex_lock(&g_vfdMutex);
    auto it = g_vfds.find(fd);
    IpaStream* s = nullptr;
    if (it != g_vfds.end()) { s = it->second; g_vfds.erase(it); }
    pthread_mutex_unlock(&g_vfdMutex);
    if (!s) { errno = EBADF; return -1; }
    StreamClose(s);
    return 0;
}

bool IpaFstat(int fd, struct stat* st) {
    IpaStream* s = VfdGet(fd);
    if (!s) return false;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    st->st_size = (off_t)s->e.usize;
    st->st_blocks = (blkcnt_t)((s->e.usize + 511) / 512);
    return true;
}

bool IpaFileToVirtualFd(FILE* f, int* outFd) {
    (void)f;
    (void)outFd;
    return false;
}

// ============================================================================
// Каталоги
// ============================================================================

struct IpaDir {
    std::vector<DirChild> items;
    size_t idx = 0;
};

static std::set<IpaDir*> g_vdirs;
static pthread_mutex_t g_vdirMutex = PTHREAD_MUTEX_INITIALIZER;

DIR* IpaOpendir(const std::string& vpath) {
    std::string rel;
    if (!ToRel(vpath, rel)) return nullptr;
    pthread_mutex_lock(&g_ipaMutex);
    auto it = g_dirs.find(rel);
    if (it == g_dirs.end()) { pthread_mutex_unlock(&g_ipaMutex); return nullptr; }
    IpaDir* d = new IpaDir();
    d->items = it->second;
    pthread_mutex_unlock(&g_ipaMutex);

    pthread_mutex_lock(&g_vdirMutex);
    g_vdirs.insert(d);
    pthread_mutex_unlock(&g_vdirMutex);
    return (DIR*)d;
}

bool IpaIsVirtualDir(DIR* d) {
    if (!d) return false;
    pthread_mutex_lock(&g_vdirMutex);
    bool ok = g_vdirs.count((IpaDir*)d) > 0;
    pthread_mutex_unlock(&g_vdirMutex);
    return ok;
}

struct dirent* IpaReaddir(DIR* dirp) {
    if (!IpaIsVirtualDir(dirp)) return nullptr;
    IpaDir* d = (IpaDir*)dirp;
    if (d->idx >= d->items.size()) return nullptr;
    const DirChild& c = d->items[d->idx++];

    static __thread struct dirent out;
    memset(&out, 0, sizeof(out));
    out.d_ino = d->idx;
    out.d_off = (off64_t)d->idx;
    out.d_reclen = sizeof(struct dirent);
    out.d_type = c.isDir ? DT_DIR : DT_REG;
    size_t n = std::min(c.name.size(), sizeof(out.d_name) - 1);
    memcpy(out.d_name, c.name.data(), n);
    out.d_name[n] = '\0';
    return &out;
}

int IpaClosedir(DIR* dirp) {
    if (!IpaIsVirtualDir(dirp)) return -1;
    IpaDir* d = (IpaDir*)dirp;
    pthread_mutex_lock(&g_vdirMutex);
    g_vdirs.erase(d);
    pthread_mutex_unlock(&g_vdirMutex);
    delete d;
    return 0;
}

// ============================================================================
// Диспетчеры путь/дескриптор
// ============================================================================

int VfsOpenAny(const char* path, int flags) {
    if (IpaIsVirtualPath(path)) return IpaOpenFd(path);
    return open(path, flags);
}

// Загрузчик Mach-O читает сегменты одним вызовом и не проверяет частичное чтение,
// поэтому дочитываем до конца сами.
ssize_t VfsReadAny(int fd, void* buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = IpaIsVirtualFd(fd) ? IpaRead(fd, (char*)buf + done, n - done)
                                       : read(fd, (char*)buf + done, n - done);
        if (r < 0) return done ? (ssize_t)done : r;
        if (r == 0) break;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

off_t VfsLseekAny(int fd, off_t off, int whence) {
    if (IpaIsVirtualFd(fd)) return IpaLseek(fd, off, whence);
    return lseek(fd, off, whence);
}

int VfsCloseAny(int fd) {
    if (IpaIsVirtualFd(fd)) return IpaCloseFd(fd);
    return close(fd);
}

bool VfsReadFileAny(const std::string& path, std::vector<unsigned char>& out) {
    if (IpaIsVirtualPath(path.c_str())) return IpaReadAll(path, out);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(sz > 0 ? (size_t)sz : 0);
    size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
    out.resize(got);
    fclose(f);
    return true;
}

FILE* VfsFopenAny(const std::string& path, const char* mode) {
    if (IpaIsVirtualPath(path.c_str())) {
        if (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')) return nullptr;
        return IpaFopen(path);
    }
    return fopen(path.c_str(), mode);
}
