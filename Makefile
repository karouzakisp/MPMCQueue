VERIFY := 1
PROFILE := 0

INCLUDE_DIR := include
SRC_DIR := src
BUILD_DIR := build

CC := g++-11
# CFLAGS := -g -Wall -pthread -I$(INCLUDE_DIR) -O0 #-O3
CXXFLAGS := -I$(INCLUDE_DIR) -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wpedantic \
-Wold-style-cast -Wcast-align -Woverloaded-virtual -Wconversion -Wsign-conversion \
-Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches -Wlogical-op \
-Wnull-dereference -Wuseless-cast -Wdouble-promotion -Wformat=2 -Wunused \
-std=c++23 -g3 -O0 #-O3
NOCXXFLAGSRECOVER := -Wno-invalid-offsetof
NOCXXFLAGS := -Wno-old-style-cast -Wno-sign-conversion -Wno-shadow -Wno-conversion -Wno-unused-parameter -Wno-vla -Wno-invalid-offsetof
LDLIBS := -lpthread -lm -lpmemobj -lpmem

ifeq (${VERIFY}, 1)
	CXXFLAGS += -DVERIFY
endif

ifeq (${PROFILE}, 1)
	CXXFLAGS += -pg
endif

SRCS := $(SRC_DIR)/halfhalf.c $(SRC_DIR)/pairwise.c $(SRC_DIR)/harness.cpp
MPMCQUEUE_BENCH := $(BUILD_DIR)/mpmcqueue_bench
RECOVER_TEST := $(BUILD_DIR)/recover_test

.DEFAULT_GOAL := all
.PHONY: clean

all: $(MPMCQUEUE_BENCH) $(RECOVER_TEST)

$(MPMCQUEUE_BENCH): $(SRCS)
	$(CC) $(CXXFLAGS) $(NOCXXFLAGS) -o $@  $^ $(LDLIBS)

clean:
	$(RM) $(MPMCQUEUE_BENCH) $(RECOVER_TEST)



$(RECOVER_TEST): $(SRC_DIR)/RecoverTest.cpp
	$(CC) $(CXXFLAGS) $(NOCXXFLAGSRECOVER) -o $@ $^ $(LDLIBS)
