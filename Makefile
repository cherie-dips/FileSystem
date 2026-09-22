# ConcurrentFS
#
#   make            build everything, optimised           -> build/release/
#   make debug      no optimisation, debug symbols        -> build/debug/
#   make asan       + AddressSanitizer and UBSan          -> build/asan/
#   make tsan       + ThreadSanitizer (needed from stage 9) -> build/tsan/
#   make run        build and run the shell
#   make clean      delete every build directory
#
# Each mode builds into its own directory, so switching between them never leaves stale object files behind.

MODE ?= release

ifeq ($(MODE),release)
  OPT := -O2
  SAN :=
else ifeq ($(MODE),debug)
  OPT := -O0 -g
  SAN :=
else ifeq ($(MODE),asan)
  OPT := -O1 -g -fno-omit-frame-pointer
  SAN := -fsanitize=address,undefined
else ifeq ($(MODE),tsan)
  OPT := -O1 -g -fno-omit-frame-pointer
  SAN := -fsanitize=thread
else
  $(error unknown MODE '$(MODE)' - use release, debug, asan or tsan)
endif

# Respect CXX from the environment or command line, but do not inherit make's
# built-in default of plain "c++".
ifeq ($(origin CXX),default)
  CXX := clang++
endif

CXXFLAGS  = -std=c++17 -Wall -Wextra -Wpedantic -Iinclude $(OPT) $(SAN) -MMD -MP
LDFLAGS   = $(SAN)

BUILD := build/$(MODE)
OBJDIR := $(BUILD)/obj

LIB_SRCS  := $(wildcard src/*.cpp)
LIB_OBJS  := $(patsubst src/%.cpp,$(OBJDIR)/src/%.o,$(LIB_SRCS))

TOOL_SRCS := $(wildcard tools/*.cpp)
TOOL_OBJS := $(patsubst tools/%.cpp,$(OBJDIR)/tools/%.o,$(TOOL_SRCS))
TOOLS     := $(patsubst tools/%.cpp,$(BUILD)/%,$(TOOL_SRCS))

DEPS := $(LIB_OBJS:.o=.d) $(TOOL_OBJS:.o=.d)

.PHONY: all debug asan tsan run clean help
.DEFAULT_GOAL := all

# Without this, make treats the .o files as intermediate and deletes them after
# linking, which means a full rebuild on every single invocation.
.SECONDARY: $(LIB_OBJS) $(TOOL_OBJS)

all: $(TOOLS)
	@echo "built ($(MODE)): $(TOOLS)"

$(BUILD)/%: $(OBJDIR)/tools/%.o $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) $^ -o $@

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

debug:
	@$(MAKE) --no-print-directory MODE=debug

asan:
	@$(MAKE) --no-print-directory MODE=asan

tsan:
	@$(MAKE) --no-print-directory MODE=tsan

run: all
	@./$(BUILD)/vfs_shell

clean:
	rm -rf build

help:
	@sed -n '3,10p' Makefile

-include $(DEPS)
