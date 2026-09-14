#pragma once

#include <string>
#include <vector>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

// Виртуальный диск поверх .ipa: игра не распаковывается, а читается прямо из архива.
// Все гостевые пути внутри бандла начинаются с IPA_MOUNT_PREFIX.
#define IPA_MOUNT_PREFIX "/ipa"
#define IPA_FD_BASE 0x7F000000

// Открывает архив и строит оглавление. outBundleDir — виртуальный путь к <Имя>.app,
// его и надо класть в g_appBundlePath. outExecName — имя исполняемого файла из Info.plist.
bool IpaMount(const std::string& ipaPath, std::string& outBundleDir, std::string& outError);
void IpaUnmount();
bool IpaIsMounted();

inline bool IpaIsVirtualPath(const char* p) {
    return p && p[0] == '/' && strncmp(p, IPA_MOUNT_PREFIX, sizeof(IPA_MOUNT_PREFIX) - 1) == 0 &&
           (p[sizeof(IPA_MOUNT_PREFIX) - 1] == '/' || p[sizeof(IPA_MOUNT_PREFIX) - 1] == '\0');
}
inline bool IpaIsVirtualFd(int fd) { return fd >= IPA_FD_BASE; }

// Срез архива для потребителей, которым нужен настоящий файл (MediaExtractor и т.п.).
// Работает только для записей без сжатия — у них байты лежат непрерывно.
bool IpaGetStoredRange(const std::string& vpath, std::string& outArchive,
                       int64_t& outOffset, int64_t& outLength);

bool IpaExists(const std::string& vpath);
bool IpaStat(const std::string& vpath, struct stat* st);
bool IpaReadAll(const std::string& vpath, std::vector<unsigned char>& out);

// FILE* поверх записи архива (funopen). Годится для fread/fseek/ftell/fgets — они
// в main.cpp и так зовут настоящий libc через unwrap_file.
FILE* IpaFopen(const std::string& vpath);
bool  IpaFileToVirtualFd(FILE* f, int* outFd);

int     IpaOpenFd(const std::string& vpath);
ssize_t IpaRead(int fd, void* buf, size_t n);
ssize_t IpaPread(int fd, void* buf, size_t n, off64_t off);
off_t   IpaLseek(int fd, off_t off, int whence);
int     IpaCloseFd(int fd);
bool    IpaFstat(int fd, struct stat* st);

DIR*           IpaOpendir(const std::string& vpath);
bool           IpaIsVirtualDir(DIR* d);
struct dirent* IpaReaddir(DIR* d);
int            IpaClosedir(DIR* d);

// Диспетчеры: путь/дескриптор может быть как виртуальным, так и настоящим.
// Ими пользуется и загрузчик Mach-O, и обёртки гостевых сисколлов.
int     VfsOpenAny(const char* path, int flags);
ssize_t VfsReadAny(int fd, void* buf, size_t n);
off_t   VfsLseekAny(int fd, off_t off, int whence);
int     VfsCloseAny(int fd);
bool    VfsReadFileAny(const std::string& path, std::vector<unsigned char>& out);
FILE*   VfsFopenAny(const std::string& path, const char* mode);
