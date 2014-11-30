/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009 Sun Microsystems, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Authors: Halton Huo <halton.huo@sun.com>
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <strings.h>
#include <glib/gi18n.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#define CK_NAME              "org.freedesktop.ConsoleKit"
#define CK_PATH              "/org/freedesktop/ConsoleKit"
#define CK_INTERFACE         "org.freedesktop.ConsoleKit"
#define CK_MANAGER_PATH      "/org/freedesktop/ConsoleKit/Manager"
#define CK_MANAGER_INTERFACE "org.freedesktop.ConsoleKit.Manager"
#define CK_SEAT_INTERFACE    "org.freedesktop.ConsoleKit.Seat"
#define CK_SESSION_INTERFACE "org.freedesktop.ConsoleKit.Session"

#define CK_DBUS_TYPE_G_STRING_STRING_HASHTABLE (dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_STRING))

#define IS_STR_SET(x) (x != NULL && x[0] != '\0')
#define CK_PATH_PREFIX       "/org/freedesktop/ConsoleKit/"

static gboolean  add = FALSE;
static gboolean  delete = FALSE;
static gboolean  show_version = FALSE;
static char     *session_type = NULL;
static char     *display_type = NULL;
static char     *seat_id = NULL;
static char     *session_id = NULL;
static gchar   **remaining_args = NULL;

static const GOptionEntry options [] = {
        { "add", 'a', 0, G_OPTION_ARG_NONE, &add, N_("Add a new session"), NULL},
        { "session-type", '\0', 0, G_OPTION_ARG_STRING, &session_type, N_("Specify session type when adding a session. Default is LoginWindow."), NULL},
        { "display-type", '\0', 0, G_OPTION_ARG_STRING, &display_type, N_("Specify display type under <etc>/ConsoleKit/displays.d/ when adding a session."), NULL},
        { "seat-id", '\0', 0, G_OPTION_ARG_STRING, &seat_id, N_("Specify seat id when adding a session. If not given, create a new seat."), NULL},
        { G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_STRING_ARRAY, &remaining_args, N_("Specify values of variables in display type. For example display=:10"), NULL },
        { "delete", 'd', 0, G_OPTION_ARG_NONE, &delete, N_("Delete a session"), NULL},
        { "session-id", '\0', 0, G_OPTION_ARG_STRING, &session_id, N_("Specify session id when deleting a session"), NULL},
        { "version", 'V', 0, G_OPTION_ARG_NONE, &show_version, N_("Version of this application"), NULL },
        { NULL }
};

