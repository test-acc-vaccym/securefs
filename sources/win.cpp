#ifdef _WIN32
#include "logger.h"
#include "platform.h"

#include <winfsp/winfsp.h>

#include <cerrno>
#include <limits>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <time.h>

#include <Windows.h>
#include <io.h>
#include <sddl.h>

static inline uint64_t convert_dword_pair(uint64_t low_part, uint64_t high_part)
{
    return low_part | (high_part << 32);
}

static void filetime_to_unix_time(const FILETIME* ft, struct fuse_timespec* out)
{
    long long ll = (static_cast<long long>(ft->dwHighDateTime) << 32)
        + static_cast<long long>(ft->dwLowDateTime) - 116444736000000000LL;
    static const long long FACTOR = 10000000LL;
    out->tv_sec = ll / FACTOR;
    out->tv_nsec = (ll % FACTOR) * 100;
}

static FILETIME unix_time_to_filetime(const fuse_timespec* t)
{
    long long ll = t->tv_sec * 10000000LL + t->tv_nsec / 100LL + 116444736000000000LL;
    FILETIME res;
    res.dwLowDateTime = (DWORD)ll;
    res.dwHighDateTime = (DWORD)(ll >> 32);
    return res;
}

static const DWORD MAX_SINGLE_BLOCK = std::numeric_limits<DWORD>::max();

