// vfs_shell — the program you drive the file system with.

// At stage 0 it only knows its own name. Every command below is listed with the stage that makes it work, 
// so running --help tells you where the build is.


#include "vfs/version.h"

#include <cstring>
#include <cstdio>

namespace {

struct Command {
    const char* name;
    const char* stage;   // nullptr once the command actually works
    const char* help;
};

// Kept in the order the build plan implements them.
constexpr Command kCommands[] = {
    {"mkfs",    "stage 2", "format a new disk image"},
    {"dump",    "stage 2", "print a superblock, bitmap or inode"},
    {"mkdir",   "stage 4", "create a directory"},
    {"ls",      "stage 4", "list a directory"},
    {"cat",     "stage 5", "print a file"},
    {"write",   "stage 5", "write text into a file"},
    {"rm",      "stage 5", "remove a file"},
    {"sync",    "stage 6", "flush everything to the disk image"},
    {"fsck",    "stage 8", "check the disk image for damage"},
};

void print_usage() {
    std::printf("%s\n\n", vfs::version_string().c_str());
    std::printf("usage: vfs_shell [--version] [--help] <command> [args]\n\n");
    std::printf("commands:\n");
    for (const Command& c : kCommands) {
        std::printf("  %-8s %-36s %s\n",
                    c.name, c.help, c.stage ? c.stage : "ready");
    }
    std::printf("\nNothing is implemented yet. This is the stage 0 skeleton:\n"
                "it builds, it runs, and it knows what it will become.\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }

    const char* arg = argv[1];

    if (std::strcmp(arg, "--version") == 0 || std::strcmp(arg, "-v") == 0) {
        std::printf("%s\n", vfs::version_string().c_str());
        return 0;
    }

    if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
        print_usage();
        return 0;
    }

    for (const Command& c : kCommands) {
        if (std::strcmp(arg, c.name) == 0) {
            std::fprintf(stderr,
                         "vfs_shell: '%s' is not built yet - it arrives at %s\n",
                         c.name, c.stage);
            return 1;
        }
    }

    std::fprintf(stderr, "vfs_shell: unknown command '%s'\n", arg);
    std::fprintf(stderr, "try: vfs_shell --help\n");
    return 1;
}
