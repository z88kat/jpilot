/*******************************************************************************
 * explorer_server.c
 * A module of J-Pilot http://jpilot.org
 *
 * Copyright (C) 2026 by Judd Montgomery
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 ******************************************************************************/

/*
 * The handheld side of the Handheld Explorer: "jpilot-sync --explorer".
 *
 * The Handheld Explorer window (explorer.c) starts this in its own process,
 * as J-Pilot does for a sync, and sends it requests over a pipe; see
 * explorer_proto.h.  Talking to the handheld from a separate process keeps
 * pilot-link out of the GUI process altogether.  That matters on macOS:
 * pilot-link's USB code starts a thread that watches for handhelds and is
 * never stopped, and a GUI process that had used it went on opening every
 * handheld that appeared, so that the sync helper could not open it and
 * every second HotSync hung on "Connecting".
 *
 * This process takes the port lock, waits for HotSync, and keeps the
 * session open (tickling it when idle) until the window asks it to
 * disconnect or goes away.
 */

/********************************* Includes ***********************************/
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <pi-socket.h>
#include <pi-dlp.h>
#include <pi-file.h>
#include <pi-error.h>
#include <pi-buffer.h>

#include "i18n.h"
#include "utils.h"
#include "sync.h"
#include "explorer_proto.h"
#include "explorer_server.h"

/********************************* Constants **********************************/
#define ACCEPT_SLICE_SECS  2      /* how often waiting checks for Disconnect */
#define TICKLE_SECS        7      /* keep an idle connection alive */
#define LOCK_TRIES         15     /* a second apart: a waiting sync letting go */
#define VFS_CHUNK          (32 * 1024)
#define MAX_DEPTH          16     /* folder nesting followed on a card */
#define LIST_BATCH         32

/* Palm OS type of a web clipping application (.pqa) */
#define TYPE_PQA 0x70716120

/********************************* Types **************************************/
typedef struct {
   int volref;
   char *label;
   long used;
   long total;
} volume_t;

typedef enum {
   REQ_LIST,
   REQ_TO_COMPUTER,
   REQ_TO_HANDHELD,
   REQ_DELETE,
   REQ_MKDIR,
   REQ_DISCONNECT
} request_type;

typedef struct {
   request_type type;
   int volref;
   char *dir;
   GPtrArray *items;    /* explorer_item_t */
   GPtrArray *paths;    /* local files */
   char *target;        /* local folder, or the name of a new folder */
} request_t;

typedef enum {
   RES_CONNECTED,
   RES_CONNECT_FAILED,
   RES_LISTING,
   RES_PROGRESS,
   RES_DONE,
   RES_DISCONNECTED
} result_type;

typedef struct {
   result_type type;
   char *message;
   int volref;
   char *dir;
   GPtrArray *items;
   GPtrArray *volumes;
   unsigned long ram_size;
   unsigned long ram_free;
   int refresh;
} result_t;

/******************************* Global vars **********************************/
static FILE *to_window;    /* results */
static int from_window;    /* requests */
static GString *pending;   /* request bytes read but not yet a whole line */

/****************************** Messages **************************************/
static void volume_free(gpointer data)
{
   volume_t *vol = data;

   g_free(vol->label);
   g_free(vol);
}

static void request_free(request_t *req)
{
   g_free(req->dir);
   if (req->items) g_ptr_array_unref(req->items);
   if (req->paths) g_ptr_array_unref(req->paths);
   g_free(req->target);
   g_free(req);
}

static void send_line(GString *line)
{
   fprintf(to_window, "%s\n", line->str);
   fflush(to_window);
   g_string_free(line, TRUE);
}

