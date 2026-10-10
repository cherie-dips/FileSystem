#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "vfs/block_device.h"

namespace vfs {
    // One bit per thing: per inode in the inode bitmap, per block in the data
    // bitmap. A set bit means in use.
    //
    // On disk, bit i lives in byte i / 8, at bit i % 8. In memory the bits are
    // held as 64-bit words, which on a little-endian machine are those same
    // bytes in the same order - so load() and store() are straight copies.
    class Bitmap {
        public:
            explicit Bitmap(uint32_t nbits); // every bit starts clear

            uint32_t size() const { return nbits_; }

            // All three throw std::out_of_range for i >= size().
            bool is_set(uint32_t i) const;
            void set(uint32_t i);
            void clear(uint32_t i);

            // The lowest clear bit, or nothing if every bit is set.
            std::optional<uint32_t> find_first_free() const;

            uint32_t count_set() const;

            // How many disk blocks a bitmap of nbits takes up.
            static uint32_t blocks_for(uint32_t nbits);

            // Read or write the blocks_for(size()) blocks starting at first_block.
            void load(BlockDevice& dev, uint32_t first_block);
            void store(BlockDevice& dev, uint32_t first_block) const;

        private:
            uint32_t nbits_;
            // Whole blocks' worth of words, so load and store copy entire
            // blocks. Bits past nbits_ are always kept clear.
            std::vector<uint64_t> words_;
    };
}
