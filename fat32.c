#include "device.h"
#include "fat32.h"

#include <math.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>

void *fat32_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
  bpb = (struct bios_param_block*)malloc(sizeof(struct bios_param_block));

  device_read_sector((char*)bpb, sizeof(struct bios_param_block), 1, BPB_OFFSET);

  fat_offset = bpb->reserved_sectors * bpb->bytes_sector;
  clusters_offset = fat_offset + (bpb->fat_amount * bpb->sectors_per_fat * bpb->bytes_sector);
  device_read_sector((char*)&last_cluster, 4, 1, fat_offset + 4);
  return NULL;
}

int GetNextFreePos()
{
  int fat_size = bpb->bytes_sector * bpb->sectors_per_fat;
  char *fat_buffer = malloc(fat_size);
  int *numero;

  device_read_sector(fat_buffer, fat_size, 1, fat_offset);

  numero = (int*)fat_buffer;
  for (int c = 0; c < fat_size/4; c++)
  {
    if(numero[c] == 0)
    {
      return c;
    }
  }
  return -1;
}

int fat32_getattr (const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
  printf("[GETATTR] %s\n", path);

  // GNU's definitions of the attributes (http://www.gnu.org/software/libc/manual/html_node/Attribute-Meanings.html):
  // 		st_uid: 	The user ID of the file’s owner.
  //		st_gid: 	The group ID of the file.
  //		st_atime: 	This is the last access time for the file.
  //		st_mtime: 	This is the time of the last modification to the contents of the file.
  //		st_mode: 	Specifies the mode of the file. This includes file type information (see Testing File Type) and the file permission bits (see Permission Bits).
  //		st_nlink: 	The number of hard links to the file. This count keeps track of how many directories have entries for this file. If the count is ever decremented to zero, then the file itself is discarded as soon
  //						as no process still holds it open. Symbolic links are not counted in the total.
  //		st_size:	This specifies the size of a regular file in bytes. For files that are really devices this field isn’t usually meaningful. For symbolic links this specifies the length of the file name the link refers to.

  stbuf->st_uid = getuid(); // The owner of the file/directory is the user who mounted the filesystem
  stbuf->st_gid = getgid(); // The group of the file/directory is the same as the group of the user who mounted the filesystem
  stbuf->st_atime = time( NULL ); // The last "a"ccess of the file/directory is right now
  stbuf->st_mtime = time( NULL ); // The last "m"odification of the file/directory is right now
  if ( strcmp( path, "/" ) == 0 )
  {
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
  }
  else
  {
    struct directory_entry *dir_entry = resolve(path);
    if(dir_entry != NULL){
      if(dir_entry->Attributes & (1<<4))
        stbuf->st_mode = S_IFDIR;
      else
        stbuf->st_mode = S_IFREG | 0644;

      stbuf->st_nlink = 1;
      stbuf->st_size = dir_entry->Filesize;
    }
  }

  return 0;
}

int fat32_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags)
{
  struct directory_entry *dir_entry = resolve(path);
  int next = ((dir_entry->First_Cluster_High<<16)|dir_entry->First_Cluster_Low);
  int num_entries = (bpb->sectors_cluster*bpb->bytes_sector) / sizeof(struct directory_entry);
  char *cluster_buffer = (char*)malloc(bpb->sectors_cluster*bpb->bytes_sector);

  device_read_sector(cluster_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster * bpb->bytes_sector) * (next - 2)));

  for(int c = 0; c < num_entries; c ++)
  {
    memcpy(dir_entry, cluster_buffer + (sizeof(struct directory_entry) * c), sizeof(struct directory_entry));
    if(is_dir_entry_empty(dir_entry))
      break;

    if(*((uint8_t*)dir_entry) != 0xE5 && !(dir_entry->Attributes & (1<<3)) && !(dir_entry->Attributes & 1))
    {
      char *name = get_long_filename(next, c);
      filler(buf, name, NULL, 0, FUSE_FILL_DIR_PLUS);
      free(name);
    }
  }

  return 0;
}