/* Send a result to the window, and free it */
static void post(result_t *res)
{
   GString *line;
   unsigned int i;

   switch (res->type) {
    case RES_CONNECTED:
      for (i = 0; res->volumes && (i < res->volumes->len); i++) {
         volume_t *vol = g_ptr_array_index(res->volumes, i);

         line = g_string_new(NULL);
         explorer_add_field(line, "VOLUME");
         explorer_add_number(line, vol->volref);
         explorer_add_field(line, vol->label);
         explorer_add_number(line, vol->used);
         explorer_add_number(line, vol->total);
         send_line(line);
      }
      line = g_string_new(NULL);
      explorer_add_field(line, "CONNECTED");
      explorer_add_field(line, res->message);
      explorer_add_number(line, res->ram_size);
      explorer_add_number(line, res->ram_free);
      send_line(line);
      break;

    case RES_CONNECT_FAILED:
      line = g_string_new(NULL);
      explorer_add_field(line, "CONNECT_FAILED");
      explorer_add_field(line, res->message);
      send_line(line);
      break;

    case RES_LISTING:
      for (i = 0; res->items && (i < res->items->len); i++) {
         explorer_item_t *item = g_ptr_array_index(res->items, i);

         line = g_string_new(NULL);
         explorer_add_field(line, "ITEM");
         explorer_add_number(line, item->kind);
         explorer_add_number(line, item->type);
         explorer_add_number(line, item->creator);
         explorer_add_number(line, item->flags);
         explorer_add_number(line, item->size);
         explorer_add_number(line, item->records);
         explorer_add_number(line, item->modified);
         explorer_add_field(line, item->name);
         send_line(line);
      }
      line = g_string_new(NULL);
      explorer_add_field(line, "LISTING");
      explorer_add_number(line, res->volref);
      explorer_add_field(line, res->dir);
      explorer_add_field(line, res->message);
      send_line(line);
      break;

    case RES_PROGRESS:
      line = g_string_new(NULL);
      explorer_add_field(line, "PROGRESS");
      explorer_add_field(line, res->message);
      send_line(line);
      break;

    case RES_DONE:
      line = g_string_new(NULL);
      explorer_add_field(line, "DONE");
      explorer_add_number(line, res->refresh);
      explorer_add_field(line, res->message);
      send_line(line);
      break;

    case RES_DISCONNECTED:
      line = g_string_new(NULL);
      explorer_add_field(line, "DISCONNECTED");
      explorer_add_field(line, res->message);
      send_line(line);
      break;
   }

   g_free(res->message);
   g_free(res->dir);
   if (res->items) g_ptr_array_unref(res->items);
   if (res->volumes) g_ptr_array_unref(res->volumes);
   g_free(res);
}

static void post_progress(const char *format, ...)
{
   result_t *res = g_new0(result_t, 1);
   va_list ap;

   va_start(ap, format);
   res->type = RES_PROGRESS;
   res->message = g_strdup_vprintf(format, ap);
   va_end(ap);
   post(res);
}

static void send_word(const char *word)
{
   GString *line = g_string_new(NULL);

   explorer_add_field(line, word);
   send_line(line);
}

/* Wait up to timeout_ms for a request.  Returns it, or NULL with *gone set
 * if the window has closed its end of the pipe. */
static request_t *read_request(int timeout_ms, int *gone)
{
   struct pollfd pfd;
   char buffer[4096];
   char *newline;
   gchar **fields;
   request_t *req;
   int n, i;

   *gone = FALSE;
   while (!(newline = strchr(pending->str, '\n'))) {
      pfd.fd = from_window;
      pfd.events = POLLIN;
      pfd.revents = 0;
      n = poll(&pfd, 1, timeout_ms);
      if (n == 0) {
         return NULL;
      }
      if ((n < 0) && (errno == EINTR)) {
         continue;
      }
      n = (n < 0) ? -1 : read(from_window, buffer, sizeof(buffer));
      if (n <= 0) {
         *gone = TRUE;
         return NULL;
      }
      g_string_append_len(pending, buffer, n);
   }

   *newline = '\0';
   fields = explorer_split_line(pending->str);
   g_string_erase(pending, 0, newline - pending->str + 1);

   req = g_new0(request_t, 1);
   req->volref = fields[0] && fields[1] ? atoi(fields[1]) : EXPLORER_INTERNAL_MEMORY;
   if (fields[0] && fields[1] && fields[2]) {
      req->dir = fields[2][0] ? g_strdup(fields[2]) : NULL;
   }

   if (!fields[0] || !strcmp(fields[0], "DISCONNECT")) {
      req->type = REQ_DISCONNECT;
   } else if (!strcmp(fields[0], "LIST")) {
      req->type = REQ_LIST;
   } else if (!strcmp(fields[0], "TOCOMPUTER") || !strcmp(fields[0], "DELETE")) {
      int first = 3;

      req->type = strcmp(fields[0], "DELETE") ? REQ_TO_COMPUTER : REQ_DELETE;
      if (req->type == REQ_TO_COMPUTER) {
         req->target = g_strdup(fields[3] ? fields[3] : "");
         first = 4;
      }
      req->items = g_ptr_array_new_with_free_func(explorer_item_free);
      for (i = first; fields[i] && fields[i + 1]; i += 2) {
         explorer_item_t *item = g_new0(explorer_item_t, 1);

         item->kind = atoi(fields[i]);
         item->name = g_strdup(fields[i + 1]);
         g_ptr_array_add(req->items, item);
      }
   } else if (!strcmp(fields[0], "TOHANDHELD")) {
      req->type = REQ_TO_HANDHELD;
      req->paths = g_ptr_array_new_with_free_func(g_free);
      for (i = 3; fields[i]; i++) {
         g_ptr_array_add(req->paths, g_strdup(fields[i]));
      }
   } else if (!strcmp(fields[0], "MKDIR")) {
      req->type = REQ_MKDIR;
      req->target = g_strdup((fields[1] && fields[2] && fields[3]) ? fields[3] : "");
   } else {
      /* Unknown: ignore it, but answer so the window is not left waiting */
      req->type = REQ_LIST;
   }
   g_strfreev(fields);
   return req;
}

