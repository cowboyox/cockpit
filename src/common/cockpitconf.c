/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "cockpitconf.h"

#include "cockpithash.h"

static GHashTable *cockpit_conf = NULL;
static GHashTable *cached_strvs = NULL;
const gchar *cockpit_config_file = "cockpit.conf";
const gchar *cockpit_config_dir = PACKAGE_SYSCONF_DIR "/cockpit/";

static gboolean
load_key_file (const gchar *file_path,
                GError **error)
{
  GKeyFile *key_file = NULL;
  GHashTable *section;
  GError *err = NULL;
  gchar **groups;
  gint i;
  gint j;

  key_file = g_key_file_new ();

  if (!g_key_file_load_from_file (key_file, file_path, G_KEY_FILE_NONE, error))
    {
      g_key_file_free (key_file);
      return FALSE;
    }

  groups = g_key_file_get_groups (key_file, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      gchar **keys;
      section = g_hash_table_lookup (cockpit_conf, groups[i]);
      if (section == NULL)
        {
          section = g_hash_table_new_full (cockpit_str_case_hash, cockpit_str_case_equal, g_free, g_free);
          g_hash_table_insert (cockpit_conf, g_strdup (groups[i]), section);
        }

      keys = g_key_file_get_keys (key_file, groups[i], NULL, &err);
      g_return_val_if_fail (err == NULL, FALSE);

      for (j = 0; keys[j] != NULL; j++)
        {
          gchar *value = g_key_file_get_value (key_file, groups[i], keys[j], &err);
          g_return_val_if_fail (err == NULL, FALSE);
          g_hash_table_insert (section, g_strdup (keys[j]), value);
        }
      g_strfreev (keys);
    }
  g_strfreev (groups);

  g_key_file_free (key_file);
  return TRUE;
}

void
cockpit_conf_init (void)
{
  GError *error = NULL;
  gchar *file = NULL;
  gchar *dir = NULL;

  cockpit_conf = g_hash_table_new_full (cockpit_str_case_hash, cockpit_str_case_equal, g_free,
                                        (GDestroyNotify)g_hash_table_unref);
  cached_strvs = g_hash_table_new_full (cockpit_str_case_hash, cockpit_str_case_equal, g_free,
                                        (GDestroyNotify)g_strfreev);


  if (cockpit_config_file)
    dir = g_path_get_dirname (cockpit_config_file);

  /* only add cockpit_config_dir if cockpit_config_file doesn't
   * already have a directory
   */
  if (cockpit_config_dir && g_strcmp0 (dir, ".") == 0)
    file = g_build_filename (cockpit_config_dir, cockpit_config_file, NULL);
  else if (cockpit_config_file)
    file = g_strdup (cockpit_config_file);

  if (file)
    {
      if (!load_key_file (file, &error))
        {
          if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            {
              g_message ("couldn't load configuration file: %s: %s",
                         file, error->message);
            }
          g_clear_error (&error);
        }

      g_debug ("Loaded configuration from: %s", file);
    }
  else
      g_debug ("No configuration to load");

  g_free (file);
  g_free (dir);
}


void
cockpit_conf_cleanup (void)
{
  if (cockpit_conf)
    {
      g_hash_table_destroy (cockpit_conf);
      cockpit_conf = NULL;
    }

  if (cached_strvs)
    {
      g_hash_table_destroy (cached_strvs);
      cached_strvs = NULL;
    }
}


static void
ensure_cockpit_conf (void)
{
  if (cockpit_conf == NULL)
    cockpit_conf_init ();
}


const gchar *
cockpit_conf_get_dir (void)
{
  return cockpit_config_dir;
}

const gchar *
cockpit_conf_string (const gchar *section,
                     const gchar *field)
{
  GHashTable *sect = NULL;
  ensure_cockpit_conf ();
  sect = g_hash_table_lookup (cockpit_conf, section);
  if (sect != NULL)
    return g_hash_table_lookup (sect, field);

  return NULL;
}

gboolean
cockpit_conf_bool (const gchar *section,
                   const gchar *field,
                   gboolean defawlt)
{
  GHashTable *sect = NULL;
  gchar *cmp = NULL;
  const gchar *value;
  gboolean ret = defawlt;

  ensure_cockpit_conf ();
  sect = g_hash_table_lookup (cockpit_conf, section);
  if (sect != NULL)
    {
      value = g_hash_table_lookup (sect, field);
      if (value)
        {
          cmp = g_ascii_strdown (value, -1);
          ret = g_strcmp0 (cmp, "yes") == 0 ||
                g_strcmp0 (cmp, "true") == 0 ||
                g_strcmp0 (cmp, "1") == 0;
        }
    }

  g_free (cmp);
  return ret;
}

static const gchar **
build_strv (const gchar *section,
            const gchar *field,
            gchar *delimiter)
{
  const gchar *string = NULL;
  const gchar **value = NULL;
  gchar *stripped = NULL;
  gchar **strv = NULL;

  string = cockpit_conf_string (section, field);

  if (string != NULL)
    {
      stripped = g_strstrip (g_strdup (string));
      strv = g_strsplit (stripped, delimiter, -1);
      if (strv)
        {
          guint n = g_strv_length (strv);
          guint i;
          value = g_new (const gchar *, n + 1);

          for (i = 0; i < n; i++)
            value[i] = g_strdup (strv[i]);

          value[i] = NULL;
          g_strfreev (strv);
        }
    }
  g_free (stripped);
  return value;
}

const gchar **
cockpit_conf_strv (const gchar *section,
                   const gchar *field,
                   gchar delimiter)
{
  const gchar **value = NULL;
  gchar *key = NULL;
  gchar delm[2] = {delimiter, '\0'};

  g_return_val_if_fail (section != NULL, NULL);
  g_return_val_if_fail (field != NULL, NULL);

  ensure_cockpit_conf ();
  key = g_strjoin (delm, section, field, NULL);

  if (!g_hash_table_contains (cached_strvs, key))
    {
      value = build_strv (section, field, delm);
      if (value)
        g_hash_table_insert (cached_strvs, g_strdup (key), value);
    }
  else
    {
      value = g_hash_table_lookup (cached_strvs, key);
    }

  g_free (key);
  return value;
}
