/*******************************************************************************
 * explorer_server.h
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

#ifndef __EXPLORER_SERVER_H__
#define __EXPLORER_SERVER_H__

/* Talk to the handheld for the Handheld Explorer window: results are
 * written to out_fd and requests read from in_fd (see explorer_proto.h).
 * Returns when the window asks to disconnect or goes away. */
int explorer_server(int out_fd, int in_fd, const char *port);

#endif