namespace securefs
{
class WindowsException : public SystemException
{
private:
    DWORD err;
    std::string msg;

public:
    explicit WindowsException(DWORD err, std::string msg) : err(err), msg(std::move(msg)) {}
    const char* type_name() const noexcept override { return "WindowsException"; }
    std::string message() const override
    {
        char buffer[2000];

        if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM,
                            nullptr,
                            err,
                            MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                            buffer,
                            sizeof(buffer),
                            nullptr))
            return strprintf("error %lu (%s)", err, msg.c_str());
        auto str = strprintf("error %lu (%s) %s", err, msg.c_str(), buffer);
        while (str.size() > 0 && (str.back() == ' ' || str.back() == '\r' || str.back() == '\n'))
            str.pop_back();
        return str;
    }
    DWORD win32_code() const noexcept { return err; }
    int error_number() const noexcept override
    {
        static int errorTable[] = {
            0,
            EINVAL,       /* ERROR_INVALID_FUNCTION	1 */
            ENOENT,       /* ERROR_FILE_NOT_FOUND		2 */
            ENOENT,       /* ERROR_PATH_NOT_FOUND		3 */
            EMFILE,       /* ERROR_TOO_MANY_OPEN_FILES	4 */
            EACCES,       /* ERROR_ACCESS_DENIED		5 */
            EBADF,        /* ERROR_INVALID_HANDLE		6 */
            ENOMEM,       /* ERROR_ARENA_TRASHED		7 */
            ENOMEM,       /* ERROR_NOT_ENOUGH_MEMORY	8 */
            ENOMEM,       /* ERROR_INVALID_BLOCK		9 */
            E2BIG,        /* ERROR_BAD_ENVIRONMENT	10 */
            ENOEXEC,      /* ERROR_BAD_FORMAT		11 */
            EACCES,       /* ERROR_INVALID_ACCESS		12 */
            EINVAL,       /* ERROR_INVALID_DATA		13 */
            EFAULT,       /* ERROR_OUT_OF_MEMORY		14 */
            ENOENT,       /* ERROR_INVALID_DRIVE		15 */
            EACCES,       /* ERROR_CURRENT_DIRECTORY	16 */
            EXDEV,        /* ERROR_NOT_SAME_DEVICE	17 */
            ENOENT,       /* ERROR_NO_MORE_FILES		18 */
            EROFS,        /* ERROR_WRITE_PROTECT		19 */
            ENXIO,        /* ERROR_BAD_UNIT		20 */
            EBUSY,        /* ERROR_NOT_READY		21 */
            EIO,          /* ERROR_BAD_COMMAND		22 */
            EIO,          /* ERROR_CRC			23 */
            EIO,          /* ERROR_BAD_LENGTH		24 */
            EIO,          /* ERROR_SEEK			25 */
            EIO,          /* ERROR_NOT_DOS_DISK		26 */
            ENXIO,        /* ERROR_SECTOR_NOT_FOUND	27 */
            EBUSY,        /* ERROR_OUT_OF_PAPER		28 */
            EIO,          /* ERROR_WRITE_FAULT		29 */
            EIO,          /* ERROR_READ_FAULT		30 */
            EIO,          /* ERROR_GEN_FAILURE		31 */
            EACCES,       /* ERROR_SHARING_VIOLATION	32 */
            EACCES,       /* ERROR_LOCK_VIOLATION		33 */
            ENXIO,        /* ERROR_WRONG_DISK		34 */
            ENFILE,       /* ERROR_FCB_UNAVAILABLE	35 */
            ENFILE,       /* ERROR_SHARING_BUFFER_EXCEEDED	36 */
            EINVAL,       /* 37 */
            EINVAL,       /* 38 */
            ENOSPC,       /* ERROR_HANDLE_DISK_FULL	39 */
            EINVAL,       /* 40 */
            EINVAL,       /* 41 */
            EINVAL,       /* 42 */
            EINVAL,       /* 43 */
            EINVAL,       /* 44 */
            EINVAL,       /* 45 */
            EINVAL,       /* 46 */
            EINVAL,       /* 47 */
            EINVAL,       /* 48 */
            EINVAL,       /* 49 */
            ENODEV,       /* ERROR_NOT_SUPPORTED		50 */
            EBUSY,        /* ERROR_REM_NOT_LIST		51 */
            EEXIST,       /* ERROR_DUP_NAME		52 */
            ENOENT,       /* ERROR_BAD_NETPATH		53 */
            EBUSY,        /* ERROR_NETWORK_BUSY		54 */
            ENODEV,       /* ERROR_DEV_NOT_EXIST		55 */
            EAGAIN,       /* ERROR_TOO_MANY_CMDS		56 */
            EIO,          /* ERROR_ADAP_HDW_ERR		57 */
            EIO,          /* ERROR_BAD_NET_RESP		58 */
            EIO,          /* ERROR_UNEXP_NET_ERR		59 */
            EINVAL,       /* ERROR_BAD_REM_ADAP		60 */
            EFBIG,        /* ERROR_PRINTQ_FULL		61 */
            ENOSPC,       /* ERROR_NO_SPOOL_SPACE		62 */
            ENOENT,       /* ERROR_PRINT_CANCELLED	63 */
            ENOENT,       /* ERROR_NETNAME_DELETED	64 */
            EACCES,       /* ERROR_NETWORK_ACCESS_DENIED	65 */
            ENODEV,       /* ERROR_BAD_DEV_TYPE		66 */
            ENOENT,       /* ERROR_BAD_NET_NAME		67 */
            ENFILE,       /* ERROR_TOO_MANY_NAMES		68 */
            EIO,          /* ERROR_TOO_MANY_SESS		69 */
            EAGAIN,       /* ERROR_SHARING_PAUSED		70 */
            EINVAL,       /* ERROR_REQ_NOT_ACCEP		71 */
            EAGAIN,       /* ERROR_REDIR_PAUSED		72 */
            EINVAL,       /* 73 */
            EINVAL,       /* 74 */
            EINVAL,       /* 75 */
            EINVAL,       /* 76 */
            EINVAL,       /* 77 */
            EINVAL,       /* 78 */
            EINVAL,       /* 79 */
            EEXIST,       /* ERROR_FILE_EXISTS		80 */
            EINVAL,       /* 81 */
            ENOSPC,       /* ERROR_CANNOT_MAKE		82 */
            EIO,          /* ERROR_FAIL_I24		83 */
            ENFILE,       /* ERROR_OUT_OF_STRUCTURES	84 */
            EEXIST,       /* ERROR_ALREADY_ASSIGNED	85 */
            EPERM,        /* ERROR_INVALID_PASSWORD	86 */
            EINVAL,       /* ERROR_INVALID_PARAMETER	87 */
            EIO,          /* ERROR_NET_WRITE_FAULT	88 */
            EAGAIN,       /* ERROR_NO_PROC_SLOTS		89 */
            EINVAL,       /* 90 */
            EINVAL,       /* 91 */
            EINVAL,       /* 92 */
            EINVAL,       /* 93 */
            EINVAL,       /* 94 */
            EINVAL,       /* 95 */
            EINVAL,       /* 96 */
            EINVAL,       /* 97 */
            EINVAL,       /* 98 */
            EINVAL,       /* 99 */
            EINVAL,       /* 100 */
            EINVAL,       /* 101 */
            EINVAL,       /* 102 */
            EINVAL,       /* 103 */
            EINVAL,       /* 104 */
            EINVAL,       /* 105 */
            EINVAL,       /* 106 */
            EXDEV,        /* ERROR_DISK_CHANGE		107 */
            EAGAIN,       /* ERROR_DRIVE_LOCKED		108 */
            EPIPE,        /* ERROR_BROKEN_PIPE		109 */
            ENOENT,       /* ERROR_OPEN_FAILED		110 */
            EINVAL,       /* ERROR_BUFFER_OVERFLOW	111 */
            ENOSPC,       /* ERROR_DISK_FULL		112 */
            EMFILE,       /* ERROR_NO_MORE_SEARCH_HANDLES	113 */
            EBADF,        /* ERROR_INVALID_TARGET_HANDLE	114 */
            EFAULT,       /* ERROR_PROTECTION_VIOLATION	115 */
            EINVAL,       /* 116 */
            EINVAL,       /* 117 */
            EINVAL,       /* 118 */
            EINVAL,       /* 119 */
            EINVAL,       /* 120 */
            EINVAL,       /* 121 */
            EINVAL,       /* 122 */
            ENOENT,       /* ERROR_INVALID_NAME		123 */
            EINVAL,       /* 124 */
            EINVAL,       /* 125 */
            EINVAL,       /* 126 */
            EINVAL,       /* ERROR_PROC_NOT_FOUND		127 */
            ECHILD,       /* ERROR_WAIT_NO_CHILDREN	128 */
            ECHILD,       /* ERROR_CHILD_NOT_COMPLETE	129 */
            EBADF,        /* ERROR_DIRECT_ACCESS_HANDLE	130 */
            EINVAL,       /* 131 */
            ESPIPE,       /* ERROR_SEEK_ON_DEVICE		132 */
            EINVAL,       /* 133 */
            EINVAL,       /* 134 */
            EINVAL,       /* 135 */
            EINVAL,       /* 136 */
            EINVAL,       /* 137 */
            EINVAL,       /* 138 */
            EINVAL,       /* 139 */
            EINVAL,       /* 140 */
            EINVAL,       /* 141 */
            EAGAIN,       /* ERROR_BUSY_DRIVE		142 */
            EINVAL,       /* 143 */
            EINVAL,       /* 144 */
            EEXIST,       /* ERROR_DIR_NOT_EMPTY		145 */
            EINVAL,       /* 146 */
            EINVAL,       /* 147 */
            EINVAL,       /* 148 */
            EINVAL,       /* 149 */
            EINVAL,       /* 150 */
            EINVAL,       /* 151 */
            EINVAL,       /* 152 */
            EINVAL,       /* 153 */
            EINVAL,       /* 154 */
            EINVAL,       /* 155 */
            EINVAL,       /* 156 */
            EINVAL,       /* 157 */
            EACCES,       /* ERROR_NOT_LOCKED		158 */
            EINVAL,       /* 159 */
            EINVAL,       /* 160 */
            ENOENT,       /* ERROR_BAD_PATHNAME	        161 */
            EINVAL,       /* 162 */
            EINVAL,       /* 163 */
            EINVAL,       /* 164 */
            EINVAL,       /* 165 */
            EINVAL,       /* 166 */
            EACCES,       /* ERROR_LOCK_FAILED		167 */
            EINVAL,       /* 168 */
            EINVAL,       /* 169 */
            EINVAL,       /* 170 */
            EINVAL,       /* 171 */
            EINVAL,       /* 172 */
            EINVAL,       /* 173 */
            EINVAL,       /* 174 */
            EINVAL,       /* 175 */
            EINVAL,       /* 176 */
            EINVAL,       /* 177 */
            EINVAL,       /* 178 */
            EINVAL,       /* 179 */
            EINVAL,       /* 180 */
            EINVAL,       /* 181 */
            EINVAL,       /* 182 */
            EEXIST,       /* ERROR_ALREADY_EXISTS		183 */
            ECHILD,       /* ERROR_NO_CHILD_PROCESS	184 */
            EINVAL,       /* 185 */
            EINVAL,       /* 186 */
            EINVAL,       /* 187 */
            EINVAL,       /* 188 */
            EINVAL,       /* 189 */
            EINVAL,       /* 190 */
            EINVAL,       /* 191 */
            EINVAL,       /* 192 */
            EINVAL,       /* 193 */
            EINVAL,       /* 194 */
            EINVAL,       /* 195 */
            EINVAL,       /* 196 */
            EINVAL,       /* 197 */
            EINVAL,       /* 198 */
            EINVAL,       /* 199 */
            EINVAL,       /* 200 */
            EINVAL,       /* 201 */
            EINVAL,       /* 202 */
            EINVAL,       /* 203 */
            EINVAL,       /* 204 */
            EINVAL,       /* 205 */
            ENAMETOOLONG, /* ERROR_FILENAME_EXCED_RANGE	206 */
            EINVAL,       /* 207 */
            EINVAL,       /* 208 */
            EINVAL,       /* 209 */
            EINVAL,       /* 210 */
            EINVAL,       /* 211 */
            EINVAL,       /* 212 */
            EINVAL,       /* 213 */
            EINVAL,       /* 214 */
            EINVAL,       /* 215 */
            EINVAL,       /* 216 */
            EINVAL,       /* 217 */
            EINVAL,       /* 218 */
            EINVAL,       /* 219 */
            EINVAL,       /* 220 */
            EINVAL,       /* 221 */
            EINVAL,       /* 222 */
            EINVAL,       /* 223 */
            EINVAL,       /* 224 */
            EINVAL,       /* 225 */
            EINVAL,       /* 226 */
            EINVAL,       /* 227 */
            EINVAL,       /* 228 */
            EINVAL,       /* 229 */
            EPIPE,        /* ERROR_BAD_PIPE		230 */
            EAGAIN,       /* ERROR_PIPE_BUSY		231 */
            EPIPE,        /* ERROR_NO_DATA		232 */
            EPIPE,        /* ERROR_PIPE_NOT_CONNECTED	233 */
            EINVAL,       /* 234 */
            EINVAL,       /* 235 */
            EINVAL,       /* 236 */
            EINVAL,       /* 237 */
            EINVAL,       /* 238 */
            EINVAL,       /* 239 */
            EINVAL,       /* 240 */
            EINVAL,       /* 241 */
            EINVAL,       /* 242 */
            EINVAL,       /* 243 */
            EINVAL,       /* 244 */
            EINVAL,       /* 245 */
            EINVAL,       /* 246 */
            EINVAL,       /* 247 */
            EINVAL,       /* 248 */
            EINVAL,       /* 249 */
            EINVAL,       /* 250 */
            EINVAL,       /* 251 */
            EINVAL,       /* 252 */
            EINVAL,       /* 253 */
            EINVAL,       /* 254 */
            EINVAL,       /* 255 */
            EINVAL,       /* 256 */
            EINVAL,       /* 257 */
            EINVAL,       /* 258 */
            EINVAL,       /* 259 */
            EINVAL,       /* 260 */
            EINVAL,       /* 261 */
            EINVAL,       /* 262 */
            EINVAL,       /* 263 */
            EINVAL,       /* 264 */
            EINVAL,       /* 265 */
            EINVAL,       /* 266 */
            ENOTDIR,      /* ERROR_DIRECTORY		267 */
        };
        if (err >= 0 && err < sizeof(errorTable))
            return errorTable[err];
        return EPERM;
    }
};

