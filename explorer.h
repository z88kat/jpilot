/*******************************************************************************
 * explorer.h
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

#ifndef __EXPLORER_H__
#define __EXPLORER_H__

#include <gtk/gtk.h>

/* File > Handheld Explorer...: open the window, or bring it to the front */
void explorer_gui(GtkWidget *main_window);

/* J-Pilot is quitting: disconnect from the handheld and close the window */
void explorer_shutdown(void);

int explorer_is_open(void);

#endif
