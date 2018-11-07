/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2016 Red Hat, Inc.
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

#include "cockpitsshknownhosts.h"

#include <glib/gstdio.h>
#include <glib.h>
#include <glib-object.h>

#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <stdio.h>

static gchar *knownhosts_file = NULL;

void shim_set_knownhosts_file (const gchar *file)
{
  g_free (knownhosts_file);
  knownhosts_file = g_strdup (file);
}

gboolean ssh_session_has_known_hosts_entry (ssh_session session)
{
  gboolean result = FALSE;
  char *host = NULL;
  guint port = 0;
  g_warn_if_fail (ssh_options_get (session, SSH_OPTIONS_HOST, &host) == SSH_OK);
  g_warn_if_fail (ssh_options_get_port (session, &port) == SSH_OK);
  if (knownhosts_file)
    {
      result = cockpit_is_host_known (knownhosts_file, host, port);
      g_clear_pointer(&knownhosts_file, g_free);
    }
  if (host)
    ssh_string_free_char (host);
  return result;
}

/*
 * HACK: SELinux prevents us from writing to the directories we want to
 * write to, so we have to try multiple locations.
 *
 * https://bugzilla.redhat.com/show_bug.cgi?id=1279430
 */
static gchar *
create_knownhosts_temp (void)
{
  const gchar *directories[] = {
      "/tmp",
      PACKAGE_LOCALSTATE_DIR,
      NULL,
  };

  gchar *name;
  int i, fd, err;

  for (i = 0; directories[i] != NULL; i++)
    {
      name = g_build_filename (directories[i], "known-hosts.XXXXXX", NULL);
      fd = g_mkstemp (name);
      err = errno;

      if (fd >= 0)
        {
          close (fd);
          return name;
        }
      g_free (name);

      if ((err == ENOENT || err == EPERM || err == EACCES) && directories[i + 1] != NULL)
        continue;

      g_warning ("couldn't make temporary file for knownhosts line in %s: %m", directories[i]);
      break;
    }

  return NULL;
}

/*
 * NOTE: This function changes the SSH_OPTIONS_KNOWNHOSTS option on
 * the session.
 *
 * We can't save and restore it since ssh_options_get doesn't allow us
 * to retrieve the old value of SSH_OPTIONS_KNOWNHOSTS.
 *
 * HACK: This function should be provided by libssh.
 *
 * https://red.libssh.org/issues/162
*/
static gchar *
get_knownhosts_line (ssh_session session)
{

  gchar *name = NULL;
  GError *error = NULL;
  gchar *line = NULL;

  name = create_knownhosts_temp ();
  if (!name)
    goto out;

  if (ssh_options_set (session, SSH_OPTIONS_KNOWNHOSTS, name) != SSH_OK)
    {
      g_warning ("Couldn't set SSH_OPTIONS_KNOWNHOSTS option.");
      goto out;
    }

  if (ssh_write_knownhost (session) != SSH_OK)
    {
      g_warning ("Couldn't write knownhosts file: %s", ssh_get_error (session));
      goto out;
    }

  if (!g_file_get_contents (name, &line, NULL, &error))
    {
      g_warning ("Couldn't read temporary known_hosts %s: %s", name, error->message);
      g_clear_error (&error);
      goto out;
    }

out:
  if (name)
    {
      g_unlink (name);
      g_free (name);
    }

  return line;
}

int ssh_session_export_known_hosts_entry (ssh_session session,
                                          char **pentry_string)
{
  gchar *line = get_knownhosts_line (session);
  if (line == NULL)
    return SSH_ERROR;
  *pentry_string = g_strdup (line);
  g_free (line);
  return SSH_OK;
}

/* HACK: The following is a hack around the fact that libssh doesn't provide any
 * API to check for the presence of a known host key without actually connecting to
 * a remote server. We want to gate our outgoing connections based on the contents
 * of known hosts so here's a implementation of that until we can get similar functionality
 * into libssh.
 * SEE: https://red.libssh.org/issues/209
 */

