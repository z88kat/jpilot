/*******************************************************************************
 * profile.h
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
 * Profiles let one user keep several handhelds apart.
 *
 * Each profile is a complete J-Pilot data directory: databases, jpilot.rc,
 * backups and so on.  The default profile is ~/.jpilot itself (or
 * $JPILOT_HOME/.jpilot), so existing setups keep working unchanged.  Other
 * profiles live in ~/.jpilot/profiles/<name>/.  Settings that belong to the
 * user rather than to a handheld -- the last profile used, whether J-Pilot
 * listens for HotSync, whether a sync may switch profile -- are kept in
 * ~/.jpilot/profiles.ini.
 *
 * get_home_file_name() resolves names inside the active profile, so the rest
 * of J-Pilot, and plugins, need not know profiles exist.
 */

#ifndef __PROFILE_H__
#define __PROFILE_H__

#include <glib.h>

/* Internal name of the default profile (the ~/.jpilot directory itself) */
#define PROFILE_DEFAULT ""
/* What the default profile is called on the command line and in the GUI */
#define PROFILE_DEFAULT_LABEL "Default"

#define PROFILE_NAME_MAX 32

/* Absolute path of a file in the base directory (~/.jpilot/), whichever
 * profile is active.  For things shared by all profiles. */
int profile_base_file_name(const char *file, char *full_name, int max_size);

/* The data directory of a profile, with a trailing '/' */
int profile_dir(const char *name, char *dir, int max_size);

/* Make a profile the active one.  NULL or "" or "Default" select the
 * default profile.  Does not create it. */
int profile_set_active(const char *name);
const char *profile_get_active(void);

/* "Default" for the default profile, the name otherwise */
const char *profile_label(const char *name);

/* Map a name typed by the user ("Default" in any case) to the internal
 * name.  Returns the internal name in a static buffer. */
const char *profile_normalize(const char *name);

int profile_name_is_valid(const char *name);
int profile_exists(const char *name);

/* Create a new, empty profile.  Connection settings (port, rate, ...) and
 * display preferences are copied from the profile settings_from (may be
 * NULL), but nothing that identifies a handheld is: the new profile is
 * bound to whichever handheld syncs with it first. */
int profile_create(const char *name, const char *settings_from);

/* Move a profile's directory to the Trash.  The default profile and the
 * active profile cannot be deleted. */
int profile_delete(const char *name, GError **error);

/* Rename a profile.  The default profile and the active profile cannot be
 * renamed. */
int profile_rename(const char *old_name, const char *new_name);

/* All profiles, default first.  A list of newly allocated names; free with
 * profile_list_free(). */
GList *profile_list(void);
void profile_list_free(GList *list);

/* The handheld a profile was last synced with, from its jpilot.rc.
 * user_id is 0 if it has never been synced. */
int profile_read_identity(const char *name, char *user, int user_size,
                          unsigned long *user_id);

/* Find the profile last synced with the handheld that has this user ID.
 * Returns TRUE and the name if one is found. */
int profile_find_by_user_id(unsigned long user_id, char *name, int max_size);

/* A valid, unused profile name based on wanted (for example a handheld's
 * user name). */
void profile_make_unique_name(const char *wanted, char *name, int max_size);

/* If the profile that a handheld whose user is wanted would be named after
 * exists and has never been synced, return TRUE and its name.  So a profile
 * made in advance for a handheld is used for it. */
int profile_find_unbound_named(const char *wanted, char *name, int max_size);

/* Remove "--profile NAME" or "--profile=NAME" from argv.  Returns TRUE and
 * the name if it was there. */
int profile_take_arg(int *argc, char *argv[], char *name, int max_size);

/* Pick the profile for this run: the one named on the command line, else
 * the last one used.  Creates a profile named on the command line if it does
 * not exist.  Call before anything is logged. */
int profile_startup(const char *cmdline_name, int have_cmdline_name);

/* User settings kept in profiles.ini */
int profile_get_last(char *name, int max_size);
void profile_set_last(const char *name);
int profile_get_listen(void);
void profile_set_listen(int on);
int profile_get_auto_switch(void);
void profile_set_auto_switch(int on);

#endif
