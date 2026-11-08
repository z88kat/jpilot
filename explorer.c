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
 * The Handheld Explorer window (File > Handheld Explorer...): browse the
 * handheld's internal memory and its memory cards, copy databases and files
 * between the handheld and the computer, delete them, and make folders on
 * a card.
 *
 * The handheld is talked to by a separate process, "jpilot-sync
 * --explorer" (explorer_server.c), never by this one: pilot-link's USB code
 * leaves a thread behind in a process that has used it, which then grabs
 * every handheld that appears and stops the sync from opening it.  The
 * window sends that process requests and reads its results over two pipes
 * (explorer_proto.h).  While the window is open, listening for HotSync is
 * paused.
 */

/********************************* Includes ***********************************/
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <gtk/gtk.h>
#include <glib-unix.h>

/* For the database flag constants only */
#include <pi-dlp.h>

#include "i18n.h"
#include "utils.h"
#include "prefs.h"
#include "sync.h"
#include "explorer.h"
#include "explorer_proto.h"

/********************************* Constants **********************************/
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
   int volref;
   char *label;
   long used;
   long total;
} volume_t;

/* In the order of the words send_request() sends */
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
   int volref;          /* EXPLORER_INTERNAL_MEMORY or a card */
   char *dir;           /* the folder on the card */
   GPtrArray *items;    /* explorer_item_t: what to copy or delete */
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
   char port[128];

   /* The explorer process */
   GPid pid;
   int to_server;           /* requests */
   GIOChannel *from_server; /* results */
   guint output_watch;
   GPtrArray *pending_items;   /* ITEM lines before their LISTING */
   GPtrArray *pending_volumes; /* VOLUME lines before CONNECTED */
   int finished;            /* it said CONNECT_FAILED or DISCONNECTED */

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

   int connected;
   int busy;
   int closing;
   unsigned long ram_size, ram_free;
   GPtrArray *volumes;      /* volume_t */
   int volref;              /* what the list shows */
   char *dir;
   GPtrArray *items;        /* explorer_item_t, as listed */
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
static void request_list(void);
static void update_buttons(void);
static void explorer_finish(void);

/*************************** Small helpers ************************************/
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

/********************************* Window *************************************/

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