/****************************** Handheld **************************************/
/* "dir/name" on this computer that does not exist yet: "name 2.ext" and so
 * on when it does.  Never overwrites a file. */
static char *unique_local_path(const char *dir, const char *name)
{
   char *legal = g_strdup(name);
   char *path, *dot, *base, *ext;
   char *p;
   int n;

   /* '/' cannot be in a file name; ':' is shown as '/' by the Finder */
   for (p = legal; *p; p++) {
      if ((*p == '/') || (*p == ':')) {
         *p = '_';
      }
   }
   path = g_build_filename(dir, legal, NULL);
   if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
      g_free(legal);
      return path;
   }
   g_free(path);

   dot = strrchr(legal, '.');
   if (dot && (dot != legal)) {
      base = g_strndup(legal, dot - legal);
      ext = g_strdup(dot);
   } else {
      base = g_strdup(legal);
      ext = g_strdup("");
   }
   for (n = 2; ; n++) {
      gchar *candidate = g_strdup_printf("%s %d%s", base, n, ext);

      path = g_build_filename(dir, candidate, NULL);
      g_free(candidate);
      if (!g_file_test(path, G_FILE_TEST_EXISTS) || (n > 9999)) {
         break;
      }
      g_free(path);
   }
   g_free(base);
   g_free(ext);
   g_free(legal);
   return path;
}

/* What went wrong with a pilot-link call, as text */
static char *error_text(int sd, int r)
{
   if (r == PI_ERR_DLP_PALMOS) {
      return g_strdup(dlp_strerror(pi_palmos_error(sd)));
   }
   return g_strdup_printf(_("error %d"), r);
}

/* Whether a pilot-link error means the connection has gone */
static int connection_lost(int r)
{
   return (r == PI_ERR_SOCK_DISCONNECTED) || (r == PI_ERR_SOCK_IO) ||
          (r == PI_ERR_SOCK_INVALID) || (r == PI_ERR_SOCK_TIMEOUT);
}

/* The databases in internal memory */
static int worker_list_ram(int sd, GPtrArray *items)
{
   pi_buffer_t *buffer = pi_buffer_new(LIST_BATCH * sizeof(struct DBInfo));
   struct DBInfo info;
   struct DBSizeInfo size;
   int start = 0;
   int r;
   unsigned int i;

   while ((r = dlp_ReadDBList(sd, 0, dlpDBListRAM | dlpDBListMultiple,
                              start, buffer)) > 0) {
      for (i = 0; i < buffer->used / sizeof(struct DBInfo); i++) {
         explorer_item_t *item = g_new0(explorer_item_t, 1);

         memcpy(&info, buffer->data + i * sizeof(struct DBInfo), sizeof(info));
         start = info.index + 1;
         item->name = g_strdup(info.name);
         item->kind = EXPLORER_KIND_DB;
         item->type = info.type;
         item->creator = info.creator;
         item->flags = info.flags;
         item->modified = info.modifyDate;
         g_ptr_array_add(items, item);
      }
   }
   pi_buffer_free(buffer);
   if (connection_lost(r)) {
      return r;
   }

   /* Sizes need a call per database (DLP 1.2, Palm OS 3 and later) */
   for (i = 0; i < items->len; i++) {
      explorer_item_t *item = g_ptr_array_index(items, i);

      memset(&size, 0, sizeof(size));
      r = dlp_FindDBByName(sd, 0, item->name, NULL, NULL, NULL, &size);
      if (connection_lost(r)) {
         return r;
      }
      if (r >= 0) {
         item->size = size.totalBytes;
         item->records = size.numRecords;
      }
   }
   return 0;
}