[[noreturn]] void throwWindowsException(DWORD err, const char* exp)
{
    if (err != 0)
        throw WindowsException(err, exp);
}

#define CHECK_CALL(exp)                                                                            \
    if (!(exp))                                                                                    \
        throwWindowsException(GetLastError(), #exp);

static void stat_file_handle(HANDLE hd, struct fuse_stat* st)
{
    memset(st, 0, sizeof(*st));
    BY_HANDLE_FILE_INFORMATION info;
    CHECK_CALL(GetFileInformationByHandle(hd, &info));
    filetime_to_unix_time(&info.ftLastAccessTime, &st->st_atim);
    filetime_to_unix_time(&info.ftLastWriteTime, &st->st_mtim);
    filetime_to_unix_time(&info.ftCreationTime, &st->st_birthtim);
    st->st_ctim = st->st_mtim;
    st->st_nlink = static_cast<fuse_nlink_t>(info.nNumberOfLinks);
    st->st_uid = securefs::OSService::getuid();
    st->st_gid = st->st_uid;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        st->st_mode = S_IFDIR | 0777;
    else
        st->st_mode = S_IFREG | 0777;
    st->st_dev = info.dwVolumeSerialNumber;
    st->st_ino = convert_dword_pair(info.nFileIndexLow, info.nFileIndexHigh);
    st->st_size = convert_dword_pair(info.nFileSizeLow, info.nFileSizeHigh);
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 4095) / 4096 * (4096 / 512);
}

