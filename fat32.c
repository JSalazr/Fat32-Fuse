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
  printf("[INIT]\n");
  bpb = (struct bios_param_block*)malloc(sizeof(struct bios_param_block));

  device_read_sector((char*)bpb, sizeof(struct bios_param_block), 1, BPB_OFFSET);
  print_bpb();

  fat_offset = bpb->reserved_sectors * bpb->bytes_sector;
  clusters_offset = fat_offset + (bpb->fat_amount * bpb->sectors_per_fat * bpb->bytes_sector);
  device_read_sector((char*)&end_of_chain, 4, 1, fat_offset + 4);
  // printf("%d\n",clusters_offset);
  //Printing info of third(second?) file. Change multiplier to print another one.
  // device_read_sector((char*)dir_entry, sizeof(struct directory_entry), 1, clusters_offset+(32*2));
  // print_dir_entry();
  //
  // unsigned int next;
  // device_read_sector((char*)&next, sizeof(int), 1, fat_offset+(((dir_entry->First_Cluster_High<<16)|dir_entry->First_Cluster_Low) * 4));
  // struct directory_entry *dir_entry = resolve("/folder");
  // if(dir_entry != NULL){
  //   print_dir_entry(dir_entry);
  // }else{
  //   printf("Not Found\n");
  // }
  // printf("remaining_clusters: %"PRId64"\n", remaining_clusters(207));
  return NULL;
}

int GetNextFreePos()
{
  int clusterReader = bpb->bytes_sector * bpb->sectors_per_fat;
  char *bufferReader = malloc(clusterReader);
  int32_t *numero;

  device_read_sector((char*)bufferReader, clusterReader, 1, fat_offset);

  numero = (int*)bufferReader;
  for (size_t i = 0; i < clusterReader/4; i++)
  {
    if(numero[i] == 0)
    {
      //printf("%" PRId32 "--", numero[i]);
      return i;
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
    stbuf->st_nlink = 2; // Why "two" hardlinks instead of "one"? The answer is here: http://unix.stackexchange.com/a/101536
  }
  else
  {
    struct directory_entry *dir_entry = resolve(path);
    if(dir_entry != NULL){
      if(dir_entry->Attributes & (1<<4))
        stbuf->st_mode = S_IFDIR;
      else
        stbuf->st_mode = S_IFREG | 0644;

      stbuf->st_nlink = 1;  //TODO: ?
      stbuf->st_size = dir_entry->Filesize;
    }
  }

  return 0;
}

int fat32_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags)
{
  printf("[READDIR] %s\n", path);

  struct directory_entry *dir_entry = resolve(path);
  int cluster = ((dir_entry->First_Cluster_High<<16)|dir_entry->First_Cluster_Low);
  int dir_entries_per_cluster = (bpb->sectors_cluster*bpb->bytes_sector) / sizeof(struct directory_entry);
  char *clust_buffer = (char*)malloc(bpb->sectors_cluster*bpb->bytes_sector);
  device_read_sector(clust_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster*bpb->bytes_sector)*(cluster - 2)));
  for(int x = 0; x < dir_entries_per_cluster; x ++)
  {
    memcpy(dir_entry, clust_buffer + (sizeof(struct directory_entry) * x), sizeof(struct directory_entry));
    if(is_dir_entry_empty(dir_entry))
      break;

    if(*((uint8_t*)dir_entry) != 0xE5 && !(dir_entry->Attributes & (1<<3)) && !(dir_entry->Attributes & 1))
    {
      printf("--\n");
      print_dir_entry(dir_entry);
      printf("--\n");
      char *name = get_long_filename(cluster, x);
      filler(buf, name, NULL, 0, FUSE_FILL_DIR_PLUS);
      free(name);
    }
  }

  return 0;
}

int fat32_open(const char* path, struct fuse_file_info* fi)
{
  printf("[OPEN] %s\n", path);
  struct directory_entry *dir_entry = resolve(path);
  if(dir_entry == NULL)
  {
    printf("Path is nonexistant");
    free(dir_entry);
    return -1;
  }

  //File handler in this case is simply the cluster in which it begins
  fi->fh = ((dir_entry->First_Cluster_High<<16)|dir_entry->First_Cluster_Low);
  printf("File Opened Succesfully!\nCluster: %"PRId64"\n", fi->fh);
  free(dir_entry);
  return 0;
}