int fat32_open(const char* path, struct fuse_file_info* fi)
{
  struct directory_entry *dir_entry = resolve(path);
  if(dir_entry == NULL)
  {
    free(dir_entry);
    return -1;
  }

  fi->fh = ((dir_entry->First_Cluster_High<<16)|dir_entry->First_Cluster_Low);
  free(dir_entry);
  return 0;
}

int fat32_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
  pthread_mutex_lock(&read_mutex);
  struct directory_entry *dir_entry = resolve(path);
  if(dir_entry == NULL || !fi->fh)
  {
    printf("Path is nonexistant");
    free(dir_entry);
    pthread_mutex_unlock(&read_mutex);
    return -1;
  }

  int cluster_size = (bpb->sectors_cluster*bpb->bytes_sector);
  int next = fi->fh;

  int cluster_offsets = floor((offset*1.0)/cluster_size);
  offset -= cluster_offsets * cluster_size;

  for(int x = 0; x < cluster_offsets; x++){
    get_next_cluster(&next);
  }

  char *cluster_buffer = (char*)malloc(size);
  for(int x = 0; x < ceil((size*1.0)/cluster_size); x++){
    if(x == 0)
      device_read_sector(cluster_buffer, cluster_size - offset, 1, offset + clusters_offset + (cluster_size * (next - 2)));
    else 
      device_read_sector(cluster_buffer + (x * cluster_size), cluster_size, 1, clusters_offset + (cluster_size * (next - 2)));

    if(!remaining_clusters(next))
      break;
    get_next_cluster(&next);
  }
  memcpy(buf, cluster_buffer, size);
  free(dir_entry);
  free(cluster_buffer);
  pthread_mutex_unlock(&read_mutex);
  return size;
}


int fat32_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info * fi){
  int cluster_size = (bpb->sectors_cluster*bpb->bytes_sector);
  struct directory_entry *dir_entry = resolve(path);
  if(dir_entry == NULL || !fi->fh)
  {
    printf("Path is nonexistant");
    free(dir_entry);
    return -1;
  }
  int next = fi->fh;
  int last;
  int cont = 0;
  int *zero_int = malloc(sizeof(int));
  *zero_int = 0x0;
  char *cluster_buffer = (char*)malloc(bpb->sectors_cluster*bpb->bytes_sector);

  while(next != last_cluster){
    if(size - cont < cluster_size){
      memcpy(cluster_buffer, &buffer[cont], size - cont);
      while(next != last_cluster){
        last = next;
        get_next_cluster(&next);
        device_write_sector((char*)zero_int, sizeof(int), 1, fat_offset + (last * 4));
      }
      device_write_sector((char*)&last_cluster, sizeof(int), 1, fat_offset + (last * 4));
      break;
    }else{
      memcpy(cluster_buffer, &buffer[cont], cluster_size);
    }
    device_write_sector(cluster_buffer, cluster_size, 1, clusters_offset + (cluster_size * (next - 2)));
    last = next;
    get_next_cluster(&next);
    cont += cluster_size;
  }

  while(cont < size){
    int new = GetNextFreePos();
    device_write_sector((char*)&new, sizeof(int), 1, fat_offset + (last * 4));
    if(size - cont < cluster_size){
      memcpy(cluster_buffer, &buffer[cont], size - cont);
    }else{
      memcpy(cluster_buffer, &buffer[cont], cluster_size);
    }
    device_write_sector(cluster_buffer, cluster_size, 1, clusters_offset + (cluster_size * (new - 2)));
    last = new;
    device_write_sector((char*)&last_cluster, sizeof(int), 1, fat_offset + (last * 4));
    cont += cluster_size;
  }

  return cont;
}

struct directory_entry *resolve(const char *path)
{
  int next = bpb->root_cluster_number;

