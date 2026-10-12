CC = gcc
TARGET = build/nektumshield.so
SRCS = ../../main/ns_core.c ../../main/ns_stats.c ./ns_cod4x.c

OBJS = $(SRCS:.c=.o)

CFLAGS = -m32 -O3 -Wall -fPIC -I. -I..
LDFLAGS = -m32 -shared -lm -lpthread

all: prepare $(TARGET)

prepare:
	@mkdir -p build

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $(TARGET)
	@echo "\nSuccessful compiling! Nektum Shield available in: $(TARGET)\n"

%.o: %.c ../../main/ns_core.h ../../main/ns_stats.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(OBJS)

all: prepare $(TARGET)
	@rm -f $(OBJS)