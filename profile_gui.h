/*******************************************************************************
 * profile_gui.h
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

#ifndef __PROFILE_GUI_H__
#define __PROFILE_GUI_H__

#include <gtk/gtk.h>

/* Show the Profiles window.  Returns the internal name of the profile the
 * user chose to open (newly allocated), or NULL. */
gchar *profiles_gui(GtkWidget *main_window);

#endif
