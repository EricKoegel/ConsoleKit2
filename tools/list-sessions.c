/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006 William Jon McCann <mccann@jhu.edu>
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
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <pwd.h>
#include <string.h>
#include <libintl.h>
#include <locale.h>

#include <glib.h>
#include <glib/gi18n.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#define CK_NAME      "org.freedesktop.ConsoleKit"
#define CK_PATH      "/org/freedesktop/ConsoleKit"
#define CK_INTERFACE "org.freedesktop.ConsoleKit"

#define CK_MANAGER_PATH      "/org/freedesktop/ConsoleKit/Manager"
#define CK_MANAGER_INTERFACE "org.freedesktop.ConsoleKit.Manager"
#define CK_SEAT_INTERFACE    "org.freedesktop.ConsoleKit.Seat"
#define CK_SESSION_INTERFACE "org.freedesktop.ConsoleKit.Session"

#define IS_STR_SET(x) (x != NULL && x[0] != '\0')

typedef struct CkSessionOutput {
        char    *prop_name;
        char    *prop_value;
} CkSessionOutput;

static gboolean do_all = FALSE;
static char *do_format = NULL;

static gboolean
get_uint (DBusGProxy *proxy,
          const char *method,
          guint      *val)
{
        GError  *error;
        gboolean res;

        error = NULL;
        res = dbus_g_proxy_call (proxy,
                                 method,
                                 &error,
                                 G_TYPE_INVALID,
                                 G_TYPE_UINT, val,
                                 G_TYPE_INVALID);
        if (! res) {
                g_warning ("%s failed: %s", method, error->message);
                g_error_free (error);
        }

        return res;
}

static gboolean
get_path (DBusGProxy *proxy,
          const char *method,
          char      **str)
{
        GError  *error;
        gboolean res;

        error = NULL;
        res = dbus_g_proxy_call (proxy,
                                 method,
                                 &error,
                                 G_TYPE_INVALID,
                                 DBUS_TYPE_G_OBJECT_PATH, str,
                                 G_TYPE_INVALID);
        if (! res) {
                g_warning ("%s failed: %s", method, error->message);
                g_error_free (error);
        }

        return res;
}

static gboolean
get_string (DBusGProxy *proxy,
            const char *method,
            char      **str)
{
        GError  *error;
        gboolean res;

        error = NULL;
        res = dbus_g_proxy_call (proxy,
                                 method,
                                 &error,
                                 G_TYPE_INVALID,
                                 G_TYPE_STRING, str,
                                 G_TYPE_INVALID);
        if (! res) {
                g_warning ("%s failed: %s", method, error->message);
                g_error_free (error);
        }

        return res;
}

static gboolean
get_boolean (DBusGProxy *proxy,
             const char *method,
             gboolean   *value)
{
        GError  *error;
        gboolean res;

        error = NULL;
        res = dbus_g_proxy_call (proxy,
                                 method,
                                 &error,
                                 G_TYPE_INVALID,
                                 G_TYPE_BOOLEAN, value,
                                 G_TYPE_INVALID);
        if (! res) {
                g_warning ("%s failed: %s", method, error->message);
                g_error_free (error);
        }

        return res;
}

static char *
get_real_name (uid_t uid)
{
        struct passwd *pwent;
        char          *realname;

        realname = NULL;

        pwent = getpwuid (uid);

        if (pwent != NULL
            && pwent->pw_gecos
            && *pwent->pw_gecos != '\0') {
                char **gecos_fields;
                char **name_parts;

                /* split the gecos field and substitute '&' */
                gecos_fields = g_strsplit (pwent->pw_gecos, ",", 0);
                name_parts = g_strsplit (gecos_fields[0], "&", 0);
                pwent->pw_name[0] = g_ascii_toupper (pwent->pw_name[0]);
                realname = g_strjoinv (pwent->pw_name, name_parts);
                g_strfreev (gecos_fields);
                g_strfreev (name_parts);
        }

        return realname;
}

