/*******************************************************************************
 * profile.c
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

/********************************* Includes ***********************************/
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "i18n.h"
#include "libplugin.h"	/* PN, EPN */
#include "profile.h"

/********************************* Constants **********************************/
#define PROFILES_SUBDIR "profiles"
#define PROFILES_INI    "profiles.ini"
#define INI_GROUP       "profiles"

/* The default value of the "session_id" (password hash) preference */
#define DEFAULT_PASSWORD_HASH \
   "09021345070413440c08135a3215135dd217ead3b5df556322e9a14a994b0f88"

/******************************* Global vars **********************************/
static char active_profile[PROFILE_NAME_MAX + 1] = PROFILE_DEFAULT;

/****************************** Main Code *************************************/

/* The directory that contains .jpilot, as get_home_file_name() always
 * worked it out */
static const char *home_root(void)
{
   const char *home;

#ifdef ENABLE_PROMETHEON
   home = getenv("COPILOT_HOME");
#else
   home = getenv("JPILOT_HOME");
#endif
   if (!home) {
      home = getenv("HOME");
      if (!home) {
         fprintf(stderr, _("Can't get HOME environment variable\n"));
         home = ".";
      }
   }
   return home;
}

int profile_base_file_name(const char *file, char *full_name, int max_size)
{
   int len;

   len = g_snprintf(full_name, max_size, "%s/."EPN"/%s", home_root(), file);
   if (len >= max_size) {
      fprintf(stderr, _("HOME environment variable is too long to process\n"));
      g_snprintf(full_name, max_size, "./."EPN"/%s", file);
      return EXIT_FAILURE;
   }
   return EXIT_SUCCESS;
}

int profile_dir(const char *name, char *dir, int max_size)
{
   char sub[PROFILE_NAME_MAX + sizeof(PROFILES_SUBDIR) + 4];

   if ((!name) || (!name[0])) {
      return profile_base_file_name("", dir, max_size);
   }
   g_snprintf(sub, sizeof(sub), PROFILES_SUBDIR"/%s/", name);
   return profile_base_file_name(sub, dir, max_size);
}

const char *profile_normalize(const char *name)
{
   static char normalized[PROFILE_NAME_MAX + 1];

   if ((!name) || (!name[0]) ||
       (!g_ascii_strcasecmp(name, PROFILE_DEFAULT_LABEL))) {
      return PROFILE_DEFAULT;
   }
   g_strlcpy(normalized, name, sizeof(normalized));
   return normalized;
}

const char *profile_label(const char *name)
{
   if ((!name) || (!name[0])) {
      return _(PROFILE_DEFAULT_LABEL);
   }
   return name;
}

int profile_name_is_valid(const char *name)
{
   size_t len;
   const char *p;

   if ((!name) || (!name[0])) {
      return FALSE;
   }
   len = strlen(name);
   if (len > PROFILE_NAME_MAX) {
      return FALSE;
   }
   /* No hidden directories, and no names that are easy to get wrong */
   if ((name[0] == '.') || (name[0] == ' ') || (name[len - 1] == ' ')) {
      return FALSE;
   }
   if (!g_ascii_strcasecmp(name, PROFILE_DEFAULT_LABEL)) {
      return FALSE;
   }
   for (p = name; *p; p++) {
      if (!g_ascii_isalnum(*p) && !strchr(" _-.+", *p)) {
         return FALSE;
      }
   }
   return TRUE;
}

int profile_set_active(const char *name)
{
   const char *normalized = profile_normalize(name);

   if (normalized[0] && !profile_name_is_valid(normalized)) {
      return EXIT_FAILURE;
   }
   g_strlcpy(active_profile, normalized, sizeof(active_profile));
   return EXIT_SUCCESS;
}

const char *profile_get_active(void)
{
   return active_profile;
}

int profile_exists(const char *name)
{
   char dir[FILENAME_MAX];
   const char *normalized = profile_normalize(name);

   if (!normalized[0]) {
      return TRUE;
   }
   if (!profile_name_is_valid(normalized)) {
      return FALSE;
   }
   profile_dir(normalized, dir, sizeof(dir));
   return g_file_test(dir, G_FILE_TEST_IS_DIR);
}