class WindowsFileStream : public FileStream
{
private:
    HANDLE m_handle;

public:
    explicit WindowsFileStream(WideStringRef path, int flags, unsigned mode)
    {
        DWORD access_flags = 0;
        switch (flags & O_ACCMODE)
        {
        case O_RDONLY:
            access_flags = GENERIC_READ;
            break;
        case O_WRONLY:
            access_flags = GENERIC_WRITE;
            break;
        case O_RDWR:
            access_flags = GENERIC_READ | GENERIC_WRITE;
            break;
        default:
            throwVFSException(EINVAL);
        }

        DWORD create_flags = 0;
        if (flags & O_CREAT)
        {
            if (flags & O_EXCL)
                create_flags = CREATE_NEW;
            else
                create_flags = OPEN_ALWAYS;
        }
        else if (flags & O_TRUNC)
        {
            create_flags = TRUNCATE_EXISTING;
        }
        else
        {
            create_flags = OPEN_EXISTING;
        }

        m_handle = CreateFileW(path.c_str(),
                               access_flags,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               create_flags,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (m_handle == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            throw WindowsException(
                err,
                strprintf("CreateFileW with path=%s, access rights %lu, create flags %lu",
                          narrow_string(path).c_str(),
                          access_flags,
                          create_flags));
        }
    }

    ~WindowsFileStream() { CloseHandle(m_handle); }

    void lock(bool exclusive) override
    {
        OVERLAPPED o;
        memset(&o, 0, sizeof(o));
        CHECK_CALL(LockFileEx(m_handle,
                              exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0,
                              0,
                              std::numeric_limits<DWORD>::max(),
                              std::numeric_limits<DWORD>::max(),
                              &o));
    }

    void unlock() override
    {
        OVERLAPPED o;
        memset(&o, 0, sizeof(o));
        CHECK_CALL(UnlockFileEx(
            m_handle, 0, std::numeric_limits<DWORD>::max(), std::numeric_limits<DWORD>::max(), &o));
    }

    length_type read32(void* output, offset_type offset, DWORD length)
    {
        OVERLAPPED ol;
        memset(&ol, 0, sizeof(ol));
        ol.Offset = static_cast<DWORD>(offset);
        ol.OffsetHigh = static_cast<DWORD>(offset >> 32);

        DWORD readlen;
        if (!ReadFile(m_handle, output, length, &readlen, &ol))
        {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF)
                return 0;
            throwWindowsException(err, "ReadFile");
        }
        return readlen;
    }