static void
list_session (DBusGConnection *connection,
              const char      *ssid)
{
        DBusGProxy *proxy;
        guint       uid;
        char       *realname;
        char       *sid;
        char       *lsid;
        char       *session_type;
        char       *display_type;
        char       *x11_display;
        char       *x11_display_device;
        char       *display_device;
        char       *remote_host_name;
        char       *creation_time;
        char       *idle_since_hint;
        gboolean    is_active;
        gboolean    is_local;
        gboolean    is_open;
        char       *short_sid;
        const char *short_ssid;
        char      **format_arr = NULL;
        int         i, j;

        proxy = dbus_g_proxy_new_for_name (connection,
                                           CK_NAME,
                                           ssid,
                                           CK_SESSION_INTERFACE);
        if (proxy == NULL) {
                return;
        }

        sid = NULL;
        lsid = NULL;
        session_type = NULL;
        display_type = NULL;
        x11_display = NULL;
        x11_display_device = NULL;
        display_device = NULL;
        remote_host_name = NULL;
        creation_time = NULL;
        idle_since_hint = NULL;

        get_uint (proxy, "GetUnixUser", &uid);
        get_path (proxy, "GetSeatId", &sid);
        get_string (proxy, "GetLoginSessionId", &lsid);
        get_string (proxy, "GetSessionType", &session_type);
        get_string (proxy, "GetDisplayType", &display_type);
        get_string (proxy, "GetX11Display", &x11_display);
        get_string (proxy, "GetX11DisplayDevice", &x11_display_device);
        get_string (proxy, "GetDisplayDevice", &display_device);
        get_string (proxy, "GetRemoteHostName", &remote_host_name);
        get_boolean (proxy, "IsOpen", &is_open);
        get_boolean (proxy, "IsActive", &is_active);
        get_boolean (proxy, "IsLocal", &is_local);
        get_string (proxy, "GetCreationTime", &creation_time);
        get_string (proxy, "GetIdleSinceHint", &idle_since_hint);

        if (!do_all && !is_open) {
                return;
        }

        realname = get_real_name (uid);

        short_sid = sid;
        short_ssid = ssid;

        if (g_str_has_prefix (sid, CK_PATH "/")) {
                short_sid = sid + strlen (CK_PATH) + 1;
        }
        if (g_str_has_prefix (ssid, CK_PATH "/")) {
                short_ssid = ssid + strlen (CK_PATH) + 1;
        }

        CkSessionOutput output[] = {
                {"session-id", g_strdup (short_ssid)},
                {"unix-user", g_strdup_printf ("%d", uid)},
                {"realname", g_strdup (realname)},
                {"seat", g_strdup (short_sid)},
                {"session-type", g_strdup (session_type)},
                {"display-type", g_strdup (display_type)},
                {"open", is_open ? "TRUE" : "FALSE"},
                {"active", is_active ? "TRUE" : "FALSE"},
                {"x11-display", g_strdup (x11_display)},
                {"x11-display-device", g_strdup (x11_display_device)},
                {"display-device", g_strdup (display_device)},
                {"remote-host-name", g_strdup (remote_host_name)},
                {"is-local", is_local ? "TRUE" : "FALSE"},
                {"on-since", g_strdup (creation_time)},
                {"login-session-id", g_strdup (lsid)},
                {"idle-since-hint", g_strdup (idle_since_hint)},
        };

        if (IS_STR_SET (do_format)) {
                format_arr = g_strsplit (do_format, ",", -1);

                for (i = 0; format_arr[i] != NULL; ++i) {
                        for (j = 0; j < G_N_ELEMENTS (output); j++) {
                                if (g_str_equal (format_arr[i], output[j].prop_name)) {
                                        printf ("'%s'\t", output[j].prop_value);
                                        break;
                                }
                        }
                }
                printf ("\n");
                g_strfreev (format_arr);

        } else {
                for (j = 0; j < G_N_ELEMENTS (output); j++) {
                        if (g_str_equal (output[j].prop_name, "session-id"))
                                printf ("%s:\n", output[j].prop_value);
                        else
                                printf ("\t%s = '%s'\n",
                                        output[j].prop_name,
                                        output[j].prop_value);
                }
        }

        g_free (idle_since_hint);
        g_free (creation_time);
        g_free (remote_host_name);
        g_free (realname);
        g_free (sid);
        g_free (lsid);
        g_free (session_type);
        g_free (display_type);
        g_free (x11_display);
        g_free (x11_display_device);
        g_free (display_device);

        g_object_unref (proxy);
}

