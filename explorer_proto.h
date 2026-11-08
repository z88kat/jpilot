/*******************************************************************************
 * explorer_proto.h
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
 * What the Handheld Explorer window (explorer.c, in jpilot) and the process
 * that talks to the handheld for it (explorer_server.c, "jpilot-sync
 * --explorer") share.
 *
 * They talk over two pipes, one line per message.  A line is fields
 * separated by tabs; each field is escaped with g_strescape(), so names may
 * contain any character.
 *
 * Requests, window to server:
 *   LIST        volref dir
 *   TOCOMPUTER  volref dir local_folder  {kind name}...
 *   TOHANDHELD  volref dir  {local_path}...
 *   DELETE      volref dir  {kind name}...
 *   MKDIR       volref dir name
 *   DISCONNECT
 * Results, server to window:
 *   WAITING                          the port is ours; press HotSync
 *   VOLUME      volref label used total     (before CONNECTED)
 *   CONNECTED   message ram_size ram_free
 *   CONNECT_FAILED message
 *   ITEM        kind type creator flags size records modified name
 *   LISTING     volref dir message   the ITEMs since the last LISTING
 *   PROGRESS    message
 *   DONE        refresh message
 *   DISCONNECTED message
 * dir is empty for the internal memory.
 *
 * The server ends the HotSync session and exits after DISCONNECT, or when
 * the window's end of the request pipe closes (J-Pilot has gone), so the
 * handheld is never left waiting on a session nobody ends.
 */

#ifndef __EXPLORER_PROTO_H__
#define __EXPLORER_PROTO_H__

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <glib.h>

/* volref of the internal memory */
#define EXPLORER_INTERNAL_MEMORY (-1)

enum {
   EXPLORER_KIND_DB,
   EXPLORER_KIND_FILE,
   EXPLORER_KIND_DIR
};

typedef struct {
   char *name;
   int kind;
   unsigned long type;
   unsigned long creator;
   unsigned int flags;
   unsigned long size;          /* bytes; 0 if not known */
   unsigned long records;
   time_t modified;
} explorer_item_t;

static inline void explorer_item_free(gpointer data)
{
   explorer_item_t *item = data;

   g_free(item->name);
   g_free(item);
}

static inline explorer_item_t *explorer_item_copy(const explorer_item_t *item)
{
   explorer_item_t *copy = g_memdup2(item, sizeof(explorer_item_t));

   copy->name = g_strdup(item->name);
   return copy;
}

/* A path on a card: dir and name joined with one '/' */
static inline char *explorer_card_path(const char *dir, const char *name)
{
   if (!dir || !dir[0]) {
      return g_strconcat("/", name, NULL);
   }
   if (dir[strlen(dir) - 1] == '/') {
      return g_strconcat(dir, name, NULL);
   }
   return g_strconcat(dir, "/", name, NULL);
}

/* Append one escaped field (preceded by a tab unless it is the first) */
static inline void explorer_add_field(GString *line, const char *field)
{
   gchar *escaped = g_strescape(field ? field : "", NULL);

   if (line->len) {
      g_string_append_c(line, '\t');
   }
   g_string_append(line, escaped);
   g_free(escaped);
}

static inline void explorer_add_number(GString *line, long long n)
{
   char number[32];

   g_snprintf(number, sizeof(number), "%lld", n);
   explorer_add_field(line, number);
}

/* The fields of a line, unescaped.  Free with g_strfreev(). */
static inline gchar **explorer_split_line(const char *line)
{
   gchar **fields = g_strsplit(line, "\t", -1);
   int i;

   for (i = 0; fields[i]; i++) {
      gchar *plain = g_strcompress(fields[i]);

      g_free(fields[i]);
      fields[i] = plain;
   }
   return fields;
}

#endif