    void close() noexcept override
    {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }

    void write32(const void* input, offset_type offset, DWORD length)
    {
        OVERLAPPED ol;
        memset(&ol, 0, sizeof(ol));
        ol.Offset = static_cast<DWORD>(offset);
        ol.OffsetHigh = static_cast<DWORD>(offset >> 32);

        DWORD writelen;
        CHECK_CALL(WriteFile(m_handle, input, length, &writelen, &ol));
        if (writelen != length)
            throwVFSException(EIO);
    }

    length_type read(void* output, offset_type offset, length_type length) override
    {
        length_type total = 0;
        while (length > MAX_SINGLE_BLOCK)
        {
            length_type readlen = read32(output, offset, MAX_SINGLE_BLOCK);
            if (readlen == 0)
                return total;
            length -= readlen;
            offset += readlen;
            output = static_cast<char*>(output) + readlen;
            total += readlen;
        }
        total += read32(output, offset, static_cast<DWORD>(length));
        return total;
    }

    void write(const void* input, offset_type offset, length_type length) override
    {
        while (length > MAX_SINGLE_BLOCK)
        {
            write32(input, offset, MAX_SINGLE_BLOCK);
            length -= MAX_SINGLE_BLOCK;
            offset += MAX_SINGLE_BLOCK;
            input = static_cast<const char*>(input) + MAX_SINGLE_BLOCK;
        }
        write32(input, offset, static_cast<DWORD>(length));
    }

