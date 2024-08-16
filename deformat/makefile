# Determine the compiler based on architecture
ifeq ($(shell uname -m),aarch64)
    CC=gcc
else
    CC=aarch64-linux-gnu-gcc
endif

all: deformat.o
	$(CC) -g -o deformat deformat.o

deformat.o: deformat.c
	$(CC) -g -c deformat.c

clean:
	rm deformat deformat.o
	# rm trc_*.dat trc_*.out trc_*.hum
