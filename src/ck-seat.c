/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006 William Jon McCann <mccann@jhu.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib-object.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "ck-sysdeps.h"

#include "ck-seat.h"
#include "ck-seat-glue.h"
#include "ck-marshal.h"

#include "ck-display-template.h"
#include "ck-session.h"
#include "ck-vt-monitor.h"
#include "ck-run-programs.h"

#define CK_SEAT_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CK_TYPE_SEAT, CkSeatPrivate))

#define CK_SESSION_DIR SYSCONFDIR "/ConsoleKit/sessions.d"

#define CK_DBUS_PATH "/org/freedesktop/ConsoleKit"
#define CK_DBUS_NAME "org.freedesktop.ConsoleKit"

#define NONULL_STRING(x) ((x) != NULL ? (x) : "")
#define N_ELEMENTS(arr)  (sizeof (arr) / sizeof ((arr)[0]))

#define IS_STR_SET(x) (x != NULL && x[0] != '\0')

#define CK_DBUS_TYPE_G_STRING_STRING_HASHTABLE (dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_STRING))

struct CkSeatPrivate
{
        char            *id;
        CkSeatKind       kind;
        char            *type;
        GHashTable      *sessions;
        GPtrArray       *devices;

        CkSession       *active_session;

        CkVtMonitor     *vt_monitor;

        DBusGConnection *connection;

        DBusGProxy      *manager_proxy;
};

enum {
        ACTIVE_SESSION_CHANGED,
        ACTIVE_SESSION_CHANGED_FULL,
        SESSION_ADDED, /* Carries the session as path for D-Bus */
        SESSION_ADDED_FULL, /* Carries the session as CkSession for other uses */
        SESSION_REMOVED,
        SESSION_REMOVED_FULL,
        DEVICE_ADDED,
        DEVICE_REMOVED,
        REMOVE_REQUEST,
        OPEN_SESSION_REQUEST,
        CLOSE_SESSION_REQUEST,
        NO_RESPAWN,
        LAST_SIGNAL
};

enum {
        PROP_0,
        PROP_ID,
        PROP_KIND,
        PROP_TYPE
};

static guint signals [LAST_SIGNAL] = { 0, };

static void     ck_seat_class_init  (CkSeatClass *klass);
static void     ck_seat_init        (CkSeat      *seat);
static void     ck_seat_finalize    (GObject     *object);

G_DEFINE_TYPE (CkSeat, ck_seat, G_TYPE_OBJECT)

GQuark
ck_seat_error_quark (void)
{
        static GQuark ret = 0;
        if (ret == 0) {
                ret = g_quark_from_static_string ("ck_seat_error");
        }

        return ret;
}

#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }

GType
ck_seat_kind_get_type (void)
{
        static GType etype = 0;

        if (etype == 0) {
                static const GEnumValue values[] = {
                        ENUM_ENTRY (CK_SEAT_KIND_STATIC, "Fixed single instance local seat"),
                        ENUM_ENTRY (CK_SEAT_KIND_DYNAMIC, "Transient seat"),
                        { 0, 0, 0 }
                };

                etype = g_enum_register_static ("CkSeatKindType", values);
        }

        return etype;
}

gboolean
ck_seat_get_active_session (CkSeat         *seat,
                            char          **ssid,
                            GError        **error)
{
        gboolean ret;
        char    *session_id;

        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        g_debug ("CkSeat: get active session");
        session_id = NULL;
        ret = FALSE;
        if (seat->priv->active_session != NULL) {
                gboolean res;
                res = ck_session_get_id (seat->priv->active_session, &session_id, NULL);
                if (res) {
                        ret = TRUE;
                }
        } else {
                g_debug ("CkSeat: seat has no active session");
        }

        if (! ret) {
                g_set_error (error,
                             CK_SEAT_ERROR,
                             CK_SEAT_ERROR_GENERAL,
                             "%s", "Seat has no active session");
        } else {
                if (ssid != NULL) {
                        *ssid = g_strdup (session_id);
                }
        }

        g_free (session_id);
        return ret;
}

typedef struct
{
        gulong                 handler_id;
        CkSeat                *seat;
        guint                  num;
        DBusGMethodInvocation *context;
} ActivateData;

static void
activated_cb (CkVtMonitor    *vt_monitor,
              guint           num,
              ActivateData   *adata)
{
        if (adata->num == num) {
                dbus_g_method_return (adata->context, TRUE);
        } else {
                GError *error;

                error = g_error_new (CK_SEAT_ERROR,
                                     CK_SEAT_ERROR_GENERAL,
                                     _("Another session was activated while waiting"));
                dbus_g_method_return_error (adata->context, error);
                g_error_free (error);
        }

        g_signal_handler_disconnect (vt_monitor, adata->handler_id);
}