    length_type size() const override
    {
        _LARGE_INTEGER SIZE;
        CHECK_CALL(GetFileSizeEx(m_handle, &SIZE));
        return SIZE.QuadPart;
    }

    void flush() override {}

    void resize(length_type len) override
    {
        auto sz = size();
        if (len > sz)
        {
            std::vector<byte> zeros(len - sz);
            write(zeros.data(), sz, len - sz);
        }
        else if (len < sz)
        {
            LARGE_INTEGER llen;
            llen.QuadPart = len;
            CHECK_CALL(SetFilePointerEx(m_handle, llen, nullptr, FILE_BEGIN));
            CHECK_CALL(SetEndOfFile(m_handle));
        }
    }

    length_type optimal_block_size() const noexcept override { return 4096; }

    void fsync() override { CHECK_CALL(FlushFileBuffers(m_handle)); }
    void utimens(const struct fuse_timespec ts[2]) override
    {
        FILETIME access_time, mod_time;
        if (!ts)
        {
            GetSystemTimeAsFileTime(&access_time);
            mod_time = access_time;
        }
        else
        {
            access_time = unix_time_to_filetime(ts + 0);
            mod_time = unix_time_to_filetime(ts + 1);
        }
        CHECK_CALL(SetFileTime(m_handle, nullptr, &access_time, &mod_time));
    }
    void fstat(struct fuse_stat* st) override { stat_file_handle(m_handle, st); }
};

OSService::OSService() : m_root_handle(INVALID_HANDLE_VALUE) {}

OSService::~OSService() { CloseHandle(m_root_handle); }

std::wstring OSService::norm_path(StringRef path) const
{
    if (m_dir_name.empty() || path.empty()
        || (path.size() > 0 && (path[0] == '/' || path[0] == '\\'))
        || (path.size() > 2 && path[1] == ':'))
        return widen_string(path);
    else
    {
        auto str = m_dir_name + widen_string(path);
        for (wchar_t& c : str)
        {
            if (c == L'/')
                c = L'\\';
        }
        return str;
    }
}

OSService::OSService(StringRef path) : m_dir_name(widen_string(path) + L"\\")
{
    m_root_handle = CreateFileW(m_dir_name.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);
}

std::shared_ptr<FileStream>
OSService::open_file_stream(StringRef path, int flags, unsigned mode) const
{
    return std::make_shared<WindowsFileStream>(norm_path(path), flags, mode);
}

void OSService::remove_file(StringRef path) const
{
    CHECK_CALL(DeleteFileW(norm_path(path).c_str()));
}

void OSService::remove_directory(StringRef path) const
{
    CHECK_CALL(RemoveDirectoryW(norm_path(path).c_str()));
}

void OSService::lock() const
{
    fprintf(stderr,
            "Warning: Windows does not support directory locking. "
            "Be careful not to mount the same data directory multiple times!\n");
}

void OSService::mkdir(StringRef path, unsigned mode) const
{
    if (CreateDirectoryW(norm_path(path).c_str(), nullptr) == 0)
    {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
            throwWindowsException(err, "CreateDirectory");
    }
}

void OSService::statfs(struct fuse_statvfs* fs_info) const
{
    memset(fs_info, 0, sizeof(*fs_info));
    ULARGE_INTEGER FreeBytesAvailable, TotalNumberOfBytes, TotalNumberOfFreeBytes;
    if (GetDiskFreeSpaceExW(
            m_dir_name.c_str(), &FreeBytesAvailable, &TotalNumberOfBytes, &TotalNumberOfFreeBytes)
        == 0)
        throwWindowsException(GetLastError(), "GetDiskFreeSpaceEx");
    auto maximum = static_cast<unsigned>(-1);
    fs_info->f_bsize = 4096;
    fs_info->f_frsize = fs_info->f_bsize;
    fs_info->f_bfree = TotalNumberOfFreeBytes.QuadPart / fs_info->f_bsize;
    fs_info->f_blocks = TotalNumberOfBytes.QuadPart / fs_info->f_bsize;
    fs_info->f_bavail = FreeBytesAvailable.QuadPart / fs_info->f_bsize;
    fs_info->f_files = maximum;
    fs_info->f_ffree = maximum;
    fs_info->f_favail = maximum;
    fs_info->f_namemax = 255;
}

