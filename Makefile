CC = g++
CFLAGS = -O3 -Wall -std=c++11
CLIB = -lrt -lpthread -llightnvm -lboost_system -lboost_filesystem -lssl -lcrypto -lazurestorage

SRCS = $(wildcard *.cc)
BUILD = $(patsubst %.cc, %, $(SRCS))

all: $(BUILD)

.cc:
	$(CC) $(CFLAGS) $< azure_access.cpp -o $@ $(CLIB)

clean:
	rm -rf $(BUILD)
