#pragma once

#include <cstdint>

#include "vfs/layout.h"

namespace vfs {
    class BlockDevice{ // Interface - defines operations (read, write, sync, block count) that any block device must implement (SSD, RAM, Disk, etc.)
        public:
            virtual ~BlockDevice() = default; 

            virtual void read_block(uint32_t block_no, Block& dst) = 0;  

            virtual void write_block(uint32_t block_no, const Block& src) = 0; 

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
            void sync() override; 

            uint32_t block_count() const override; 
    }; 
}
