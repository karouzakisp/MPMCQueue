INCLUDE_DIR := include
SRC_DIR := src

CC := g++
CFLAGS := -g -Wall -pthread -I$(INCLUDE_DIR) -O0 #-O3
LDLIBS := -lpthread -lm

VERIFY = 1
ifeq (${VERIFY}, 1)
	CFLAGS += -DVERIFY
endif


TARGET := mpmcqueue_bench
SRCS := $(SRC_DIR)/halfhalf.c $(SRC_DIR)/pairwise.c $(SRC_DIR)/harness.cpp

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

clean:
	$(RM) $(TARGET)