static gboolean
_seat_activate_session (CkSeat                *seat,
                        CkSession             *session,
                        DBusGMethodInvocation *context)
{
        gboolean      res;
        gboolean      ret;
        guint         num;
        char         *device;
        ActivateData *adata;
        GError       *vt_error;

        device = NULL;
        adata = NULL;
        ret = FALSE;

        /* for now, only support switching on static seat */
        if (seat->priv->kind != CK_SEAT_KIND_STATIC) {
                GError *error;
                error = g_error_new (CK_SEAT_ERROR,
                                     CK_SEAT_ERROR_GENERAL,
                                     _("Activation is not supported for this kind of seat"));
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        if (session == NULL) {
                GError *error;
                error = g_error_new (CK_SEAT_ERROR,
                                     CK_SEAT_ERROR_GENERAL,
                                     _("Unknown session id"));
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        device = NULL;
        ck_session_get_x11_display_device (session, &device, NULL);
        if (device == NULL) {
                ck_session_get_display_device (session, &device, NULL);
        }
        res = ck_get_console_num_from_device (device, &num);
        if (! res) {
                GError *error;
                error = g_error_new (CK_SEAT_ERROR,
                                     CK_SEAT_ERROR_GENERAL,
                                     _("Unable to activate session"));
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        adata = g_new0 (ActivateData, 1);
        adata->context = context;
        adata->seat = seat;
        adata->num = num;
        adata->handler_id = g_signal_connect_data (seat->priv->vt_monitor,
                                                   "active-changed",
                                                   G_CALLBACK (activated_cb),
                                                   adata,
                                                   (GClosureNotify)g_free,
                                                   0);


        g_debug ("Attempting to activate VT %u", num);

        vt_error = NULL;
        ret = ck_vt_monitor_set_active (seat->priv->vt_monitor, num, &vt_error);
        if (! ret) {
                g_debug ("Unable to activate session: %s", vt_error->message);
                dbus_g_method_return_error (context, vt_error);
                g_signal_handler_disconnect (seat->priv->vt_monitor, adata->handler_id);
                g_error_free (vt_error);
                goto out;
        }

 out:
        g_free (device);

        return ret;
}

/*
  Example:
  dbus-send --system --dest=org.freedesktop.ConsoleKit \
  --type=method_call --print-reply --reply-timeout=2000 \
  /org/freedesktop/ConsoleKit/Seat1 \
  org.freedesktop.ConsoleKit.Seat.ActivateSession \
  objpath:/org/freedesktop/ConsoleKit/Session2
*/

gboolean
ck_seat_activate_session (CkSeat                *seat,
                          const char            *ssid,
                          DBusGMethodInvocation *context)
{
        CkSession *session;
        gboolean   ret;
        gboolean   is_open;

        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        session = NULL;

        g_debug ("Trying to activate session: %s", ssid);

        if (ssid != NULL) {
                session = g_hash_table_lookup (seat->priv->sessions, ssid);
        }

        ck_session_is_open (session, &is_open, NULL);
        if (!is_open) {
                g_debug ("CkSeat: session is not open.  Request open session");
                ret = ck_seat_request_open_session (seat, session, NULL);
                dbus_g_method_return (context, NULL);
        } else {
                g_debug ("CkSeat: session is open.  Activate session");
                ret = _seat_activate_session (seat, session, context);
        }

        return ret;
}

static gboolean
on_substitution_match (const GMatchInfo *match_info,
                       GString          *result,
                       GHashTable       *substitution_variables)
{
        char *match;
        char *value = NULL;

        match = g_match_info_fetch (match_info, 1);

        if (substitution_variables != NULL)
                value = g_hash_table_lookup (substitution_variables, match);

        if (value != NULL) {
                g_string_append (result, value);
        } else {
                char *original_string;

                original_string = g_match_info_fetch (match_info, 0);
                g_string_append (result, original_string);
                g_free (original_string);
        }
        g_free (match);

        return FALSE;
}

static char *
apply_substitutions (const char *value,
                     GHashTable *substitution_variables)
{
        GRegex *expression;
        char *expanded_string;

        expression = g_regex_new ("\\$([^[:space:]]+)", 0, 0, NULL);
        expanded_string = g_regex_replace_eval (expression,
                                                value,
                                                -1, 0, 0,
                                                (GRegexEvalCallback)
                                                on_substitution_match,
                                                substitution_variables,
                                                NULL);

        if (expanded_string == NULL) {
                expanded_string = g_strdup (value);
        }

        return expanded_string;
}

static GHashTable *
get_evaluated_parameter_map (GHashTable    *parameters,
                             GHashTable    *substitution_variables)
{
        GHashTable *evaluated_parameters;
        GHashTableIter iter;
        gpointer key, value;

        evaluated_parameters = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      (GDestroyNotify) g_free,
                                                      (GDestroyNotify) g_free);

        g_hash_table_iter_init (&iter, parameters);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
                char *expanded_string;

                expanded_string = apply_substitutions ((char *) value,
                                                       substitution_variables);

                g_hash_table_insert (evaluated_parameters,
                                     g_strdup ((char *) key),
                                     expanded_string);
        }

        return evaluated_parameters;
}

static void
request_session (gpointer key,
                 gpointer value,
                 gpointer user_data)
{
        CkSeat      *seat = CK_SEAT (user_data);
        CkSession   *session = (CkSession *) value;

        g_debug ("CkSeat: Request session");
        ck_session_set_ever_open (session, FALSE, NULL);
        ck_session_set_under_request (session, FALSE, NULL);
        ck_seat_request_open_session (seat, session, NULL);
}

static void
append_hash_table_to_dbus_message_iter (DBusMessageIter *iter,
                                        GHashTable      *hash_table)
{
        GHashTableIter hash_table_iter;
        gpointer key, value;
        DBusMessageIter array_iter;

        dbus_message_iter_open_container (iter,
                                          DBUS_TYPE_ARRAY,
                                          DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                          DBUS_TYPE_STRING_AS_STRING
                                          DBUS_TYPE_STRING_AS_STRING
                                          DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
                                          &array_iter);


        g_hash_table_iter_init (&hash_table_iter, hash_table);
        while (g_hash_table_iter_next (&hash_table_iter, &key, &value)) {
                DBusMessageIter dict_iter;

                dbus_message_iter_open_container (&array_iter,
                                                  DBUS_TYPE_DICT_ENTRY,
                                                  NULL,
                                                  &dict_iter);

                dbus_message_iter_append_basic (&dict_iter, DBUS_TYPE_STRING, &key);
                dbus_message_iter_append_basic (&dict_iter, DBUS_TYPE_STRING, &value);
                dbus_message_iter_close_container (&array_iter, &dict_iter);
        }
        dbus_message_iter_close_container (iter, &array_iter);
}

static void
emit_session_open_request (CkSeat     *seat,
                          const char *ssid,
                          const char *session_type,
                          const char *display_template_name,
                          GHashTable *display_variables,
                          const char *display_type,
                          GHashTable *evaluated_parameters)
{
        DBusMessage    *message;
        DBusConnection *connection;
        DBusMessageIter iter;

        if (!ck_seat_is_managed (seat))
                return;

        g_debug ("CkSeat: Open Session Request");

        message = dbus_message_new_signal (seat->priv->id,
                                           "org.freedesktop.ConsoleKit.Seat",
                                           "OpenSessionRequest");

        dbus_message_set_destination (message,
                                      dbus_g_proxy_get_bus_name (seat->priv->manager_proxy));

        dbus_message_iter_init_append (message, &iter);
        dbus_message_iter_append_basic (&iter, DBUS_TYPE_OBJECT_PATH, &ssid);
        dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &session_type);
        dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &display_template_name);
        append_hash_table_to_dbus_message_iter (&iter, display_variables);
        dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &display_type);
        append_hash_table_to_dbus_message_iter (&iter, evaluated_parameters);

        connection = dbus_bus_get (DBUS_BUS_SYSTEM, NULL);
        dbus_connection_send (connection, message, NULL);
        dbus_connection_unref (connection);
        dbus_message_unref (message);
}

gboolean
ck_seat_request_open_session (CkSeat                *seat,
                              CkSession             *session,
                              GError               **error)
{
        char        *ssid;
        char        *type;
        CkDisplayTemplate *display_template;
        GHashTable  *display_variables;
        GHashTable  *display_parameters;
        GHashTable  *evaluated_parameters;
        gboolean    is_open;
        gboolean    ever_open;
        gboolean    under_request;

        g_debug ("CkSeat: Request Open Session");

        ck_session_is_open (session, &is_open, NULL);

        if (is_open) {
                g_debug ("CkSeat: Is Open");
                return TRUE;
        }

        ck_session_get_under_request (session, &under_request, NULL);
        if (under_request) {
                g_debug ("CkSeat: Is Under");
                return TRUE;
        }

        ck_session_set_under_request (session, TRUE, NULL);

        ck_session_get_ever_open (session, &ever_open, NULL);

        display_template = ck_session_get_display_template (session);

        if (display_template == NULL) {
                g_debug ("CkSeat: Is Display Template");
                return TRUE;
        }

        ck_session_get_session_type (session, &type, NULL);

        if (type == NULL) {
                g_debug ("CkSeat: Type Is NULL");
                g_object_unref (display_template);
                return TRUE;
        }

        /* substitute $display $vt etc */
        display_variables = ck_session_get_display_variables (session);
        display_parameters = ck_display_template_get_parameters (display_template);

        if (display_parameters == NULL) {
                g_debug ("CkSeat: Display Parms Are NULL");
                g_free (type);
                g_object_unref (display_template);
                g_hash_table_unref (display_variables);
                return TRUE;
        }

        if (!ever_open) {
                evaluated_parameters = get_evaluated_parameter_map (display_parameters, display_variables);
        } else {
                evaluated_parameters = get_evaluated_parameter_map (display_parameters, NULL);
        }

        g_hash_table_unref (display_parameters);

        ck_session_get_id (session, &ssid, NULL);

        g_debug ("CkSeat: Emitting Open Request");
        emit_session_open_request (seat, ssid, type,
                                   ck_display_template_get_name (display_template),
                                   display_variables,
                                   ck_display_template_get_type_string (display_template),
                                   evaluated_parameters);

        g_free (ssid);
        g_free (type);
        g_hash_table_unref (evaluated_parameters);
        g_hash_table_unref (display_variables);
        g_object_unref (display_template);

        return TRUE;
}