/* Preferences that describe a particular handheld, or that should be
 * detected again for a new one.  They are not copied into a new profile. */
static int is_handheld_pref(const char *line)
{
   static const char *keys[] = {
      "user", "user_id", "pc_id", "session_id",
      "datebook_version", "address_version", "todo_version", "memo_version",
      NULL
   };
   int i;
   size_t len;

   for (i = 0; keys[i]; i++) {
      len = strlen(keys[i]);
      if (!strncmp(line, keys[i], len) &&
          ((line[len] == ' ') || (line[len] == '\n') || (line[len] == '\0'))) {
         return TRUE;
      }
   }
   return FALSE;
}

int profile_create(const char *name, const char *settings_from)
{
   char dir[FILENAME_MAX];
   char rc_file[FILENAME_MAX];
   char line[1024];
   FILE *in, *out;

   if (!profile_name_is_valid(name) || profile_exists(name)) {
      return EXIT_FAILURE;
   }
   profile_dir(name, dir, sizeof(dir));
   if (g_mkdir_with_parents(dir, 0700)) {
      fprintf(stderr, _("Can't create directory %s\n"), dir);
      return EXIT_FAILURE;
   }

   g_snprintf(rc_file, sizeof(rc_file), "%s"EPN".rc", dir);
   out = fopen(rc_file, "w");
   if (!out) {
      fprintf(stderr, _("Unable to open file: %s\n"), rc_file);
      return EXIT_FAILURE;
   }
   g_chmod(rc_file, 0600);

   if (settings_from && profile_exists(settings_from)) {
      char from_rc[FILENAME_MAX];

      profile_dir(profile_normalize(settings_from), from_rc, sizeof(from_rc));
      g_strlcat(from_rc, EPN".rc", sizeof(from_rc));
      in = fopen(from_rc, "r");
      if (in) {
         while (fgets(line, sizeof(line), in)) {
            if (!is_handheld_pref(line)) {
               fputs(line, out);
            }
         }
         fclose(in);
      }
   }

   /* Written out explicitly so that reading this file over another
    * profile's preferences resets them. */
   fprintf(out, "user \n");
   fprintf(out, "user_id 0\n");
   fprintf(out, "pc_id 0\n");
   fprintf(out, "session_id %s\n", DEFAULT_PASSWORD_HASH);
   fprintf(out, "datebook_version 0\n");
   fprintf(out, "address_version 0\n");
   fprintf(out, "todo_version 0\n");
   fprintf(out, "memo_version 0\n");
   fclose(out);

   return EXIT_SUCCESS;
}

int profile_delete(const char *name, GError **error)
{
   char dir[FILENAME_MAX];
   GFile *file;
   gboolean ok;

   if ((!name) || (!name[0]) || !profile_name_is_valid(name) ||
       !strcmp(name, active_profile) || !profile_exists(name)) {
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                  _("The profile cannot be deleted"));
      return EXIT_FAILURE;
   }
   profile_dir(name, dir, sizeof(dir));
   /* No trailing slash for GIO */
   dir[strlen(dir) - 1] = '\0';

   file = g_file_new_for_path(dir);
   ok = g_file_trash(file, NULL, error);
   g_object_unref(file);

   return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int profile_rename(const char *old_name, const char *new_name)
{
   char old_dir[FILENAME_MAX];
   char new_dir[FILENAME_MAX];
   char last[PROFILE_NAME_MAX + 1];

   if ((!old_name) || (!old_name[0]) || !profile_name_is_valid(old_name) ||
       !strcmp(old_name, active_profile) || !profile_exists(old_name) ||
       !profile_name_is_valid(new_name) || profile_exists(new_name)) {
      return EXIT_FAILURE;
   }
   profile_dir(old_name, old_dir, sizeof(old_dir));
   profile_dir(new_name, new_dir, sizeof(new_dir));
   old_dir[strlen(old_dir) - 1] = '\0';
   new_dir[strlen(new_dir) - 1] = '\0';
   if (g_rename(old_dir, new_dir)) {
      return EXIT_FAILURE;
   }

   if (profile_get_last(last, sizeof(last)) && !strcmp(last, old_name)) {
      profile_set_last(new_name);
   }
   return EXIT_SUCCESS;
}

