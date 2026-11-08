/*******************************************************************************
 * profile_gui.c
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

/* The Profiles window: list, create, open, rename and delete profiles */

/********************************* Includes ***********************************/
#include "config.h"
#include <string.h>
#include <gtk/gtk.h>

#include "i18n.h"
#include "utils.h"
#include "profile.h"
#include "profile_gui.h"

/********************************* Constants **********************************/
enum {
   RESPONSE_NEW = 1,
   RESPONSE_OPEN,
   RESPONSE_RENAME,
   RESPONSE_DELETE
};

enum {
   COLUMN_LABEL,
   COLUMN_HANDHELD,
   COLUMN_NAME,      /* internal name, "" for the default profile */
   COLUMN_WEIGHT,
   NUM_COLUMNS
};

/****************************** Main Code *************************************/

static void message(GtkWindow *parent, GtkMessageType type, const char *text)
{
   GtkWidget *dialog;

   dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, type,
                                   GTK_BUTTONS_CLOSE, "%s", text);
   gtk_dialog_run(GTK_DIALOG(dialog));
   gtk_widget_destroy(dialog);
}

/* Ask for a profile name.  Returns a newly allocated, valid name that is
 * not in use, or NULL if cancelled. */
static gchar *ask_name(GtkWindow *parent, const char *title,
                       const char *prompt, const char *initial)
{
   GtkWidget *dialog, *content, *label, *entry;
   gchar *name = NULL;

   dialog = gtk_dialog_new_with_buttons(title, parent,
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        _("_Cancel"), GTK_RESPONSE_CANCEL,
                                        _("_OK"), GTK_RESPONSE_OK,
                                        NULL);
   gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
   content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
   gtk_container_set_border_width(GTK_CONTAINER(content), 12);
   gtk_box_set_spacing(GTK_BOX(content), 6);

   label = gtk_label_new(prompt);
   gtk_label_set_xalign(GTK_LABEL(label), 0.0);
   gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

   entry = gtk_entry_new();
   gtk_entry_set_max_length(GTK_ENTRY(entry), PROFILE_NAME_MAX);
   gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
   if (initial) {
      gtk_entry_set_text(GTK_ENTRY(entry), initial);
   }
   gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);
   gtk_widget_show_all(dialog);

   while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
      gchar *text = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));

      if (!profile_name_is_valid(text)) {
         gchar *msg = g_strdup_printf(
            _("\"%s\" cannot be used as a profile name.\n\n"
              "Use letters, digits, spaces and the characters _-.+ "
              "(at most %d), not starting with a dot.  "
              "\"%s\" is reserved."),
            text, PROFILE_NAME_MAX, _(PROFILE_DEFAULT_LABEL));
         message(GTK_WINDOW(dialog), GTK_MESSAGE_WARNING, msg);
         g_free(msg);
         g_free(text);
      } else if (profile_exists(text)) {
         gchar *msg = g_strdup_printf(_("There is already a profile called \"%s\"."), text);
         message(GTK_WINDOW(dialog), GTK_MESSAGE_WARNING, msg);
         g_free(msg);
         g_free(text);
      } else {
         name = text;
         break;
      }
   }
   gtk_widget_destroy(dialog);
   return name;
}

static void fill_list(GtkListStore *store, GtkTreeView *view, const char *select)
{
   GList *names, *temp;
   GtkTreeIter iter;
   GtkTreeSelection *selection = gtk_tree_view_get_selection(view);

   gtk_list_store_clear(store);
   names = profile_list();
   for (temp = names; temp; temp = temp->next) {
      const char *name = temp->data;
      char user[128];
      unsigned long user_id;
      gchar *handheld;
      gchar *label;

      profile_read_identity(name, user, sizeof(user), &user_id);
      if (user_id) {
         handheld = g_strdup_printf(_("%s (user ID %lu)"),
                                    user[0] ? user : _("no user name"), user_id);
      } else {
         handheld = g_strdup(_("not synced yet"));
      }
      if (!strcmp(name, profile_get_active())) {
         label = g_strdup_printf(_("%s (open)"), profile_label(name));
      } else {
         label = g_strdup(profile_label(name));
      }

      gtk_list_store_append(store, &iter);
      gtk_list_store_set(store, &iter,
                         COLUMN_LABEL, label,
                         COLUMN_HANDHELD, handheld,
                         COLUMN_NAME, name,
                         COLUMN_WEIGHT, strcmp(name, profile_get_active()) ?
                                        PANGO_WEIGHT_NORMAL : PANGO_WEIGHT_BOLD,
                         -1);
      if (select && !strcmp(select, name)) {
         gtk_tree_selection_select_iter(selection, &iter);
      }
      g_free(handheld);
      g_free(label);
   }
   profile_list_free(names);
}