/* Send a request to the explorer process as one line, and free it */
static void send_request(request_t *req)
{
   static const char *words[] = {
      "LIST", "TOCOMPUTER", "TOHANDHELD", "DELETE", "MKDIR", "DISCONNECT"
   };
   GString *line = g_string_new(NULL);
   unsigned int i;

   explorer_add_field(line, words[req->type]);
   if (req->type != REQ_DISCONNECT) {
      explorer_add_number(line, req->volref);
      explorer_add_field(line, req->dir);
   }
   if (req->type == REQ_TO_COMPUTER) {
      explorer_add_field(line, req->target);
   }
   if (req->type == REQ_MKDIR) {
      explorer_add_field(line, req->target);
   }
   for (i = 0; req->items && (i < req->items->len); i++) {
      explorer_item_t *item = g_ptr_array_index(req->items, i);

      explorer_add_number(line, item->kind);
      explorer_add_field(line, item->name);
   }
   for (i = 0; req->paths && (i < req->paths->len); i++) {
      explorer_add_field(line, g_ptr_array_index(req->paths, i));
   }
   g_string_append_c(line, '\n');

   if (ex->to_server >= 0) {
      ex->busy = (req->type != REQ_DISCONNECT);
      update_buttons();
      if (write(ex->to_server, line->str, line->len) != (ssize_t) line->len) {
         /* The process has gone; its exit is handled in cb_server_exit() */
         ex->busy = FALSE;
      }
   }
   g_string_free(line, TRUE);
   request_free(req);
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
   int card = (ex->volref != EXPLORER_INTERNAL_MEMORY);
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

   if (ex->volref == EXPLORER_INTERNAL_MEMORY) {
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
      explorer_item_t *item = g_ptr_array_index(items, i);
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
      if (item->kind == EXPLORER_KIND_DIR) {
         icon = "folder";
      } else if ((item->kind == EXPLORER_KIND_DB) && (item->flags & dlpDBFlagStream) &&
                 (g_str_has_suffix(item->name, ".jpg") || g_str_has_suffix(item->name, ".JPG") ||
                  g_str_has_suffix(item->name, ".jpeg") || g_str_has_suffix(item->name, ".png") ||
                  g_str_has_suffix(item->name, ".bmp") || g_str_has_suffix(item->name, ".gif"))) {
         icon = "image-x-generic";
      } else if ((item->kind == EXPLORER_KIND_DB) && (item->flags & dlpDBFlagResource)) {
         icon = "application-x-executable";
      } else {
         icon = "text-x-generic";
      }

      gtk_list_store_append(ex->list_store, &iter);
      gtk_list_store_set(ex->list_store, &iter,
                         COL_ICON, icon,
                         COL_NAME, item->name,
                         COL_TYPE, (item->kind == EXPLORER_KIND_DB) ? type : "",
                         COL_CREATOR, (item->kind == EXPLORER_KIND_DB) ? creator : "",
                         COL_SIZE, size,
                         /* folders sort before files */
                         COL_SIZE_SORT, (gint64) ((item->kind == EXPLORER_KIND_DIR) ? -1 : (gint64) item->size),
                         COL_RECORDS, (item->kind == EXPLORER_KIND_DB) ? records : "",
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
   GPtrArray *items = g_ptr_array_new_with_free_func(explorer_item_free);

   for (row = rows; row; row = row->next) {
      GtkTreeIter iter;
      gint index;

      if (gtk_tree_model_get_iter(model, &iter, row->data)) {
         gtk_tree_model_get(model, &iter, COL_INDEX, &index, -1);
         if (ex->items && (index >= 0) && ((guint) index < ex->items->len)) {
            g_ptr_array_add(items, explorer_item_copy(g_ptr_array_index(ex->items, index)));
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
                      LOC_VOLREF, EXPLORER_INTERNAL_MEMORY, -1);
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

static void handle_result(result_t *res)
{
   if (!ex) {
      result_free(res);
      return;
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
      ex->finished = TRUE;
      if (res->message) {
         set_status(res->message);
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
      ex->finished = TRUE;
      set_status(res->message ? res->message : _("Disconnected."));
      gtk_list_store_clear(ex->list_store);
      break;
   }

   update_buttons();
   result_free(res);
}

/* A line from the explorer process: turn it into a result.  ITEM and
 * VOLUME lines are collected until the LISTING or CONNECTED line that ends
 * them. */
static void handle_line(const char *line)
{
   gchar **f = explorer_split_line(line);
   int n = g_strv_length(f);
   result_t *res = NULL;

#define FIELD(i) (((i) < n) ? f[i] : "")
#define NONEMPTY(s) ((s)[0] ? g_strdup(s) : NULL)
   if (!n) {
      g_strfreev(f);
      return;
   }
   if (!strcmp(f[0], "WAITING")) {
      set_status(_("Press HotSync on the handheld to connect."));
   } else if (!strcmp(f[0], "VOLUME")) {
      volume_t *vol = g_new0(volume_t, 1);

      vol->volref = atoi(FIELD(1));
      vol->label = g_strdup(FIELD(2));
      vol->used = atol(FIELD(3));
      vol->total = atol(FIELD(4));
      g_ptr_array_add(ex->pending_volumes, vol);
   } else if (!strcmp(f[0], "CONNECTED")) {
      res = g_new0(result_t, 1);
      res->type = RES_CONNECTED;
      res->message = g_strdup(FIELD(1));
      res->ram_size = strtoul(FIELD(2), NULL, 10);
      res->ram_free = strtoul(FIELD(3), NULL, 10);
      res->volumes = ex->pending_volumes;
      ex->pending_volumes = g_ptr_array_new_with_free_func(volume_free);
   } else if (!strcmp(f[0], "CONNECT_FAILED")) {
      res = g_new0(result_t, 1);
      res->type = RES_CONNECT_FAILED;
      res->message = NONEMPTY(FIELD(1));
   } else if (!strcmp(f[0], "ITEM")) {
      explorer_item_t *item = g_new0(explorer_item_t, 1);

      item->kind = atoi(FIELD(1));
      item->type = strtoul(FIELD(2), NULL, 10);
      item->creator = strtoul(FIELD(3), NULL, 10);
      item->flags = strtoul(FIELD(4), NULL, 10);
      item->size = strtoul(FIELD(5), NULL, 10);
      item->records = strtoul(FIELD(6), NULL, 10);
      item->modified = (time_t) strtoll(FIELD(7), NULL, 10);
      item->name = g_strdup(FIELD(8));
      g_ptr_array_add(ex->pending_items, item);
   } else if (!strcmp(f[0], "LISTING")) {
      res = g_new0(result_t, 1);
      res->type = RES_LISTING;
      res->volref = atoi(FIELD(1));
      res->dir = NONEMPTY(FIELD(2));
      res->message = NONEMPTY(FIELD(3));
      res->items = ex->pending_items;
      ex->pending_items = g_ptr_array_new_with_free_func(explorer_item_free);
   } else if (!strcmp(f[0], "PROGRESS")) {
      res = g_new0(result_t, 1);
      res->type = RES_PROGRESS;
      res->message = g_strdup(FIELD(1));
   } else if (!strcmp(f[0], "DONE")) {
      res = g_new0(result_t, 1);
      res->type = RES_DONE;
      res->refresh = atoi(FIELD(1));
      res->message = NONEMPTY(FIELD(2));
   } else if (!strcmp(f[0], "DISCONNECTED")) {
      res = g_new0(result_t, 1);
      res->type = RES_DISCONNECTED;
      res->message = NONEMPTY(FIELD(1));
   }
#undef FIELD
#undef NONEMPTY
   g_strfreev(f);
   if (res) {
      handle_result(res);
   }
}

static gboolean cb_server_output(GIOChannel *channel, GIOCondition condition, gpointer data)
{
   gchar *line;
   gsize terminator;
   GIOStatus status;

   for (;;) {
      line = NULL;
      status = g_io_channel_read_line(channel, &line, NULL, &terminator, NULL);
      if (status != G_IO_STATUS_NORMAL) {
         break;
      }
      line[terminator] = '\0';
      if (ex) {
         handle_line(line);
      }
      g_free(line);
      if (!ex) {
         return FALSE;
      }
   }
   if (status == G_IO_STATUS_AGAIN) {
      /* No whole line yet */
      return TRUE;
   }
   /* End of the pipe, or an error: nothing more will come, and
    * cb_server_exit() finishes up */
   if (ex) {
      ex->output_watch = 0;
   }
   return FALSE;
}

/* The explorer process has exited */
static void cb_server_exit(GPid pid, gint status, gpointer data)
{
   g_spawn_close_pid(pid);
   if (!ex || (ex->pid != pid)) {
      return;
   }
   ex->pid = 0;
   if (ex->closing) {
      explorer_finish();
      return;
   }
   if (!ex->finished) {
      /* It stopped without saying so */
      result_t *res = g_new0(result_t, 1);

      res->type = ex->connected ? RES_DISCONNECTED : RES_CONNECT_FAILED;
      res->message = g_strdup(_("The connection to the handheld was lost."));
      handle_result(res);
   }
   gtk_spinner_stop(GTK_SPINNER(ex->spinner));
   gtk_widget_hide(ex->spinner);
}

/* Release the port, resume listening, and forget the explorer */
static void explorer_finish(void)
{
   explorer_t *e = ex;

   if (!e) {
      return;
   }
   if (e->output_watch) {
      g_source_remove(e->output_watch);
   }
   if (e->from_server) {
      g_io_channel_shutdown(e->from_server, FALSE, NULL);
      g_io_channel_unref(e->from_server);
   }
   if (e->to_server >= 0) {
      close(e->to_server);
   }
   ex = NULL;
   jpilot_listen_resume();

   if (e->window) {
      gtk_widget_destroy(e->window);
   }
   g_ptr_array_unref(e->pending_items);
   g_ptr_array_unref(e->pending_volumes);
   if (e->volumes) g_ptr_array_unref(e->volumes);
   if (e->items) g_ptr_array_unref(e->items);
   g_free(e->dir);
   g_free(e->done_message);
   g_free(e);
}

/* Disconnect, then close once the explorer process has exited */
static void explorer_close(void)
{
   request_t *req;

   if (!ex || ex->closing) {
      return;
   }
   ex->closing = TRUE;
   if (!ex->pid) {
      explorer_finish();
      return;
   }
   gtk_widget_hide(ex->window);
   /* Ends the session if connected, or stops waiting for HotSync */
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
   /* Already shown, or about to be: the explorer process lists the internal memory
    * by itself right after connecting */
   if ((volref == ex->volref) && (ex->items || ex->busy)) {
      return;
   }
   ex->volref = volref;
   g_free(ex->dir);
   ex->dir = (volref == EXPLORER_INTERNAL_MEMORY) ? NULL : g_strdup("/");
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
   explorer_item_t *item;
   char *dir;

   if (!ex->connected || ex->busy || !gtk_tree_model_get_iter(model, &iter, path)) {
      return;
   }
   gtk_tree_model_get(model, &iter, COL_INDEX, &index, -1);
   item = g_ptr_array_index(ex->items, index);
   if (item->kind != EXPLORER_KIND_DIR) {
      return;
   }
   dir = explorer_card_path(ex->dir, item->name);
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
      explorer_item_t *item = g_ptr_array_index(ex->items, i);

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
   if (ex->volref == EXPLORER_INTERNAL_MEMORY) {
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
         if ((ex->volref != EXPLORER_INTERNAL_MEMORY) && listed(base)) {
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
      g_string_append_printf(names, "%s\n", ((explorer_item_t *) g_ptr_array_index(items, i))->name);
   }
   if (items->len > 15) {
      g_string_append_printf(names, _("and %u more\n"), items->len - 15);
   }
   primary = g_strdup_printf(
      g_dngettext(NULL, "Delete %u item from the handheld?", "Delete %u items from the handheld?",
                  items->len), items->len);
   if (ex->volref == EXPLORER_INTERNAL_MEMORY) {
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

/* Start "jpilot-sync --explorer", which talks to the handheld for us.
 * It takes the port lock itself, and says WAITING when it has it. */
static int start_server(void)
{
   gchar *helper = jp_find_sync_helper();
   gchar *argv[12];
   gchar out_fd[16], in_fd[16];
   int results[2], requests[2];
   GError *error = NULL;
   GPid pid;
   int argc = 0;

   if (!helper) {
      set_status(_("Cannot find jpilot-sync, which the Handheld Explorer needs."));
      return EXIT_FAILURE;
   }
   if (!g_unix_open_pipe(results, FD_CLOEXEC, NULL) ||
       !g_unix_open_pipe(requests, FD_CLOEXEC, NULL)) {
      g_free(helper);
      set_status(_("Unable to open a pipe."));
      return EXIT_FAILURE;
   }
   /* Only the explorer process's ends stay open in it.  It must not get
    * our end of the request pipe, or it would never see the pipe close
    * when J-Pilot goes away. */
   fcntl(results[1], F_SETFD, 0);
   fcntl(requests[0], F_SETFD, 0);

   g_snprintf(out_fd, sizeof(out_fd), "%d", results[1]);
   g_snprintf(in_fd, sizeof(in_fd), "%d", requests[0]);
   argv[argc++] = helper;
   argv[argc++] = "--explorer";
   argv[argc++] = out_fd;
   argv[argc++] = in_fd;
   argv[argc++] = "--port";
   argv[argc++] = ex->port;
   argv[argc++] = "--profile";
   argv[argc++] = profile_get_active()[0] ? (gchar *) profile_get_active() : PROFILE_DEFAULT_LABEL;
   argv[argc] = NULL;

   if (!g_spawn_async(NULL, argv, NULL,
                      G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                      NULL, NULL, &pid, &error)) {
      char *text = g_strdup_printf(_("Unable to start %s: %s"), helper, error->message);

      set_status(text);
      g_free(text);
      g_clear_error(&error);
      g_free(helper);
      close(results[0]);
      close(results[1]);
      close(requests[0]);
      close(requests[1]);
      return EXIT_FAILURE;
   }
   g_free(helper);
   close(results[1]);
   close(requests[0]);

   ex->pid = pid;
   ex->to_server = requests[1];
   ex->from_server = g_io_channel_unix_new(results[0]);
   g_io_channel_set_close_on_unref(ex->from_server, TRUE);
   g_io_channel_set_encoding(ex->from_server, NULL, NULL);
   /* Read what is there without waiting for more: the GUI must not block */
   g_io_channel_set_flags(ex->from_server, G_IO_FLAG_NONBLOCK, NULL);
   ex->output_watch = g_io_add_watch(ex->from_server, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                     cb_server_output, NULL);
   g_child_watch_add(pid, cb_server_exit, NULL);
   return EXIT_SUCCESS;
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
   ex->volref = EXPLORER_INTERNAL_MEMORY;
   ex->to_server = -1;
   ex->pending_items = g_ptr_array_new_with_free_func(explorer_item_free);
   ex->pending_volumes = g_ptr_array_new_with_free_func(volume_free);
   get_pref(PREF_PORT, NULL, &port);
   g_strlcpy(ex->port, (port && port[0]) ? port : "usb:", sizeof(ex->port));

   build_window(main_window);
   update_buttons();

   /* Stop a sync that is waiting for a handheld, so that the port is free */
   jpilot_listen_pause();
   if (start_server() != EXIT_SUCCESS) {
      gtk_spinner_stop(GTK_SPINNER(ex->spinner));
      gtk_widget_hide(ex->spinner);
   }
}

void explorer_shutdown(void)
{
   int i;

   if (!ex) {
      return;
   }
   if (ex->pid) {
      GPid pid = ex->pid;
      request_t *req = g_new0(request_t, 1);

      /* Ask it to end the session, and give it time to: a handheld whose
       * session is cut off can hang until it is reset */
      req->type = REQ_DISCONNECT;
      send_request(req);
      for (i = 0; i < 100; i++) {
         int r = waitpid(pid, NULL, WNOHANG);

         if ((r == pid) || ((r < 0) && (errno == ECHILD))) {
            break;
         }
         g_usleep(100000);
      }
      if (i == 100) {
         kill(pid, SIGTERM);
      }
      ex->pid = 0;
   }
   ex->closing = TRUE;
   explorer_finish();
}

int explorer_is_open(void)
{
   return ex != NULL;
}