int fat32_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
  pthread_mutex_lock(&read_mutex);
  printf("[READ] %s, %lu, %lu\n", path, size, offset);
  struct directory_entry *dir_entry = resolve(path);
  if(dir_entry == NULL || !fi->fh)
  {
    printf("Path is nonexistant");
    free(dir_entry);
    pthread_mutex_unlock(&read_mutex);
    return -1;
  }

  printf("Cluster: %" PRId64 "\n", fi->fh);
  int cluster_size = (bpb->sectors_cluster*bpb->bytes_sector);
  int return_size = 0;

  int cluster_offsets = floor(offset/cluster_size);
  uint32_t current_cluster = (uint32_t)fi->fh;
  offset -= cluster_offsets * cluster_size;

  for(int x = 0; x < cluster_offsets; x++, get_next_cluster(&current_cluster));

  char *cluster_buffer = (char*)malloc(size);
  for(int x = 0; x < ceil((size*1.0)/cluster_size); x++){
    printf("CO: %d\t\tOff: %lu\t\tCC: %"PRId32"\t\tCount: %d\n", cluster_offsets, offset, current_cluster, x);

    //On the first read (!x) we take in count the remaining offset that is not divisible by cluster_size
    device_read_sector(cluster_buffer + (x * cluster_size), cluster_size - (!x ? offset : 0), 1, (!x ? offset : 0) + clusters_offset + (cluster_size * (current_cluster - 2)));

    if(!remaining_clusters(current_cluster))
      break;
    get_next_cluster(&current_cluster);
  }
  memcpy(buf, cluster_buffer, size);
  free(dir_entry);
  free(cluster_buffer);
  pthread_mutex_unlock(&read_mutex);
  return size;
}


int fat32_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info * fi){
  printf("%li\n", size);
  int cluster_size = (bpb->sectors_cluster*bpb->bytes_sector);
  struct directory_entry *dir_entry = resolve(path);
  if(dir_entry == NULL || !fi->fh)
  {
    printf("Path is nonexistant");
    free(dir_entry);
    return -1;
  }
  uint64_t next = fi->fh;
  uint64_t last;
  int cont = 0;
  int *zero_int = malloc(sizeof(int));
  *zero_int = 0x0;
  char *cluster_buffer = (char*)malloc(bpb->sectors_cluster*bpb->bytes_sector);

  while(next != end_of_chain){
    printf("%li \n", next);
    if(size - cont < cluster_size){
      memcpy(cluster_buffer, &buffer[cont], size - cont);
      while(next != end_of_chain){
        last = next;
        get_next_cluster(&next);
        device_write_sector((char*)zero_int, sizeof(int), 1, fat_offset + (last * 4));
      }
      device_write_sector((char*)&end_of_chain, sizeof(uint64_t), 1, fat_offset + (last * 4));
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
    device_write_sector((char*)&new, sizeof(uint64_t), 1, fat_offset + (last * 4));
    if(size - cont < cluster_size){
      memcpy(cluster_buffer, &buffer[cont], size - cont);
    }else{
      memcpy(cluster_buffer, &buffer[cont], cluster_size);
    }
    device_write_sector(cluster_buffer, cluster_size, 1, clusters_offset + (cluster_size * (new - 2)));
    last = new;
    device_write_sector((char*)&end_of_chain, sizeof(uint64_t), 1, fat_offset + (last * 4));
    cont += cluster_size;
  }

  return cont;
}

struct directory_entry *resolve(const char *path)
{
  printf("[RESOLVE] %s\n", path);
  int current_cluster = bpb->root_cluster_number;

  char *path_copy = strdup(path);
  char *token = strtok(path_copy, "/");
  int dir_entries_per_cluster = (bpb->sectors_cluster*bpb->bytes_sector) / sizeof(struct directory_entry);
  struct directory_entry *dir_entry = (struct directory_entry*) malloc(sizeof(struct directory_entry));
  char *cluster_buffer = (char*)malloc(bpb->sectors_cluster*bpb->bytes_sector);
  device_read_sector(cluster_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster*bpb->bytes_sector)*(current_cluster - 2)));

  if(!strcmp(path_copy, "/"))
  {
    //This is a hack
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

      char *lfn = get_long_filename(current_cluster, x);
      // printf("Comparing %s and %s\n", token, lfn);
      if(!strncmp(token, lfn, strlen(lfn)+1))
      {
        free(lfn);
        //If the file found is a Directory
        if(dir_entry->Attributes & 0b00010000)
        {
          current_cluster = ((dir_entry->First_Cluster_High<<16)|dir_entry->First_Cluster_Low);
          device_read_sector(cluster_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster*bpb->bytes_sector)*(current_cluster - 2)));
          token = strtok(NULL, "/");
          if(token == NULL){
            free(cluster_buffer);
            return dir_entry;
          }
          x = 0;
          break;
        }else{
          //This way we prevent errors when looking for folder has same name as a file
          token = strtok(NULL, "/");
          free(path_copy);
          free(cluster_buffer);
          return token != NULL ? NULL : dir_entry;
        }
      }else
        free(lfn);
    }
  }
  free(path_copy);
  free(dir_entry);
  return NULL;
}