/* The files and folders in a folder on a card */
static int worker_list_dir(int sd, int volref, const char *dir, GPtrArray *items,
                           int with_details)
{
   struct VFSDirInfo entries[LIST_BATCH];
   unsigned long iterator = vfsIteratorStart;
   FileRef dirref, fileref;
   int count, r, i;
   unsigned int n;

   r = dlp_VFSFileOpen(sd, volref, dir, dlpVFSOpenRead, &dirref);
   if (r < 0) {
      return r;
   }
   while (iterator != (unsigned long) vfsIteratorStop) {
      count = LIST_BATCH;
      r = dlp_VFSDirEntryEnumerate(sd, dirref, &iterator, &count, entries);
      if (r < 0) {
         break;
      }
      for (i = 0; i < count; i++) {
         explorer_item_t *item = g_new0(explorer_item_t, 1);

         item->name = g_strdup(entries[i].name);
         item->kind = (entries[i].attr & vfsFileAttrDirectory) ? EXPLORER_KIND_DIR : EXPLORER_KIND_FILE;
         g_ptr_array_add(items, item);
      }
      if (count == 0) {
         break;
      }
   }
   dlp_VFSFileClose(sd, dirref);
   if (connection_lost(r)) {
      return r;
   }

   if (!with_details) {
      return 0;
   }
   /* Size and date need the file opened */
   for (n = 0; n < items->len; n++) {
      explorer_item_t *item = g_ptr_array_index(items, n);
      char *path = explorer_card_path(dir, item->name);

      r = dlp_VFSFileOpen(sd, volref, path, dlpVFSOpenRead, &fileref);
      g_free(path);
      if (connection_lost(r)) {
         return r;
      }
      if (r < 0) {
         continue;
      }
      if (item->kind == EXPLORER_KIND_FILE) {
         int size = 0;

         if (dlp_VFSFileSize(sd, fileref, &size) >= 0) {
            item->size = size;
         }
      }
      dlp_VFSFileGetDate(sd, fileref, vfsFileDateModified, &item->modified);
      dlp_VFSFileClose(sd, fileref);
   }
   return 0;
}

static int worker_list(int sd, int volref, const char *dir)
{
   result_t *res = g_new0(result_t, 1);
   int r;

   res->type = RES_LISTING;
   res->volref = volref;
   res->dir = g_strdup(dir);
   res->items = g_ptr_array_new_with_free_func(explorer_item_free);

   if (volref == EXPLORER_INTERNAL_MEMORY) {
      post_progress(_("Reading the list of databases..."));
      r = worker_list_ram(sd, res->items);
   } else {
      post_progress(_("Reading %s..."), dir);
      r = worker_list_dir(sd, volref, dir, res->items, TRUE);
   }
   if (r < 0) {
      char *why = error_text(sd, r);

      res->message = g_strdup_printf(_("Unable to read the list: %s"), why);
      g_free(why);
   }
   post(res);
   return connection_lost(r) ? r : 0;
}

/* A stream database holds a plain file -- the Photos application keeps each
 * picture as one, named "3.jpg" and so on -- stored as records that each
 * start with an 8 byte header.  Save the file itself rather than the
 * database. */
static int worker_stream_to_computer(int sd, const struct DBInfo *info,
                                     const char *local_dir, char **error)
{
   pi_buffer_t *buffer;
   recordid_t id;
   struct utimbuf times;
   char *path;
   FILE *out;
   int db, index, attr, category, r;

   r = dlp_OpenDB(sd, 0, dlpOpenRead, info->name, &db);
   if (r < 0) {
      *error = error_text(sd, r);
      return r;
   }
   path = unique_local_path(local_dir, info->name);
   out = g_fopen(path, "wb");
   if (!out) {
      *error = g_strdup_printf(_("cannot create %s"), path);
      dlp_CloseDB(sd, db);
      g_free(path);
      return -1;
   }
   buffer = pi_buffer_new(0xffff);
   for (index = 0; ; index++) {
      r = dlp_ReadRecordByIndex(sd, db, index, buffer, &id, &attr, &category);
      if (r < 0) {
         /* Reading past the last record fails with "not found".  Look now:
          * the next call (closing the database) resets the error. */
         if ((r == PI_ERR_DLP_PALMOS) && (pi_palmos_error(sd) == dlpErrNotFound)) {
            r = 0;
         } else {
            *error = error_text(sd, r);
         }
         break;
      }
      if ((buffer->used > 8) &&
          (fwrite(buffer->data + 8, 1, buffer->used - 8, out) != buffer->used - 8)) {
         *error = g_strdup_printf(_("cannot write %s"), path);
         r = -1;
         break;
      }
   }
   pi_buffer_free(buffer);
   fclose(out);
   dlp_CloseDB(sd, db);

   if (r < 0) {
      g_unlink(path);
      g_free(path);
      return r;
   }
   times.actime = info->createDate;
   times.modtime = info->modifyDate;
   utime(path, &times);
   g_free(path);
   return 0;
}

