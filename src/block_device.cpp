#include "vfs/block_device.h"
#include "vfs/layout.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vfs {

// Private to this file. Without the anonymous namespace these would be exported
// as vfs::throw_errno, vfs::read_full and so on, and the next .cpp that wants a
// helper by the same name would collide with them at link time.
namespace {

    [[noreturn]] void throw_errno(const char* operation) {
        throw std::runtime_error(
            std::string(operation) + ": " + std::strerror(errno)
        );
    }

    off_t block_offset(uint32_t block_no) {
        return static_cast<off_t>(block_no) * BLOCK_SIZE;
    }

    void read_full(int fd, void* dst, off_t offset) {
        char* buffer = static_cast<char*>(dst);
        size_t done = 0;

        while (done < BLOCK_SIZE) {
            ssize_t n = ::pread(
                fd,
                buffer + done,
                BLOCK_SIZE - done,
                offset + static_cast<off_t>(done)
            );

            if (n < 0) {
                if (errno == EINTR)
                    continue;

                throw_errno("pread");
            }

            if (n == 0) {
                throw std::runtime_error("short read");
            }

            done += static_cast<size_t>(n);
        }
    }

    void write_full(int fd, const void* src, off_t offset) {
        const char* buffer = static_cast<const char*>(src);
        size_t done = 0;

        while (done < BLOCK_SIZE) {
            ssize_t n = ::pwrite(
                fd,
                buffer + done,
                BLOCK_SIZE - done,
                offset + static_cast<off_t>(done)
            );

            if (n < 0) {
                if (errno == EINTR)
                    continue;

                throw_errno("pwrite");
            }

            if (n == 0) {
                throw std::runtime_error("short write");
            }

            done += static_cast<size_t>(n);
        }
    }

} // anonymous namespace

    PreadDevice::PreadDevice(int fd, uint32_t blocks)
        : fd_(fd), block_count_(blocks) {
    }

    PreadDevice::PreadDevice(PreadDevice&& other) noexcept
        : fd_(other.fd_),
        block_count_(other.block_count_) {
        other.fd_ = -1;
        other.block_count_ = 0;
    }

    PreadDevice& PreadDevice::operator=(PreadDevice&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                ::close(fd_);

            fd_ = other.fd_;
            block_count_ = other.block_count_;

            other.fd_ = -1;
            other.block_count_ = 0;
        }

        return *this;
    }

    PreadDevice PreadDevice::create(
        const char* path,
        uint32_t block_count
    ) {
        if (block_count == 0)
            throw std::invalid_argument(
                "cannot create a disk with zero blocks"
            );

        // O_EXCL, not O_TRUNC: creating a disk over a file that already exists
        // would silently destroy whatever file system was in it.
        int fd = ::open(path, O_RDWR | O_CREAT | O_EXCL, 0644);

        if (fd < 0) {
            if (errno == EEXIST)
                throw std::runtime_error(
                    std::string("refusing to overwrite an existing file: ") + path
                );

            throw_errno("open");
        }

        off_t size =
            static_cast<off_t>(block_count) * BLOCK_SIZE;

        if (::ftruncate(fd, size) < 0) {
            int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            throw_errno("ftruncate");
        }

        return PreadDevice(fd, block_count);
    }

    PreadDevice PreadDevice::open(const char* path) {
        int fd = ::open(path, O_RDWR);

        if (fd < 0)
            throw_errno("open");

        struct stat st{};

        if (::fstat(fd, &st) < 0) {
            int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            throw_errno("fstat");
        }

        if (st.st_size % BLOCK_SIZE != 0) {
            ::close(fd);
            throw std::runtime_error(
                "disk size is not a multiple of BLOCK_SIZE"
            );
        }

        if (st.st_size == 0) {
            ::close(fd);
            throw std::runtime_error("disk image is empty");
        }

        uint32_t blocks =
            static_cast<uint32_t>(st.st_size / BLOCK_SIZE);

        return PreadDevice(fd, blocks);
    }

    void PreadDevice::read_block(uint32_t block_no, Block& dst) {
        if (block_no >= block_count_)
            throw std::out_of_range("block number out of range");

        read_full(fd_, dst.data(), block_offset(block_no));
    }

    void PreadDevice::write_block(
        uint32_t block_no,
        const Block& src
    ) {
        if (block_no >= block_count_)
            throw std::out_of_range("block number out of range");

        write_full(fd_, src.data(), block_offset(block_no));
    }

    void PreadDevice::sync() {
    #ifdef F_FULLFSYNC
        // On macOS plain fsync() only pushes data to the drive; it does not ask
        // the drive to persist its own cache. F_FULLFSYNC does. It is not
        // supported on every file system, so fall through to fsync() if it fails.
        while (true) {
            if (::fcntl(fd_, F_FULLFSYNC, 0) == 0)
                return;

            if (errno != EINTR)
                break;
        }
    #endif

        while (::fsync(fd_) < 0) {
            if (errno != EINTR)
                throw_errno("fsync");
        }
    }

    uint32_t PreadDevice::block_count() const {
        return block_count_;
    }

    PreadDevice::~PreadDevice() {
        if (fd_ >= 0)
            ::close(fd_);
    }

} // namespace vfs