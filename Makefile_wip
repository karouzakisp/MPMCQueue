OBJ_DIR := obj
INCLUDE_DIR := include
BIN_DIR := .

CC = g++
CFLAGS = -g -Wall -O3 -pthread
LDLIBS = -lpthread -lm 


C_SRC = $(wildcard $(SRC_DIR)/*.c)
CPP_SRC = $(wildcard $(SRC_DIR)/*.cpp)

OBJS:= $(SRC:$(C_SRC)/%.c=$(OBJ_DIR)/%.o)  
OBJS = $(CPP_SRC:.cpp=$(OBJ_DIR)/%.o) $(C_SRC:.c=.o) 

TARGET = mpmcqueue 



mpmcqueue: 
	g++ -O3 $(CFLAGS) $(OBJS) $^ -o $(TARGET)

clean:
	rm mpmcq; 
	rm -rf OBJ_DIR;
