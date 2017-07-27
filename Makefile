CC = g++
CFLAGS = -O3 -Wall -std=c++11
CLIB = -lrt -lpthread -llightnvm

SRCS = $(wildcard *.cc)
BUILD = $(patsubst %.cc, %, $(SRCS))

all: $(BUILD)

.cc:
	$(CC) $(CFLAGS) $< -o $@ $(CLIB)

clean:
	rm -rf $(BUILD)