/* The internal name of the selected profile, newly allocated, or NULL */
static gchar *selected_name(GtkTreeView *view)
{
   GtkTreeModel *model;
   GtkTreeIter iter;
   gchar *name = NULL;

   if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(view),
                                       &model, &iter)) {
      gtk_tree_model_get(model, &iter, COLUMN_NAME, &name, -1);
   }
   return name;
}

static void cb_selection_changed(GtkTreeSelection *selection, gpointer data)
{
   GtkDialog *dialog = GTK_DIALOG(data);
   gchar *name = selected_name(gtk_tree_selection_get_tree_view(selection));
   int is_active = name && !strcmp(name, profile_get_active());
   int is_default = name && !name[0];

   gtk_dialog_set_response_sensitive(dialog, RESPONSE_OPEN, name && !is_active);
   gtk_dialog_set_response_sensitive(dialog, RESPONSE_RENAME,
                                     name && !is_active && !is_default);
   gtk_dialog_set_response_sensitive(dialog, RESPONSE_DELETE,
                                     name && !is_active && !is_default);
   g_free(name);
}

static void cb_row_activated(GtkTreeView *view, GtkTreePath *path,
                             GtkTreeViewColumn *column, gpointer data)
{
   gchar *name = selected_name(view);

   if (name && strcmp(name, profile_get_active())) {
      gtk_dialog_response(GTK_DIALOG(data), RESPONSE_OPEN);
   }
   g_free(name);
}

static void cb_auto_switch_toggled(GtkToggleButton *button, gpointer data)
{
   profile_set_auto_switch(gtk_toggle_button_get_active(button));
}