static void
on_seat_manager_disappeared (CkSeat *seat)
{
        g_debug ("CkSeat: Seat Manager Disappeared.");

        g_signal_handlers_disconnect_by_func (seat->priv->manager_proxy,
                                              G_CALLBACK (on_seat_manager_disappeared),
                                              seat);
        g_object_unref (seat->priv->manager_proxy);
        seat->priv->manager_proxy = NULL;

        /* FIXME: should probably emit a signal so a new display manager
         * knows that the seat is now unmanaged
         *
         * (maybe only if its kind is static?)
         */
}

gboolean
ck_seat_manage (CkSeat                *seat,
                DBusGMethodInvocation *context)
{
        char *sender_name;

        g_debug ("CkSeat: Seat manage.");
        sender_name = dbus_g_method_get_sender (context);

        if (seat->priv->manager_proxy != NULL) {
                GError *error;
                const char   *existing_manager_name;

                existing_manager_name = dbus_g_proxy_get_bus_name (seat->priv->manager_proxy);

                if (existing_manager_name == NULL) {
                        g_warning ("Seat manager lacks bus unique name");
                        existing_manager_name = "<unknown>";
                }

                error = g_error_new (CK_SEAT_ERROR,
                                     CK_SEAT_ERROR_GENERAL,
                                     _("Seat already managed (by '%s')"),
                                     existing_manager_name);

                dbus_g_method_return_error (context, error);
                g_error_free (error);
                return FALSE;
        }

        /* FIXME: We pass in a bogus object path (the path we use on this side of
         * the pipe) and interface here.
         *
         * We only use the proxy to watch for when the other side disappears, not
         * for communicating with it.  All communication is one-way using signals.
         */
        seat->priv->manager_proxy = dbus_g_proxy_new_for_name (seat->priv->connection,

                                                               sender_name,
                                                               seat->priv->id,
                                                               "org.freedesktop.ConsoleKit.SeatManager");
        g_free (sender_name);

        g_signal_connect_swapped (seat->priv->manager_proxy,
                                  "destroy",
                                  G_CALLBACK (on_seat_manager_disappeared),
                                  seat);

        g_hash_table_foreach (seat->priv->sessions, request_session, seat);

        dbus_g_method_return (context);
        return TRUE;
}

gboolean
ck_seat_unmanage (CkSeat                *seat,
                  DBusGMethodInvocation *context)
{
        GError *error;
        const char   *existing_manager_name;
        char *sender_name;

        g_debug ("CkSeat: Seat unmanage.");
        if (seat->priv->manager_proxy == NULL) {
                GError *error;

                error = g_error_new (CK_SEAT_ERROR,
                                     CK_SEAT_ERROR_GENERAL,
                                     _("Seat not managed"));

                dbus_g_method_return_error (context, error);
                g_error_free (error);
                return FALSE;
        }

        sender_name = dbus_g_method_get_sender (context);
        existing_manager_name = dbus_g_proxy_get_bus_name (seat->priv->manager_proxy);

        if (strcmp (sender_name, existing_manager_name) != 0) {

            error = g_error_new (CK_SEAT_ERROR,
                                 CK_SEAT_ERROR_GENERAL,
                                 _("Seat managed by '%s' not '%s'"),
                                 existing_manager_name,
                                 sender_name);

            dbus_g_method_return_error (context, error);
            g_error_free (error);

            return FALSE;
        }

        on_seat_manager_disappeared (seat);

        dbus_g_method_return (context);
        return TRUE;
}

void
ck_seat_request_removal (CkSeat *seat)
{
        DBusMessage    *message;
        DBusConnection *connection;

        g_return_if_fail (CK_IS_SEAT (seat));
        g_return_if_fail (ck_seat_is_managed (seat));

        g_debug ("CkSeat: Seat request removal.");

        message = dbus_message_new_signal (seat->priv->id,
                                           "org.freedesktop.ConsoleKit.Seat",
                                           "RemoveRequest");

        dbus_message_set_destination (message,
                                      dbus_g_proxy_get_bus_name (seat->priv->manager_proxy));

        connection = dbus_bus_get (DBUS_BUS_SYSTEM, NULL);
        dbus_connection_send (connection, message, NULL);
        dbus_connection_unref (connection);
        dbus_message_unref (message);
}

static gboolean
match_session_display_device (const char *key,
                              CkSession  *session,
                              const char *display_device)
{
        char    *device;
        gboolean ret;

        device = NULL;
        ret = FALSE;

        g_debug ("CkSeat: Session display device.");

        if (session == NULL) {
                goto out;
        }

        ck_session_get_display_device (session, &device, NULL);

        if (device != NULL
            && display_device != NULL
            && strcmp (device, display_device) == 0) {
                g_debug ("Matched display-device %s to %s", display_device, key);
                ret = TRUE;
        }
out:

        g_free (device);

        return ret;
}

static gboolean
match_session_x11_display_device (const char *key,
                                  CkSession  *session,
                                  const char *x11_display_device)
{
        char    *device;
        gboolean ret;

        device = NULL;
        ret = FALSE;

        if (session == NULL) {
                goto out;
        }

        ck_session_get_x11_display_device (session, &device, NULL);

        if (device != NULL
            && x11_display_device != NULL
            && strcmp (device, x11_display_device) == 0) {
                g_debug ("Matched x11-display-device %s to %s", x11_display_device, key);
                ret = TRUE;
        }
out:

        g_free (device);

        return ret;
}

typedef struct
{
        GHRFunc  predicate;
        gpointer user_data;
        GList   *list;
} HashTableFindAllData;

static void
find_all_func (gpointer              key,
               gpointer              value,
               HashTableFindAllData *data)
{
        gboolean res;

        res = data->predicate (key, value, data->user_data);
        if (res) {
                data->list = g_list_prepend (data->list, value);
        }
}

static GList *
hash_table_find_all (GHashTable *hash_table,
                     GHRFunc     predicate,
                     gpointer    user_data)
{
        HashTableFindAllData *data;
        GList                *list;

        data = g_new0 (HashTableFindAllData, 1);
        data->predicate = predicate;
        data->user_data = user_data;
        g_hash_table_foreach (hash_table, (GHFunc) find_all_func, data);
        list = data->list;
        g_free (data);
        return list;
}

static GList *
find_sessions_for_display_device (CkSeat     *seat,
                                  const char *device)
{
        GList *sessions;

        sessions = hash_table_find_all (seat->priv->sessions,
                                        (GHRFunc) match_session_display_device,
                                        (gpointer) device);
        return sessions;
}

static GList *
find_sessions_for_x11_display_device (CkSeat     *seat,
                                      const char *device)
{
        GList *sessions;

        sessions = hash_table_find_all (seat->priv->sessions,
                                        (GHRFunc) match_session_x11_display_device,
                                        (gpointer) device);
        return sessions;
}

static int
sort_sessions_by_age (CkSession *a,
                      CkSession *b)
{
        char *iso_a;
        char *iso_b;
        int   ret;

        ck_session_get_creation_time (a, &iso_a, NULL);
        ck_session_get_creation_time (b, &iso_b, NULL);

        ret = strcmp (iso_a, iso_b);

        g_free (iso_a);
        g_free (iso_b);

        return ret;
}

static CkSession *
find_oldest_session (GList *sessions)
{

        sessions = g_list_sort (sessions,
                                (GCompareFunc) sort_sessions_by_age);
        return sessions->data;
}

static CkSession *
find_session_for_display_device (CkSeat     *seat,
                                 const char *device)
{
        GList     *sessions;
        CkSession *session;

        sessions = find_sessions_for_display_device (seat, device);
        if (sessions == NULL) {
                sessions = find_sessions_for_x11_display_device (seat, device);
        }

        if (sessions == NULL) {
                return NULL;
        }

        if (g_list_length (sessions) == 1) {
                session = sessions->data;
        } else {
                session = find_oldest_session (sessions);
        }

        g_list_free (sessions);

        return session;
}

