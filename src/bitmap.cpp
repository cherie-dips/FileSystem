#include "vfs/bitmap.h"
#include "vfs/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

namespace vfs {

namespace {

    constexpr uint32_t WORD_BITS       = 64;
    constexpr uint32_t WORDS_PER_BLOCK = BLOCK_SIZE / sizeof(uint64_t);

    uint64_t bit_mask(uint32_t i) {
        return uint64_t{1} << (i % WORD_BITS);
    }

} // anonymous namespace

    Bitmap::Bitmap(uint32_t nbits)
        : nbits_(nbits),
        words_(static_cast<size_t>(blocks_for(nbits)) * WORDS_PER_BLOCK, 0) {
    }

    uint32_t Bitmap::blocks_for(uint32_t nbits) {
        // 64-bit, or nbits close to 2^32 would wrap before the divide.
        return static_cast<uint32_t>(
            (uint64_t{nbits} + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK
        );
    }

    bool Bitmap::is_set(uint32_t i) const {
        if (i >= nbits_)
            throw std::out_of_range(
                "bit " + std::to_string(i) + " is past the end of a "
                + std::to_string(nbits_) + "-bit bitmap"
            );

        return (words_[i / WORD_BITS] & bit_mask(i)) != 0;
    }

    void Bitmap::set(uint32_t i) {
        is_set(i); // bounds check
        words_[i / WORD_BITS] |= bit_mask(i);
    }

    void Bitmap::clear(uint32_t i) {
        is_set(i); // bounds check
        words_[i / WORD_BITS] &= ~bit_mask(i);
    }

    std::optional<uint32_t> Bitmap::find_first_free() const {
        // 64 bits per step: a word with every bit set is skipped in one compare,
        // and in the first word that is not full, the lowest clear bit is the
        // lowest set bit of its complement - which ctz finds in one instruction.
        for (size_t w = 0; w < words_.size(); w++) {
            if (words_[w] == ~uint64_t{0})
                continue;

            uint64_t i = w * WORD_BITS
                       + static_cast<uint64_t>(__builtin_ctzll(~words_[w]));

            // Bits past the end are always clear, so a full bitmap ends up here
            // with an index that does not exist.
            if (i >= nbits_)
                return std::nullopt;

            return static_cast<uint32_t>(i);
        }

        return std::nullopt;
    }

    uint32_t Bitmap::count_set() const {
        uint32_t n = 0;
        for (uint64_t w : words_)
            n += static_cast<uint32_t>(__builtin_popcountll(w));
        return n;
    }

    void Bitmap::load(BlockDevice& dev, uint32_t first_block) {
        Block b;
        uint32_t blocks = blocks_for(nbits_);

        for (uint32_t k = 0; k < blocks; k++) {
            dev.read_block(first_block + k, b);
            std::memcpy(&words_[static_cast<size_t>(k) * WORDS_PER_BLOCK],
                        b.data(), BLOCK_SIZE);
        }

        // The last block usually holds more bits than the bitmap has. Those
        // bits do not stand for anything, so whatever the disk says about them
        // is dropped - otherwise find_first_free and count_set would see them.
        if (nbits_ % WORD_BITS != 0)
            words_[nbits_ / WORD_BITS] &= bit_mask(nbits_) - 1;

        size_t first_spare_word = (static_cast<size_t>(nbits_) + WORD_BITS - 1) / WORD_BITS;
        std::fill(words_.begin() + static_cast<std::ptrdiff_t>(first_spare_word),
                  words_.end(), 0);
    }

    void Bitmap::store(BlockDevice& dev, uint32_t first_block) const {
        Block b;
        uint32_t blocks = blocks_for(nbits_);

        for (uint32_t k = 0; k < blocks; k++) {
            std::memcpy(b.data(),
                        &words_[static_cast<size_t>(k) * WORDS_PER_BLOCK],
                        BLOCK_SIZE);
            dev.write_block(first_block + k, b);
        }
    }

} // namespace vfs