static void
add_session (DBusGConnection  *connection)
{
        DBusGProxy *mgr_proxy = NULL;
        DBusGProxy *seat_proxy = NULL;
        GError     *error = NULL;
        gboolean    res;
        char       *sid = NULL;
        GPtrArray  *seats;
        char       *ssid = NULL;
        int         i;
        gboolean    found;
        char       *sstype = NULL;
        GHashTable *variables = NULL;

        if (! IS_STR_SET (session_type)) {
                sstype = g_strdup ("LoginWindow");
        } else {
                sstype = g_strdup (session_type);
        }

        mgr_proxy = dbus_g_proxy_new_for_name (connection,
                                               CK_NAME,
                                               CK_MANAGER_PATH,
                                               CK_MANAGER_INTERFACE);
        if (mgr_proxy == NULL) {
                return;
        }

        if (! IS_STR_SET(seat_id)) {

                /* If seat id is not given, create a new seat */
                error = NULL;
                res = dbus_g_proxy_call (mgr_proxy,
                                         "AddSeat",
                                         &error,
                                         G_TYPE_STRING, "Default",
                                         G_TYPE_INVALID,
                                         DBUS_TYPE_G_OBJECT_PATH, &sid,
                                         G_TYPE_INVALID);
                if (!res) {
                        g_warning ("Unable to add seat: %s", error->message);
                        g_error_free (error);
                        g_object_unref (mgr_proxy);
                        return;
                }

        } else {
                if (!g_str_has_prefix (seat_id, CK_PATH_PREFIX)) {
                        sid = g_strdup_printf ("%s%s", CK_PATH_PREFIX, seat_id);
                } else {
                        sid = g_strdup (seat_id);
                }
                /* Check whether seat is existing, if not, try to create it. */

                error = NULL;
                res = dbus_g_proxy_call (mgr_proxy,
                                         "GetSeats",
                                         &error,
                                         G_TYPE_INVALID,
                                         dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH),
                                         &seats,
                                         G_TYPE_INVALID);
                if (!res) {
                        g_warning ("Unable to get seat list: %s", error->message);
                        g_error_free (error);
                        g_object_unref (mgr_proxy);
                        return;
                }

                found = FALSE;
                for (i = 0; i < seats->len; i++) {
                        char *tmp_sid;

                        tmp_sid = g_ptr_array_index (seats, i);
                        if (g_str_equal (sid, tmp_sid)) {
                                found = TRUE;
                                g_free (tmp_sid);
                                break;
                        }

                        g_free (tmp_sid);
                }

                if (! found) {
                        error = NULL;
                        res = dbus_g_proxy_call (mgr_proxy,
                                                 "AddSeatById",
                                                 &error,
                                                 G_TYPE_STRING, "Default",
                                                 DBUS_TYPE_G_OBJECT_PATH, sid,
                                                 G_TYPE_INVALID,
                                                 G_TYPE_INVALID);
                        if (!res) {
                                g_warning ("Unable to add seat: %s", error->message);
                                g_error_free (error);
                                g_object_unref (mgr_proxy);
                                return;
                        }
                }
        }

        seat_proxy = dbus_g_proxy_new_for_name (connection,
                                                CK_NAME,
                                                sid,
                                                CK_SEAT_INTERFACE);

        if (seat_proxy == NULL) {
                g_warning ("Failed to talk to seat '%s'", sid);
                g_object_unref (mgr_proxy);
                return;
        }

        variables = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           (GDestroyNotify) g_free,
                                           (GDestroyNotify) g_free);

        if (remaining_args) {
                for (i = 0; i < G_N_ELEMENTS (remaining_args); i++) {
                        char **arr;

                        /* split var=value */
                        arr = g_strsplit (remaining_args [i], "=", 2);
                        if (arr[0] && arr[1]) {
                                g_hash_table_insert (variables,
                                                     g_strdup(arr[0]),
                                                     g_strdup (arr[1]));
                        }
                        g_strfreev (arr);
                }
        }

        error = NULL;
        res = dbus_g_proxy_call (mgr_proxy,
                                 "AddSession",
                                 &error,
                                 DBUS_TYPE_G_OBJECT_PATH, sid,
                                 G_TYPE_STRING, sstype,
                                 G_TYPE_STRING, display_type,
                                 CK_DBUS_TYPE_G_STRING_STRING_HASHTABLE, variables,
                                 G_TYPE_INVALID,
                                 DBUS_TYPE_G_OBJECT_PATH, &ssid,
                                 G_TYPE_INVALID);

        if (!res) {
                g_warning ("Unable to add dynamic session: %s", error->message);
                g_error_free (error);
        } else {
                dbus_g_proxy_call_no_reply (seat_proxy,
                                            "Manage",
                                            G_TYPE_INVALID,
                                            G_TYPE_INVALID);
                g_print ("Seat %s with session %s has been added\n", sid, ssid);
        }

        g_object_unref (seat_proxy);
        g_object_unref (mgr_proxy);
}

static gboolean
is_session_on_seat (DBusGConnection *connection,
                    const char      *sid,
                    const char      *ssid,
                    gboolean        *is_last_session)
{

        DBusGProxy *seat_proxy = NULL;
        GPtrArray  *sessions = NULL;
        char       *ssid_tmp = NULL;
        gboolean    res;
        gboolean    retval = FALSE;
        int         i;
        GError     *error = NULL;

        seat_proxy = dbus_g_proxy_new_for_name (connection,
                                                CK_NAME,
                                                sid,
                                                CK_SEAT_INTERFACE);

        if (seat_proxy == NULL) {
                g_warning ("Failed to talk to seat '%s'", sid);
                return FALSE;
        }

        error = NULL;
        res = dbus_g_proxy_call (seat_proxy,
                                 "GetSessions",
                                 &error,
                                 G_TYPE_INVALID,
                                 dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH),
                                 &sessions,
                                 G_TYPE_INVALID);
        if (! res) {
                g_warning ("Failed to get list of sessions for %s: %s", sid, error->message);
                g_error_free (error);
                g_object_unref (seat_proxy);
                return FALSE;
        }

        for (i = 0; i < sessions->len; i++) {

                ssid_tmp = g_ptr_array_index (sessions, i);

                if (g_str_equal (ssid, ssid_tmp)) {
                        retval = TRUE;
                        break;
                }

                g_free (ssid_tmp);
                ssid_tmp = NULL;
        }

        if (is_last_session != NULL) {
                *is_last_session = sessions->len == 1;
        }

        g_ptr_array_free (sessions, TRUE);
        g_object_unref (seat_proxy);

        return retval;
}