static void
change_active_session (CkSeat    *seat,
                       CkSession *session)
{
        char      *ssid;
        CkSession *old_session;

        g_debug ("CkSeat: Change active session.");

        if (seat->priv->active_session == session) {
                return;
        }

        old_session = seat->priv->active_session;

        if (old_session != NULL) {
                ck_session_set_active (old_session, FALSE, NULL);
        }

        seat->priv->active_session = session;

        ssid = NULL;
        if (session != NULL) {
                g_object_ref (session);
                ck_session_get_id (session, &ssid, NULL);
                ck_session_set_active (session, TRUE, NULL);
        }

        g_debug ("Active session changed: %s", ssid ? ssid : "(null)");

        /* The order of signal emission matters here. The manager
         * dumps the database when receiving the
         * 'active-session-changed-full' signal and does callout
         * handling. dbus-glib will then send out a D-Bus on the
         * 'active-session-changed' signal. Since the D-Bus signal
         * must be sent when the database dump is finished it is
         * important that the '-full' signalled is emitted first. */

        if (CK_IS_SESSION (old_session)) {
                g_signal_emit (seat, signals [ACTIVE_SESSION_CHANGED_FULL], 0, old_session, session);
        }
        g_signal_emit (seat, signals [ACTIVE_SESSION_CHANGED], 0, ssid);

        if (old_session != NULL) {
                g_object_unref (old_session);
        }

        g_free (ssid);
}

static void
find_possible_session_to_activate (CkSeat *seat)
{
        GHashTableIter iter;
        gpointer       key, value;
        gboolean       is_open;
        char          *session_type = NULL;
        CkSession     *login_session = NULL;

        g_debug ("CkSeat: Find possible session to activate");
        g_hash_table_iter_init (&iter, seat->priv->sessions);
        while (g_hash_table_iter_next (&iter, &key, &value)) {

                ck_session_is_open (value, &is_open, NULL);

                if (is_open) {
                        GError  *vt_error = NULL;
                        char    *device;
                        guint    num;
                        gboolean ret;

                        login_session = NULL;
                        g_debug ("Found open session.");

                        switch (seat->priv->kind) {
                           case CK_SEAT_KIND_STATIC:
                                device = NULL;
                                ck_session_get_x11_display_device (value, &device, NULL);

                                if (device != NULL) {
                                   ret = ck_get_console_num_from_device (device, &num);
                                   if (ret) {
                                           g_debug ("Activating VT %d", num);
                                           ck_vt_monitor_set_active (seat->priv->vt_monitor, num, &vt_error);
                                   }
                                }
                                break;
                            case CK_SEAT_KIND_DYNAMIC:
                                change_active_session (seat, value);
                                break;
                        }
                }

                ck_session_get_session_type (value, &session_type, NULL);

                g_debug ("CkSeat: Session Type %s", session_type);

                if (IS_STR_SET (session_type) &&
                    g_str_equal (session_type, "LoginWindow")) {
                        login_session = value;
                        g_debug ("CkSeat: Setting login_session");
                        g_free (session_type);
                }
        }

        if (login_session != NULL) {
                g_debug ("CkSeat: Login Session is not NULL");
                ck_session_set_ever_open (login_session, FALSE, NULL);
                ck_seat_request_open_session (seat, login_session, NULL);
        }

}

static void
update_active_vt (CkSeat *seat,
                  guint   num)
{
        CkSession *session;
        char      *device;

        device = ck_get_console_device_for_num (num);

        g_debug ("Active device: %s", device);

        session = find_session_for_display_device (seat, device);

        if (session == NULL) {
                find_possible_session_to_activate (seat);
        } else {
                change_active_session (seat, session);
        }

        g_free (device);
}

static void
maybe_update_active_session (CkSeat *seat)
{
        guint num;

        switch (seat->priv->kind){
        case CK_SEAT_KIND_STATIC:
                if (ck_vt_monitor_get_active (seat->priv->vt_monitor,
                                              &num, NULL)) {
                        update_active_vt (seat, num);
                }
                break;
        case CK_SEAT_KIND_DYNAMIC:
                find_possible_session_to_activate (seat);
                break;
        default:
                break;
        }
}

static gboolean
session_activate (CkSession             *session,
                  DBusGMethodInvocation *context,
                  CkSeat                *seat)
{
        g_debug ("CkSeat: Session activate.");
        _seat_activate_session (seat, session, context);

        /* always return TRUE to indicate that the signal was handled */
        return TRUE;
}

gboolean
ck_seat_remove_session (CkSeat         *seat,
                        CkSession      *session,
                        GError        **error)
{
        char       *ssid;
        char      *orig_ssid;
        CkSession *orig_session;
        gboolean   res;
        gboolean   ret;
        GHashTableIter iter;
        gpointer       key, value;
        gboolean       found_login = FALSE;
        gboolean       is_open;
        char          *session_type = NULL;

        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        ret = FALSE;
        ssid = NULL;
        ck_session_get_id (session, &ssid, NULL);

        g_debug ("CkSeat: Removing session '%s'", ssid);

        /* Need to get the original key/value */
        res = g_hash_table_lookup_extended (seat->priv->sessions,
                                            ssid,
                                            (gpointer *)&orig_ssid,
                                            (gpointer *)&orig_session);
        if (! res) {
                g_debug ("Session %s is not attached to seat %s", ssid, seat->priv->id);
                g_set_error (error,
                             CK_SEAT_ERROR,
                             CK_SEAT_ERROR_GENERAL,
                             _("Session is not attached to this seat"));
                goto out;
        }

        g_signal_handlers_disconnect_by_func (session, session_activate, seat);

        /* Remove the session from the list but don't call
         * unref until the signal is emitted */
        g_hash_table_steal (seat->priv->sessions, ssid);

        g_debug ("Emitting session-removed: %s", ssid);

        /* The order of signal emission matters here, too, for similar
         * reasons as for 'session-added'/'session-added-full'. See
         * above. */

        g_signal_emit (seat, signals [SESSION_REMOVED_FULL], 0, session);
        g_signal_emit (seat, signals [SESSION_REMOVED], 0, ssid);

        /* try to change the active session */

        /* On session exit, first try to switch to any active login sessions */
        g_hash_table_iter_init (&iter, seat->priv->sessions);
        if (seat->priv->kind == CK_SEAT_KIND_STATIC) {
                while (seat->priv->kind == CK_SEAT_KIND_STATIC && g_hash_table_iter_next (&iter, &key, &value)) {
                        CkSession *login_session = value;
                        ck_session_get_session_type (login_session, &session_type, NULL);
                        ck_session_is_open (login_session, &is_open, NULL);

                        if (is_open && IS_STR_SET (session_type) &&
                            g_str_equal (session_type, "LoginWindow")) {
                                GError    *vt_error = NULL;
                                char      *device;
                                guint      num;

                                g_debug ("Found a LoginWindow");
                                device = NULL;
                                ck_session_get_x11_display_device (login_session, &device, NULL);

                                if (device != NULL) {
                                        ret = ck_get_console_num_from_device (device, &num);
                                        if (ret) {
                                                g_debug ("Setting active to %d", num);
                                                found_login = TRUE;
                                                ck_vt_monitor_set_active (seat->priv->vt_monitor, num, &vt_error);
                                        } else {
                                               g_debug ("The LoginWindow device has no console number, not using.");
                                        }
                                } else {
                                       g_debug ("The LoginWindow display has no x11 display device, not using.");
                                }
                        }
                        if (session_type != NULL)
                                g_free (session_type);
                }
        }

        /* Otherwise, look for an active session */
        if (found_login == FALSE) {
                g_debug ("Login not found.  Maybe update active session");
                maybe_update_active_session (seat);
        }

        if (orig_session != NULL) {
                g_object_unref (orig_session);
        }
        g_free (orig_ssid);

        ret = TRUE;
 out:
        g_free (ssid);

        return ret;
}

