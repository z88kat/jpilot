/*******************************************************************************
 * explorer.c
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
 * The Handheld Explorer (File > Handheld Explorer...): browse the handheld's
 * internal memory and its memory cards, copy databases and files between the
 * handheld and the computer, delete them, and make folders on a card.
 *
 * The connection is a HotSync session that stays open while the window is
 * connected.  One worker thread owns it and makes every pilot-link call; the
 * window hands it requests through a queue, and the worker reports back by
 * scheduling callbacks in the GTK main loop.  libpisock is not thread-safe,
 * so nothing else in J-Pilot may use the handheld meanwhile: listening for
 * HotSync is paused, and the port lock that all J-Pilot processes share is
 * held until the window has disconnected.
 *
 * The worker must not call jp_logf() at levels that reach the output pane,
 * since that is GTK; it reports through its results instead.
 */

/********************************* Includes ***********************************/
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <utime.h>
#include <sys/stat.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>

#include <pi-socket.h>
#include <pi-dlp.h>
#include <pi-file.h>
#include <pi-error.h>
#include <pi-buffer.h>

#include "i18n.h"
#include "utils.h"
#include "log.h"
#include "prefs.h"
#include "sync.h"
#include "explorer.h"

/********************************* Constants **********************************/
#define ACCEPT_SLICE_SECS  2      /* how often waiting checks for Cancel */
#define TICKLE_SECS        7      /* keep an idle connection alive */
#define LOCK_TRY_MS        1000   /* waiting for a sync process to let go */
#define LOCK_TRIES         15
#define VFS_CHUNK          (32 * 1024)
#define MAX_DEPTH          16     /* folder nesting followed on a card */
#define LIST_BATCH         32
#define INTERNAL_MEMORY    (-1)   /* volref of the internal memory */

/* Palm OS type of a web clipping application (.pqa) */
#define TYPE_PQA 0x70716120

enum {
   KIND_DB,
   KIND_FILE,
   KIND_DIR
};

/* The columns of the list of databases or files */
enum {
   COL_ICON,
   COL_NAME,
   COL_TYPE,
   COL_CREATOR,
   COL_SIZE,
   COL_SIZE_SORT,
   COL_RECORDS,
   COL_MODIFIED,
   COL_MODIFIED_SORT,
   COL_INDEX,
   NUM_COLS
};

/* The columns of the list of locations */
enum {
   LOC_ICON,
   LOC_LABEL,
   LOC_VOLREF,
   LOC_NUM_COLS
};

/********************************* Types **************************************/
typedef struct {
   char *name;
   int kind;
   unsigned long type;
   unsigned long creator;
   unsigned int flags;
   unsigned long size;          /* bytes; 0 if not known */
   unsigned long records;
   time_t modified;
} item_t;

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
   int volref;          /* INTERNAL_MEMORY or a card */
   char *dir;           /* the folder on the card */
   GPtrArray *items;    /* item_t: what to copy or delete */
   GPtrArray *paths;    /* local files to copy to the handheld */
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
   int volref;              /* RES_LISTING */
   char *dir;               /* RES_LISTING */
   GPtrArray *items;        /* RES_LISTING */
   GPtrArray *volumes;      /* RES_CONNECTED */
   unsigned long ram_size;  /* RES_CONNECTED */
   unsigned long ram_free;  /* RES_CONNECTED */
   int refresh;             /* RES_DONE: list the location again */
} result_t;

typedef struct {
   /* Set before the worker starts; the worker only reads it */
   char port[128];

   /* Shared with the worker */
   GAsyncQueue *requests;
   gint cancel;             /* stop waiting for the handheld */
   GThread *thread;

   /* Main thread only */
   GtkWidget *window;
   GtkWidget *status_label;
   GtkWidget *spinner;
   GtkWidget *location_view;
   GtkListStore *location_store;
   GtkWidget *list_view;
   GtkListStore *list_store;
   GtkWidget *path_label;
   GtkWidget *message_label;
   GtkWidget *btn_to_computer, *btn_to_handheld, *btn_mkdir, *btn_delete;
   GtkWidget *btn_up, *btn_refresh, *btn_disconnect;
   GtkTreeViewColumn *col_type, *col_creator, *col_records;

   int lock_fd;
   int locked;
   int lock_tries;
   guint lock_timer;
   int connected;
   int busy;
   int closing;
   unsigned long ram_size, ram_free;
   GPtrArray *volumes;      /* volume_t */
   int volref;              /* what the list shows */
   char *dir;
   GPtrArray *items;        /* item_t, as listed */
   char *done_message;      /* result of the last operation, kept in view
                             * while the list is read again after it */
} explorer_t;

/******************************* Global vars **********************************/
/* The one Handheld Explorer, if open */
static explorer_t *ex = NULL;

/* The folder last copied to on this computer */
static char *last_local_dir = NULL;

/* From jpilot.c */
extern int jpilot_sync_connected(void);
extern void jpilot_listen_pause(void);
extern void jpilot_listen_resume(void);

/****************************** Prototypes ************************************/
static gboolean cb_result(gpointer data);
static void request_list(void);
static void update_buttons(void);
static void explorer_finish(void);

/*************************** Small helpers ************************************/
static void item_free(gpointer data)
{
   item_t *item = data;

   g_free(item->name);
   g_free(item);
}

static item_t *item_copy(const item_t *item)
{
   item_t *copy = g_memdup2(item, sizeof(item_t));

   copy->name = g_strdup(item->name);
   return copy;
}

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

