#include "operations.h"

#include <mutex>
#include <algorithm>
#include <utility>
#include <string>

namespace securefs
{
namespace internal
{
    class FileGuard
    {
    private:
        FileTable* m_ft;
        FileBase* m_fb;

    public:
        explicit FileGuard(FileTable* ft, FileBase* fb) : m_ft(ft), m_fb(fb) {}

        FileGuard(const FileGuard&) = delete;
        FileGuard& operator=(const FileGuard&) = delete;

        FileGuard(FileGuard&& other) noexcept : m_ft(other.m_ft), m_fb(other.m_fb)
        {
            other.m_ft = nullptr;
            other.m_fb = nullptr;
        }

        FileGuard& operator=(FileGuard&& other) noexcept
        {
            if (this == &other)
                return *this;
            swap(other);
            return *this;
        }

        ~FileGuard()
        {
            try
            {
                reset(nullptr);
            }
            catch (...)
            {
            }
        }

        FileBase* get() noexcept { return m_fb; }
        template <class T>
        T* get_as() noexcept
        {
            return static_cast<T*>(m_fb);
        }
        FileBase& operator*() noexcept { return *m_fb; }
        FileBase* operator->() noexcept { return m_fb; }
        FileBase* release() noexcept
        {
            auto rt = m_fb;
            m_fb = nullptr;
            return rt;
        }
        void reset(FileBase* fb)
        {
            if (m_ft && m_fb)
            {
                std::lock_guard<FileTable> lg(*m_ft);
                m_ft->close(m_fb);
            }
            m_fb = fb;
        }
        void swap(FileGuard& other) noexcept
        {
            std::swap(m_ft, other.m_ft);
            std::swap(m_fb, other.m_fb);
        }
    };

    FileBase* table_open_as(FileTable& t, const id_type& id, int type)
    {
        std::lock_guard<FileTable> lg(t);
        return t.open_as(id, type);
    }

    FileBase* table_create_as(FileTable& t, const id_type& id, int type)
    {
        std::lock_guard<FileTable> lg(t);
        return t.create_as(id, type);
    }

    bool dir_get_entry(Directory* dir, const std::string& name, id_type& id, int& type)
    {
        assert(dir);
        std::lock_guard<FileBase> lg(*dir);
        return dir->get_entry(name, id, type);
    }

    bool dir_add_entry(Directory* dir, const std::string& name, const id_type& id, int type)
    {
        assert(dir);
        std::lock_guard<FileBase> lg(*dir);
        bool result = dir->add_entry(name, id, type);
        dir->flush();
        return result;
    }

    FileGuard
    open_base_dir(struct fuse_context* ctx, const std::string& path, std::string& last_component)
    {
        assert(ctx);
        auto components = split(path, '/');
        auto fs = static_cast<operations::FileSystem*>(ctx->private_data);
        FileGuard result(&fs->table, table_open_as(fs->table, fs->root_id, FileBase::DIRECTORY));
        if (components.empty())
        {
            last_component = std::string();
            return result;
        }
        id_type id;
        int type;

        for (size_t i = 0; i + 1 < components.size(); ++i)
        {
            bool exists = dir_get_entry(result.get_as<Directory>(), components[i], id, type);
            if (!exists)
                throw OSException(ENOENT);
            if (type != FileBase::DIRECTORY)
                throw OSException(ENOTDIR);
            result.reset(table_open_as(fs->table, id, type));
        }
        last_component = components.back();
        return result;
    }

    FileGuard open_all(struct fuse_context* ctx, const std::string& path)
    {
        auto fs = static_cast<operations::FileSystem*>(ctx->private_data);
        std::string last_component;
        auto fg = open_base_dir(ctx, path, last_component);
        if (last_component.empty())
            return fg;
        id_type id;
        int type;
        bool exists = dir_get_entry(fg.get_as<Directory>(), last_component, id, type);
        if (!exists)
            throw OSException(ENOENT);
        fg.reset(table_open_as(fs->table, id, type));
        return fg;
    }

    FileGuard create(struct fuse_context* ctx, const std::string& path, int type)
    {
        auto fs = static_cast<operations::FileSystem*>(ctx->private_data);
        std::string last_component;
        auto dir = open_base_dir(ctx, path, last_component);
        id_type id;
        generate_random(id.data(), id.size());
        FileGuard result(&fs->table, table_create_as(fs->table, id, type));
        bool success = false;
        try
        {
            success = dir_add_entry(dir.get_as<Directory>(), last_component, id, type);
        }
        catch (...)
        {
            result->unlink();
            throw;
        }
        if (!success)
        {
            result->unlink();
            throw OSException(EEXIST);
        }
        return result;
    }