/* Copy one database from internal memory to this computer */
static int worker_db_to_computer(int sd, const explorer_item_t *item, const char *local_dir,
                                 char **error)
{
   struct DBInfo info;
   struct utimbuf times;
   struct pi_file *pf;
   const char *ext;
   char *file_name, *path;
   int r;

   r = dlp_FindDBByName(sd, 0, item->name, NULL, NULL, &info, NULL);
   if (r < 0) {
      *error = error_text(sd, r);
      return r;
   }
   if (info.flags & dlpDBFlagStream) {
      return worker_stream_to_computer(sd, &info, local_dir, error);
   }
   if (info.flags & dlpDBFlagResource) {
      ext = ".prc";
   } else if (info.type == TYPE_PQA) {
      ext = ".pqa";
   } else {
      ext = ".pdb";
   }
   file_name = g_strconcat(info.name, ext, NULL);
   path = unique_local_path(local_dir, file_name);
   g_free(file_name);

   info.flags &= 0xff;
   pf = pi_file_create(path, &info);
   if (!pf) {
      *error = g_strdup_printf(_("cannot create %s"), path);
      g_free(path);
      return -1;
   }
   r = pi_file_retrieve(pf, sd, 0, NULL);
   pi_file_close(pf);
   if (r < 0) {
      *error = error_text(sd, r);
      g_unlink(path);
      g_free(path);
      return r;
   }
   times.actime = info.createDate;
   times.modtime = info.modifyDate;
   utime(path, &times);
   g_free(path);
   return 0;
}

/* Copy one file from a card to local_path */
static int worker_file_to_computer(int sd, int volref, const char *src,
                                   const char *local_path, char **error)
{
   FileRef ref;
   pi_buffer_t *buffer;
   FILE *out;
   time_t modified = 0;
   int size = 0, done = 0, r;

   r = dlp_VFSFileOpen(sd, volref, src, dlpVFSOpenRead, &ref);
   if (r < 0) {
      *error = error_text(sd, r);
      return r;
   }
   dlp_VFSFileSize(sd, ref, &size);
   dlp_VFSFileGetDate(sd, ref, vfsFileDateModified, &modified);

   out = g_fopen(local_path, "wb");
   if (!out) {
      *error = g_strdup_printf(_("cannot create %s"), local_path);
      dlp_VFSFileClose(sd, ref);
      return -1;
   }
   buffer = pi_buffer_new(VFS_CHUNK);
   while (done < size) {
      r = dlp_VFSFileRead(sd, ref, buffer, MIN(VFS_CHUNK, size - done));
      if (r <= 0) {
         /* Before closing the file, which resets the error */
         *error = (r < 0) ? error_text(sd, r) : g_strdup(_("the file was cut short"));
         break;
      }
      if (fwrite(buffer->data, 1, buffer->used, out) != buffer->used) {
         *error = g_strdup_printf(_("cannot write %s"), local_path);
         r = -1;
         break;
      }
      done += buffer->used;
   }
   pi_buffer_free(buffer);
   fclose(out);
   dlp_VFSFileClose(sd, ref);

   if (done < size) {
      g_unlink(local_path);
      return (r < 0) ? r : -1;
   }
   if (modified) {
      struct utimbuf times;

      times.actime = modified;
      times.modtime = modified;
      utime(local_path, &times);
   }
   return 0;
}

/* Copy a file or a whole folder from a card into local_dir */
static int worker_card_to_computer(int sd, int volref, const char *dir,
                                   const explorer_item_t *item, const char *local_dir,
                                   int depth, int *copied, char **error)
{
   char *src = explorer_card_path(dir, item->name);
   char *local = unique_local_path(local_dir, item->name);
   int r = 0;
   unsigned int i;

   if (item->kind == EXPLORER_KIND_FILE) {
      post_progress(_("Copying %s..."), src);
      r = worker_file_to_computer(sd, volref, src, local, error);
      if (r >= 0) {
         (*copied)++;
      }
   } else if (depth >= MAX_DEPTH) {
      *error = g_strdup(_("folders are nested too deeply"));
      r = -1;
   } else {
      GPtrArray *children = g_ptr_array_new_with_free_func(explorer_item_free);

      if (g_mkdir_with_parents(local, 0755)) {
         *error = g_strdup_printf(_("cannot create %s"), local);
         r = -1;
      } else {
         r = worker_list_dir(sd, volref, src, children, FALSE);
         if (r < 0) {
            *error = error_text(sd, r);
         }
      }
      for (i = 0; (r >= 0) && (i < children->len); i++) {
         r = worker_card_to_computer(sd, volref, src, g_ptr_array_index(children, i),
                                     local, depth + 1, copied, error);
      }
      g_ptr_array_unref(children);
   }
   g_free(src);
   g_free(local);
   return r;
}