static char *
find_seat_id_from_session_id (DBusGConnection *connection,
                              DBusGProxy      *proxy,
                              const char      *ssid,
                              gboolean        *is_last_session)
{
        GError     *error;
        GPtrArray  *seats;
        int         i;
        char       *sid;
        gboolean    res;

        error = NULL;
        res = dbus_g_proxy_call (proxy,
                                 "GetSeats",
                                 &error,
                                 G_TYPE_INVALID,
                                 dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH),
                                 &seats,
                                 G_TYPE_INVALID);

        if (! res) {
                g_warning ("Failed to get list of seats: %s", error->message);
                g_error_free (error);
                return NULL;
        }

        for (i = 0; i < seats->len; i++) {

                sid = g_ptr_array_index (seats, i);
                if (is_session_on_seat (connection, sid, ssid, is_last_session)) {
                        break;
                }

                g_free (sid);
        }
        g_ptr_array_free (seats, TRUE);

        return sid;
}

static void
delete_session (DBusGConnection *connection)
{
        DBusGProxy *proxy;
        char       *ssid;
        char       *sid;
        gboolean    is_last_session;

        if (!g_str_has_prefix (session_id, CK_PATH_PREFIX)) {
                ssid = g_strdup_printf ("%s%s", CK_PATH_PREFIX, session_id);
        } else {
                ssid = g_strdup (session_id);
        }

        proxy = dbus_g_proxy_new_for_name (connection,
                                           CK_NAME,
                                           CK_MANAGER_PATH,
                                           CK_MANAGER_INTERFACE);
        if (proxy == NULL) {
                return;
        }

        sid = find_seat_id_from_session_id (connection, proxy, ssid, &is_last_session);

        dbus_g_proxy_call_no_reply (proxy,
                                   "WillNotRespawn",
                                   DBUS_TYPE_G_OBJECT_PATH, ssid,
                                   G_TYPE_INVALID,
                                   G_TYPE_INVALID);

        dbus_g_proxy_call_no_reply (proxy,
                                   "RemoveSession",
                                   DBUS_TYPE_G_OBJECT_PATH, ssid,
                                   G_TYPE_INVALID,
                                   G_TYPE_INVALID);

        if (is_last_session) {
                dbus_g_proxy_call_no_reply (proxy,
                                           "RemoveSeat",
                                           DBUS_TYPE_G_OBJECT_PATH, sid,
                                           G_TYPE_INVALID,
                                           G_TYPE_INVALID);
        }

        g_object_unref (proxy);
}

int
main (int argc, char *argv[])
{
        DBusGConnection *connection;
        GOptionContext  *ctx;
        GError          *error = NULL;
        gboolean         res;

        g_type_init ();

        /* Option parsing */
        ctx = g_option_context_new (_("- Manage dynamic sessions"));
        g_option_context_add_main_entries (ctx, options, GETTEXT_PACKAGE);
        res = g_option_context_parse (ctx, &argc, &argv, &error);

        if (!res) {
                if (error) {
                        g_warning ("%s", error->message);
                        g_error_free (error);
                }
                exit (1);
        }

        g_option_context_free (ctx);

        if (show_version) {
                g_print ("%s %s\n", argv[0], VERSION);
                exit (0);
        }

        if (add && delete) {
                g_warning ("Can not specify -a and -d at the same time!");
                exit (1);
        }

        if (!add && !delete) {
                g_warning ("Must specify -a, -d!");
                exit (1);
        }

        if (delete && (! IS_STR_SET (session_id))) {
                g_warning ("You must specify session id for deleting a session. You can get all sessions by ck-list-sessions");
                exit (1);
        }

        if (add && (! IS_STR_SET (display_type)) ) {
                g_warning ("You must specify display type for adding a session. You can get all display types under <etc>/ConsoleKit/displays.d/");
                g_warning ("Invalid display type!");
                exit (1);
        }

        error = NULL;
        connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
        if (connection == NULL) {
                g_message ("Failed to connect to the D-Bus daemon: %s", error->message);
                g_error_free (error);
                exit (1);
        }


        if (add) {
                add_session (connection);
        } else if (delete) {
                delete_session (connection);
        } else {
                g_warning ("Invaild parameters!");
                exit (1);
        }

        return 0;
}
