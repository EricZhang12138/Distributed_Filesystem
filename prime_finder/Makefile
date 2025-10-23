SHELL := /bin/bash
CC = gcc
CFLAGS = -Wall -g -std=c99 -pthread
TARGET = prime_finder
OBJS = prime_finder.o primality_test.o utils.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) -lm

prime_finder.o: prime_finder.c primality_test.h utils.h
	$(CC) $(CFLAGS) -c prime_finder.c

primality_test.o: primality_test.c primality_test.h
	$(CC) $(CFLAGS) -c primality_test.c

utils.o: utils.c utils.h
	$(CC) $(CFLAGS) -c utils.c


# Rule to create a large random test dataset and a small prime dataset
setup:
	mkdir -p test_data
	shuf -i 1-1000000 -n 10000 > test_data/input_large_01.txt
	shuf -i 1-1000000 -n 10000 > test_data/input_large_02.txt
	shuf -i 1-1000000 -n 10000 > test_data/input_large_03.txt

clean:
	rm -f $(TARGET) $(OBJS)
	rm -rf output.txt