/* Copy a local file to a folder on a card, replacing a file of that name */
static int worker_file_to_card(int sd, int volref, const char *dir,
                               const char *local_path, char **error)
{
   char *base = g_path_get_basename(local_path);
   char *dest = explorer_card_path(dir, base);
   char data[VFS_CHUNK];
   FileRef ref;
   FILE *in;
   size_t len;
   int r;

   g_free(base);
   in = g_fopen(local_path, "rb");
   if (!in) {
      *error = g_strdup_printf(_("cannot open %s"), local_path);
      g_free(dest);
      return -1;
   }
   r = dlp_VFSFileCreate(sd, volref, dest);
   if ((r < 0) && !connection_lost(r)) {
      /* Probably there already: replace it */
      if (dlp_VFSFileDelete(sd, volref, dest) >= 0) {
         r = dlp_VFSFileCreate(sd, volref, dest);
      }
   }
   if (r >= 0) {
      /* Read/write, as pi_file_install_VFS() in sync.c opens it; Palm OS
       * refuses write-only (dlpVFSOpenWrite) */
      r = dlp_VFSFileOpen(sd, volref, dest, dlpVFSOpenReadWrite, &ref);
   }
   if (r < 0) {
      *error = error_text(sd, r);
      fclose(in);
      g_free(dest);
      return r;
   }
   while ((r >= 0) && ((len = fread(data, 1, sizeof(data), in)) > 0)) {
      size_t offset = 0;

      /* The handheld may take less than it was given */
      while (len > 0) {
         r = dlp_VFSFileWrite(sd, ref, data + offset, len);
         if (r <= 0) {
            /* Before closing the file, which resets the error */
            *error = (r < 0) ? error_text(sd, r) : g_strdup(_("the handheld stopped accepting data"));
            r = -1;
            break;
         }
         offset += r;
         len -= r;
      }
   }
   if ((r >= 0) && ferror(in)) {
      *error = g_strdup_printf(_("cannot read %s"), local_path);
      r = -1;
   }
   fclose(in);
   dlp_VFSFileClose(sd, ref);
   if (r < 0) {
      dlp_VFSFileDelete(sd, volref, dest);
   }
   g_free(dest);
   return r;
}

/* Delete a file, or a folder and everything in it, from a card */
static int worker_card_delete(int sd, int volref, const char *dir,
                              const explorer_item_t *item, int depth, char **error)
{
   char *path = explorer_card_path(dir, item->name);
   int r = 0;
   unsigned int i;

   if (item->kind == EXPLORER_KIND_DIR) {
      GPtrArray *children = g_ptr_array_new_with_free_func(explorer_item_free);

      if (depth >= MAX_DEPTH) {
         *error = g_strdup(_("folders are nested too deeply"));
         r = -1;
      } else {
         r = worker_list_dir(sd, volref, path, children, FALSE);
         if (r < 0) {
            *error = error_text(sd, r);
         }
      }
      for (i = 0; (r >= 0) && (i < children->len); i++) {
         r = worker_card_delete(sd, volref, path, g_ptr_array_index(children, i),
                                depth + 1, error);
      }
      g_ptr_array_unref(children);
   }
   if (r >= 0) {
      post_progress(_("Deleting %s..."), path);
      r = dlp_VFSFileDelete(sd, volref, path);
      if (r < 0) {
         *error = error_text(sd, r);
      }
   }
   g_free(path);
   return r;
}

/* Carry out a request.  Returns a negative number if the connection has
 * gone. */