static void result_free(result_t *res)
{
   g_free(res->message);
   g_free(res->dir);
   if (res->items) g_ptr_array_unref(res->items);
   if (res->volumes) g_ptr_array_unref(res->volumes);
   g_free(res);
}

/* A four character code such as 'appl' as text */
static void fourcc(unsigned long code, char *str)
{
   int i;

   for (i = 0; i < 4; i++) {
      char c = (code >> (24 - 8 * i)) & 0xff;
      str[i] = g_ascii_isprint(c) ? c : '?';
   }
   str[4] = '\0';
}

/* A path on a card: dir and name joined with one '/' */
static char *card_path(const char *dir, const char *name)
{
   if (!dir || !dir[0]) {
      return g_strconcat("/", name, NULL);
   }
   if (dir[strlen(dir) - 1] == '/') {
      return g_strconcat(dir, name, NULL);
   }
   return g_strconcat(dir, "/", name, NULL);
}

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

/******************************** Worker **************************************/
/* Everything in this section runs in the worker thread. */

static void post(result_t *res)
{
   g_idle_add(cb_result, res);
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

/* Wait for the handheld, in slices so that Cancel works.  Returns the
 * socket, or a negative number: -1 cancelled, -2 the port cannot be used */
static int worker_connect(explorer_t *e)
{
   int sd, r;

   for (;;) {
      if (g_atomic_int_get(&e->cancel)) {
         return -1;
      }
      sd = pi_socket(PI_AF_PILOT, PI_SOCK_STREAM, PI_PF_DLP);
      if (sd < 0) {
         return -2;
      }
      if ((pi_bind(sd, e->port) < 0) || (pi_listen(sd, 1) < 0)) {
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
         item_t *item = g_new0(item_t, 1);

         memcpy(&info, buffer->data + i * sizeof(struct DBInfo), sizeof(info));
         start = info.index + 1;
         item->name = g_strdup(info.name);
         item->kind = KIND_DB;
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
      item_t *item = g_ptr_array_index(items, i);

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
         item_t *item = g_new0(item_t, 1);

         item->name = g_strdup(entries[i].name);
         item->kind = (entries[i].attr & vfsFileAttrDirectory) ? KIND_DIR : KIND_FILE;
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
      item_t *item = g_ptr_array_index(items, n);
      char *path = card_path(dir, item->name);

      r = dlp_VFSFileOpen(sd, volref, path, dlpVFSOpenRead, &fileref);
      g_free(path);
      if (connection_lost(r)) {
         return r;
      }
      if (r < 0) {
         continue;
      }
      if (item->kind == KIND_FILE) {
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
   res->items = g_ptr_array_new_with_free_func(item_free);

   if (volref == INTERNAL_MEMORY) {
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
static int worker_db_to_computer(int sd, const item_t *item, const char *local_dir,
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
                                   const item_t *item, const char *local_dir,
                                   int depth, int *copied, char **error)
{
   char *src = card_path(dir, item->name);
   char *local = unique_local_path(local_dir, item->name);
   int r = 0;
   unsigned int i;

   if (item->kind == KIND_FILE) {
      post_progress(_("Copying %s..."), src);
      r = worker_file_to_computer(sd, volref, src, local, error);
      if (r >= 0) {
         (*copied)++;
      }
   } else if (depth >= MAX_DEPTH) {
      *error = g_strdup(_("folders are nested too deeply"));
      r = -1;
   } else {
      GPtrArray *children = g_ptr_array_new_with_free_func(item_free);

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
   char *dest = card_path(dir, base);
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
                              const item_t *item, int depth, char **error)
{
   char *path = card_path(dir, item->name);
   int r = 0;
   unsigned int i;

   if (item->kind == KIND_DIR) {
      GPtrArray *children = g_ptr_array_new_with_free_func(item_free);

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
         item_t *item = g_ptr_array_index(req->items, i);

         if (req->volref == INTERNAL_MEMORY) {
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
         if (req->volref == INTERNAL_MEMORY) {
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
         item_t *item = g_ptr_array_index(req->items, i);

         if (req->volref == INTERNAL_MEMORY) {
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
      char *path = card_path(req->dir, req->target);

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

static gpointer worker(gpointer data)
{
   explorer_t *e = data;
   struct SysInfo sys_info;
   struct PilotUser user;
   struct CardInfo card;
   result_t *res;
   request_t *req;
   int volrefs[8];
   int num_vols = 8;
   int sd, r, i;
   int lost = FALSE;

   sd = worker_connect(e);
   if (sd < 0) {
      res = g_new0(result_t, 1);
      res->type = RES_CONNECT_FAILED;
      if (sd == -2) {
         res->message = g_strdup_printf(_("Unable to use the port %s. Check the sync port in Preferences."),
                                        e->port);
      }
      post(res);
      return NULL;
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

   if (worker_list(sd, INTERNAL_MEMORY, NULL) < 0) {
      lost = TRUE;
   }

   while (!lost) {
      req = g_async_queue_timeout_pop(e->requests, TICKLE_SECS * G_USEC_PER_SEC);
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
   return NULL;
}

/********************************* Window *************************************/
/* Everything from here on runs in the main thread. */

static void set_status(const char *text)
{
   if (ex && ex->status_label) {
      gtk_label_set_text(GTK_LABEL(ex->status_label), text);
   }
}

static void set_message(const char *text)
{
   if (ex && ex->message_label) {
      gtk_label_set_text(GTK_LABEL(ex->message_label), text ? text : "");
   }
}

static void send_request(request_t *req)
{
   ex->busy = (req->type != REQ_DISCONNECT);
   update_buttons();
   g_async_queue_push(ex->requests, req);
}

static void request_list(void)
{
   request_t *req;

   if (!ex || !ex->connected) {
      return;
   }
   req = g_new0(request_t, 1);
   req->type = REQ_LIST;
   req->volref = ex->volref;
   req->dir = g_strdup(ex->dir);
   send_request(req);
}

static void update_buttons(void)
{
   int ready = ex->connected && !ex->busy;
   int card = (ex->volref != INTERNAL_MEMORY);
   int selected = gtk_tree_selection_count_selected_rows(
      gtk_tree_view_get_selection(GTK_TREE_VIEW(ex->list_view)));

   gtk_widget_set_sensitive(ex->btn_to_computer, ready && selected);
   gtk_widget_set_sensitive(ex->btn_to_handheld, ready);
   gtk_widget_set_sensitive(ex->btn_delete, ready && selected);
   gtk_widget_set_sensitive(ex->btn_mkdir, ready && card);
   gtk_widget_set_sensitive(ex->btn_up, ready && card && ex->dir && strcmp(ex->dir, "/"));
   gtk_widget_set_sensitive(ex->btn_refresh, ready);
   gtk_widget_set_sensitive(ex->location_view, ready);
   gtk_widget_set_sensitive(ex->btn_disconnect, !ex->closing);
   gtk_widget_set_visible(ex->btn_mkdir, card);
   gtk_widget_set_visible(ex->btn_up, card);
   gtk_tree_view_column_set_visible(ex->col_type, !card);
   gtk_tree_view_column_set_visible(ex->col_creator, !card);
   gtk_tree_view_column_set_visible(ex->col_records, !card);
}

static void show_path(void)
{
   char *text;

   if (ex->volref == INTERNAL_MEMORY) {
      char *free_str = g_format_size(ex->ram_free);
      char *size_str = g_format_size(ex->ram_size);

      text = g_strdup_printf(_("Internal memory: %s free of %s"), free_str, size_str);
      g_free(free_str);
      g_free(size_str);
   } else {
      text = g_strdup_printf(_("Card folder: %s"), ex->dir);
   }
   gtk_label_set_text(GTK_LABEL(ex->path_label), text);
   g_free(text);
}

static void fill_list(GPtrArray *items)
{
   unsigned int i;
   GtkTreeIter iter;

   gtk_list_store_clear(ex->list_store);
   if (ex->items) {
      g_ptr_array_unref(ex->items);
   }
   ex->items = g_ptr_array_ref(items);

   for (i = 0; i < items->len; i++) {
      item_t *item = g_ptr_array_index(items, i);
      char type[5], creator[5], modified[32], records[16];
      char *size = item->size ? g_format_size(item->size) : g_strdup("");
      const char *icon;

      fourcc(item->type, type);
      fourcc(item->creator, creator);
      modified[0] = '\0';
      /* 0x83DAC000 (1904) means never modified */
      if (item->modified > 0) {
         strftime(modified, sizeof(modified), "%Y-%m-%d %H:%M", localtime(&item->modified));
      }
      g_snprintf(records, sizeof(records), "%lu", item->records);
      if (item->kind == KIND_DIR) {
         icon = "folder";
      } else if ((item->kind == KIND_DB) && (item->flags & dlpDBFlagStream) &&
                 (g_str_has_suffix(item->name, ".jpg") || g_str_has_suffix(item->name, ".JPG") ||
                  g_str_has_suffix(item->name, ".jpeg") || g_str_has_suffix(item->name, ".png") ||
                  g_str_has_suffix(item->name, ".bmp") || g_str_has_suffix(item->name, ".gif"))) {
         icon = "image-x-generic";
      } else if ((item->kind == KIND_DB) && (item->flags & dlpDBFlagResource)) {
         icon = "application-x-executable";
      } else {
         icon = "text-x-generic";
      }

      gtk_list_store_append(ex->list_store, &iter);
      gtk_list_store_set(ex->list_store, &iter,
                         COL_ICON, icon,
                         COL_NAME, item->name,
                         COL_TYPE, (item->kind == KIND_DB) ? type : "",
                         COL_CREATOR, (item->kind == KIND_DB) ? creator : "",
                         COL_SIZE, size,
                         /* folders sort before files */
                         COL_SIZE_SORT, (gint64) ((item->kind == KIND_DIR) ? -1 : (gint64) item->size),
                         COL_RECORDS, (item->kind == KIND_DB) ? records : "",
                         COL_MODIFIED, modified,
                         COL_MODIFIED_SORT, (gint64) item->modified,
                         COL_INDEX, (gint) i,
                         -1);
      g_free(size);
   }
}

/* Folders first, then by name */
static gint sort_by_name(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
   gint64 size_a, size_b;
   gchar *name_a, *name_b;
   gint r;

   gtk_tree_model_get(model, a, COL_SIZE_SORT, &size_a, COL_NAME, &name_a, -1);
   gtk_tree_model_get(model, b, COL_SIZE_SORT, &size_b, COL_NAME, &name_b, -1);
   if ((size_a < 0) != (size_b < 0)) {
      r = (size_a < 0) ? -1 : 1;
   } else {
      r = g_utf8_collate(name_a ? name_a : "", name_b ? name_b : "");
   }
   g_free(name_a);
   g_free(name_b);
   return r;
}

/* The selected items, copied, or NULL */
static GPtrArray *selected_items(void)
{
   GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(ex->list_view));
   GtkTreeModel *model;
   GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);
   GList *row;
   GPtrArray *items = g_ptr_array_new_with_free_func(item_free);

   for (row = rows; row; row = row->next) {
      GtkTreeIter iter;
      gint index;

      if (gtk_tree_model_get_iter(model, &iter, row->data)) {
         gtk_tree_model_get(model, &iter, COL_INDEX, &index, -1);
         if (ex->items && (index >= 0) && ((guint) index < ex->items->len)) {
            g_ptr_array_add(items, item_copy(g_ptr_array_index(ex->items, index)));
         }
      }
   }
   g_list_free_full(rows, (GDestroyNotify) gtk_tree_path_free);
   if (!items->len) {
      g_ptr_array_unref(items);
      return NULL;
   }
   return items;
}

static void fill_locations(void)
{
   GtkTreeIter iter;
   unsigned int i;

   gtk_list_store_clear(ex->location_store);
   gtk_list_store_append(ex->location_store, &iter);
   gtk_list_store_set(ex->location_store, &iter,
                      LOC_ICON, "drive-harddisk",
                      LOC_LABEL, _("Internal memory"),
                      LOC_VOLREF, INTERNAL_MEMORY, -1);
   gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(ex->location_view)), &iter);

   for (i = 0; ex->volumes && (i < ex->volumes->len); i++) {
      volume_t *vol = g_ptr_array_index(ex->volumes, i);
      char *free_str = g_format_size(vol->total - vol->used);
      char *label = g_strdup_printf(_("%s (%s free)"), vol->label, free_str);

      gtk_list_store_append(ex->location_store, &iter);
      gtk_list_store_set(ex->location_store, &iter,
                         LOC_ICON, "media-flash",
                         LOC_LABEL, label,
                         LOC_VOLREF, vol->volref, -1);
      g_free(label);
      g_free(free_str);
   }
}

static gboolean cb_result(gpointer data)
{
   result_t *res = data;

   if (!ex) {
      /* The window has gone */
      result_free(res);
      return FALSE;
   }

   switch (res->type) {
    case RES_CONNECTED:
      ex->connected = TRUE;
      ex->busy = TRUE;      /* until the first listing arrives */
      ex->ram_size = res->ram_size;
      ex->ram_free = res->ram_free;
      ex->volumes = g_ptr_array_ref(res->volumes);
      gtk_spinner_stop(GTK_SPINNER(ex->spinner));
      gtk_widget_hide(ex->spinner);
      set_status(res->message);
      fill_locations();
      show_path();
      break;

    case RES_CONNECT_FAILED:
      gtk_spinner_stop(GTK_SPINNER(ex->spinner));
      gtk_widget_hide(ex->spinner);
      if (res->message) {
         set_status(res->message);
      }
      g_thread_join(ex->thread);
      ex->thread = NULL;
      if (ex->closing) {
         explorer_finish();
         result_free(res);
         return FALSE;
      }
      break;

    case RES_LISTING:
      if ((res->volref == ex->volref) &&
          (g_strcmp0(res->dir, ex->dir) == 0)) {
         fill_list(res->items);
         ex->busy = FALSE;
         /* An error reading the list, else what the operation that
          * caused this listing reported, else nothing */
         set_message(res->message ? res->message : ex->done_message);
         g_clear_pointer(&ex->done_message, g_free);
      }
      break;

    case RES_PROGRESS:
      set_message(res->message);
      break;

    case RES_DONE:
      ex->busy = FALSE;
      set_message(res->message);
      if (res->refresh) {
         g_free(ex->done_message);
         ex->done_message = g_strdup(res->message);
         request_list();
      }
      break;

    case RES_DISCONNECTED:
      ex->connected = FALSE;
      ex->busy = FALSE;
      g_thread_join(ex->thread);
      ex->thread = NULL;
      if (ex->closing) {
         explorer_finish();
         result_free(res);
         return FALSE;
      }
      set_status(res->message ? res->message : _("Disconnected."));
      gtk_list_store_clear(ex->list_store);
      break;
   }

   update_buttons();
   result_free(res);
   return FALSE;
}

/* Release the port, resume listening, and forget the explorer */
static void explorer_finish(void)
{
   explorer_t *e = ex;

   if (!e) {
      return;
   }
   if (e->lock_timer) {
      g_source_remove(e->lock_timer);
   }
   if (e->locked) {
      jp_port_unlock(e->lock_fd);
   }
   ex = NULL;
   jpilot_listen_resume();

   if (e->window) {
      gtk_widget_destroy(e->window);
   }
   g_async_queue_unref(e->requests);
   if (e->volumes) g_ptr_array_unref(e->volumes);
   if (e->items) g_ptr_array_unref(e->items);
   g_free(e->dir);
   g_free(e->done_message);
   g_free(e);
}

/* Disconnect, then close once the worker has finished */
static void explorer_close(void)
{
   request_t *req;

   if (!ex || ex->closing) {
      return;
   }
   ex->closing = TRUE;
   if (!ex->thread) {
      explorer_finish();
      return;
   }
   gtk_widget_hide(ex->window);
   g_atomic_int_set(&ex->cancel, TRUE);
   req = g_new0(request_t, 1);
   req->type = REQ_DISCONNECT;
   send_request(req);
}

static gboolean cb_delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
   explorer_close();
   return TRUE;
}

static void cb_disconnect(GtkWidget *widget, gpointer data)
{
   explorer_close();
}

static void cb_location_changed(GtkTreeSelection *selection, gpointer data)
{
   GtkTreeModel *model;
   GtkTreeIter iter;
   gint volref;

   if (!ex || !gtk_tree_selection_get_selected(selection, &model, &iter)) {
      return;
   }
   gtk_tree_model_get(model, &iter, LOC_VOLREF, &volref, -1);
   /* Already shown, or about to be: the worker lists the internal memory
    * by itself right after connecting */
   if ((volref == ex->volref) && (ex->items || ex->busy)) {
      return;
   }
   ex->volref = volref;
   g_free(ex->dir);
   ex->dir = (volref == INTERNAL_MEMORY) ? NULL : g_strdup("/");
   gtk_list_store_clear(ex->list_store);
   show_path();
   request_list();
   update_buttons();
}

static void open_folder(const char *dir)
{
   g_free(ex->dir);
   ex->dir = g_strdup(dir);
   gtk_list_store_clear(ex->list_store);
   show_path();
   request_list();
}

static void cb_row_activated(GtkTreeView *view, GtkTreePath *path,
                             GtkTreeViewColumn *column, gpointer data)
{
   GtkTreeModel *model = gtk_tree_view_get_model(view);
   GtkTreeIter iter;
   gint index;
   item_t *item;
   char *dir;

   if (!ex->connected || ex->busy || !gtk_tree_model_get_iter(model, &iter, path)) {
      return;
   }
   gtk_tree_model_get(model, &iter, COL_INDEX, &index, -1);
   item = g_ptr_array_index(ex->items, index);
   if (item->kind != KIND_DIR) {
      return;
   }
   dir = card_path(ex->dir, item->name);
   open_folder(dir);
   g_free(dir);
}

static void cb_up(GtkWidget *widget, gpointer data)
{
   char *parent;

   if (!ex->dir || !strcmp(ex->dir, "/")) {
      return;
   }
   parent = g_path_get_dirname(ex->dir);
   open_folder(parent);
   g_free(parent);
}

static void cb_refresh(GtkWidget *widget, gpointer data)
{
   request_list();
}

static void cb_selection_changed(GtkTreeSelection *selection, gpointer data)
{
   if (ex) {
      update_buttons();
   }
}

static void cb_to_computer(GtkWidget *widget, gpointer data)
{
   GtkWidget *chooser;
   GPtrArray *items = selected_items();
   request_t *req;
   char *folder = NULL;

   if (!items) {
      return;
   }
   chooser = gtk_file_chooser_dialog_new(_("Copy to This Computer"), GTK_WINDOW(ex->window),
                                         GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                         _("_Cancel"), GTK_RESPONSE_CANCEL,
                                         _("_Copy Here"), GTK_RESPONSE_ACCEPT, NULL);
   gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                       last_local_dir ? last_local_dir : g_get_home_dir());
   if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
      folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
   }
   gtk_widget_destroy(chooser);
   if (!folder || !ex) {
      g_free(folder);
      g_ptr_array_unref(items);
      return;
   }
   g_free(last_local_dir);
   last_local_dir = g_strdup(folder);

   req = g_new0(request_t, 1);
   req->type = REQ_TO_COMPUTER;
   req->volref = ex->volref;
   req->dir = g_strdup(ex->dir);
   req->items = items;
   req->target = folder;
   send_request(req);
}

/* Whether name is in the current listing */
static int listed(const char *name)
{
   unsigned int i;

   for (i = 0; ex->items && (i < ex->items->len); i++) {
      item_t *item = g_ptr_array_index(ex->items, i);

      if (!g_ascii_strcasecmp(item->name, name)) {
         return TRUE;
      }
   }
   return FALSE;
}

static int confirm(const char *primary, const char *secondary, const char *button)
{
   GtkWidget *dialog;
   int answer;

   dialog = gtk_message_dialog_new(GTK_WINDOW(ex->window), GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", primary);
   if (secondary) {
      gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", secondary);
   }
   gtk_dialog_add_buttons(GTK_DIALOG(dialog), _("_Cancel"), GTK_RESPONSE_CANCEL,
                          button, GTK_RESPONSE_ACCEPT, NULL);
   gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
   answer = gtk_dialog_run(GTK_DIALOG(dialog));
   gtk_widget_destroy(dialog);
   return answer == GTK_RESPONSE_ACCEPT;
}

static void cb_to_handheld(GtkWidget *widget, gpointer data)
{
   GtkWidget *chooser;
   GtkFileFilter *filter;
   GSList *files = NULL, *f;
   GPtrArray *paths;
   GString *replaced = g_string_new(NULL);
   int n_replaced = 0;
   request_t *req;

   chooser = gtk_file_chooser_dialog_new(_("Copy to the Handheld"), GTK_WINDOW(ex->window),
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         _("_Cancel"), GTK_RESPONSE_CANCEL,
                                         _("_Copy"), GTK_RESPONSE_ACCEPT, NULL);
   gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(chooser), TRUE);
   if (last_local_dir) {
      gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), last_local_dir);
   }
   if (ex->volref == INTERNAL_MEMORY) {
      filter = gtk_file_filter_new();
      gtk_file_filter_set_name(filter, _("Palm applications and databases"));
      gtk_file_filter_add_pattern(filter, "*.[pP][rR][cC]");
      gtk_file_filter_add_pattern(filter, "*.[pP][dD][bB]");
      gtk_file_filter_add_pattern(filter, "*.[pP][qQ][aA]");
      gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
   }
   filter = gtk_file_filter_new();
   gtk_file_filter_set_name(filter, _("All files"));
   gtk_file_filter_add_pattern(filter, "*");
   gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

   if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
      files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(chooser));
   }
   gtk_widget_destroy(chooser);
   if (!files || !ex) {
      g_slist_free_full(files, g_free);
      g_string_free(replaced, TRUE);
      return;
   }

   paths = g_ptr_array_new_with_free_func(g_free);
   for (f = files; f; f = f->next) {
      char *base = g_path_get_basename(f->data);

      if (g_file_test(f->data, G_FILE_TEST_IS_REGULAR)) {
         g_ptr_array_add(paths, g_strdup(f->data));
         if ((ex->volref != INTERNAL_MEMORY) && listed(base)) {
            n_replaced++;
            g_string_append_printf(replaced, "%s\n", base);
         }
      }
      g_free(base);
   }
   g_slist_free_full(files, g_free);
   g_free(last_local_dir);
   last_local_dir = (paths->len) ? g_path_get_dirname(g_ptr_array_index(paths, 0)) : NULL;

   if (!paths->len ||
       (n_replaced && !confirm(_("Replace the files of the same name on the card?"),
                               replaced->str, _("_Replace")))) {
      g_ptr_array_unref(paths);
      g_string_free(replaced, TRUE);
      return;
   }
   g_string_free(replaced, TRUE);

   req = g_new0(request_t, 1);
   req->type = REQ_TO_HANDHELD;
   req->volref = ex->volref;
   req->dir = g_strdup(ex->dir);
   req->paths = paths;
   send_request(req);
}

static void cb_delete(GtkWidget *widget, gpointer data)
{
   GPtrArray *items = selected_items();
   GString *names;
   char *primary;
   const char *secondary;
   unsigned int i;
   request_t *req;

   if (!items) {
      return;
   }
   names = g_string_new(NULL);
   for (i = 0; i < items->len && i < 15; i++) {
      g_string_append_printf(names, "%s\n", ((item_t *) g_ptr_array_index(items, i))->name);
   }
   if (items->len > 15) {
      g_string_append_printf(names, _("and %u more\n"), items->len - 15);
   }
   primary = g_strdup_printf(
      g_dngettext(NULL, "Delete %u item from the handheld?", "Delete %u items from the handheld?",
                  items->len), items->len);
   if (ex->volref == INTERNAL_MEMORY) {
      secondary = _("Deleting cannot be undone.  Deleting a database the handheld itself "
                    "uses (preferences, system extensions) can make it misbehave; back "
                    "the handheld up first if unsure.");
   } else {
      secondary = _("Deleting cannot be undone.  Folders are deleted with everything in them.");
   }
   g_string_prepend(names, "\n");
   g_string_prepend(names, secondary);

   if (!confirm(primary, names->str, _("_Delete"))) {
      g_ptr_array_unref(items);
   } else {
      req = g_new0(request_t, 1);
      req->type = REQ_DELETE;
      req->volref = ex->volref;
      req->dir = g_strdup(ex->dir);
      req->items = items;
      send_request(req);
   }
   g_free(primary);
   g_string_free(names, TRUE);
}

static void cb_mkdir(GtkWidget *widget, gpointer data)
{
   GtkWidget *dialog, *content, *entry;
   char *name = NULL;
   request_t *req;

   dialog = gtk_dialog_new_with_buttons(_("New Folder"), GTK_WINDOW(ex->window),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        _("_Cancel"), GTK_RESPONSE_CANCEL,
                                        _("C_reate"), GTK_RESPONSE_OK, NULL);
   gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
   content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
   gtk_container_set_border_width(GTK_CONTAINER(content), 12);
   gtk_box_pack_start(GTK_BOX(content), gtk_label_new(_("Name of the new folder:")), FALSE, FALSE, 6);
   entry = gtk_entry_new();
   gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
   gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);
   gtk_widget_show_all(dialog);
   if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
      name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
   }
   gtk_widget_destroy(dialog);

   if (!name || !name[0] || strchr(name, '/') || !ex) {
      g_free(name);
      return;
   }
   req = g_new0(request_t, 1);
   req->type = REQ_MKDIR;
   req->volref = ex->volref;
   req->dir = g_strdup(ex->dir);
   req->target = name;
   send_request(req);
}

/* Take the port lock and start waiting for the handheld.  A sync process
 * that was waiting may take a moment to let go of the lock. */
static gboolean cb_try_lock(gpointer data)
{
   if (!ex) {
      return G_SOURCE_REMOVE;
   }
   if (jp_port_trylock(&ex->lock_fd) != EXIT_SUCCESS) {
      if (++ex->lock_tries < LOCK_TRIES) {
         return G_SOURCE_CONTINUE;
      }
      ex->lock_timer = 0;
      gtk_spinner_stop(GTK_SPINNER(ex->spinner));
      gtk_widget_hide(ex->spinner);
      set_status(_("The handheld port is in use by another J-Pilot or jpilot-sync. "
                   "Close it, then open the Handheld Explorer again."));
      return G_SOURCE_REMOVE;
   }
   ex->lock_timer = 0;
   ex->locked = TRUE;
   set_status(_("Press HotSync on the handheld to connect."));
   ex->thread = g_thread_new("handheld-explorer", worker, ex);
   return G_SOURCE_REMOVE;
}

static GtkWidget *add_button(GtkWidget *box, const char *label, const char *tooltip,
                             GCallback callback)
{
   GtkWidget *button = gtk_button_new_with_mnemonic(label);

   gtk_widget_set_tooltip_text(button, tooltip);
   g_signal_connect(G_OBJECT(button), "clicked", callback, NULL);
   gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
   return button;
}

static GtkTreeViewColumn *add_text_column(GtkWidget *view, const char *title,
                                          int column, int sort_column, float xalign)
{
   GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
   GtkTreeViewColumn *col;

   g_object_set(renderer, "xalign", xalign, NULL);
   col = gtk_tree_view_column_new_with_attributes(title, renderer, "text", column, NULL);
   gtk_tree_view_column_set_resizable(col, TRUE);
   gtk_tree_view_column_set_sort_column_id(col, sort_column);
   gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
   return col;
}

static void build_window(GtkWidget *main_window)
{
   GtkWidget *vbox, *header, *paned, *scrolled, *right, *buttons, *nav;
   GtkCellRenderer *renderer;
   GtkTreeViewColumn *col;
   GtkTreeSelection *selection;
   GtkTreeSortable *sortable;
   const char *profile = profile_get_active();
   char *title;

   if (profile[0]) {
      title = g_strdup_printf(_("Handheld Explorer - %s"), profile);
   } else {
      title = g_strdup(_("Handheld Explorer"));
   }
   ex->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
   gtk_window_set_title(GTK_WINDOW(ex->window), title);
   g_free(title);
   gtk_window_set_transient_for(GTK_WINDOW(ex->window), GTK_WINDOW(main_window));
   gtk_window_set_default_size(GTK_WINDOW(ex->window), 980, 560);
   g_signal_connect(G_OBJECT(ex->window), "delete-event", G_CALLBACK(cb_delete_event), NULL);

   vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
   gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
   gtk_container_add(GTK_CONTAINER(ex->window), vbox);

   /* Connection status */
   header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
   gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);
   ex->spinner = gtk_spinner_new();
   gtk_box_pack_start(GTK_BOX(header), ex->spinner, FALSE, FALSE, 0);
   ex->status_label = gtk_label_new(_("Getting the handheld port..."));
   gtk_label_set_xalign(GTK_LABEL(ex->status_label), 0.0);
   gtk_label_set_line_wrap(GTK_LABEL(ex->status_label), TRUE);
   gtk_box_pack_start(GTK_BOX(header), ex->status_label, TRUE, TRUE, 0);
   ex->btn_disconnect = gtk_button_new_with_mnemonic(_("_Disconnect"));
   gtk_widget_set_tooltip_text(ex->btn_disconnect, _("End the HotSync session and close this window"));
   g_signal_connect(G_OBJECT(ex->btn_disconnect), "clicked", G_CALLBACK(cb_disconnect), NULL);
   gtk_box_pack_end(GTK_BOX(header), ex->btn_disconnect, FALSE, FALSE, 0);

   paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
   gtk_paned_set_position(GTK_PANED(paned), 230);
   gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

   /* Locations: internal memory and cards */
   ex->location_store = gtk_list_store_new(LOC_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
   ex->location_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ex->location_store));
   g_object_unref(ex->location_store);
   col = gtk_tree_view_column_new();
   gtk_tree_view_column_set_title(col, _("Location"));
   renderer = gtk_cell_renderer_pixbuf_new();
   gtk_tree_view_column_pack_start(col, renderer, FALSE);
   gtk_tree_view_column_add_attribute(col, renderer, "icon-name", LOC_ICON);
   renderer = gtk_cell_renderer_text_new();
   gtk_tree_view_column_pack_start(col, renderer, TRUE);
   gtk_tree_view_column_add_attribute(col, renderer, "text", LOC_LABEL);
   gtk_tree_view_append_column(GTK_TREE_VIEW(ex->location_view), col);
   selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(ex->location_view));
   g_signal_connect(G_OBJECT(selection), "changed", G_CALLBACK(cb_location_changed), NULL);
   scrolled = gtk_scrolled_window_new(NULL, NULL);
   gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled), GTK_SHADOW_IN);
   gtk_container_add(GTK_CONTAINER(scrolled), ex->location_view);
   gtk_paned_pack1(GTK_PANED(paned), scrolled, FALSE, FALSE);

   /* Contents */
   right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
   gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);

   nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_pack_start(GTK_BOX(right), nav, FALSE, FALSE, 0);
   ex->path_label = gtk_label_new("");
   gtk_label_set_xalign(GTK_LABEL(ex->path_label), 0.0);
   gtk_label_set_ellipsize(GTK_LABEL(ex->path_label), PANGO_ELLIPSIZE_MIDDLE);
   gtk_box_pack_start(GTK_BOX(nav), ex->path_label, TRUE, TRUE, 0);
   ex->btn_up = gtk_button_new_with_mnemonic(_("_Up"));
   gtk_widget_set_tooltip_text(ex->btn_up, _("Go to the folder that contains this one"));
   g_signal_connect(G_OBJECT(ex->btn_up), "clicked", G_CALLBACK(cb_up), NULL);
   gtk_box_pack_start(GTK_BOX(nav), ex->btn_up, FALSE, FALSE, 0);
   ex->btn_refresh = gtk_button_new_with_mnemonic(_("Re_fresh"));
   g_signal_connect(G_OBJECT(ex->btn_refresh), "clicked", G_CALLBACK(cb_refresh), NULL);
   gtk_box_pack_start(GTK_BOX(nav), ex->btn_refresh, FALSE, FALSE, 0);

   ex->list_store = gtk_list_store_new(NUM_COLS,
                                       G_TYPE_STRING,   /* icon */
                                       G_TYPE_STRING,   /* name */
                                       G_TYPE_STRING,   /* type */
                                       G_TYPE_STRING,   /* creator */
                                       G_TYPE_STRING,   /* size */
                                       G_TYPE_INT64,    /* size to sort by */
                                       G_TYPE_STRING,   /* records */
                                       G_TYPE_STRING,   /* modified */
                                       G_TYPE_INT64,    /* modified to sort by */
                                       G_TYPE_INT);     /* index in ex->items */
   sortable = GTK_TREE_SORTABLE(ex->list_store);
   gtk_tree_sortable_set_sort_func(sortable, COL_NAME, sort_by_name, NULL, NULL);
   gtk_tree_sortable_set_sort_column_id(sortable, COL_NAME, GTK_SORT_ASCENDING);
   ex->list_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ex->list_store));
   g_object_unref(ex->list_store);

   col = gtk_tree_view_column_new();
   gtk_tree_view_column_set_title(col, _("Name"));
   renderer = gtk_cell_renderer_pixbuf_new();
   gtk_tree_view_column_pack_start(col, renderer, FALSE);
   gtk_tree_view_column_add_attribute(col, renderer, "icon-name", COL_ICON);
   renderer = gtk_cell_renderer_text_new();
   gtk_tree_view_column_pack_start(col, renderer, TRUE);
   gtk_tree_view_column_add_attribute(col, renderer, "text", COL_NAME);
   gtk_tree_view_column_set_resizable(col, TRUE);
   gtk_tree_view_column_set_expand(col, TRUE);
   gtk_tree_view_column_set_sort_column_id(col, COL_NAME);
   gtk_tree_view_append_column(GTK_TREE_VIEW(ex->list_view), col);
   ex->col_type = add_text_column(ex->list_view, _("Type"), COL_TYPE, COL_TYPE, 0.0);
   ex->col_creator = add_text_column(ex->list_view, _("Creator"), COL_CREATOR, COL_CREATOR, 0.0);
   add_text_column(ex->list_view, _("Size"), COL_SIZE, COL_SIZE_SORT, 1.0);
   ex->col_records = add_text_column(ex->list_view, _("Records"), COL_RECORDS, COL_RECORDS, 1.0);
   add_text_column(ex->list_view, _("Modified"), COL_MODIFIED, COL_MODIFIED_SORT, 0.0);

   selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(ex->list_view));
   gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
   g_signal_connect(G_OBJECT(selection), "changed", G_CALLBACK(cb_selection_changed), NULL);
   g_signal_connect(G_OBJECT(ex->list_view), "row-activated", G_CALLBACK(cb_row_activated), NULL);
   scrolled = gtk_scrolled_window_new(NULL, NULL);
   gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled), GTK_SHADOW_IN);
   gtk_container_add(GTK_CONTAINER(scrolled), ex->list_view);
   gtk_box_pack_start(GTK_BOX(right), scrolled, TRUE, TRUE, 0);

   /* Actions */
   buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_pack_start(GTK_BOX(vbox), buttons, FALSE, FALSE, 0);
   ex->btn_to_computer = add_button(buttons, _("Copy to _Computer..."),
                                    _("Copy the selected databases, files or folders to a folder on this computer"),
                                    G_CALLBACK(cb_to_computer));
   ex->btn_to_handheld = add_button(buttons, _("Copy to _Handheld..."),
                                    _("Install applications and databases (.prc, .pdb, .pqa) in internal memory, "
                                      "or copy files into this folder of the card"),
                                    G_CALLBACK(cb_to_handheld));
   ex->btn_mkdir = add_button(buttons, _("_New Folder..."),
                              _("Make a folder in this folder of the card"),
                              G_CALLBACK(cb_mkdir));
   ex->btn_delete = add_button(buttons, _("De_lete..."),
                               _("Delete the selected items from the handheld"),
                               G_CALLBACK(cb_delete));

   ex->message_label = gtk_label_new("");
   gtk_label_set_xalign(GTK_LABEL(ex->message_label), 0.0);
   gtk_label_set_line_wrap(GTK_LABEL(ex->message_label), TRUE);
   gtk_label_set_selectable(GTK_LABEL(ex->message_label), TRUE);
   gtk_box_pack_start(GTK_BOX(vbox), ex->message_label, FALSE, FALSE, 0);

   gtk_widget_show_all(ex->window);
   gtk_spinner_start(GTK_SPINNER(ex->spinner));
}

