#define FUSE_USE_VERSION 312
#include "store.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <fuse3/fuse_lowlevel.h>
#include <iostream>
#include <linux/falloc.h>
#include <memory>
#include <vector>

using localfs::Id;
using localfs::Store;
namespace {
Store &store(fuse_req_t request) {
    return *static_cast<Store *>(fuse_req_userdata(request));
}
template <class F> void call(fuse_req_t request, F operation) noexcept {
    try {
        operation();
    } catch (const localfs::Error &e) {
        if (e.code == EIO)
            std::cerr << "local-fusefs: " << e.what() << '\n';
        fuse_reply_err(request, e.code);
    } catch (const std::bad_alloc &) {
        fuse_reply_err(request, ENOMEM);
    } catch (const std::exception &e) {
        std::cerr << "local-fusefs: " << e.what() << '\n';
        fuse_reply_err(request, EIO);
    } catch (...) {
        fuse_reply_err(request, EIO);
    }
}
fuse_entry_param entry(fuse_req_t request, Id inode) {
    fuse_entry_param result{};
    result.ino = static_cast<fuse_ino_t>(inode);
    result.generation = 1;
    result.attr = store(request).stat(inode);
    return result;
}
void prepare(fuse_file_info *file, Id inode) {
    file->fh = static_cast<std::uint64_t>(inode);
    file->direct_io = 1;
    file->keep_cache = 0;
}
fuse_lowlevel_ops operations() {
    fuse_lowlevel_ops op{};
    op.init = [](void *, fuse_conn_info *conn) {
        conn->want &= ~FUSE_CAP_WRITEBACK_CACHE;
        if (conn->capable & FUSE_CAP_ATOMIC_O_TRUNC)
            conn->want |= FUSE_CAP_ATOMIC_O_TRUNC;
    };
    op.lookup = [](fuse_req_t req, fuse_ino_t parent, const char *name) {
        call(req, [&] {
            auto value = entry(req, store(req).lookup_at(static_cast<Id>(parent), name));
            fuse_reply_entry(req, &value);
        });
    };
    op.forget = [](fuse_req_t req, fuse_ino_t, std::uint64_t) { fuse_reply_none(req); };
    op.getattr = [](fuse_req_t req, fuse_ino_t ino, fuse_file_info *) {
        call(req, [&] {
            auto value = store(req).stat(static_cast<Id>(ino));
            fuse_reply_attr(req, &value, 0);
        });
    };
    op.setattr = [](fuse_req_t req, fuse_ino_t ino, struct stat *attr, int flags, fuse_file_info *) {
        call(req, [&] {
            auto &s = store(req);
            auto id = static_cast<Id>(ino);
            if (flags & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID))
                s.chown(id, (flags & FUSE_SET_ATTR_UID) ? attr->st_uid : static_cast<uid_t>(-1),
                        (flags & FUSE_SET_ATTR_GID) ? attr->st_gid : static_cast<gid_t>(-1));
            if (flags & FUSE_SET_ATTR_MODE)
                s.chmod(id, attr->st_mode);
            if (flags & FUSE_SET_ATTR_SIZE)
                s.truncate(id, attr->st_size);
            if (flags & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME | FUSE_SET_ATTR_ATIME_NOW |
                         FUSE_SET_ATTR_MTIME_NOW)) {
                timespec times[2]{{0, UTIME_OMIT}, {0, UTIME_OMIT}};
                if (flags & FUSE_SET_ATTR_ATIME)
                    times[0] = attr->st_atim;
                if (flags & FUSE_SET_ATTR_MTIME)
                    times[1] = attr->st_mtim;
                if (flags & FUSE_SET_ATTR_ATIME_NOW)
                    times[0].tv_nsec = UTIME_NOW;
                if (flags & FUSE_SET_ATTR_MTIME_NOW)
                    times[1].tv_nsec = UTIME_NOW;
                s.utimens(id, times);
            }
            auto value = s.stat(id);
            fuse_reply_attr(req, &value, 0);
        });
    };
    op.mkdir = [](fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
        call(req, [&] {
            auto *ctx = fuse_req_ctx(req);
            auto value = entry(req, store(req).create_at(static_cast<Id>(parent), name,
                                                         S_IFDIR | (mode & 07777), ctx->uid, ctx->gid));
            fuse_reply_entry(req, &value);
        });
    };
    op.create = [](fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, fuse_file_info *file) {
        call(req, [&] {
            auto *ctx = fuse_req_ctx(req);
            auto id = store(req).create_at(static_cast<Id>(parent), name, S_IFREG | (mode & 07777), ctx->uid,
                                           ctx->gid);
            auto value = entry(req, id);
            prepare(file, id);
            fuse_reply_create(req, &value, file);
        });
    };
    op.open = [](fuse_req_t req, fuse_ino_t ino, fuse_file_info *file) {
        call(req, [&] {
            auto id = static_cast<Id>(ino);
            auto value = store(req).stat(id);
            if (!S_ISREG(value.st_mode))
                throw localfs::Error(S_ISDIR(value.st_mode) ? EISDIR : EINVAL, "Not a regular file");
            if ((file->flags & O_TRUNC) && (file->flags & O_ACCMODE) != O_RDONLY)
                store(req).truncate(id, 0);
            prepare(file, id);
            fuse_reply_open(req, file);
        });
    };
    op.read = [](fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset, fuse_file_info *file) {
        call(req, [&] {
            if (size > INT_MAX || (file->flags & O_ACCMODE) == O_WRONLY)
                throw localfs::Error(EBADF, "Invalid read");
            std::vector<char> buffer(size);
            auto count = store(req).read(static_cast<Id>(ino), buffer.data(), size, offset);
            fuse_reply_buf(req, buffer.data(), count);
        });
    };
    op.write = [](fuse_req_t req, fuse_ino_t ino, const char *buffer, size_t size, off_t offset,
                  fuse_file_info *file) {
        call(req, [&] {
            if (size > INT_MAX || (file->flags & O_ACCMODE) == O_RDONLY)
                throw localfs::Error(EBADF, "Invalid write");
            auto count =
                store(req).write(static_cast<Id>(ino), buffer, size, offset, (file->flags & O_APPEND) != 0);
            fuse_reply_write(req, count);
        });
    };
    op.unlink = [](fuse_req_t req, fuse_ino_t parent, const char *name) {
        call(req, [&] {
            store(req).remove_at(static_cast<Id>(parent), name, false);
            fuse_reply_err(req, 0);
        });
    };
    op.rmdir = [](fuse_req_t req, fuse_ino_t parent, const char *name) {
        call(req, [&] {
            store(req).remove_at(static_cast<Id>(parent), name, true);
            fuse_reply_err(req, 0);
        });
    };
    op.rename = [](fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t new_parent,
                   const char *new_name, unsigned flags) {
        call(req, [&] {
            store(req).rename_at(static_cast<Id>(parent), name, static_cast<Id>(new_parent), new_name, flags);
            fuse_reply_err(req, 0);
        });
    };
    op.symlink = [](fuse_req_t req, const char *target, fuse_ino_t parent, const char *name) {
        call(req, [&] {
            auto *ctx = fuse_req_ctx(req);
            auto value = entry(req, store(req).create_at(static_cast<Id>(parent), name, S_IFLNK | 0777,
                                                         ctx->uid, ctx->gid, target));
            fuse_reply_entry(req, &value);
        });
    };
    op.readlink = [](fuse_req_t req, fuse_ino_t ino) {
        call(req, [&] {
            auto target = store(req).readlink(static_cast<Id>(ino));
            fuse_reply_readlink(req, target.c_str());
        });
    };
    op.opendir = [](fuse_req_t req, fuse_ino_t ino, fuse_file_info *file) {
        call(req, [&] {
            if (!S_ISDIR(store(req).stat(static_cast<Id>(ino)).st_mode))
                throw localfs::Error(ENOTDIR, "Not a directory");
            file->fh = ino;
            fuse_reply_open(req, file);
        });
    };
    op.readdir = [](fuse_req_t req, fuse_ino_t ino, size_t size, off_t offset, fuse_file_info *) {
        call(req, [&] {
            if (offset < 0)
                throw localfs::Error(EINVAL, "Negative directory offset");
            auto id = static_cast<Id>(ino);
            auto entries = store(req).list(id);
            Id parent = 1;
            try {
                parent = store(req).lookup_at(id, "..");
            } catch (const localfs::Error &e) {
                if (e.code != ENOENT)
                    throw;
            }
            entries.insert(entries.begin(), {{".", id}, {"..", parent}});
            std::vector<char> output(size);
            std::size_t used = 0;
            for (auto i = static_cast<std::size_t>(offset); i < entries.size(); ++i) {
                struct stat st {};
                st.st_ino = static_cast<ino_t>(entries[i].inode);
                auto length = fuse_add_direntry(req, output.data() + used, size - used,
                                                entries[i].name.c_str(), &st, static_cast<off_t>(i + 1));
                if (length > size - used)
                    break;
                used += length;
            }
            fuse_reply_buf(req, output.data(), used);
        });
    };
    op.release = [](fuse_req_t req, fuse_ino_t, fuse_file_info *) { fuse_reply_err(req, 0); };
    op.releasedir = op.release;
    op.flush = op.release; // Each write already committed with synchronous=FULL.
    op.fsync = [](fuse_req_t req, fuse_ino_t, int, fuse_file_info *) {
        call(req, [&] {
            store(req).sync();
            fuse_reply_err(req, 0);
        });
    };
    op.fsyncdir = op.fsync;
    op.statfs = [](fuse_req_t req, fuse_ino_t) {
        call(req, [&] {
            auto value = store(req).space();
            fuse_reply_statfs(req, &value);
        });
    };
    op.fallocate = [](fuse_req_t req, fuse_ino_t ino, int mode, off_t offset, off_t length,
                      fuse_file_info *) {
        call(req, [&] {
            if (mode != (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE))
                throw localfs::Error(EOPNOTSUPP, "Only hole punching is supported");
            store(req).punch_hole(static_cast<Id>(ino), offset, length);
            fuse_reply_err(req, 0);
        });
    };
    op.lseek = [](fuse_req_t req, fuse_ino_t ino, off_t offset, int whence, fuse_file_info *) {
        call(req, [&] { fuse_reply_lseek(req, store(req).seek(static_cast<Id>(ino), offset, whence)); });
    };
    // Omitting getlk/setlk/flock keeps local advisory locking in the kernel.
    return op;
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::string directory, key;
        std::size_t cache_mib = 64;
        std::vector<std::string> arguments{argv[0], "-f"};
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--version") {
                std::cout << "local-fusefs 0.2.0 (storage format 1)\n";
                return 0;
            }
            if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: local-fusefs --store DIRECTORY --key-file KEY [--cache-mib N] "
                             "MOUNTPOINT [-o FUSE_OPTIONS]\nForeground, multithreaded Linux FUSE filesystem. "
                             "Initialize with fusefs-admin first.\n";
                return 0;
            }
            if (arg == "--store" || arg == "--key-file" || arg == "--cache-mib") {
                if (++i == argc)
                    throw localfs::Error(EINVAL, "Missing value for " + arg);
                if (arg == "--store")
                    directory = argv[i];
                else if (arg == "--key-file")
                    key = argv[i];
                else {
                    std::string value = argv[i];
                    std::size_t end = 0;
                    cache_mib = std::stoull(value, &end);
                    if (end != value.size() || cache_mib > 4096)
                        throw localfs::Error(EINVAL, "Cache must be 0..4096 MiB");
                }
            } else
                arguments.push_back(arg);
        }
        if (directory.empty() || key.empty())
            throw localfs::Error(EINVAL, "--store and --key-file are required; see --help");
        arguments.insert(arguments.end(), {"-o", "default_permissions"});
        std::vector<char *> pointers;
        for (auto &arg : arguments)
            pointers.push_back(arg.data());
        fuse_args args = FUSE_ARGS_INIT(static_cast<int>(pointers.size()), pointers.data());
        fuse_cmdline_opts options{};
        struct OptionsCleanup {
            fuse_args &args;
            fuse_cmdline_opts &options;
            ~OptionsCleanup() {
                fuse_opt_free_args(&args);
                free(options.mountpoint);
            }
        } cleanup{args, options};
        if (fuse_parse_cmdline(&args, &options) != 0 || !options.mountpoint)
            throw localfs::Error(EINVAL, "A mountpoint is required");
        Store backend(directory, key, cache_mib * 1024 * 1024);
        auto op = operations();
        std::unique_ptr<fuse_session, decltype(&fuse_session_destroy)> session(
            fuse_session_new(&args, &op, sizeof(op), &backend), fuse_session_destroy);
        if (!session)
            throw localfs::Error(EIO, "Cannot create FUSE session");
        if (fuse_set_signal_handlers(session.get()) != 0)
            throw localfs::Error(EIO, "Cannot install signal handlers");
        if (fuse_session_mount(session.get(), options.mountpoint) != 0) {
            fuse_remove_signal_handlers(session.get());
            throw localfs::Error(EIO, "Cannot mount filesystem");
        }
        // SQLite is already open: always stay in the foreground, without fork.
        std::unique_ptr<fuse_loop_config, decltype(&fuse_loop_cfg_destroy)> loop(fuse_loop_cfg_create(),
                                                                                 fuse_loop_cfg_destroy);
        if (!loop) {
            fuse_session_unmount(session.get());
            fuse_remove_signal_handlers(session.get());
            throw std::bad_alloc();
        }
        fuse_loop_cfg_set_max_threads(loop.get(), 16);
        fuse_loop_cfg_set_idle_threads(loop.get(), 4);
        int result = options.singlethread ? fuse_session_loop(session.get())
                                          : fuse_session_loop_mt(session.get(), loop.get());
        fuse_session_unmount(session.get());
        fuse_remove_signal_handlers(session.get());
        return result == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << "local-fusefs: " << e.what() << '\n';
        return 1;
    }
}
