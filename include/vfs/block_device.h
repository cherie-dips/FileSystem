#pragma once

#include <atomic>
#include <cstdint>

#include "vfs/layout.h"

namespace vfs {
    class BlockDevice{ // Interface - defines operations (read, write, sync, block count) that any block device must implement (SSD, RAM, Disk, etc.)
        public:
            virtual ~BlockDevice() = default; 

            virtual void read_block(uint32_t block_no, Block& dst) = 0;  

            virtual void write_block(uint32_t block_no, const Block& src) = 0; 

            // Writes count consecutive blocks, first onwards, from data
            // (count * BLOCK_SIZE bytes). The default writes them one at a
            // time; a device that can move them in one I/O overrides it.
            virtual void write_run(uint32_t first, const uint8_t* data, uint32_t count);

            virtual void sync() = 0; 

            virtual uint32_t block_count() const = 0; // function that returns the number of blocks in the device
    };

    class PreadDevice : public BlockDevice { // Concrete Implementation of BlockDevice 
        private: 
            PreadDevice(int fd, uint32_t blocks); // private constructor
            int fd_; 
            uint32_t block_count_; 
        public:
            // Creates a NEW disk image. Refuses to touch a file that already
            // exists - overwriting someone's file system is not something that
            // should happen by accident. Delete it first if that is what you want.
            static PreadDevice create(const char* path, uint32_t block_count);

            // Opens an existing disk image. The block count comes from the file
            // size, which must be a whole number of blocks.
            static PreadDevice open(const char* path);

            ~PreadDevice();

            PreadDevice(const PreadDevice&) = delete; // delete copy constructor to prevent copying of PreadDevice objects because it manages a file descriptor, which should not be copied.
            PreadDevice& operator=(const PreadDevice&) = delete; // delete copy assignment operator to prevent copying of PreadDevice objects 
            
            PreadDevice(PreadDevice&& other) noexcept;
            PreadDevice& operator=(PreadDevice&& other) noexcept;

            void read_block(uint32_t block_no, Block& dst) override;
            void write_block(uint32_t block_no, const Block& src) override; 
            void write_run(uint32_t first, const uint8_t* data, uint32_t count) override;
            void sync() override; 

            uint32_t block_count() const override; 
    }; 

    // Wraps another device. Counts every block read and write; can sleep
    // before each I/O, standing in for a real disk's seek and rotation (OSTEP
    // 37.4) so that what the cache saves is measurable - once per call, so a
    // run of neighbouring blocks pays one seek, as on a real disk; and can
    // turn sync() into a no-op, for runs that never need to survive a power
    // cut.
    class InstrumentedDevice : public BlockDevice {
        public:
            InstrumentedDevice(BlockDevice& inner, uint32_t latency_us, bool sync_enabled);

            void read_block(uint32_t block_no, Block& dst) override;
            void write_block(uint32_t block_no, const Block& src) override;
            void write_run(uint32_t first, const uint8_t* data, uint32_t count) override;
            void sync() override;
            uint32_t block_count() const override;

            uint64_t reads() const;
            uint64_t writes() const;
            void reset_counts();

        private:
            void delay() const;

            BlockDevice& inner_;
            const uint32_t latency_us_;
            const bool sync_enabled_;
            std::atomic<uint64_t> reads_{0};
            std::atomic<uint64_t> writes_{0};
    };
}
