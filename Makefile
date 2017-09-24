CFLAGS = -g -Wall -DFUSE_USE_VERSION=30 `pkg-config fuse --cflags`
LINKFLAGS = -Wall -lm `pkg-config fuse --libs`

all: bin/fat32

clean:
	rm -rf bin obj

bin:
	mkdir -p bin

bin/fat32: bin obj/fat32.o obj/device.o
	gcc -g -o bin/fat32 obj/* $(LINKFLAGS)

obj:
	mkdir -p obj

obj/fat32.o: obj fat32.c fat32.h
	gcc -g $(CFLAGS) -c fat32.c -o $@

obj/device.o: obj device.c device.h
	gcc -g $(CFLAGS) -c device.c -o $@