static int worker_handle(int sd, request_t *req)
{
   result_t *res;
   char *error = NULL;
   int ok = 0, failed = 0, r = 0, copied = 0;
   unsigned int i;
   GString *failures = g_string_new(NULL);

   if (req->type == REQ_LIST) {
      g_string_free(failures, TRUE);
      return worker_list(sd, req->volref, req->dir);
   }

   res = g_new0(result_t, 1);
   res->type = RES_DONE;

   switch (req->type) {
    case REQ_TO_COMPUTER:
      for (i = 0; i < req->items->len; i++) {
         explorer_item_t *item = g_ptr_array_index(req->items, i);

         if (req->volref == EXPLORER_INTERNAL_MEMORY) {
            post_progress(_("Copying %s (%u of %u)..."), item->name, i + 1, req->items->len);
            r = worker_db_to_computer(sd, item, req->target, &error);
            if (r >= 0) copied++;
         } else {
            r = worker_card_to_computer(sd, req->volref, req->dir, item, req->target,
                                        0, &copied, &error);
         }
         if (r < 0) {
            failed++;
            g_string_append_printf(failures, "\n%s: %s", item->name, error ? error : "?");
         }
         g_clear_pointer(&error, g_free);
         if (connection_lost(r)) break;
      }
      res->message = g_strdup_printf(
         g_dngettext(NULL, "Copied %d item to %s.", "Copied %d items to %s.", copied),
         copied, req->target);
      break;

    case REQ_TO_HANDHELD:
      for (i = 0; i < req->paths->len; i++) {
         const char *path = g_ptr_array_index(req->paths, i);
         char *base = g_path_get_basename(path);

         post_progress(_("Copying %s to the handheld (%u of %u)..."), base, i + 1, req->paths->len);
         if (req->volref == EXPLORER_INTERNAL_MEMORY) {
            struct pi_file *pf = pi_file_open(path);

            if (!pf) {
               error = g_strdup(_("not a Palm database or application"));
               r = -1;
            } else {
               r = pi_file_install(pf, sd, 0, NULL);
               if (r < 0) error = error_text(sd, r);
               pi_file_close(pf);
            }
         } else {
            r = worker_file_to_card(sd, req->volref, req->dir, path, &error);
         }
         if (r < 0) {
            failed++;
            g_string_append_printf(failures, "\n%s: %s", base, error ? error : "?");
         } else {
            ok++;
         }
         g_clear_pointer(&error, g_free);
         g_free(base);
         if (connection_lost(r)) break;
      }
      res->message = g_strdup_printf(
         g_dngettext(NULL, "Copied %d item to the handheld.", "Copied %d items to the handheld.", ok), ok);
      res->refresh = TRUE;
      break;

    case REQ_DELETE:
      for (i = 0; i < req->items->len; i++) {
         explorer_item_t *item = g_ptr_array_index(req->items, i);

         if (req->volref == EXPLORER_INTERNAL_MEMORY) {
            post_progress(_("Deleting %s..."), item->name);
            r = dlp_DeleteDB(sd, 0, item->name);
            if (r < 0) error = error_text(sd, r);
         } else {
            r = worker_card_delete(sd, req->volref, req->dir, item, 0, &error);
         }
         if (r < 0) {
            failed++;
            g_string_append_printf(failures, "\n%s: %s", item->name, error ? error : "?");
         } else {
            ok++;
         }
         g_clear_pointer(&error, g_free);
         if (connection_lost(r)) break;
      }
      res->message = g_strdup_printf(
         g_dngettext(NULL, "Deleted %d item.", "Deleted %d items.", ok), ok);
      res->refresh = TRUE;
      break;

    case REQ_MKDIR: {
      char *path = explorer_card_path(req->dir, req->target);

      r = dlp_VFSDirCreate(sd, req->volref, path);
      if (r < 0) {
         error = error_text(sd, r);
         failed++;
         g_string_append_printf(failures, "\n%s: %s", path, error);
         g_free(error);
      }
      res->message = (r < 0) ? g_strdup(_("The folder was not created."))
                             : g_strdup_printf(_("Created %s."), path);
      res->refresh = TRUE;
      g_free(path);
      break;
    }

    default:
      break;
   }

   if (failed) {
      char *summary = g_strdup_printf(
         g_dngettext(NULL, "%s  %d failed:%s", "%s  %d failed:%s", failed),
         res->message, failed, failures->str);

      g_free(res->message);
      res->message = summary;
   }
   g_string_free(failures, TRUE);
   post(res);
   return connection_lost(r) ? r : 0;
}


/* Wait for the handheld, in slices so that a disconnect request (or the
 * window going away) is noticed.  Returns the socket, or -1 if asked to
 * stop, or -2 if the port cannot be used. */
static int server_connect(const char *port)
{
   struct pollfd pfd;
   int sd, r;

   for (;;) {
      pfd.fd = from_window;
      pfd.events = POLLIN;
      pfd.revents = 0;
      if (poll(&pfd, 1, 0) > 0) {
         /* Anything from the window while waiting means stop: a
          * DISCONNECT, or the end of the pipe */
         return -1;
      }
      sd = pi_socket(PI_AF_PILOT, PI_SOCK_STREAM, PI_PF_DLP);
      if (sd < 0) {
         return -2;
      }
      if ((pi_bind(sd, port) < 0) || (pi_listen(sd, 1) < 0)) {
         pi_close(sd);
         return -2;
      }
      r = pi_accept_to(sd, 0, 0, ACCEPT_SLICE_SECS);
      if (r > 0) {
         return r;
      }
      if (r == 0) {
         /* The libusb backend's timeout leaves the socket open */
         pi_close(sd);
      } else if (r != PI_ERR_SOCK_TIMEOUT) {
         /* pi_accept_to() has closed the socket */
         return -2;
      }
   }
}

