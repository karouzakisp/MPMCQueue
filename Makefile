INCLUDE_DIR := include
SRC_DIR := src
BUILD_DIR := build

CC := g++
CFLAGS := -g -Wall -pthread -I$(INCLUDE_DIR) -O0 #-O3
LDLIBS := -lpthread -lm

VERIFY = 1
ifeq (${VERIFY}, 1)
	CFLAGS += -DVERIFY
endif

SRCS := $(SRC_DIR)/halfhalf.c $(SRC_DIR)/pairwise.c $(SRC_DIR)/harness.cpp
MPMCQUEUE_BENCH := $(BUILD_DIR)/mpmcqueue_bench
RECOVER := $(BUILD_DIR)/recover

.DEFAULT_GOAL := $(MPMCQUEUE_BENCH)
.PHONY: clean

$(MPMCQUEUE_BENCH): $(SRCS)
	$(CC) $(CFLAGS) -o $@  $^ $(LDLIBS)

clean:
	$(RM) $(MPMCQUEUE_BENCH) $(RECOVER)

CXXFLAGS := -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wpedantic \
-Wold-style-cast -Wcast-align -Woverloaded-virtual -Wconversion -Wsign-conversion \
-Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches -Wlogical-op \
-Wnull-dereference -Wuseless-cast -Wdouble-promotion -Wformat=2 \
-Wunused \
-std=c++23 -g3 -O0
#-O3

$(RECOVER): $(SRC_DIR)/recover.cpp
	$(CC) $(CXXFLAGS) -o $@ $^