  char *token = strtok(path, "/");
  int dir_entries_per_cluster = (bpb->sectors_cluster*bpb->bytes_sector) / sizeof(struct directory_entry);
  struct directory_entry *dir_entry = (struct directory_entry*) malloc(sizeof(struct directory_entry));
  char *cluster_buffer = (char*)malloc(bpb->sectors_cluster*bpb->bytes_sector);
  device_read_sector(cluster_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster*bpb->bytes_sector)*(next - 2)));

  if(!strcmp(path, "/"))
  {
    dir_entry->First_Cluster_High = 0xFF00 & bpb->root_cluster_number;
    dir_entry->First_Cluster_Low = 0x00FF & bpb->root_cluster_number;
    return dir_entry;
  }

  while(token != NULL){
    for(int x = 0; x < dir_entries_per_cluster; x++)
    {
      memcpy(dir_entry, cluster_buffer + (sizeof(struct directory_entry) * x), sizeof(struct directory_entry));
      if(*((uint8_t*)dir_entry) == 0){
        free(cluster_buffer);
        free(dir_entry);
        return NULL;
      }

      char *lfn = get_long_filename(next, x);
      if(!strncmp(token, lfn, strlen(lfn)+1))
      {
        free(lfn);
        if(dir_entry->Attributes & 0b00010000)
        {
          next = ((dir_entry->First_Cluster_High<<16)|dir_entry->First_Cluster_Low);
          device_read_sector(cluster_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster*bpb->bytes_sector)*(next - 2)));
          token = strtok(NULL, "/");
          if(token == NULL){
            free(cluster_buffer);
            return dir_entry;
          }
          x = 0;
          break;
        }else{
          token = strtok(NULL, "/");
          free(cluster_buffer);
          if(token == NULL){
            return dir_entry;
          }
          return NULL;
        }
      }else
        free(lfn);
    }
  }
  free(dir_entry);
  return NULL;
}

int remaining_clusters(int starting_cluster)
{
  int current_cluster = starting_cluster;
  int remaining_clusters = 0;

  do
  {
    get_next_cluster(&current_cluster);
    remaining_clusters++;
  } while(current_cluster != last_cluster);
  return remaining_clusters - 1;
}

void get_next_cluster(int *current_cluster)
{
  device_read_sector((char*)current_cluster, sizeof(int), 1, fat_offset + (*current_cluster * 4));
}

void delete_dir(const char* path, struct directory_entry *to_del)
{
  int current_cluster = bpb->root_cluster_number;
  char *cluster_buffer = (char*)malloc(bpb->sectors_cluster*bpb->bytes_sector);
  char *path_copy = strdup(path);
  char *token = strtok(path_copy, "/");
  int dir_entries_per_cluster = (bpb->sectors_cluster*bpb->bytes_sector) / sizeof(struct directory_entry);
  struct directory_entry *dir_entry = (struct directory_entry*) malloc(sizeof(struct directory_entry));

  device_read_sector(cluster_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster*bpb->bytes_sector)*(current_cluster - 2)));
  if(!strcmp(path_copy, "/"))
  {
    dir_entry->First_Cluster_High = 0xFF00 & bpb->root_cluster_number;
    dir_entry->First_Cluster_Low = 0x00FF & bpb->root_cluster_number;
    return;
  }
  while(token != NULL){
    for(int x = 0; x < dir_entries_per_cluster; x++)
    {
      memcpy(dir_entry, cluster_buffer + (sizeof(struct directory_entry) * x), sizeof(struct directory_entry));
      if(*((uint8_t*)dir_entry) == 0){
        free(cluster_buffer);
        free(dir_entry);
        return;
      }

      char *lfn = get_long_filename(current_cluster, x);
      if(!strncmp(token, lfn, strlen(lfn)+1))
      {
        free(lfn);
        if(dir_entry->Attributes & 0b00010000)
        {
          int cluster = ((dir_entry->First_Cluster_High<<16)|dir_entry->First_Cluster_Low);
          device_read_sector(cluster_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster*bpb->bytes_sector)*(cluster - 2)));
          token = strtok(NULL, "/");
          if(token == NULL){
            if(memcmp(dir_entry, to_del, sizeof(struct directory_entry)) == 0){
              dir_entry->Short_Filename[0] = 0xE5;
              memcpy(cluster_buffer + (sizeof(struct directory_entry) * x), dir_entry, sizeof(struct directory_entry));
              device_write_sector(cluster_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster*bpb->bytes_sector)*(current_cluster - 2)));
              return;
            }
          }
          break;
        }else{
          token = strtok(NULL, "/");
          free(path_copy);
          if(token == NULL){
            if(memcmp(dir_entry, to_del, sizeof(struct directory_entry)) == 0){
              dir_entry->Short_Filename[0] = 0xE5;
              memcpy(cluster_buffer + (sizeof(struct directory_entry) * x), dir_entry, sizeof(struct directory_entry));
              device_write_sector(cluster_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster*bpb->bytes_sector)*(current_cluster - 2)));
              return;
            }
          }
          return;
        }
      }
    }
  }
  free(path_copy);
}