gchar *profiles_gui(GtkWidget *main_window)
{
   GtkWidget *dialog, *content, *label, *scrolled, *check;
   GtkListStore *store;
   GtkWidget *view;
   GtkCellRenderer *renderer;
   GtkTreeSelection *selection;
   gchar *open_name = NULL;
   int response;

   dialog = gtk_dialog_new_with_buttons(_("Profiles"), GTK_WINDOW(main_window),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        _("_New..."), RESPONSE_NEW,
                                        _("_Rename..."), RESPONSE_RENAME,
                                        _("_Delete..."), RESPONSE_DELETE,
                                        _("_Open"), RESPONSE_OPEN,
                                        _("_Close"), GTK_RESPONSE_CLOSE,
                                        NULL);
   gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 320);
   content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
   gtk_container_set_border_width(GTK_CONTAINER(content), 12);
   gtk_box_set_spacing(GTK_BOX(content), 8);

   label = gtk_label_new(_("Each profile keeps the data of one handheld.  "
                           "When a handheld syncs, its data goes into the "
                           "profile that was last synced with it, or into a "
                           "new profile if no profile knows it yet."));
   gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
   gtk_label_set_xalign(GTK_LABEL(label), 0.0);
   gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

   store = gtk_list_store_new(NUM_COLUMNS, G_TYPE_STRING, G_TYPE_STRING,
                              G_TYPE_STRING, G_TYPE_INT);
   view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
   g_object_unref(store);

   renderer = gtk_cell_renderer_text_new();
   gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(view), -1,
                                               _("Profile"), renderer,
                                               "text", COLUMN_LABEL,
                                               "weight", COLUMN_WEIGHT, NULL);
   renderer = gtk_cell_renderer_text_new();
   gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(view), -1,
                                               _("Handheld"), renderer,
                                               "text", COLUMN_HANDHELD, NULL);

   scrolled = gtk_scrolled_window_new(NULL, NULL);
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled), GTK_SHADOW_IN);
   gtk_container_add(GTK_CONTAINER(scrolled), view);
   gtk_box_pack_start(GTK_BOX(content), scrolled, TRUE, TRUE, 0);

   check = gtk_check_button_new_with_mnemonic(
      _("_Switch to the handheld's own profile when it syncs"));
   gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), profile_get_auto_switch());
   g_signal_connect(G_OBJECT(check), "toggled", G_CALLBACK(cb_auto_switch_toggled), NULL);
   gtk_box_pack_start(GTK_BOX(content), check, FALSE, FALSE, 0);

   selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
   g_signal_connect(G_OBJECT(selection), "changed", G_CALLBACK(cb_selection_changed), dialog);
   g_signal_connect(G_OBJECT(view), "row-activated", G_CALLBACK(cb_row_activated), dialog);

   fill_list(store, GTK_TREE_VIEW(view), profile_get_active());
   cb_selection_changed(selection, dialog);
   gtk_widget_show_all(dialog);

   for (;;) {
      gchar *name;

      response = gtk_dialog_run(GTK_DIALOG(dialog));
      if ((response != RESPONSE_NEW) && (response != RESPONSE_OPEN) &&
          (response != RESPONSE_RENAME) && (response != RESPONSE_DELETE)) {
         break;
      }
      name = selected_name(GTK_TREE_VIEW(view));

      if (response == RESPONSE_OPEN) {
         if (name) {
            open_name = name;
            break;
         }
      } else if (response == RESPONSE_NEW) {
         gchar *new_name = ask_name(GTK_WINDOW(dialog), _("New Profile"),
                                    _("Name of the new profile:"), NULL);
         if (new_name) {
            if (profile_create(new_name, profile_get_active())) {
               message(GTK_WINDOW(dialog), GTK_MESSAGE_ERROR,
                       _("The profile could not be created."));
            }
            fill_list(store, GTK_TREE_VIEW(view), new_name);
            g_free(new_name);
         }
      } else if ((response == RESPONSE_RENAME) && name) {
         gchar *new_name = ask_name(GTK_WINDOW(dialog), _("Rename Profile"),
                                    _("New name of the profile:"), name);
         if (new_name) {
            if (profile_rename(name, new_name)) {
               message(GTK_WINDOW(dialog), GTK_MESSAGE_ERROR,
                       _("The profile could not be renamed."));
               fill_list(store, GTK_TREE_VIEW(view), name);
            } else {
               fill_list(store, GTK_TREE_VIEW(view), new_name);
            }
            g_free(new_name);
         }
      } else if ((response == RESPONSE_DELETE) && name) {
         GtkWidget *confirm;
         int answer;

         confirm = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                          _("Move the profile \"%s\" and all its data to the Trash?"),
                                          name);
         gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(confirm),
            _("Its databases, settings and backups go with it.  "
              "The handheld itself is not changed."));
         gtk_dialog_add_buttons(GTK_DIALOG(confirm),
                                _("_Cancel"), GTK_RESPONSE_CANCEL,
                                _("_Move to Trash"), GTK_RESPONSE_ACCEPT, NULL);
         answer = gtk_dialog_run(GTK_DIALOG(confirm));
         gtk_widget_destroy(confirm);

         if (answer == GTK_RESPONSE_ACCEPT) {
            GError *error = NULL;

            if (profile_delete(name, &error)) {
               gchar *msg = g_strdup_printf(_("The profile could not be moved to the Trash: %s"),
                                            error ? error->message : "?");
               message(GTK_WINDOW(dialog), GTK_MESSAGE_ERROR, msg);
               g_free(msg);
               g_clear_error(&error);
            }
            fill_list(store, GTK_TREE_VIEW(view), profile_get_active());
         }
      }
      if (name != open_name) {
         g_free(name);
      }
   }

   gtk_widget_destroy(dialog);
   return open_name;
}