static void
emit_session_close_request (CkSeat     *seat,
                            const char *ssid)
{
        DBusMessage    *message;
        DBusConnection *connection;
        DBusMessageIter iter;

        g_debug ("CkSeat: Emit Session Close request");

        message = dbus_message_new_signal (seat->priv->id,
                                           "org.freedesktop.ConsoleKit.Seat",
                                           "CloseSessionRequest");

        dbus_message_set_destination (message,
                                      dbus_g_proxy_get_bus_name (seat->priv->manager_proxy));

        dbus_message_iter_init_append (message, &iter);
        dbus_message_iter_append_basic (&iter, DBUS_TYPE_OBJECT_PATH, &ssid);

        connection = dbus_bus_get (DBUS_BUS_SYSTEM, NULL);
        dbus_connection_send (connection, message, NULL);
        dbus_connection_unref (connection);
        dbus_message_unref (message);
}

gboolean
ck_seat_request_close_session (CkSeat                *seat,
                               CkSession             *session,
                               GError               **error)
{
        char      *ssid;

        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);
        g_return_val_if_fail (CK_IS_SESSION (session), FALSE);
        g_return_val_if_fail (ck_seat_is_managed (seat), FALSE);

        g_debug ("CkSeat: Request close session.");
        ck_session_get_id (session, &ssid, NULL);

        emit_session_close_request (seat, ssid);

        g_free (ssid);

        return FALSE;
}

gboolean
ck_seat_no_respawn (CkSeat                *seat,
                    CkSession             *session,
                    GError               **error)
{
        DBusMessage    *message;
        DBusConnection *connection;
        DBusMessageIter iter;
        char           *ssid;

        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);
        g_return_val_if_fail (CK_IS_SESSION (session), FALSE);
        g_return_val_if_fail (ck_seat_is_managed (seat), FALSE);

        g_debug ("CkSeat: No Respawn.");
        ck_session_get_id (session, &ssid, NULL);

        message = dbus_message_new_signal (seat->priv->id,
                                           "org.freedesktop.ConsoleKit.Seat",
                                           "NoRespawn");

        dbus_message_set_destination (message,
                                      dbus_g_proxy_get_bus_name (seat->priv->manager_proxy));

        dbus_message_iter_init_append (message, &iter);
        dbus_message_iter_append_basic (&iter, DBUS_TYPE_OBJECT_PATH, &ssid);

        connection = dbus_bus_get (DBUS_BUS_SYSTEM, NULL);
        dbus_connection_send (connection, message, NULL);
        dbus_connection_unref (connection);
        dbus_message_unref (message);

        g_free (ssid);

        return FALSE;
}

gboolean
ck_seat_add_session (CkSeat         *seat,
                     CkSession      *session,
                     GError        **error)
{
        char *ssid;
        GHashTableIter iter;
        gpointer key, value;
        gboolean found;

        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        g_debug ("CkSeat: Add session.");
        ck_session_get_id (session, &ssid, NULL);

        found = FALSE;
        g_hash_table_iter_init (&iter, seat->priv->sessions);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
                if (g_str_equal ((gchar *)key, ssid)) {
                        found = TRUE;
                        break;
                }
        }

        if (! found)
                g_hash_table_insert (seat->priv->sessions, g_strdup (ssid), g_object_ref (session));
        else
                g_object_ref (session);

        ck_session_set_seat_id (session, seat->priv->id, NULL);

        g_signal_connect_object (session, "activate", G_CALLBACK (session_activate), seat, 0);
        /* FIXME: attach to property notify signals? */

        g_debug ("Emitting added signal: %s", ssid);

        /* The order of signal emission matters here, too. See
         * above. */

        g_signal_emit (seat, signals [SESSION_ADDED_FULL], 0, session);
        g_signal_emit (seat, signals [SESSION_ADDED], 0, ssid);

        if (ck_seat_is_managed (seat)) {
                g_debug ("CkSeat: Seat is managed.  Request open session");
                ck_seat_request_open_session (seat, session, NULL);
        } else {
                g_debug ("CkSeat: Seat is not managed.  Maybe update active session");
                maybe_update_active_session (seat);
        }

        g_free (ssid);

        return TRUE;
}

gboolean
ck_seat_can_activate_sessions (CkSeat   *seat,
                               gboolean *can_activate,
                               GError  **error)
{
        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        if (can_activate != NULL) {
                *can_activate = (seat->priv->kind == CK_SEAT_KIND_STATIC);
        }

        return TRUE;
}

static gboolean
ck_seat_has_device (CkSeat      *seat,
                    GValueArray *device,
                    gboolean    *result,
                    GError      *error)
{
        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        return TRUE;
}

gboolean
ck_seat_add_device (CkSeat         *seat,
                    GValueArray    *device,
                    GError        **error)
{
        gboolean present;

        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        /* FIXME: check if already present */
        present = FALSE;
        ck_seat_has_device (seat, device, &present, NULL);
        if (present) {
                g_set_error (error, CK_SEAT_ERROR, CK_SEAT_ERROR_GENERAL, "%s", "Device already present");
                return FALSE;
        }

        g_ptr_array_add (seat->priv->devices, g_boxed_copy (CK_TYPE_DEVICE, device));

        g_debug ("Emitting device added signal");
        g_signal_emit (seat, signals [DEVICE_ADDED], 0, device);

        return TRUE;
}

gboolean
ck_seat_remove_device (CkSeat         *seat,
                       GValueArray    *device,
                       GError        **error)
{
        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        /* FIXME: check if already present */
        if (0) {
                g_debug ("Emitting device removed signal");
                g_signal_emit (seat, signals [DEVICE_REMOVED], 0, device);
        }

        return TRUE;
}

gboolean
ck_seat_get_kind (CkSeat        *seat,
                  CkSeatKind    *kind,
                  GError        **error)
{
        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        if (kind != NULL) {
                *kind = seat->priv->kind;
        }

        return TRUE;
}

gboolean
ck_seat_get_type_string (CkSeat                *seat,
                         char                 **type,
                         GError               **error)
{
        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        if (type != NULL) {
                *type = g_strdup (seat->priv->type);
        }

        return TRUE;
}

gboolean
ck_seat_get_id (CkSeat         *seat,
                char          **id,
                GError        **error)
{
        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        if (id != NULL) {
                *id = g_strdup (seat->priv->id);
        }

        return TRUE;
}

static void
active_vt_changed (CkVtMonitor    *vt_monitor,
                   guint           num,
                   CkSeat         *seat)
{
        g_debug ("Active vt changed: %u", num);
}

gboolean
ck_seat_register (CkSeat *seat)
{
        GError *error = NULL;

        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        error = NULL;
        g_debug ("CkSeat: Register seat.");
        seat->priv->connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
        if (seat->priv->connection == NULL) {
                if (error != NULL) {
                        g_critical ("error getting system bus: %s", error->message);
                        g_error_free (error);
                }
                return FALSE;
        }

        dbus_g_connection_register_g_object (seat->priv->connection, seat->priv->id, G_OBJECT (seat));

        return TRUE;
}

static void
listify_session_ids (char       *id,
                     CkSession  *session,
                     GPtrArray **array)
{
        g_ptr_array_add (*array, g_strdup (id));
}