int explorer_server(int out_fd, int in_fd, const char *port)
{
   struct SysInfo sys_info;
   struct PilotUser user;
   struct CardInfo card;
   result_t *res;
   request_t *req;
   int volrefs[8];
   int num_vols = 8;
   int lock_fd = -1;
   int sd, r, i, gone;
   int lost = FALSE;

   to_window = fdopen(out_fd, "w");
   from_window = in_fd;
   pending = g_string_new(NULL);
   if (!to_window) {
      return EXIT_FAILURE;
   }

   /* A sync that was waiting for a handheld may take a moment to let go */
   for (i = 0; jp_port_trylock(&lock_fd) != EXIT_SUCCESS; i++) {
      if (i + 1 >= LOCK_TRIES) {
         res = g_new0(result_t, 1);
         res->type = RES_CONNECT_FAILED;
         res->message = g_strdup(_("The handheld port is in use by another J-Pilot or jpilot-sync. "
                                   "Close it, then open the Handheld Explorer again."));
         post(res);
         return EXIT_FAILURE;
      }
      sleep(1);
   }
   send_word("WAITING");

   sd = server_connect(port);
   if (sd < 0) {
      res = g_new0(result_t, 1);
      res->type = RES_CONNECT_FAILED;
      if (sd == -2) {
         res->message = g_strdup_printf(_("Unable to use the port %s. Check the sync port in Preferences."),
                                        port);
      }
      post(res);
      jp_port_unlock(lock_fd);
      return EXIT_FAILURE;
   }

   res = g_new0(result_t, 1);
   res->type = RES_CONNECTED;
   res->volumes = g_ptr_array_new_with_free_func(volume_free);

   memset(&sys_info, 0, sizeof(sys_info));
   dlp_ReadSysInfo(sd, &sys_info);
   memset(&user, 0, sizeof(user));
   dlp_ReadUserInfo(sd, &user);
   memset(&card, 0, sizeof(card));
   if (dlp_ReadStorageInfo(sd, 0, &card) >= 0) {
      res->ram_size = card.ramSize;
      res->ram_free = card.ramFree;
   }

   /* Memory cards (Palm OS 4 and later) */
   if (dlp_VFSVolumeEnumerate(sd, &num_vols, volrefs) >= 0) {
      for (i = 0; (i < num_vols) && (i < 8); i++) {
         volume_t *vol = g_new0(volume_t, 1);
         char label[256];
         int len = sizeof(label);

         vol->volref = volrefs[i];
         label[0] = '\0';
         if ((dlp_VFSVolumeGetLabel(sd, volrefs[i], &len, label) < 0) || !label[0]) {
            g_strlcpy(label, _("Card"), sizeof(label));
         }
         vol->label = g_strdup(label);
         dlp_VFSVolumeSize(sd, volrefs[i], &vol->used, &vol->total);
         g_ptr_array_add(res->volumes, vol);
      }
   }

   /* Shows "Synchronizing" on the handheld while connected */
   dlp_OpenConduit(sd);

   res->message = g_strdup_printf(_("Connected to \"%s\" (Palm OS %lu.%lu)."),
                                  user.username[0] ? user.username : _("no user name"),
                                  (sys_info.romVersion >> 24) & 0xff,
                                  (sys_info.romVersion >> 20) & 0x0f);
   post(res);

   if (worker_list(sd, EXPLORER_INTERNAL_MEMORY, NULL) < 0) {
      lost = TRUE;
   }

   while (!lost) {
      req = read_request(TICKLE_SECS * 1000, &gone);
      if (gone) {
         /* J-Pilot has gone: still end the session properly */
         break;
      }
      if (!req) {
         /* Keep the handheld from giving up on an idle connection */
         r = pi_tickle(sd);
         if (connection_lost(r)) {
            lost = TRUE;
         }
         continue;
      }
      if (req->type == REQ_DISCONNECT) {
         request_free(req);
         break;
      }
      r = worker_handle(sd, req);
      request_free(req);
      if (r < 0) {
         lost = TRUE;
      }
   }

   if (!lost) {
      dlp_AddSyncLogEntry(sd, _("J-Pilot Handheld Explorer\n"));
      dlp_EndOfSync(sd, dlpEndCodeNormal);
   }
   pi_close(sd);

   res = g_new0(result_t, 1);
   res->type = RES_DISCONNECTED;
   if (lost) {
      res->message = g_strdup(_("The connection to the handheld was lost."));
   }
   post(res);
   jp_port_unlock(lock_fd);
   return EXIT_SUCCESS;
}