/* match_pattern and match_pattern_list are copied from libssh/match.c */
/*
 * Copyright (c) 2000 Markus Friedl.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


/*
 * Returns true if the given string matches the pattern (which may contain ?
 * and * as wildcards), and zero if it does not match.
 */
static int
match_pattern (const char *s,
               const char *pattern)
{
  if (s == NULL || pattern == NULL)
    return 0;

  for (;;)
    {
      /* If at end of pattern, accept if also at end of string. */
      if (*pattern == '\0')
        return (*s == '\0');

      if (*pattern == '*')
        {
          /* Skip the asterisk. */
          pattern++;

          /* If at end of pattern, accept immediately. */
          if (!*pattern)
            return 1;

          /* If next character in pattern is known, optimize. */
          if (*pattern != '?' && *pattern != '*')
            {
              /*
               * Look instances of the next character in
               * pattern, and try to match starting from
               * those.
               */
              for (; *s; s++)
                {
                  if (*s == *pattern && match_pattern (s + 1, pattern + 1))
                    return 1;
                }
              /* Failed. */
              return 0;
            }

          /*
           * Move ahead one character at a time and try to
           * match at each position.
           */
          for (; *s; s++)
            {
              if (match_pattern (s, pattern))
                return 1;
            }

          /* Failed. */
          return 0;
        }

      /*
       * There must be at least one more character in the string.
       * If we are at the end, fail.
       */
      if (!*s)
        return 0;

      /* Check if the next character of the string is acceptable. */
      if (*pattern != '?' && *pattern != *s)
        return 0;

      /* Move to the next character, both in string and in pattern. */
      s++;
      pattern++;
    }
  /* NOTREACHED */
}

/*
 * Tries to match the string against the comma-separated sequence of subpatterns
 * (each possibly preceded by ! to indicate negation).
 * Returns -1 if negation matches, 1 if there is a positive match, 0 if there is
 * no match at all.
 */
static
int match_pattern_list (const char *string,
                        const char *pattern,
                        unsigned int len,
                        int dolower)
{
  char sub[1024];
  int negated;
  int got_positive;
  unsigned int i, subi;

  got_positive = 0;
  for (i = 0; i < len;)
    {
      /* Check if the subpattern is negated. */
      if (pattern[i] == '!')
        {
          negated = 1;
          i++;
        }
      else
        {
          negated = 0;
        }

      /*
       * Extract the subpattern up to a comma or end.  Convert the
       * subpattern to lowercase.
       */
      for (subi = 0; i < len && subi < sizeof(sub) - 1 && pattern[i] != ','; subi++, i++)
        {
          sub[subi] = dolower && isupper (pattern[i]) ?
                      (char)tolower(pattern[i]) : pattern[i];
        }

      /* If subpattern too long, return failure (no match). */
      if (subi >= sizeof(sub) - 1)
        return 0;

      /* If the subpattern was terminated by a comma, skip the comma. */
      if (i < len && pattern[i] == ',')
        i++;

      /* Null-terminate the subpattern. */
      sub[subi] = '\0';

      /* Try to match the subpattern against the string. */
      if (match_pattern (string, sub))
        {
          if (negated)
            return -1;        /* Negative */
          else
            got_positive = 1; /* Positive */
        }
    }

  /*
   * Return success if got a positive match.  If there was a negative
   * match, we have already returned -1 and never get here.
   */
  return got_positive;
}

/* Matches against a openssh hash:
 * |1|base64 encoded salt|base64 encoded hash
 * hash := HMAC_SHA1(key=salt,data=host)
 */