int fat32_unlink(const char *file_path){
  struct directory_entry *dir_entry = (struct directory_entry*) malloc(sizeof(struct directory_entry));
  dir_entry = resolve(file_path);
  delete_dir(file_path, dir_entry);
  int next;
  int current;
  int *zero_int = malloc(sizeof(int));
  *zero_int = 0x0;
  next = (dir_entry->First_Cluster_High << 16) | dir_entry->First_Cluster_Low;
  
  while(next != last_cluster){
    current = next;
    get_next_cluster(&next);
    device_write_sector((char*)zero_int, sizeof(int), 1, fat_offset + (current * 4));
  }

  device_flush();
  return 0;
}

int fat32_truncate(struct fuse_fs *fs, const char *path, off_t size, struct fuse_file_info *fi){
  return 0;
}

int fat32_rmdir(const char *file_path){
  struct directory_entry *dir_entry = (struct directory_entry*) malloc(sizeof(struct directory_entry));
  dir_entry = resolve(file_path);

  if(!(dir_entry->Attributes & 0b00010000))
  {
    printf("No se encontro el folder\n");
    return -1;
  }
  delete_dir(file_path, dir_entry);
  int next;
  int current;
  int *zero_int = malloc(sizeof(int));
  *zero_int = 0x0;
  next = (dir_entry->First_Cluster_High << 16) | dir_entry->First_Cluster_Low;
  
  while(next != last_cluster){
    current = next;
    get_next_cluster(&next);
    device_write_sector((char*)zero_int, sizeof(int), 1, fat_offset + (current * 4));
  }

  device_flush();
  return 0;
}


int is_dir_entry_empty(struct directory_entry *dir_entry)
{
  for(int x = 0; x < sizeof(struct directory_entry); x++)
  {
    if(*((int8_t*)dir_entry) != 0)
      return 0;
  }
  return 1;
}

char *get_long_filename(int cluster, int entry)
{
  char *tmp_cluster_buffer = (char*)malloc(bpb->bytes_sector * bpb->sectors_cluster);
  device_read_sector(tmp_cluster_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster*bpb->bytes_sector)*(cluster - 2)));
  struct directory_entry special_dir;
  struct long_filename_entry lfn_entry;

  char *name = (char*)malloc(sizeof(char) * 255);

  memcpy(&special_dir, tmp_cluster_buffer + (sizeof(struct long_filename_entry)*(entry)), sizeof(struct long_filename_entry));
  if(!strncmp("..", special_dir.Short_Filename, 2)){
    strncpy(name, "..", 3);
  }else if(!strncmp(".", special_dir.Short_Filename, 1)){
    strncpy(name, ".", 2);
  }else{

    memcpy(&lfn_entry, tmp_cluster_buffer + (sizeof(struct long_filename_entry)*(entry-1)), sizeof(struct long_filename_entry));
    for(int x = 1, y = 0; x < 20 && lfn_entry.attribute == 15; x++)
    {
      int z;
      for(z = 0; z < 5; z++)
        name[y++] = lfn_entry.name_1[z*2];
      for(z = 0; z < 6; z++)
        name[y++] = lfn_entry.name_2[z*2];
      for(z = 0; z < 2; z++)
        name[y++] = lfn_entry.name_3[z*2];
      memcpy(&lfn_entry, tmp_cluster_buffer + (sizeof(struct long_filename_entry)*(entry-x-1)), sizeof(struct long_filename_entry));
    }
  }
  free(tmp_cluster_buffer);
  return name;
}

int main(int argc, char *argv[])
{
  if(!device_open(realpath("/dev/sdb1", NULL)))
  {
    printf("Cannot open device file %s\n", "/dev/disk/by-label/fuse");
    return -1;
  }

  return fuse_main(argc, argv, &fuse_ops, NULL);
}