gboolean
ck_seat_get_sessions (CkSeat         *seat,
                      GPtrArray     **sessions,
                      GError        **error)
{
        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        if (sessions == NULL) {
                return FALSE;
        }

        *sessions = g_ptr_array_new ();
        g_hash_table_foreach (seat->priv->sessions, (GHFunc)listify_session_ids, sessions);

        return TRUE;
}

static void
copy_devices (gpointer    data,
              GPtrArray **array)
{
        g_ptr_array_add (*array, data);
}

/*
  Example:
  dbus-send --system --dest=org.freedesktop.ConsoleKit \
  --type=method_call --print-reply --reply-timeout=2000 \
  /org/freedesktop/ConsoleKit/Seat1 \
  org.freedesktop.ConsoleKit.Seat.GetDevices
*/

gboolean
ck_seat_get_devices (CkSeat         *seat,
                     GPtrArray     **devices,
                     GError        **error)
{
        g_return_val_if_fail (CK_IS_SEAT (seat), FALSE);

        if (devices == NULL) {
                return FALSE;
        }

        *devices = g_ptr_array_sized_new (seat->priv->devices->len);
        g_ptr_array_foreach (seat->priv->devices, (GFunc)copy_devices, devices);

        return TRUE;
}

static void
_ck_seat_set_id (CkSeat         *seat,
                 const char     *id)
{
        g_free (seat->priv->id);
        seat->priv->id = g_strdup (id);
}

static void
_ck_seat_set_kind (CkSeat    *seat,
                   CkSeatKind kind)
{
        seat->priv->kind = kind;
}

static void
_ck_seat_set_type_string (CkSeat         *seat,
                          const char     *type)
{
        g_free (seat->priv->type);
        seat->priv->type = g_strdup (type);
}