static gboolean
matches_hashed (const gchar *line,
                const gchar *host)
{
  GHmac *hmac = NULL;
  gchar *copied = NULL;
  gchar *marker;
  gsize salt_length;
  gsize hash_length;
  gsize generated_length = 20; // SHA1 length
  guchar generated_hash[generated_length];
  gboolean ret = FALSE;

  if (strncmp (line, "|1|", 3) != 0)
    goto out;

  copied = g_strdup (line + 3);
  marker = strchr(copied, '|');
  if (!marker)
    goto out;

  // Null to seperate salt and hash
  *marker = '\0';
  // Set marker to hash start
  marker++;

  if (!g_base64_decode_inplace (copied, &salt_length) || salt_length < 1)
    goto out;

  if (!g_base64_decode_inplace (marker, &hash_length) || hash_length < 1)
    goto out;

  // Generate the sha1 hmac
  hmac = g_hmac_new (G_CHECKSUM_SHA1, (guchar *)copied, salt_length);
  g_hmac_update (hmac, (guchar *)host, strlen (host));
  g_hmac_get_digest (hmac, generated_hash, &generated_length);

  if (generated_length == hash_length && memcmp (generated_hash, marker, hash_length) == 0)
    ret = TRUE;

out:
  if (hmac)
    g_hmac_unref (hmac);
  g_free (copied);
  return ret;
}

gboolean
cockpit_is_host_known (const gchar *known_hosts_file,
                       const gchar *host,
                       guint port)
{
  gchar buffer[4096] = {0};
  gchar *ptr;
  gchar *hostport = NULL;

  if (!known_hosts_file)
    return FALSE;

  FILE *file = g_fopen (known_hosts_file, "r");
  gboolean ret = FALSE;

  if (!file)
    {
      if (errno == ENOENT)
        g_debug ("known hosts file %s does not exist", known_hosts_file);
      else
        g_message ("failed to open known hosts file %s: %m", known_hosts_file);
      return FALSE;
    }

  hostport = g_strdup_printf ("[%s]:%d", host, port);
  while (fgets (buffer, sizeof (buffer), file))
    {
      gchar **tokens = NULL;

      ptr = strchr (buffer, '\n');
      if (ptr)
        *ptr =  '\0';

      ptr = strchr (buffer,'\r');
      if (ptr)
        *ptr = '\0';

      if (buffer[0] == '\0' || buffer[0] == '#')
        continue; /* skip empty lines */

      tokens = g_strsplit (buffer, " ", -1);

      /* it should have 3 or 4 tokens, we aren't strict since all
       * we care about is the host */
      if (g_strv_length (tokens) == 3 || g_strv_length (tokens) == 4)
        {
          ret = matches_hashed (tokens[0], hostport);
          if (!ret)
            ret = match_pattern_list (hostport, tokens[0], strlen(tokens[0]), 1) == 1;
          if (!ret)
            ret = matches_hashed (tokens[0], host);
          if (!ret)
            ret = match_pattern_list (host, tokens[0], strlen(tokens[0]), 1) == 1;
        }

      g_strfreev (tokens);

      if (ret)
        break;
  }

  if (file)
    fclose(file);
  file = NULL;

  g_free (hostport);
  /* we did not find anything, end of file*/
  return ret;
}

enum ssh_known_hosts_e
ssh_session_is_known_server (ssh_session session)
{
    enum ssh_server_known_e result;

    result = ssh_is_server_known (session);

    switch(result)
      {
          case SSH_SERVER_ERROR: return SSH_KNOWN_HOSTS_ERROR;
          case SSH_SERVER_FILE_NOT_FOUND: return SSH_KNOWN_HOSTS_NOT_FOUND;
          case SSH_SERVER_NOT_KNOWN: return SSH_KNOWN_HOSTS_UNKNOWN;
          case SSH_SERVER_KNOWN_OK: return SSH_KNOWN_HOSTS_OK;
          case SSH_SERVER_KNOWN_CHANGED: return SSH_KNOWN_HOSTS_CHANGED;
          case SSH_SERVER_FOUND_OTHER: return SSH_KNOWN_HOSTS_OTHER;
      }

    g_assert_not_reached ();
}
