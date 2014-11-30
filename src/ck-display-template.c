/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Authors: halton.huo@sun.com, Ray Strode <rstrode@redhat.com>
 * Copyright (C) 2009 Sun Microsystems, Inc.
 *                    Red Hat, Inc.
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

#include <string.h>
#include <glib.h>
#include <glib-object.h>

#include "ck-display-template.h"

#define CK_DISPLAY_TEMPLATE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CK_TYPE_DISPLAY_TEMPLATE, CkDisplayTemplatePrivate))

#define CK_DISPLAY_TEMPLATES_DIR     SYSCONFDIR "/ConsoleKit/displays.d"

struct CkDisplayTemplatePrivate
{
        char            *name;
        char            *type;
        GHashTable      *parameters;
};

enum {
        PROP_0,
        PROP_NAME,
        PROP_TYPE,
        PROP_PARAMETERS,
};

static void     ck_display_template_class_init  (CkDisplayTemplateClass *klass);
static void     ck_display_template_init        (CkDisplayTemplate      *display);
static void     ck_display_template_finalize    (GObject            *object);
static gboolean ck_display_template_load        (CkDisplayTemplate      *display);

static GHashTable *ck_display_templates;

G_DEFINE_TYPE (CkDisplayTemplate, ck_display_template, G_TYPE_OBJECT)

static void
_ck_display_template_set_name (CkDisplayTemplate  *display,
                               const char     *name)
{
        g_free (display->priv->name);
        display->priv->name = g_strdup (name);
}

static void
_ck_display_template_set_type_string (CkDisplayTemplate  *display,
                                      const char     *type)
{
        g_free (display->priv->type);
        display->priv->type = g_strdup (type);
}

static void
_ck_display_template_set_parameters (CkDisplayTemplate  *display,
                                     GHashTable     *parameters)
{
        if (display->priv->parameters != NULL) {
                g_hash_table_unref (display->priv->parameters);
        }

        if (parameters == NULL) {
                display->priv->parameters = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                                   (GDestroyNotify) g_free,
                                                                   (GDestroyNotify) g_free);
        } else {
                display->priv->parameters = g_hash_table_ref (parameters);
        }
}