static void
list_sessions (DBusGConnection *connection,
               const char      *sid)
{
        DBusGProxy *proxy;
        GError     *error;
        gboolean    res;
        GPtrArray  *sessions;
        int         i;

        proxy = dbus_g_proxy_new_for_name (connection,
                                           CK_NAME,
                                           sid,
                                           CK_SEAT_INTERFACE);
        if (proxy == NULL) {
                return;
        }

        sessions = NULL;

        error = NULL;
        res = dbus_g_proxy_call (proxy,
                                 "GetSessions",
                                 &error,
                                 G_TYPE_INVALID,
                                 dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH),
                                 &sessions,
                                 G_TYPE_INVALID);
        if (! res) {
                g_warning ("Failed to get list of sessions for %s: %s", sid, error->message);
                g_error_free (error);
                goto out;
        }

        for (i = 0; i < sessions->len; i++) {
                char *ssid;

                ssid = g_ptr_array_index (sessions, i);

                list_session (connection, ssid);

                g_free (ssid);
        }

        g_ptr_array_free (sessions, TRUE);
 out:
        g_object_unref (proxy);
}

static void
list_seats (DBusGConnection *connection)
{
        DBusGProxy *proxy;
        GError     *error;
        gboolean    res;
        GPtrArray  *seats;
        int         i;

        proxy = dbus_g_proxy_new_for_name (connection,
                                           CK_NAME,
                                           CK_MANAGER_PATH,
                                           CK_MANAGER_INTERFACE);
        if (proxy == NULL) {
                return;
        }

        seats = NULL;

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
                goto out;
        }

        for (i = 0; i < seats->len; i++) {
                char *sid;

                sid = g_ptr_array_index (seats, i);

                list_sessions (connection, sid);

                g_free (sid);
        }

        g_ptr_array_free (seats, TRUE);
 out:
        g_object_unref (proxy);
}

int
main (int    argc,
      char **argv)
{
        DBusGConnection *connection;

        GOptionContext *context;
        gboolean        retval;
        GError         *error = NULL;
        static gboolean do_version = FALSE;
        static GOptionEntry entries [] = {
                { "all", 'a', 0, G_OPTION_ARG_NONE, &do_all, N_("List all sessions. If not given, only list open sessions"), NULL },
                { "format", 'f', 0, G_OPTION_ARG_STRING, &do_format, N_("Prints information according to the given format"), NULL },
                { "version", 'V', 0, G_OPTION_ARG_NONE, &do_version, N_("Version of this application"), NULL },
                { NULL }
        };

        /* Setup for i18n */
        setlocale(LC_ALL, "");
 
#ifdef ENABLE_NLS
        bindtextdomain(PACKAGE, LOCALEDIR);
        textdomain(PACKAGE);
#endif

#if !GLIB_CHECK_VERSION(2, 36, 0)
        g_type_init ();
#endif

        context = g_option_context_new (NULL);
        g_option_context_add_main_entries (context, entries, NULL);
        retval = g_option_context_parse (context, &argc, &argv, &error);

        g_option_context_free (context);

        if (! retval) {
                g_warning ("%s", error->message);
                g_error_free (error);
                exit (1);
        }

        if (do_version) {
                g_print ("%s %s\n", argv [0], VERSION);
                exit (1);
        }

        error = NULL;
        connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
        if (connection == NULL) {
                g_message ("Failed to connect to the D-Bus daemon: %s", error->message);
                g_error_free (error);
                exit (1);
        }

        list_seats (connection);

        return 0;
}