void OSService::utimens(StringRef path, const fuse_timespec ts[2]) const
{
    FILETIME atime, mtime;
    if (!ts)
    {
        GetSystemTimeAsFileTime(&atime);
        mtime = atime;
    }
    else
    {
        atime = unix_time_to_filetime(ts);
        mtime = unix_time_to_filetime(ts + 1);
    }
    HANDLE hd = CreateFileW(norm_path(path).c_str(),
                            FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
                            nullptr,
                            OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS,
                            nullptr);
    if (hd == INVALID_HANDLE_VALUE)
        throwWindowsException(GetLastError(), "CreateFileW");
    DEFER(CloseHandle(hd));
    CHECK_CALL(SetFileTime(hd, nullptr, &atime, &mtime));
}

bool OSService::stat(StringRef path, struct fuse_stat* stat) const
{
    if (path == "." && m_root_handle != INVALID_HANDLE_VALUE)
    {
        // Special case which occurs very frequently
        stat_file_handle(m_root_handle, stat);
        return true;
    }
    HANDLE handle = CreateFileW(norm_path(path).c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        if (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND || err == ERROR_NOT_FOUND)
            return false;
        throwWindowsException(err, "CreateFileW");
    }

    DEFER(CloseHandle(handle));
    stat_file_handle(handle, stat);
    return true;
}

void OSService::link(StringRef source, StringRef dest) const { throwVFSException(ENOSYS); }
void OSService::chmod(StringRef path, fuse_mode_t mode) const
{
    int rc = ::_wchmod(norm_path(path).c_str(), mode);
    if (rc < 0)
        throwPOSIXException(errno, "_wchmod");
}

ssize_t OSService::readlink(StringRef path, char* output, size_t size) const
{
    throwVFSException(ENOSYS);
}
void OSService::symlink(StringRef source, StringRef dest) const { throwVFSException(ENOSYS); }

void OSService::rename(StringRef a, StringRef b) const
{
    auto wa = norm_path(a);
    auto wb = norm_path(b);
    DeleteFileW(wb.c_str());
    CHECK_CALL(MoveFileW(wa.c_str(), wb.c_str()));
}

int OSService::raise_fd_limit()
{
    return 65535;
    // The handle limit on Windows is high enough that no adjustments are necessary
}

// void OSService::recursive_traverse(StringRef dir,
//                                   const recursive_traverse_callback& callback) const
//{
//    struct Finder
//    {
//        HANDLE handle;
//
//        explicit Finder(HANDLE h) : handle(h) {}
//        ~Finder()
//        {
//            if (handle != INVALID_HANDLE_VALUE)
//                FindClose(handle);
//        }
//    };
//
//    WIN32_FIND_DATAA data;
//    auto find_pattern = norm_path(dir) + "\\*";
//    Finder finder(FindFirstFileA(find_pattern.c_str(), &data));
//
//    if (finder.handle == INVALID_HANDLE_VALUE)
//        throwWindowsException(GetLastError(), "FindFirstFile on pattern " + find_pattern);
//
//    do
//    {
//        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0)
//            continue;
//        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
//        {
//            recursive_traverse(dir + '\\' + data.cFileName, callback);
//        }
//        else
//        {
//            if (!callback(dir, data.cFileName))
//                return;
//        }
//    } while (FindNextFileA(finder.handle, &data));
//
//    if (GetLastError() != ERROR_NO_MORE_FILES)
//        throwWindowsException(GetLastError(), "FindNextFile");
//}

class WindowsDirectoryTraverser : public DirectoryTraverser
{
private:
    HANDLE m_handle;
    WIN32_FIND_DATAW m_data;

public:
    explicit WindowsDirectoryTraverser(WideStringRef pattern)
    {
        m_handle = FindFirstFileW(pattern.c_str(), &m_data);
        if (m_handle == INVALID_HANDLE_VALUE)
        {
            throwWindowsException(GetLastError(), "FindFirstFileW");
        }
    }

    ~WindowsDirectoryTraverser()
    {
        if (m_handle != INVALID_HANDLE_VALUE)
            FindClose(m_handle);
    }