static gint compare_names(gconstpointer a, gconstpointer b)
{
   return g_ascii_strcasecmp((const char *) a, (const char *) b);
}

GList *profile_list(void)
{
   char dir[FILENAME_MAX];
   GList *names = NULL;
   GDir *gdir;
   const char *entry;

   profile_base_file_name(PROFILES_SUBDIR, dir, sizeof(dir));
   gdir = g_dir_open(dir, 0, NULL);
   if (gdir) {
      while ((entry = g_dir_read_name(gdir))) {
         if (profile_name_is_valid(entry) && profile_exists(entry)) {
            names = g_list_insert_sorted(names, g_strdup(entry), compare_names);
         }
      }
      g_dir_close(gdir);
   }

   return g_list_prepend(names, g_strdup(PROFILE_DEFAULT));
}

void profile_list_free(GList *list)
{
   g_list_free_full(list, g_free);
}

int profile_read_identity(const char *name, char *user, int user_size,
                          unsigned long *user_id)
{
   char rc_file[FILENAME_MAX];
   char line[1024];
   FILE *in;

   if (user && (user_size > 0)) {
      user[0] = '\0';
   }
   if (user_id) {
      *user_id = 0;
   }

   profile_dir(profile_normalize(name), rc_file, sizeof(rc_file));
   g_strlcat(rc_file, EPN".rc", sizeof(rc_file));
   in = fopen(rc_file, "r");
   if (!in) {
      return EXIT_FAILURE;
   }
   while (fgets(line, sizeof(line), in)) {
      g_strchomp(line);
      if (!strncmp(line, "user_id ", 8)) {
         if (user_id) {
            *user_id = strtoul(line + 8, NULL, 10);
         }
      } else if (!strncmp(line, "user ", 5)) {
         if (user && (user_size > 0)) {
            g_strlcpy(user, line + 5, user_size);
         }
      }
   }
   fclose(in);

   return EXIT_SUCCESS;
}

int profile_find_by_user_id(unsigned long user_id, char *name, int max_size)
{
   GList *list, *temp;
   unsigned long id;
   int found = FALSE;

   if (user_id == 0) {
      return FALSE;
   }
   list = profile_list();
   for (temp = list; temp; temp = temp->next) {
      if ((profile_read_identity(temp->data, NULL, 0, &id) == EXIT_SUCCESS) &&
          (id == user_id)) {
         g_strlcpy(name, temp->data, max_size);
         found = TRUE;
         break;
      }
   }
   profile_list_free(list);

   return found;
}

void profile_make_unique_name(const char *wanted, char *name, int max_size)
{
   char base[PROFILE_NAME_MAX + 1];
   char candidate[PROFILE_NAME_MAX + 1];
   const char *p;
   int len = 0;
   int n;

   /* Keep the characters a profile name may contain, leaving room for a
    * " 99" suffix */
   for (p = wanted ? wanted : ""; *p && (len < PROFILE_NAME_MAX - 3); p++) {
      if (g_ascii_isalnum(*p) || strchr(" _-.+", *p)) {
         base[len++] = *p;
      } else {
         base[len++] = '_';
      }
   }
   base[len] = '\0';
   g_strstrip(base);
   while (base[0] == '.') {
      memmove(base, base + 1, strlen(base));
   }
   if (!profile_name_is_valid(base)) {
      g_strlcpy(base, "Handheld", sizeof(base));
   }

   g_strlcpy(candidate, base, sizeof(candidate));
   for (n = 2; profile_exists(candidate) && (n < 100); n++) {
      g_snprintf(candidate, sizeof(candidate), "%s %d", base, n);
   }
   g_strlcpy(name, candidate, max_size);
}

int profile_take_arg(int *argc, char *argv[], char *name, int max_size)
{
   int i, remove, found = FALSE;

   for (i = 1; i < *argc; i++) {
      remove = 0;
      if (!strncmp(argv[i], "--profile=", 10)) {
         g_strlcpy(name, argv[i] + 10, max_size);
         remove = 1;
      } else if (!strcmp(argv[i], "--profile") && (i + 1 < *argc)) {
         g_strlcpy(name, argv[i + 1], max_size);
         remove = 2;
      }
      if (remove) {
         memmove(&argv[i], &argv[i + remove],
                 (*argc - i - remove + 1) * sizeof(char *));
         *argc -= remove;
         found = TRUE;
         i--;
      }
   }
   return found;
}

