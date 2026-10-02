/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "assets.h"
#include "nuvio.h"

NUVIO_INCASSET(www_tgz, "build/www.tar.gz");
/* Short content hash of www.tar.gz, written by the Makefile. */
NUVIO_INCASSET(www_id, "build/www.id");

static char g_www_dir[256];

/* Reads the embedded tar.gz with libarchive and writes entries with plain
 * POSIX calls; libarchive's disk writer trips over PS5 lstat() behaviour. */
static int write_entry_data(struct archive *reader, const char *path) {
  char tmp_path[800];
  const void *block;
  size_t size;
  la_int64_t offset;
  int fd, rc = 0;

  snprintf(tmp_path, sizeof(tmp_path), "%s.part", path);
  if ((fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    nuvio_log("www: open %s failed errno=%d", tmp_path, errno);
    return -1;
  }
  for (;;) {
    int r = archive_read_data_block(reader, &block, &size, &offset);
    if (r == ARCHIVE_EOF)
      break;
    if (r != ARCHIVE_OK) {
      nuvio_log("www: read %s failed: %s", path, archive_error_string(reader));
      rc = -1;
      break;
    }
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
      rc = -1;
      break;
    }
    for (size_t done = 0; done < size;) {
      ssize_t n = write(fd, (const char *)block + done, size - done);
      if (n <= 0) {
        nuvio_log("www: write %s failed errno=%d", tmp_path, errno);
        rc = -1;
        break;
      }
      done += (size_t)n;
    }
    if (rc)
      break;
  }
  close(fd);
  if (rc == 0 && rename(tmp_path, path) != 0) {
    nuvio_log("www: rename %s failed errno=%d", path, errno);
    rc = -1;
  }
  if (rc)
    unlink(tmp_path);
  return rc;
}

int nuvio_extract_tgz(const uint8_t *data, size_t size, const char *dest) {
  struct archive *reader = archive_read_new();
  struct archive_entry *entry;
  int rc = 0, files = 0;

  archive_read_support_format_tar(reader);
  archive_read_support_filter_gzip(reader);
  if (archive_read_open_memory(reader, (void *)data, size) != ARCHIVE_OK) {
    nuvio_log("www: open embedded archive failed: %s", archive_error_string(reader));
    archive_read_free(reader);
    return -1;
  }

  while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
    char path[768];
    const char *name = archive_entry_pathname(entry);
    mode_t type = archive_entry_filetype(entry);

    if (!name || strstr(name, ".."))
      continue;
    while (name[0] == '.' && name[1] == '/')
      name += 2;
    if (!name[0] || !strcmp(name, "."))
      continue;
    snprintf(path, sizeof(path), "%s/%s", dest, name);
    if (path[strlen(path) - 1] == '/')
      path[strlen(path) - 1] = '\0';

    if (type == AE_IFDIR) {
      if (nuvio_mkdir_p(path, 0755) != 0) {
        nuvio_log("www: mkdir %s failed errno=%d", path, errno);
        rc = -1;
        break;
      }
      continue;
    }
    if (type != AE_IFREG)
      continue;
    {
      char parent[768];
      char *slash;
      snprintf(parent, sizeof(parent), "%s", path);
      if ((slash = strrchr(parent, '/'))) {
        *slash = '\0';
        nuvio_mkdir_p(parent, 0755);
      }
    }
    if (write_entry_data(reader, path) != 0) {
      rc = -1;
      break;
    }
    files++;
  }
  nuvio_log("www: extracted %d files to %s", files, dest);
  archive_read_free(reader);
  return rc;
}

/* Deletes every build directory except the current one. */
static void remove_stale_builds(const char *keep) {
  DIR *dir = opendir(NUVIO_WWW_DIR);
  struct dirent *entry;

  if (!dir)
    return;
  while ((entry = readdir(dir))) {
    char path[512];
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
        !strcmp(entry->d_name, keep))
      continue;
    snprintf(path, sizeof(path), "%s/%s", NUVIO_WWW_DIR, entry->d_name);
    nuvio_log("www: removing stale build %s", path);
    nuvio_rm_rf(path);
  }
  closedir(dir);
}

const char *nuvio_www_prepare(void) {
  char id[64] = {0};
  char marker[320];
  struct stat st;

  /* A web build copied to www-dev (over FTP) wins, for fast iteration. */
  if (stat(NUVIO_WWW_OVERRIDE_DIR "/index.html", &st) == 0) {
    nuvio_log("www: serving override %s", NUVIO_WWW_OVERRIDE_DIR);
    snprintf(g_www_dir, sizeof(g_www_dir), "%s", NUVIO_WWW_OVERRIDE_DIR);
    return g_www_dir;
  }

  snprintf(id, sizeof(id), "%.*s", (int)(www_id_size < 63 ? www_id_size : 63),
           (const char *)www_id);
  id[strcspn(id, "\r\n ")] = '\0';
  snprintf(g_www_dir, sizeof(g_www_dir), "%s/%s", NUVIO_WWW_DIR, id);
  snprintf(marker, sizeof(marker), "%s/.complete", g_www_dir);

  if (stat(marker, &st) == 0) {
    nuvio_log("www: build %s already unpacked", id);
    return g_www_dir;
  }

  nuvio_mkdir_p(NUVIO_WWW_DIR, 0755);
  remove_stale_builds(id);
  nuvio_rm_rf(g_www_dir);
  if (nuvio_mkdir_p(g_www_dir, 0755) != 0 || nuvio_extract_tgz(www_tgz, www_tgz_size, g_www_dir) != 0) {
    nuvio_log("www: unpack of build %s failed", id);
    return NULL;
  }
  nuvio_write_file(marker, id, strlen(id), 0644);
  return g_www_dir;
}