    bool next(std::string* name, fuse_mode_t* type) override
    {
        while (wcscmp(m_data.cFileName, L".") == 0 || wcscmp(m_data.cFileName, L"..") == 0)
        {
            if (!FindNextFileW(m_handle, &m_data))
            {
                DWORD err = GetLastError();
                if (err == ERROR_NO_MORE_FILES)
                    return false;
                throwWindowsException(err, "FindNextFileW");
            }
        }

        if (name)
            *name = narrow_string(m_data.cFileName);
        if (type)
        {
            if (m_data.dwFileAttributes == FILE_ATTRIBUTE_NORMAL)
                *type = S_IFREG;
            else if (m_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                *type = S_IFDIR;
            else
                *type = 0;
        }
        m_data.cFileName[0] = L'.';
        m_data.cFileName[1] = 0;
        return true;
    }
};

std::unique_ptr<DirectoryTraverser> OSService::create_traverser(StringRef dir) const
{
    return securefs::make_unique<WindowsDirectoryTraverser>(norm_path(dir) + L"\\*");
}

uint32_t OSService::getuid()
{
    thread_local uint32_t cached_uid = 0;
    if (cached_uid > 0)
        return static_cast<uint32_t>(cached_uid);

    HANDLE token;
    CHECK_CALL(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DEFER(CloseHandle(token));

    DWORD outsize = 1;
    TOKEN_USER* tkuser = nullptr;
    DEFER(free(tkuser));
    GetTokenInformation(token, TokenUser, nullptr, 0, &outsize);
    tkuser = (TOKEN_USER*)malloc(outsize);
    CHECK_CALL(GetTokenInformation(token, TokenUser, tkuser, outsize, &outsize));
    NTSTATUS rc = FspPosixMapSidToUid(tkuser->User.Sid, &cached_uid);
    if (rc)
        global_logger->warn("FspPosixMapSidToUid returns NTSTATUS %d", (int)rc);
    return cached_uid;
}

uint32_t OSService::getgid() { return getuid(); }

bool OSService::isatty(int fd) noexcept { return ::_isatty(fd) != 0; }

void OSService::get_current_time(fuse_timespec& current_time)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    filetime_to_unix_time(&ft, &current_time);
}

/*
* Use the following function to delay load the WinFsp DLL
* directly from the WinFsp installation directory.
*
* You will also need to update your project settings:
* - Linker > Input > Delay Loaded DLL's: winfsp-$(PlatformTarget).dll
*
* Written by Bill Zissimopoulos, 2017. Released to the public domain.
*/

static inline NTSTATUS FspLoad(PVOID* PModule)
{
#if defined(_WIN64)
#define FSP_DLLNAME "winfsp-x64.dll"
#else
#define FSP_DLLNAME "winfsp-x86.dll"
#endif
#define FSP_DLLPATH "bin\\" FSP_DLLNAME

    WCHAR PathBuf[MAX_PATH];
    DWORD Size;
    HKEY RegKey;
    LONG Result;
    HMODULE Module;

    if (0 != PModule)
        *PModule = 0;

    Module = LoadLibraryW(L"" FSP_DLLNAME);
    if (0 == Module)
    {
        Result = RegOpenKeyExW(
            HKEY_LOCAL_MACHINE, L"Software\\WinFsp", 0, KEY_READ | KEY_WOW64_32KEY, &RegKey);
        if (ERROR_SUCCESS == Result)
        {
            Size = sizeof PathBuf - sizeof L"" FSP_DLLPATH + sizeof(WCHAR);
            Result = RegGetValueW(RegKey, 0, L"InstallDir", RRF_RT_REG_SZ, 0, PathBuf, &Size);
            RegCloseKey(RegKey);
        }
        if (ERROR_SUCCESS != Result)
            return STATUS_OBJECT_NAME_NOT_FOUND;

        RtlCopyMemory(
            PathBuf + (Size / sizeof(WCHAR) - 1), L"" FSP_DLLPATH, sizeof L"" FSP_DLLPATH);
        Module = LoadLibraryW(PathBuf);
        if (0 == Module)
            return STATUS_DLL_NOT_FOUND;
    }

    if (0 != PModule)
        *PModule = Module;

    return STATUS_SUCCESS;

#undef FSP_DLLNAME
#undef FSP_DLLPATH
}

static int win_init(void)
{
    SetConsoleOutputCP(CP_UTF8);
    FspLoad(nullptr);
    OSService::getuid();    // Force the call to WinFsp so that DLL failure will be caught sooner
    return 0;
}

static int win_inited_flag = win_init();
}

#endif