    void remove(struct fuse_context* ctx, const std::string& path)
    {
        auto fs = static_cast<operations::FileSystem*>(ctx->private_data);
        std::string last_component;
        auto dir = open_base_dir(ctx, path, last_component);
        if (last_component.empty())
            throw OSException(EPERM);
        id_type id;
        int type;
        {
            std::lock_guard<FileBase> guard(*dir);
            if (!dir.get_as<Directory>()->remove_entry(last_component, id, type))
                throw OSException(ENOENT);
            dir->flush();
        }
        FileGuard to_be_removed(&fs->table, table_open_as(fs->table, id, type));
        {
            std::lock_guard<FileBase> guard(*to_be_removed);
            to_be_removed->unlink();
        }
    }

    bool is_readonly(struct fuse_context* ctx)
    {
        return static_cast<operations::FileSystem*>(ctx->private_data)->table.is_readonly();
    }

    int log_error(struct fuse_context*, const ExceptionBase& e)
    {
        fprintf(stderr, "%s: %s\n", e.type_name(), e.message().c_str());
        return -e.error_number();
    }

    int log_general_error(struct fuse_context*, const std::exception& e)
    {
        fprintf(stderr, "An unexpected exception occured: %s\n", e.what());
        return -EPERM;
    }
}

namespace operations
{

#define COMMON_CATCH_BLOCK                                                                         \
    catch (const OSException& e) { return -e.error_number(); }                                     \
    catch (const ExceptionBase& e) { return internal::log_error(ctx, e); }                         \
    catch (const std::exception& e) { return internal::log_general_error(ctx, e); }