void explorer_gui(GtkWidget *main_window)
{
   const char *port;

   if (ex) {
      if (ex->window && !ex->closing) {
         gtk_window_present(GTK_WINDOW(ex->window));
      }
      return;
   }
   if (jpilot_sync_connected()) {
      GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(main_window), GTK_DIALOG_MODAL,
                                                 GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                                                 _("A HotSync is in progress."));
      gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
         _("Open the Handheld Explorer when it has finished."));
      gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);
      return;
   }

   ex = g_new0(explorer_t, 1);
   ex->volref = INTERNAL_MEMORY;
   ex->lock_fd = -1;
   ex->requests = g_async_queue_new_full((GDestroyNotify) request_free);
   get_pref(PREF_PORT, NULL, &port);
   g_strlcpy(ex->port, (port && port[0]) ? port : "usb:", sizeof(ex->port));

   build_window(main_window);
   update_buttons();

   /* Stop a sync that is waiting for a handheld, so that the port is free */
   jpilot_listen_pause();
   if (cb_try_lock(NULL) == G_SOURCE_CONTINUE) {
      ex->lock_timer = g_timeout_add(LOCK_TRY_MS, cb_try_lock, NULL);
   }
}

void explorer_shutdown(void)
{
   request_t *req;

   if (!ex) {
      return;
   }
   if (ex->thread) {
      g_atomic_int_set(&ex->cancel, TRUE);
      req = g_new0(request_t, 1);
      req->type = REQ_DISCONNECT;
      g_async_queue_push(ex->requests, req);
      g_thread_join(ex->thread);
      ex->thread = NULL;
   }
   ex->closing = TRUE;
   explorer_finish();
}

int explorer_is_open(void)
{
   return ex != NULL;
}