static void
ck_seat_set_property (GObject            *object,
                      guint               prop_id,
                      const GValue       *value,
                      GParamSpec         *pspec)
{
        CkSeat *self;

        self = CK_SEAT (object);

        switch (prop_id) {
        case PROP_ID:
                _ck_seat_set_id (self, g_value_get_string (value));
                break;
        case PROP_KIND:
                _ck_seat_set_kind (self, g_value_get_enum (value));
                break;
        case PROP_TYPE:
                _ck_seat_set_type_string (self, g_value_get_string (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
ck_seat_get_property (GObject    *object,
                      guint       prop_id,
                      GValue     *value,
                      GParamSpec *pspec)
{
        CkSeat *self;

        self = CK_SEAT (object);

        switch (prop_id) {
        case PROP_ID:
                g_value_set_string (value, self->priv->id);
                break;
        case PROP_KIND:
                g_value_set_enum (value, self->priv->kind);
                break;
        case PROP_TYPE:
                g_value_set_string (value, self->priv->type);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static GObject *
ck_seat_constructor (GType                  type,
                     guint                  n_construct_properties,
                     GObjectConstructParam *construct_properties)
{
        CkSeat      *seat;

        seat = CK_SEAT (G_OBJECT_CLASS (ck_seat_parent_class)->constructor (type,
                                                                            n_construct_properties,
                                                                            construct_properties));

        if (seat->priv->kind == CK_SEAT_KIND_STATIC) {
                seat->priv->vt_monitor = ck_vt_monitor_new ();
                g_signal_connect (seat->priv->vt_monitor, "active-changed", G_CALLBACK (active_vt_changed), seat);
        }

        return G_OBJECT (seat);
}

static void
ck_seat_class_init (CkSeatClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->get_property = ck_seat_get_property;
        object_class->set_property = ck_seat_set_property;
        object_class->constructor = ck_seat_constructor;
        object_class->finalize = ck_seat_finalize;

        signals [ACTIVE_SESSION_CHANGED] = g_signal_new ("active-session-changed",
                                                         G_TYPE_FROM_CLASS (object_class),
                                                         G_SIGNAL_RUN_LAST,
                                                         G_STRUCT_OFFSET (CkSeatClass, active_session_changed),
                                                         NULL,
                                                         NULL,
                                                         g_cclosure_marshal_VOID__STRING,
                                                         G_TYPE_NONE,
                                                         1, G_TYPE_STRING);
        signals [ACTIVE_SESSION_CHANGED_FULL] = g_signal_new ("active-session-changed-full",
                                                         G_TYPE_FROM_CLASS (object_class),
                                                         G_SIGNAL_RUN_LAST,
                                                         0,
                                                         NULL,
                                                         NULL,
                                                         ck_marshal_VOID__OBJECT_OBJECT,
                                                         G_TYPE_NONE,
                                                         2, CK_TYPE_SESSION, CK_TYPE_SESSION);
        signals [SESSION_ADDED] = g_signal_new ("session-added",
                                                G_TYPE_FROM_CLASS (object_class),
                                                G_SIGNAL_RUN_LAST,
                                                G_STRUCT_OFFSET (CkSeatClass, session_added),
                                                NULL,
                                                NULL,
                                                g_cclosure_marshal_VOID__BOXED,
                                                G_TYPE_NONE,
                                                1, DBUS_TYPE_G_OBJECT_PATH);
        signals [SESSION_ADDED_FULL] = g_signal_new ("session-added-full",
                                                G_TYPE_FROM_CLASS (object_class),
                                                G_SIGNAL_RUN_LAST,
                                                0,
                                                NULL,
                                                NULL,
                                                g_cclosure_marshal_VOID__OBJECT,
                                                G_TYPE_NONE,
                                                1, CK_TYPE_SESSION);
        signals [SESSION_REMOVED] = g_signal_new ("session-removed",
                                                  G_TYPE_FROM_CLASS (object_class),
                                                  G_SIGNAL_RUN_LAST,
                                                  G_STRUCT_OFFSET (CkSeatClass, session_removed),
                                                  NULL,
                                                  NULL,
                                                  g_cclosure_marshal_VOID__BOXED,
                                                  G_TYPE_NONE,
                                                  1, DBUS_TYPE_G_OBJECT_PATH);
        signals [SESSION_REMOVED_FULL] = g_signal_new ("session-removed-full",
                                                  G_TYPE_FROM_CLASS (object_class),
                                                  G_SIGNAL_RUN_LAST,
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  g_cclosure_marshal_VOID__OBJECT,
                                                  G_TYPE_NONE,
                                                  1, CK_TYPE_SESSION);
        signals [DEVICE_ADDED] = g_signal_new ("device-added",
                                               G_TYPE_FROM_CLASS (object_class),
                                               G_SIGNAL_RUN_LAST,
                                               G_STRUCT_OFFSET (CkSeatClass, device_added),
                                               NULL,
                                               NULL,
                                               g_cclosure_marshal_VOID__BOXED,
                                               G_TYPE_NONE,
                                               1, CK_TYPE_DEVICE);
        signals [DEVICE_REMOVED] = g_signal_new ("device-removed",
                                                 G_TYPE_FROM_CLASS (object_class),
                                                 G_SIGNAL_RUN_LAST,
                                                 G_STRUCT_OFFSET (CkSeatClass, device_removed),
                                                 NULL,
                                                 NULL,
                                                 g_cclosure_marshal_VOID__BOXED,
                                                 G_TYPE_NONE,
                                                 1, CK_TYPE_DEVICE);
        signals [REMOVE_REQUEST] = g_signal_new ("remove-request",
                                                  G_TYPE_FROM_CLASS (object_class),
                                                  G_SIGNAL_RUN_LAST,
                                                  G_STRUCT_OFFSET (CkSeatClass, remove_request),
                                                  NULL,
                                                  NULL,
                                                  g_cclosure_marshal_VOID__VOID,
                                                  G_TYPE_NONE, 0);
        signals [OPEN_SESSION_REQUEST] = g_signal_new ("open-session-request",
                                                 G_TYPE_FROM_CLASS (object_class),
                                                 G_SIGNAL_RUN_LAST,
                                                 G_STRUCT_OFFSET (CkSeatClass, open_session_request),
                                                 NULL,
                                                 NULL,
                                                 ck_marshal_VOID__STRING_STRING_STRING_POINTER_STRING_POINTER,
                                                 G_TYPE_NONE,
                                                 6,
                                                 DBUS_TYPE_G_OBJECT_PATH,
                                                 G_TYPE_STRING,
                                                 G_TYPE_STRING,
                                                 CK_DBUS_TYPE_G_STRING_STRING_HASHTABLE,
                                                 G_TYPE_STRING,
                                                 CK_DBUS_TYPE_G_STRING_STRING_HASHTABLE,
                                                 G_TYPE_INVALID);
        signals [CLOSE_SESSION_REQUEST] = g_signal_new ("close-session-request",
                                                  G_TYPE_FROM_CLASS (object_class),
                                                  G_SIGNAL_RUN_LAST,
                                                  G_STRUCT_OFFSET (CkSeatClass, close_session_request),
                                                  NULL,
                                                  NULL,
                                                  g_cclosure_marshal_VOID__VOID,
                                                  G_TYPE_NONE,
                                                  1, DBUS_TYPE_G_OBJECT_PATH);
        signals [NO_RESPAWN] = g_signal_new ("no-respawn",
                                                  G_TYPE_FROM_CLASS (object_class),
                                                  G_SIGNAL_RUN_LAST,
                                                  G_STRUCT_OFFSET (CkSeatClass, no_respawn),
                                                  NULL,
                                                  NULL,
                                                  g_cclosure_marshal_VOID__VOID,
                                                  G_TYPE_NONE, 0);

        g_object_class_install_property (object_class,
                                         PROP_ID,
                                         g_param_spec_string ("id",
                                                              "id",
                                                              "id",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_KIND,
                                         g_param_spec_enum ("kind",
                                                            "kind",
                                                            "kind",
                                                            CK_TYPE_SEAT_KIND,
                                                            CK_SEAT_KIND_DYNAMIC,
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

        g_object_class_install_property (object_class,
                                         PROP_TYPE,
                                         g_param_spec_string ("type",
                                                              "type",
                                                              "type",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_type_class_add_private (klass, sizeof (CkSeatPrivate));

        dbus_g_object_type_install_info (CK_TYPE_SEAT, &dbus_glib_ck_seat_object_info);
}

static void
ck_seat_init (CkSeat *seat)
{
        seat->priv = CK_SEAT_GET_PRIVATE (seat);

        seat->priv->sessions = g_hash_table_new_full (g_str_hash,
                                                      g_str_equal,
                                                      g_free,
                                                      (GDestroyNotify) g_object_unref);
        seat->priv->devices = g_ptr_array_new ();
        seat->priv->manager_proxy = NULL;
}

static void
ck_seat_finalize (GObject *object)
{
        CkSeat *seat;

        g_return_if_fail (object != NULL);
        g_return_if_fail (CK_IS_SEAT (object));

        seat = CK_SEAT (object);

        g_return_if_fail (seat->priv != NULL);

        if (seat->priv->vt_monitor != NULL) {
                g_object_unref (seat->priv->vt_monitor);
        }

        if (seat->priv->active_session != NULL) {
                g_object_unref (seat->priv->active_session);
        }

        g_ptr_array_free (seat->priv->devices, TRUE);
        g_hash_table_destroy (seat->priv->sessions);
        g_free (seat->priv->id);
        g_free (seat->priv->type);

        G_OBJECT_CLASS (ck_seat_parent_class)->finalize (object);
}

CkSeat *
ck_seat_new (const char *sid,
             CkSeatKind  kind,
             const char *type)
{
        GObject *object;

        object = g_object_new (CK_TYPE_SEAT,
                               "id", sid,
                               "kind", kind,
                               "type", type,
                               NULL);

        return CK_SEAT (object);
}

CkSeat *
ck_seat_new_with_devices_and_sessions (const char *sid,
                                       CkSeatKind  kind,
                                       GPtrArray  *devices,
                                       GPtrArray  *sessions)
{
        GObject *object;
        int      i;

        object = g_object_new (CK_TYPE_SEAT,
                               "id", sid,
                               "kind", kind,
                               NULL);

        if (devices != NULL) {
                for (i = 0; i < devices->len; i++) {
                        ck_seat_add_device (CK_SEAT (object), g_ptr_array_index (devices, i), NULL);
                }
        }
        if (sessions != NULL) {
                for (i = 0; i < sessions->len; i++) {
                        ck_seat_add_session (CK_SEAT (object), g_ptr_array_index (sessions, i), NULL);
                }
        }


        return CK_SEAT (object);
}

static char *
generate_static_session_id (const char *sid,
                            const char *session_name)
{
        const char *seat_name;
        char *ssid;

        seat_name = strrchr (sid, '/');

        if (seat_name == NULL) {
                g_warning ("Seat id '%s' lacks a /", sid);
                seat_name = sid;
        } else {
                seat_name++;
        }

        ssid = g_strdup_printf ("%s/Session%s%s",
                                CK_DBUS_PATH, seat_name,
                                session_name);

        return ssid;
}

CkSeat *
ck_seat_new_from_file (char       **sid,
                       const char *path)
{
        GKeyFile      *key_file;
        gboolean       res;
        GError        *error;
        char          *group;
        CkSeat        *seat;
        char          *read_sid;
        gboolean       hidden;
        GPtrArray     *sessions;
        char         **session_list;
        gsize          nsessions;
        GPtrArray     *devices;
        char         **device_list;
        gsize          ndevices;
        gsize          i;

        seat = NULL;

        g_debug ("CkSeat: New seat from file.");

        key_file = g_key_file_new ();
        error = NULL;
        res = g_key_file_load_from_file (key_file,
                                         path,
                                         G_KEY_FILE_NONE,
                                         &error);
        if (! res) {
                g_warning ("Unable to load seats from file %s: %s", path, error->message);
                g_error_free (error);
                goto out;
        }

        group = g_key_file_get_start_group (key_file);
        if (group == NULL || strcmp (group, "Seat Entry") != 0) {
                g_warning ("Not a seat file: %s", path);
                goto out;
        }

        hidden = g_key_file_get_boolean (key_file, group, "Hidden", NULL);
        if (hidden) {
                g_debug ("Seat is hidden");
                goto out;
        }

        read_sid = g_key_file_get_string (key_file, group, "ID", NULL);
        if (IS_STR_SET (read_sid)) {
                g_free (*sid);
                *sid = g_strdup_printf ("%s/%s", CK_DBUS_PATH, read_sid);
        } else {
                g_free (read_sid);
        }

        session_list = g_key_file_get_string_list (key_file, group, "Sessions", &nsessions, NULL);

        sessions = g_ptr_array_sized_new (nsessions);

        for (i = 0; i < nsessions; i++) {
                char *path;
                char *file;
                char *ssid;
                CkSession *session;

                file = g_strconcat (session_list[i], ".session", NULL);
                path = g_build_filename (CK_SESSION_DIR, file, NULL);
                g_free (file);

                /* FIXME: we should probably use the same naming pool as
                 * sessions generated from ck-manager.  We mangle the name
                 * here so we don't clash with names from ck-manager
                 */
                ssid = generate_static_session_id (*sid, session_list[i]);

                session = ck_session_new_from_file (ssid, path);

                if (session == NULL) {
                        g_warning ("Unable to load session from file %s", path);
                        g_free (path);
                        continue;
                }
                g_free (path);
                ck_session_set_seat_id (session, *sid, NULL);

                g_ptr_array_add (sessions, session);
        }

        g_strfreev (session_list);

        device_list = g_key_file_get_string_list (key_file, group, "Devices", &ndevices, NULL);

        g_debug ("CkSeat: Creating seat %s with %zd devices", *sid, ndevices);

        devices = g_ptr_array_sized_new (ndevices);

        for (i = 0; i < ndevices; i++) {
                char **split;
                GValue device_val = { 0, };

                split = g_strsplit (device_list[i], ":", 2);

                if (split == NULL) {
                        continue;
                }

                g_debug ("Adding device: %s %s", split[0], split[1]);

                g_value_init (&device_val, CK_TYPE_DEVICE);
                g_value_take_boxed (&device_val,
                                    dbus_g_type_specialized_construct (CK_TYPE_DEVICE));
                dbus_g_type_struct_set (&device_val,
                                        0, split[0],
                                        1, split[1],
                                        G_MAXUINT);

                g_ptr_array_add (devices, g_value_get_boxed (&device_val));

                g_strfreev (split);
        }
        g_strfreev (device_list);

        seat = ck_seat_new_with_devices_and_sessions (*sid, CK_SEAT_KIND_STATIC, devices, sessions);
        g_ptr_array_free (devices, TRUE);

        g_free (group);

out:

        g_key_file_free (key_file);

        return seat;
}

static void
env_add_session_info (CkSession  *session,
                      const char *prefix,
                      char      **extra_env,
                      int        *n)
{
        char     *s;
        gboolean  b;
        guint     u;

        if (session == NULL) {
                return;
        }

        s = NULL;
        if (ck_session_get_id (session, &s, NULL) && s != NULL && *s != '\0') {
                extra_env[(*n)++] = g_strdup_printf ("%sID=%s", prefix, s);
        }
        g_free (s);

        s = NULL;
        if (ck_session_get_session_type (session, &s, NULL) && s != NULL && *s != '\0') {
                extra_env[(*n)++] = g_strdup_printf ("%sTYPE=%s", prefix, s);
        }
        g_free (s);

        if (ck_session_get_unix_user (session, &u, NULL)) {
                extra_env[(*n)++] = g_strdup_printf ("%sUSER_UID=%u", prefix, u);
        }

        s = NULL;
        if (ck_session_get_display_device (session, &s, NULL) && s != NULL && *s != '\0') {
                extra_env[(*n)++] = g_strdup_printf ("%sDISPLAY_DEVICE=%s", prefix, s);
        }
        g_free (s);

        s = NULL;
        if (ck_session_get_x11_display_device (session, &s, NULL) && s != NULL && *s != '\0') {
                extra_env[(*n)++] = g_strdup_printf ("%sX11_DISPLAY_DEVICE=%s", prefix, s);
        }
        g_free (s);

        s = NULL;
        if (ck_session_get_x11_display (session, &s, NULL) && s != NULL && *s != '\0') {
                extra_env[(*n)++] = g_strdup_printf ("%sX11_DISPLAY=%s", prefix, s);
        }
        g_free (s);

        s = NULL;
        if (ck_session_get_remote_host_name (session, &s, NULL) && s != NULL && *s != '\0') {
                extra_env[(*n)++] = g_strdup_printf ("%sREMOTE_HOST_NAME=%s", prefix, s);
        }
        g_free (s);

        if (ck_session_is_local (session, &b, NULL)) {
                extra_env[(*n)++] = g_strdup_printf ("%sIS_LOCAL=%s", prefix, b ? "true" : "false");
        }
}

void
ck_seat_run_programs (CkSeat    *seat,
                      CkSession *old_session,
                      CkSession *new_session,
                      const char *action)
{
        int   n;
        char *extra_env[18]; /* be sure to adjust this as needed when
                              * you add more variables to the callout's
                              * environment */

        n = 0;

        extra_env[n++] = g_strdup_printf ("CK_SEAT_ID=%s", seat->priv->id);

        /* Callout scripts/binaries should check if CK_SEAT_SESSION_ID
         * resp. CK_SEAT_OLD_SESSON_ID is set to figure out if there
         * will be an active session after the switch, or if there was
         * one before. At least one of those environment variables
         * will be set, possibly both. Only after checking these
         * variables the script should check for the other session
         * property variables. */

        env_add_session_info (old_session, "CK_SEAT_OLD_SESSION_", extra_env, &n);
        env_add_session_info (new_session, "CK_SEAT_SESSION_", extra_env, &n);

        extra_env[n++] = NULL;

        g_assert(n <= G_N_ELEMENTS(extra_env));

        ck_run_programs (SYSCONFDIR "/ConsoleKit/run-seat.d", action, extra_env);
        ck_run_programs (PREFIX "/lib/ConsoleKit/run-seat.d", action, extra_env);

        for (n = 0; extra_env[n] != NULL; n++) {
                g_free (extra_env[n]);
        }
}

static void
dump_seat_session_iter (char      *id,
                        CkSession *session,
                        GString   *str)
{
        char   *session_id;
        GError *error;

        error = NULL;
        if (! ck_session_get_id (session, &session_id, &error)) {
                g_warning ("Cannot get session id from seat: %s", error->message);
                g_error_free (error);
        } else {
                if (str->len > 0) {
                        g_string_append_c (str, ' ');
                }
                g_string_append (str, session_id);
                g_free (session_id);
        }
}

void
ck_seat_dump (CkSeat   *seat,
              GKeyFile *key_file)
{
        char    *group_name;
        GString *str;
        char    *s;
        int      n;

        group_name = g_strdup_printf ("Seat %s", seat->priv->id);

        g_key_file_set_integer (key_file, group_name, "kind", seat->priv->kind);

        str = g_string_new (NULL);
        g_hash_table_foreach (seat->priv->sessions, (GHFunc) dump_seat_session_iter, str);
        s = g_string_free (str, FALSE);
        g_key_file_set_string (key_file, group_name, "sessions", s);
        g_free (s);

        str = g_string_new (NULL);
        if (seat->priv->devices != NULL) {
                for (n = 0; n < seat->priv->devices->len; n++) {
                        int          m;
                        GValueArray *va;

                        va = seat->priv->devices->pdata[n];

                        if (str->len > 0)
                                g_string_append_c (str, ' ');
                        for (m = 0; m < va->n_values; m++) {
                                if (m > 0)
                                        g_string_append_c (str, ':');
                                g_string_append (str, g_value_get_string ((const GValue *) &((va->values)[m])));
                        }

                        g_debug ("foo %d", va->n_values);
                }
        }
        s = g_string_free (str, FALSE);
        g_key_file_set_string (key_file, group_name, "devices", s);
        g_free (s);


        if (seat->priv->active_session != NULL) {
                char   *session_id;
                GError *error;

                error = NULL;
                if (! ck_session_get_id (seat->priv->active_session, &session_id, &error)) {
                        if (error) {
                                g_warning ("Cannot get session id for active session on seat %s: %s",
                                           seat->priv->id,
                                           error->message);
                                g_error_free (error);
                        } else {
                                g_warning ("Cannot get session id for active session on seat %s",
                                           seat->priv->id);
                        }
                } else {
                        g_key_file_set_string (key_file,
                                               group_name,
                                               "active_session",
                                               NONULL_STRING (session_id));
                        g_free (session_id);
                }
        }

        g_free (group_name);
}

gboolean
ck_seat_is_managed (CkSeat *seat)
{
        return seat->priv->manager_proxy != NULL;
}

CkSession *
ck_seat_get_session (CkSeat                *seat,
                     const char            *ssid)
{
        CkSession *session;

        session = g_hash_table_lookup (seat->priv->sessions, ssid);

        if (session == NULL) {
                return NULL;
        }

        return g_object_ref (session);
}
