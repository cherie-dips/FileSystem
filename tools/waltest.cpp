//
// waltest — proves the write-ahead log (stage 7) works.
//
// Every record written comes back, in order, with a valid checksum - and when
// the end of the log is torn, or a byte in the middle is wrong, reading stops
// right there instead of handing back anything damaged.
//
// Run it under `make asan` as well as `make`.
//

#include "vfs/crc32.h"
#include "vfs/layout.h"
#include "vfs/wal.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

const std::string kPath = "/tmp/vfs_waltest_" + std::to_string(::getpid()) + ".log";

void fill_pattern(vfs::Block& b, uint32_t seed) {
    for (size_t i = 0; i < b.size(); i++)
        b[i] = static_cast<uint8_t>((seed * 131u + i * 7u) & 0xFF);
}

uint64_t file_size(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

void truncate_to(const std::string& path, uint64_t size) {
    if (::truncate(path.c_str(), static_cast<off_t>(size)) != 0)
        throw std::runtime_error("truncate failed");
}

void flip_byte(const std::string& path, uint64_t offset) {
    int fd = ::open(path.c_str(), O_RDWR);
    uint8_t byte = 0;
    if (fd < 0 || ::pread(fd, &byte, 1, static_cast<off_t>(offset)) != 1)
        throw std::runtime_error("flip_byte: read failed");
    byte ^= 0x01;
    if (::pwrite(fd, &byte, 1, static_cast<off_t>(offset)) != 1)
        throw std::runtime_error("flip_byte: write failed");
    ::close(fd);
}

constexpr uint64_t kRecord = sizeof(vfs::WalRecordHeader);
constexpr uint64_t kBlockRecord = kRecord + vfs::BLOCK_SIZE;

// Three transactions, nine records:
//   lsn 1 BEGIN   2 BLOCK 10   3 BLOCK 11   4 COMMIT    (txn 1)
//   lsn 5 BEGIN   6 BLOCK 12   7 COMMIT                 (txn 5)
//   lsn 8 BEGIN   9 BLOCK 13                            (txn 8, never committed)
void write_sample() {
    std::remove(kPath.c_str());
    vfs::Wal wal(kPath.c_str(), false);
    wal.reset(1);

    vfs::Block b;
    uint64_t t = wal.append(vfs::WAL_BEGIN, 1, 0, nullptr);
    fill_pattern(b, 10); wal.append(vfs::WAL_BLOCK, t, 10, &b);
    fill_pattern(b, 11); wal.append(vfs::WAL_BLOCK, t, 11, &b);
    wal.append(vfs::WAL_COMMIT, t, 0, nullptr);

    t = wal.append(vfs::WAL_BEGIN, 5, 0, nullptr);
    fill_pattern(b, 12); wal.append(vfs::WAL_BLOCK, t, 12, &b);
    wal.append(vfs::WAL_COMMIT, t, 0, nullptr);

    t = wal.append(vfs::WAL_BEGIN, 8, 0, nullptr);
    fill_pattern(b, 13); wal.append(vfs::WAL_BLOCK, t, 13, &b);
    wal.sync();
}

std::vector<vfs::WalRecord> read_back(uint32_t total_blocks = 1000) {
    vfs::Wal wal(kPath.c_str(), false);
    return wal.scan(total_blocks);
}

// --------------------------------------------------------------------- crc32

void check_crc() {
    std::printf("crc32\n");

    check(vfs::crc32("123456789", 9) == 0xCBF43926u,
          "the standard check value: crc32(\"123456789\") = 0xCBF43926");
    check(vfs::crc32("", 0) == 0, "no bytes give 0");
    check(vfs::crc32("6789", 4, vfs::crc32("12345", 5)) == 0xCBF43926u,
          "a CRC can be carried on over more bytes");

    vfs::Block b;
    fill_pattern(b, 1);
    uint32_t before = vfs::crc32(b.data(), b.size());
    b[2000] ^= 0x10;
    check(vfs::crc32(b.data(), b.size()) != before, "one flipped bit changes it");
}

// ------------------------------------------------------------------- records

void check_records() {
    std::printf("every record comes back\n");
    write_sample();

    std::vector<vfs::WalRecord> recs = read_back();
    check(recs.size() == 9, "nine records written, nine read back");

    bool in_order = true, checksums = true, payloads = true;
    for (size_t i = 0; i < recs.size(); i++) {
        const vfs::WalRecordHeader& h = recs[i].header;
        in_order = in_order && h.lsn == i + 1;

        vfs::WalRecordHeader copy = h;
        copy.checksum = 0;
        uint32_t c = vfs::crc32(&copy, sizeof copy);
        if (h.payload_len)
            c = vfs::crc32(recs[i].payload.data(), h.payload_len, c);
        checksums = checksums && c == h.checksum;

        if (h.type == vfs::WAL_BLOCK) {
            vfs::Block want;
            fill_pattern(want, h.block_no);
            payloads = payloads && recs[i].payload == want;
        }
    }
    check(in_order, "in order, lsns 1 to 9 with no gaps");
    check(checksums, "every one with a valid checksum");
    check(payloads, "and every block image intact");

    const uint16_t types[] = {vfs::WAL_BEGIN, vfs::WAL_BLOCK, vfs::WAL_BLOCK, vfs::WAL_COMMIT,
                              vfs::WAL_BEGIN, vfs::WAL_BLOCK, vfs::WAL_COMMIT,
                              vfs::WAL_BEGIN, vfs::WAL_BLOCK};
    bool types_ok = recs.size() == 9;
    for (size_t i = 0; types_ok && i < 9; i++)
        types_ok = recs[i].header.type == types[i];
    check(types_ok, "BEGIN, BLOCK..., COMMIT, as written");
    check(recs.size() == 9 && recs[1].header.txn_id == 1 && recs[5].header.txn_id == 5 &&
          recs[8].header.txn_id == 8,
          "each record carries its transaction's BEGIN lsn");

    check(file_size(kPath) == 5 * kRecord + 4 * kBlockRecord,
          "the file is exactly the records: 40 bytes each, plus 4096 per block");

    char magic[5] = {};
    if (FILE* f = std::fopen(kPath.c_str(), "rb")) {
        if (std::fread(magic, 1, 4, f) != 4) magic[0] = 0;
        std::fclose(f);
    }
    check(std::strcmp(magic, "WALR") == 0, "a record starts \"WALR\" in a hex dump");
}

void check_durable_lsn() {
    std::printf("durability\n");
    std::remove(kPath.c_str());
    vfs::Wal wal(kPath.c_str(), true);   // real fsyncs here
    wal.reset(1);
    wal.append(vfs::WAL_BEGIN, 1, 0, nullptr);
    wal.append(vfs::WAL_COMMIT, 1, 0, nullptr);
    check(wal.durable_lsn() == 0, "nothing is durable before sync()");
    wal.sync();
    check(wal.durable_lsn() == 2, "after sync(), everything appended is");
}

// ------------------------------------------------------------- damaged logs

void check_torn_tail() {
    std::printf("a torn tail is dropped\n");

    write_sample();
    uint64_t full = file_size(kPath);

    truncate_to(kPath, full - 100);
    std::vector<vfs::WalRecord> recs = read_back();
    check(recs.size() == 8 && recs.back().header.lsn == 8,
          "a block record cut short loses only itself");

    write_sample();
    truncate_to(kPath, full - kBlockRecord + 20);
    recs = read_back();
    check(recs.size() == 8, "so does one whose header is cut in half");

    write_sample();
    truncate_to(kPath, full - kBlockRecord - kRecord);
    recs = read_back();
    check(recs.size() == 7 && recs.back().header.type == vfs::WAL_COMMIT,
          "cut exactly between records, everything before the cut stays");
}

void check_corruption() {
    std::printf("damage in the middle stops the scan there\n");

    // Record 6 (BLOCK 12) starts after BEGIN, BLOCK, BLOCK, COMMIT, BEGIN.
    const uint64_t record6 = 3 * kRecord + 2 * kBlockRecord;

    write_sample();
    flip_byte(kPath, record6 + kRecord + 1234);
    std::vector<vfs::WalRecord> recs = read_back();
    check(recs.size() == 5 && recs.back().header.lsn == 5,
          "a flipped payload byte in record 6: records 1-5 come back, nothing after");

    write_sample();
    flip_byte(kPath, record6 + 8);   // its block_no
    recs = read_back();
    check(recs.size() == 5, "a flipped header byte fails the checksum too");

    write_sample();
    flip_byte(kPath, record6);       // its magic
    recs = read_back();
    check(recs.size() == 5, "and so does a broken magic number");
}

void check_bounds() {
    std::printf("records that cannot be right\n");

    std::remove(kPath.c_str());
    {
        vfs::Wal wal(kPath.c_str(), false);
        wal.reset(1);
        vfs::Block b;
        fill_pattern(b, 5);
        wal.append(vfs::WAL_BEGIN, 1, 0, nullptr);
        wal.append(vfs::WAL_BLOCK, 1, 5, &b);
        wal.append(vfs::WAL_COMMIT, 1, 0, nullptr);
        wal.sync();
    }
    check(read_back(6).size() == 3, "block 5 on a 6-block disk is fine");
    check(read_back(5).size() == 1, "on a 5-block disk it is not, and the scan stops at it");

    // Two logs glued together: the lsns jump from 3 back to 1.
    {
        int fd = ::open(kPath.c_str(), O_RDWR);
        std::vector<uint8_t> all(file_size(kPath));
        if (fd < 0 || ::pread(fd, all.data(), all.size(), 0) != static_cast<ssize_t>(all.size()) ||
            ::pwrite(fd, all.data(), all.size(), static_cast<off_t>(all.size())) !=
                static_cast<ssize_t>(all.size()))
            throw std::runtime_error("could not double the log");
        ::close(fd);
    }
    check(read_back(0).size() == 3, "an lsn that does not follow on stops the scan");

    try {
        vfs::Wal wal(kPath.c_str(), false);
        wal.append(vfs::WAL_BLOCK, 1, 5, nullptr);
        check(false, "a BLOCK record without a payload is refused");
    } catch (const std::logic_error&) {
        check(true, "a BLOCK record without a payload is refused");
    }
}

void check_reset() {
    std::printf("reset\n");
    write_sample();
    vfs::Wal wal(kPath.c_str(), false);
    wal.reset(42);
    check(file_size(kPath) == 0 && wal.size_bytes() == 0, "empties the file");
    check(wal.durable_lsn() == 41, "everything before the new start counts as durable");
    check(wal.append(vfs::WAL_BEGIN, 42, 0, nullptr) == 42, "and lsns carry on from where they were set");
    wal.sync();
    std::vector<vfs::WalRecord> recs = wal.scan(0);
    check(recs.size() == 1 && recs[0].header.lsn == 42, "the new record reads back");
}

} // namespace

int main() {
    try {
        check_crc();
        check_records();
        check_durable_lsn();
        check_torn_tail();
        check_corruption();
        check_bounds();
        check_reset();
        std::remove(kPath.c_str());

        std::printf("\n%s\n", failures == 0
                                  ? "stage 7: all checks passed"
                                  : "stage 7: FAILURES");
        return failures == 0 ? 0 : 1;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "waltest: unexpected exception: %s\n", e.what());
        std::remove(kPath.c_str());
        return 1;
    }
}
