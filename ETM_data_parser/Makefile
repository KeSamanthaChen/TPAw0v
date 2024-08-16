SRC_FILES := $(shell find src/*.c)

# Determine the compiler based on architecture
ifeq ($(shell uname -m),aarch64)
    CC=gcc
else
    CC=aarch64-linux-gnu-gcc
endif

all: ctrace

ctrace: $(SRC_FILES)
	$(CC) -o ctrace $(SRC_FILES) -I headers/ -Wall -g

clean:
	rm -f ctrace