int profile_startup(const char *cmdline_name, int have_cmdline_name)
{
   char name[PROFILE_NAME_MAX + 1];

   if (have_cmdline_name) {
      g_strlcpy(name, profile_normalize(cmdline_name), sizeof(name));
      if (name[0] && !profile_name_is_valid(name)) {
         fprintf(stderr, _("\"%s\" is not a valid profile name.\n"),
                 cmdline_name);
         fprintf(stderr, _("Use letters, digits, spaces and _-.+ "
                           "(at most %d characters).\n"), PROFILE_NAME_MAX);
         return EXIT_FAILURE;
      }
      if (!profile_exists(name)) {
         char last[PROFILE_NAME_MAX + 1];

         if (!profile_get_last(last, sizeof(last)) || !profile_exists(last)) {
            last[0] = '\0';
         }
         if (profile_create(name, last)) {
            fprintf(stderr, _("Unable to create profile \"%s\".\n"), name);
            return EXIT_FAILURE;
         }
         fprintf(stderr, _("Created profile \"%s\".\n"), name);
      }
   } else {
      if (!profile_get_last(name, sizeof(name)) || !profile_exists(name)) {
         name[0] = '\0';
      }
   }

   return profile_set_active(name);
}

/***************************** profiles.ini ***********************************/

static GKeyFile *ini_load(void)
{
   char ini_file[FILENAME_MAX];
   GKeyFile *key_file = g_key_file_new();

   profile_base_file_name(PROFILES_INI, ini_file, sizeof(ini_file));
   g_key_file_load_from_file(key_file, ini_file, G_KEY_FILE_KEEP_COMMENTS, NULL);
   return key_file;
}

static void ini_save(GKeyFile *key_file)
{
   char ini_file[FILENAME_MAX];
   char base_dir[FILENAME_MAX];
   GError *error = NULL;

   profile_base_file_name("", base_dir, sizeof(base_dir));
   g_mkdir_with_parents(base_dir, 0700);
   profile_base_file_name(PROFILES_INI, ini_file, sizeof(ini_file));
   if (!g_key_file_save_to_file(key_file, ini_file, &error)) {
      fprintf(stderr, _("Unable to write %s: %s\n"), ini_file, error->message);
      g_clear_error(&error);
   }
}

static int ini_get_boolean(const char *key, int default_value)
{
   GKeyFile *key_file = ini_load();
   GError *error = NULL;
   gboolean value;

   value = g_key_file_get_boolean(key_file, INI_GROUP, key, &error);
   if (error) {
      g_clear_error(&error);
      value = default_value;
   }
   g_key_file_free(key_file);
   return value;
}

static void ini_set_boolean(const char *key, int value)
{
   GKeyFile *key_file = ini_load();

   g_key_file_set_boolean(key_file, INI_GROUP, key, value ? TRUE : FALSE);
   ini_save(key_file);
   g_key_file_free(key_file);
}

int profile_get_last(char *name, int max_size)
{
   GKeyFile *key_file = ini_load();
   gchar *value;

   value = g_key_file_get_string(key_file, INI_GROUP, "last", NULL);
   g_key_file_free(key_file);
   if (!value) {
      return FALSE;
   }
   g_strlcpy(name, value, max_size);
   g_free(value);
   return TRUE;
}

void profile_set_last(const char *name)
{
   GKeyFile *key_file = ini_load();

   g_key_file_set_string(key_file, INI_GROUP, "last", profile_normalize(name));
   ini_save(key_file);
   g_key_file_free(key_file);
}

int profile_get_listen(void)
{
   return ini_get_boolean("listen", FALSE);
}

void profile_set_listen(int on)
{
   ini_set_boolean("listen", on);
}

int profile_get_auto_switch(void)
{
   return ini_get_boolean("auto_switch", TRUE);
}

void profile_set_auto_switch(int on)
{
   ini_set_boolean("auto_switch", on);
}