//Function that returns amount of clusters left in chain
uint64_t remaining_clusters(uint64_t starting_cluster)
{
  uint64_t current_cluster = starting_cluster;
  int remaining_clusters = 0;

  do
  {
    //Reading one int at a time for convenience. Not efficient but prevents errors
    get_next_cluster(&current_cluster);
    remaining_clusters++;
  } while(current_cluster != end_of_chain);
  return remaining_clusters - 1;
}

void get_next_cluster(uint64_t *current_cluster)
{
  device_read_sector((char*)current_cluster, sizeof(uint64_t), 1, fat_offset + (*current_cluster * 4));
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
    //This is a hack
    dir_entry->First_Cluster_High = 0xFF00 & bpb->root_cluster_number;
    dir_entry->First_Cluster_Low = 0x00FF & bpb->root_cluster_number;
    return;
  }
  while(token != NULL){
    //TODO: Create function that returns amount of clusters left in chain and use a while (hasn't reached last cluster+1) it continues and reads next cluster
    for(int x = 0; x < dir_entries_per_cluster; x++)
    {
      memcpy(dir_entry, cluster_buffer + (sizeof(struct directory_entry) * x), sizeof(struct directory_entry));
      if(*((uint8_t*)dir_entry) == 0){
        free(cluster_buffer);
        free(dir_entry);
        return;
      }

      char *lfn = get_long_filename(current_cluster, x);
      // printf("Comparing %s and %s\n", token, lfn);
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
          //This way we prevent errors when looking for folder has same name as a file
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
  uint64_t next;
  uint64_t current;
  int *zero_int = malloc(sizeof(int));
  *zero_int = 0x0;
  next = (dir_entry->First_Cluster_High << 16) | dir_entry->First_Cluster_Low;
  
  while(next != end_of_chain){
    current = next;
    get_next_cluster(&next);
    device_write_sector((char*)zero_int, sizeof(int), 1, fat_offset + (current * 4));
  }

  device_flush();
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
  uint64_t next;
  uint64_t current;
  int *zero_int = malloc(sizeof(int));
  *zero_int = 0x0;
  next = (dir_entry->First_Cluster_High << 16) | dir_entry->First_Cluster_Low;
  
  while(next != end_of_chain){
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

//Should remember to free after use
char *get_long_filename(int cluster, int entry)
{
  // printf("[GET_LONG_FILENAME]\n");
  char *tmp_cluster_buffer = (char*)malloc(bpb->bytes_sector * bpb->sectors_cluster);
  device_read_sector(tmp_cluster_buffer, bpb->bytes_sector, bpb->sectors_cluster, clusters_offset + ((bpb->sectors_cluster*bpb->bytes_sector)*(cluster - 2)));
  struct directory_entry special_dir;
  struct long_filename_entry lfn_entry;

  char *name = (char*)malloc(sizeof(char) * 255); //Max name size (should be 260 but fuck life)

  //Special cases are obligatory
  memcpy(&special_dir, tmp_cluster_buffer + (sizeof(struct long_filename_entry)*(entry)), sizeof(struct long_filename_entry));
  if(!strncmp("..", special_dir.Short_Filename, 2)){
    strncpy(name, "..", 3);
  }else if(!strncmp(".", special_dir.Short_Filename, 1)){
    strncpy(name, ".", 2);
  }else{
    // Look for 1st LFN entry. 20 is max number of lfn dirs

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

//Printing only the relevant data
void print_bpb()
{
  printf("\n-- BIOS Parameter Block -- \n");
  printf("Bytes per Sector: %" PRId16 "\n",         bpb->bytes_sector);
  printf("Sectors per Cluster: %" PRId8 "\n",       bpb->sectors_cluster);
  printf("Reserved Logical Sectors: %" PRId16 "\n", bpb->reserved_sectors);
  printf("File Allocation Tables #: %" PRId8 "\n",  bpb->fat_amount);
  printf("Logical Sectors per FAT: %" PRId32 "\n",  bpb->sectors_per_fat);
  printf("Root Cluster Number: %" PRId32 "\n",      bpb->root_cluster_number);
}

void print_dir_entry(struct directory_entry *dir_entry)
{
  int32_t first_cluster = (dir_entry->First_Cluster_High << 16) | dir_entry->First_Cluster_Low;
  printf("\n-- Directory Entry -- \n");
  printf("Filename: %s\n",                    dir_entry->Short_Filename);
  printf("Attributes: %X\n",                  dir_entry->Attributes);
  printf("First Cluster: %" PRId16 "\n",      first_cluster);
  printf("File Size: %" PRId32 "\n",          dir_entry->Filesize);
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
