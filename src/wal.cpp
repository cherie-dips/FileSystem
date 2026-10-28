#include "vfs/wal.h"
#include "vfs/crc32.h"
#include "vfs/fault.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vfs {

namespace {

    [[noreturn]] void throw_errno(const std::string& operation) {
        throw std::runtime_error("log: " + operation + ": " + std::strerror(errno));
    }

    uint32_t record_checksum(const WalRecordHeader& h, const uint8_t* payload) {
        WalRecordHeader copy = h;
        copy.checksum = 0;
        uint32_t c = crc32(&copy, sizeof copy);
        if (h.payload_len > 0)
            c = crc32(payload, h.payload_len, c);
        return c;
    }

    // Reads up to n bytes at offset; fewer only at the end of the file.
    size_t read_at(int fd, void* dst, size_t n, uint64_t offset) {
        char* bytes = static_cast<char*>(dst);
        size_t done = 0;
        while (done < n) {
            ssize_t r = ::pread(fd, bytes + done, n - done,
                                static_cast<off_t>(offset + done));
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                throw_errno("pread");
            }
            if (r == 0)
                break;
            done += static_cast<size_t>(r);
        }
        return done;
    }

    // As PreadDevice::sync: on macOS only F_FULLFSYNC asks the drive to empty
    // its own cache, and a COMMIT record sitting in a drive cache is not
    // committed.
    void full_sync(int fd) {
    #ifdef F_FULLFSYNC
        while (true) {
            if (::fcntl(fd, F_FULLFSYNC, 0) == 0)
                return;
            if (errno != EINTR)
                break;
        }
    #endif
        while (::fsync(fd) < 0) {
            if (errno != EINTR)
                throw_errno("fsync");
        }
    }

} // anonymous namespace

    Wal::Wal(const char* path, bool sync_enabled)
        : sync_enabled_(sync_enabled) {
        batch_ = std::make_unique<uint8_t[]>(BATCH_BYTES + sizeof(WalRecordHeader) + BLOCK_SIZE);
        fd_ = ::open(path, O_RDWR | O_CREAT, 0644);
        if (fd_ < 0)
            throw_errno(std::string("open ") + path);

        struct stat st{};
        if (::fstat(fd_, &st) < 0) {
            int saved_errno = errno;
            ::close(fd_);
            errno = saved_errno;
            throw_errno("fstat");
        }
        end_ = static_cast<uint64_t>(st.st_size);
    }

    Wal::~Wal() {
        if (fd_ >= 0)
            ::close(fd_);
    }

    std::vector<WalRecord> Wal::scan(uint32_t total_blocks) const {
        struct stat st{};
        if (::fstat(fd_, &st) < 0)
            throw_errno("fstat");
        const uint64_t size = static_cast<uint64_t>(st.st_size);

        std::vector<WalRecord> records;
        uint64_t offset = 0;

        while (true) {
            WalRecord r{};
            WalRecordHeader& h = r.header;
            if (read_at(fd_, &h, sizeof h, offset) != sizeof h)
                break;

            const bool is_block = h.type == WAL_BLOCK;
            if (h.magic != WAL_MAGIC || h.reserved != 0 || h.pad != 0)
                break;
            if (h.type != WAL_BEGIN && h.type != WAL_BLOCK && h.type != WAL_COMMIT)
                break;
            if (h.payload_len != (is_block ? BLOCK_SIZE : 0))
                break;
            if (is_block && (h.block_no == 0 ||
                             (total_blocks != 0 && h.block_no >= total_blocks)))
                break;
            if (!is_block && h.block_no != 0)
                break;
            if (h.lsn == 0 || (!records.empty() && h.lsn != records.back().header.lsn + 1))
                break;

            if (is_block && (offset + sizeof h + BLOCK_SIZE > size ||
                             read_at(fd_, r.payload.data(), BLOCK_SIZE,
                                     offset + sizeof h) != BLOCK_SIZE))
                break;

            if (record_checksum(h, r.payload.data()) != h.checksum)
                break;

            offset += sizeof h + h.payload_len;
            records.push_back(r);
        }

        return records;
    }

    uint64_t Wal::encode(
        uint16_t type,
        uint64_t txn_id,
        uint32_t block_no,
        bool has_payload,
        const std::function<void(uint8_t*)>& fill
    ) {
        if ((type == WAL_BLOCK) != has_payload)
            throw std::logic_error("log: a BLOCK record, and only a BLOCK record, has a payload");

        WalRecordHeader h{};
        h.magic       = WAL_MAGIC;
        h.type        = type;
        h.block_no    = block_no;
        h.payload_len = has_payload ? BLOCK_SIZE : 0;
        h.lsn         = next_lsn_;
        h.txn_id      = txn_id;

        uint8_t* at = batch_.get() + batch_len_;
        uint8_t* payload = at + sizeof h;
        if (has_payload)
            fill(payload);
        h.checksum = record_checksum(h, has_payload ? payload : nullptr);
        std::memcpy(at, &h, sizeof h);

        batch_len_ += sizeof h + h.payload_len;
        return next_lsn_++;
    }

    // One call, one counted write: a crash can tear it at any sector, which
    // leaves whole records followed by at most one torn one - and scan()
    // stops at the torn one.
    void Wal::write_out(const uint8_t* bytes, size_t len) {
        fault::before_write(fd_, bytes, len, static_cast<off_t>(end_));

        size_t done = 0;
        while (done < len) {
            ssize_t w = ::pwrite(fd_, bytes + done, len - done,
                                 static_cast<off_t>(end_ + done));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                throw_errno("pwrite");
            }
            if (w == 0)
                throw std::runtime_error("log: short write");
            done += static_cast<size_t>(w);
        }

        end_ += len;
        bytes_appended_.fetch_add(len);
    }

    uint64_t Wal::append(
        uint16_t type,
        uint64_t txn_id,
        uint32_t block_no,
        const Block* payload
    ) {
        // Header and payload go down in one write, so a crash can tear a
        // record but never interleave two.
        flush();
        const uint64_t lsn = encode(type, txn_id, block_no, payload != nullptr,
                                    [&](uint8_t* dst) { std::memcpy(dst, payload->data(), BLOCK_SIZE); });
        flush();
        return lsn;
    }

    uint64_t Wal::stage_record(
        uint16_t type,
        uint64_t txn_id,
        uint32_t block_no,
        bool has_payload,
        const std::function<void(uint8_t*)>& fill
    ) {
        const size_t len = sizeof(WalRecordHeader) + (has_payload ? BLOCK_SIZE : 0);
        if (batch_len_ > 0 && batch_len_ + len > BATCH_BYTES)
            flush();
        return encode(type, txn_id, block_no, has_payload, fill);
    }

    uint64_t Wal::stage(
        uint16_t type,
        uint64_t txn_id,
        uint32_t block_no,
        const Block* payload
    ) {
        return stage_record(type, txn_id, block_no, payload != nullptr,
                            [&](uint8_t* dst) { std::memcpy(dst, payload->data(), BLOCK_SIZE); });
    }

    uint64_t Wal::stage_block(
        uint64_t txn_id,
        uint32_t block_no,
        const std::function<void(uint8_t*)>& fill
    ) {
        return stage_record(WAL_BLOCK, txn_id, block_no, true, fill);
    }

    void Wal::flush() {
        if (batch_len_ == 0)
            return;
        write_out(batch_.get(), batch_len_);
        batch_len_ = 0;
    }

    void Wal::sync() {
        flush();
        if (sync_enabled_)
            full_sync(fd_);
        durable_lsn_.store(next_lsn_ - 1);
    }

    void Wal::reset(uint64_t next_lsn) {
        if (next_lsn == 0)
            throw std::invalid_argument("log: lsns start at 1");

        // Nothing is ever staged here: every commit flushes before it
        // syncs, and a reset only follows a sync.
        batch_len_ = 0;
        if (::ftruncate(fd_, 0) < 0)
            throw_errno("ftruncate");
        if (sync_enabled_)
            full_sync(fd_);

        end_ = 0;
        next_lsn_ = next_lsn;
        durable_lsn_.store(next_lsn - 1);
    }

} // namespace vfs
