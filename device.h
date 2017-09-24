/*
  https://github.com/ideras/fuse-smallfs
*/

#ifndef _DEVICE_H_
#define _DEVICE_H_

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int device_open(const char *path);
void device_close();
int device_read_sector(char buffer[], int size, int count, int offset);
int device_write_sector(char buffer[], int size, int count, int offset);
void device_flush();

#endif