    int getattr(const char* path, struct stat* st)
    {
        auto ctx = fuse_get_context();
        try
        {
            auto fg = internal::open_all(ctx, path);
            std::lock_guard<FileBase> lg(*fg);
            fg->stat(st);
            fg->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int opendir(const char* path, struct fuse_file_info* info)
    {
        auto ctx = fuse_get_context();
        try
        {
            auto fg = internal::open_all(ctx, path);
            if (fg->type() != FileBase::DIRECTORY)
                return -ENOTDIR;
            info->fh = reinterpret_cast<uintptr_t>(fg.release());
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int releasedir(const char* path, struct fuse_file_info* info)
    {
        return ::securefs::operations::release(path, info);
    }

    int
    readdir(const char*, void* buffer, fuse_fill_dir_t filler, off_t, struct fuse_file_info* info)
    {
        auto ctx = fuse_get_context();
        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EINVAL;
            if (fb->type() != FileBase::DIRECTORY)
                return -ENOTDIR;
            struct stat st;
            memset(&st, 0, sizeof(st));
            auto actions = [&](const std::string& name, const id_type&, int type) -> bool
            {
                st.st_mode = FileBase::mode_for_type(type);
                return filler(buffer, name.c_str(), &st, 0) == 0;
            };
            std::lock_guard<FileBase> lg(*fb);
            static_cast<Directory*>(fb)->iterate_over_entries(actions);
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int create(const char* path, mode_t mode, struct fuse_file_info* info)
    {
        auto ctx = fuse_get_context();
        mode &= ~static_cast<uint32_t>(S_IFMT);
        mode |= S_IFREG;
        try
        {
            if (internal::is_readonly(ctx))
                return -EROFS;
            auto fg = internal::create(ctx, path, FileBase::REGULAR_FILE);
            if (fg->type() != FileBase::REGULAR_FILE)
                return -EPERM;
            fg->set_uid(ctx->uid);
            fg->set_gid(ctx->gid);
            fg->set_nlink(1);
            fg->set_mode(mode);
            fg->flush();
            info->fh = reinterpret_cast<uintptr_t>(fg.release());
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int open(const char* path, struct fuse_file_info* info)
    {
        auto ctx = fuse_get_context();
        // bool rdonly = info->flags & O_RDONLY;
        bool rdwr = info->flags & O_RDWR;
        bool wronly = info->flags & O_WRONLY;
        bool append = info->flags & O_APPEND;
        // bool require_read = rdonly | rdwr;
        bool require_write = wronly | append | rdwr;

        try
        {
            if (require_write && internal::is_readonly(ctx))
                return -EROFS;
            auto fg = internal::open_all(ctx, path);
            if (fg->type() != FileBase::REGULAR_FILE)
                return -EPERM;
            if (info->flags & O_TRUNC)
            {
                std::lock_guard<FileBase> lg(*fg);
                fg.get_as<RegularFile>()->truncate(0);
            }
            info->fh = reinterpret_cast<uintptr_t>(fg.release());
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int release(const char*, struct fuse_file_info* info)
    {
        auto ctx = fuse_get_context();
        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EINVAL;
            fb->flush();
            internal::FileGuard fg(&static_cast<FileSystem*>(ctx->private_data)->table, fb);
            fg.reset(nullptr);
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int read(const char*, char* buffer, size_t len, off_t off, struct fuse_file_info* info)
    {
        auto ctx = fuse_get_context();
        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EINVAL;
            if (fb->type() != FileBase::REGULAR_FILE)
                return -EPERM;
            std::lock_guard<FileBase> lg(*fb);
            return static_cast<int>(static_cast<RegularFile*>(fb)->read(buffer, off, len));
        }
        COMMON_CATCH_BLOCK
    }

    int write(const char*, const char* buffer, size_t len, off_t off, struct fuse_file_info* info)
    {
        auto ctx = fuse_get_context();
        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EINVAL;
            if (fb->type() != FileBase::REGULAR_FILE)
                return -EPERM;
            std::lock_guard<FileBase> lg(*fb);
            static_cast<RegularFile*>(fb)->write(buffer, off, len);
            return static_cast<int>(len);
        }
        COMMON_CATCH_BLOCK
    }

    int flush(const char*, struct fuse_file_info* info)
    {
        auto ctx = fuse_get_context();
        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EINVAL;
            if (fb->type() != FileBase::REGULAR_FILE)
                return -EPERM;
            std::lock_guard<FileBase> lg(*fb);
            fb->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int truncate(const char* path, off_t size)
    {
        auto ctx = fuse_get_context();
        try
        {
            auto fg = internal::open_all(ctx, path);
            if (fg->type() != FileBase::REGULAR_FILE)
                return -EINVAL;
            std::lock_guard<FileBase> lg(*fg);
            fg.get_as<RegularFile>()->truncate(size);
            fg->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int ftruncate(const char*, off_t size, struct fuse_file_info* info)
    {
        auto ctx = fuse_get_context();
        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EINVAL;
            if (fb->type() != FileBase::REGULAR_FILE)
                return -EINVAL;
            std::lock_guard<FileBase> lg(*fb);
            static_cast<RegularFile*>(fb)->truncate(size);
            fb->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int unlink(const char* path)
    {
        auto ctx = fuse_get_context();
        try
        {
            if (internal::is_readonly(ctx))
                return -EROFS;
            internal::remove(ctx, path);
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int mkdir(const char* path, mode_t mode)
    {
        auto ctx = fuse_get_context();
        mode &= ~static_cast<uint32_t>(S_IFMT);
        mode |= S_IFDIR;
        try
        {
            if (internal::is_readonly(ctx))
                return -EROFS;
            auto fg = internal::create(ctx, path, FileBase::DIRECTORY);
            if (fg->type() != FileBase::DIRECTORY)
                return -ENOTDIR;
            fg->set_uid(ctx->uid);
            fg->set_gid(ctx->gid);
            fg->set_nlink(1);
            fg->set_mode(mode);
            fg->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int rmdir(const char* path) { return ::securefs::operations::unlink(path); }

    int chmod(const char* path, mode_t mode)
    {
        auto ctx = fuse_get_context();
        try
        {
            auto fg = internal::open_all(ctx, path);
            std::lock_guard<FileBase> lg(*fg);
            auto original_mode = fg->get_mode();
            mode &= 0777;
            mode |= original_mode & S_IFMT;
            fg->set_mode(mode);
            fg->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int chown(const char* path, uid_t uid, gid_t gid)
    {
        auto ctx = fuse_get_context();
        try
        {
            auto fg = internal::open_all(ctx, path);
            std::lock_guard<FileBase> lg(*fg);
            fg->set_uid(uid);
            fg->set_gid(gid);
            fg->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int symlink(const char* to, const char* from)
    {
        auto ctx = fuse_get_context();
        try
        {
            if (internal::is_readonly(ctx))
                return -EROFS;
            auto fg = internal::create(ctx, from, FileBase::SYMLINK);
            if (fg->type() != FileBase::SYMLINK)
                return -EINVAL;
            fg->set_uid(ctx->uid);
            fg->set_gid(ctx->gid);
            fg->set_nlink(1);
            fg->set_mode(S_IFLNK | 0755);
            fg.get_as<Symlink>()->set(to);
            fg->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int readlink(const char* path, char* buf, size_t size)
    {
        if (size == 0)
            return -EINVAL;
        auto ctx = fuse_get_context();
        try
        {
            auto fg = internal::open_all(ctx, path);
            if (fg->type() != FileBase::SYMLINK)
                return -EINVAL;
            auto destination = fg.get_as<Symlink>()->get();
            memset(buf, 0, size);
            memcpy(buf, destination.data(), std::min(destination.size(), size - 1));
            return 0;
        }
        COMMON_CATCH_BLOCK
    }
}
}
