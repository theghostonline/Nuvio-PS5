#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nuvio.h"

int nuvio_mkdir_p(const char *path, int mode) {
  char tmp[512];
  size_t len;

  snprintf(tmp, sizeof(tmp), "%s", path);
  len = strlen(tmp);
  if (len == 0)
    return 0;
  if (tmp[len - 1] == '/')
    tmp[len - 1] = '\0';
  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = '\0';
    if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST)
      return -1;
    *p = '/';
  }
  if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

int nuvio_write_file(const char *path, const void *data, size_t size, int mode) {
  char tmp_path[512];
  const uint8_t *bytes = data;
  size_t off = 0;
  int fd;

  /* Write to a temp file and rename, so a crash never leaves a torn file. */
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
  fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)mode);
  if (fd < 0)
    return -1;
  while (off < size) {
    ssize_t n = write(fd, bytes + off, size - off);
    if (n <= 0) {
      close(fd);
      unlink(tmp_path);
      return -1;
    }
    off += (size_t)n;
  }
  fsync(fd);
  close(fd);
  chmod(tmp_path, (mode_t)mode);
  if (rename(tmp_path, path) != 0) {
    unlink(tmp_path);
    return -1;
  }
  return 0;
}

int nuvio_file_equals(const char *path, const void *data, size_t size) {
  struct stat st;
  uint8_t *buf;
  FILE *f;
  int equal;

  if (stat(path, &st) != 0 || (size_t)st.st_size != size)
    return 0;
  if (!(f = fopen(path, "rb")))
    return 0;
  if (!(buf = malloc(size ? size : 1))) {
    fclose(f);
    return 0;
  }
  equal = fread(buf, 1, size, f) == size && memcmp(buf, data, size) == 0;
  free(buf);
  fclose(f);
  return equal;
}

int nuvio_rm_rf(const char *path) {
  struct stat st;
  DIR *dir;
  struct dirent *entry;

  if (lstat(path, &st) != 0)
    return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(st.st_mode))
    return unlink(path);
  if (!(dir = opendir(path)))
    return -1;
  while ((entry = readdir(dir))) {
    char child[1024];
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    nuvio_rm_rf(child);
  }
  closedir(dir);
  return rmdir(path);
}

uint8_t *fs_readfile(const char *path, size_t *size) {
  struct stat st;
  uint8_t *buf;
  size_t off = 0;
  int fd;

  if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
    return NULL;
  if (!(buf = malloc((size_t)st.st_size)))
    return NULL;
  if ((fd = open(path, O_RDONLY)) < 0) {
    free(buf);
    return NULL;
  }
  while (off < (size_t)st.st_size) {
    ssize_t n = read(fd, buf + off, (size_t)st.st_size - off);
    if (n <= 0) {
      close(fd);
      free(buf);
      return NULL;
    }
    off += (size_t)n;
  }
  close(fd);
  if (size)
    *size = (size_t)st.st_size;
  return buf;
}
