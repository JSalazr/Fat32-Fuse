/*
  https://github.com/ideras/fuse-smallfs
*/

#include <stdlib.h>
#include <stdio.h>
#include "device.h"

static const char *device_path;
static FILE *f;

int device_open(const char *path)
{
    device_path = path;
    f = fopen(path, "r+");

    if (f == NULL)
    {
      perror("Fopen");
      return 0;
    }
    return 1;
}

void device_close()
{
    fflush(f);
    fclose(f);
}

//Consider using void pointers
int device_read_sector(char buffer[], int size, int count, int offset)
{
    fseek(f, offset, SEEK_SET);

    return ( fread(buffer, count, size, f) == (size*count) );
}

int device_write_sector(char buffer[], int size, int count, int offset)
{
    fseek(f, offset, SEEK_SET);

    return ( fwrite(buffer, count, size, f) == (size*count) );
}

void device_flush()
{
    fflush(f);
}