static void
ck_display_template_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
        CkDisplayTemplate *self;

        self = CK_DISPLAY_TEMPLATE (object);

        switch (prop_id) {
        case PROP_NAME:
                _ck_display_template_set_name (self, g_value_get_string (value));
                break;
        case PROP_TYPE:
                _ck_display_template_set_type_string (self, g_value_get_string (value));
                break;
        case PROP_PARAMETERS:
                _ck_display_template_set_parameters (self, g_value_get_boxed (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
ck_display_template_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
        CkDisplayTemplate *self;

        self = CK_DISPLAY_TEMPLATE (object);

        switch (prop_id) {
        case PROP_NAME:
                g_value_set_string (value, self->priv->name);
                break;
        case PROP_TYPE:
                g_value_set_string (value, self->priv->type);
                break;
        case PROP_PARAMETERS:
                g_value_set_boxed (value, self->priv->parameters);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
ck_display_template_class_init (CkDisplayTemplateClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->get_property = ck_display_template_get_property;
        object_class->set_property = ck_display_template_set_property;
        object_class->finalize = ck_display_template_finalize;

        g_object_class_install_property (object_class,
                                         PROP_NAME,
                                         g_param_spec_string ("name",
                                                              "display type name",
                                                              "display type name",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_TYPE,
                                         g_param_spec_string ("type",
                                                              "type",
                                                              "Type",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_PARAMETERS,
                                         g_param_spec_boxed ("parameters",
                                                              "Parameters",
                                                              "Parameters",
                                                              G_TYPE_HASH_TABLE,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_type_class_add_private (klass, sizeof (CkDisplayTemplatePrivate));
}

static void
ck_display_template_init (CkDisplayTemplate *display)
{
        display->priv = CK_DISPLAY_TEMPLATE_GET_PRIVATE (display);

        display->priv->name = NULL;
        display->priv->type = NULL;
        display->priv->parameters = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                           (GDestroyNotify) g_free,
                                                           (GDestroyNotify) g_free);
}

static void
ck_display_template_finalize (GObject *object)
{
        CkDisplayTemplate *display;

        g_return_if_fail (object != NULL);
        g_return_if_fail (CK_IS_DISPLAY_TEMPLATE (object));

        display = CK_DISPLAY_TEMPLATE (object);

        g_return_if_fail (display->priv != NULL);

        g_free (display->priv->name);
        g_free (display->priv->type);
        g_hash_table_unref (display->priv->parameters);

        G_OBJECT_CLASS (ck_display_template_parent_class)->finalize (object);
}

static gboolean
ck_display_template_load (CkDisplayTemplate *display)
{
        GKeyFile   *key_file;
        const char *name;
        char       *group;
        char       *filename;
        gboolean    hidden;
        char       *type;
        gboolean    res;
        GError     *error;
        char      **type_keys;
        GHashTable *parameters;

        name = ck_display_template_get_name (display);

        g_return_val_if_fail (name && !g_str_equal (name, ""), FALSE);

        filename = g_strdup_printf ("%s/%s.display", CK_DISPLAY_TEMPLATES_DIR, name);

        key_file = g_key_file_new ();

        error = NULL;
        res = g_key_file_load_from_file (key_file,
                                         filename,
                                         G_KEY_FILE_NONE,
                                         &error);
        if (! res) {
                g_warning ("Unable to load display from file %s: %s", filename, error->message);
                g_error_free (error);
                return FALSE;
        }
        g_free (filename);

        group = g_key_file_get_start_group (key_file);

        if (group == NULL || strcmp (group, "Display") != 0) {
                g_warning ("Not a display type file: %s", filename);
                g_free (group);
                g_key_file_free (key_file);
                return FALSE;
        }

        hidden = g_key_file_get_boolean (key_file, group, "Hidden", NULL);

        type = g_key_file_get_string (key_file, group, "Type", NULL);

        if (type == NULL) {
                g_warning ("Unable to read type from display file");
                g_free (group);
                g_key_file_free (key_file);
                return FALSE;
        }

        display->priv->type = type;

        parameters = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            (GDestroyNotify) g_free,
                                            (GDestroyNotify) g_free);

        type_keys = g_key_file_get_keys (key_file, type, NULL, NULL);

        if (type_keys != NULL) {
                int        i;
                for (i = 0; type_keys[i] != NULL; i++) {
                        char   *string;

                        string = g_key_file_get_string (key_file, type, type_keys[i], NULL);
                        g_hash_table_insert (parameters, g_strdup (type_keys[i]), string);
                }
                g_strfreev (type_keys);
        }

        _ck_display_template_set_parameters (display, parameters);
        g_hash_table_unref (parameters);

        g_free (group);
        g_key_file_free (key_file);
        return TRUE;
}

CkDisplayTemplate *
ck_display_template_get_from_name (const char *name)
{
        CkDisplayTemplate *display_template;

        if (ck_display_templates == NULL) {
                ck_display_templates = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                          (GDestroyNotify) g_free,
                                                          (GDestroyNotify) g_object_unref);
        }

        display_template = g_hash_table_lookup (ck_display_templates, name);

        if (display_template == NULL) {
                GObject *object;

                object = g_object_new (CK_TYPE_DISPLAY_TEMPLATE,
                                       "name", name,
                                       NULL);

                if (!ck_display_template_load (CK_DISPLAY_TEMPLATE (object))) {
                        g_object_unref (object);
                        return NULL;
                }

                g_hash_table_insert (ck_display_templates, g_strdup (name), object);
                display_template = CK_DISPLAY_TEMPLATE (object);
        }

        return g_object_ref (display_template);
}

G_CONST_RETURN char*
ck_display_template_get_name (CkDisplayTemplate   *display)
{
        g_return_val_if_fail (CK_IS_DISPLAY_TEMPLATE (display), NULL);

        return display->priv->name;
}

G_CONST_RETURN char *
ck_display_template_get_type_string (CkDisplayTemplate  *display)
{
        return display->priv->type;
}

GHashTable *
ck_display_template_get_parameters (CkDisplayTemplate   *display)
{
        g_return_val_if_fail (CK_IS_DISPLAY_TEMPLATE (display), NULL);

        return g_hash_table_ref (display->priv->parameters);
}
