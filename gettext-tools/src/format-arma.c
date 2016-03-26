/* Arma (SQF and config files) format strings.
   Copyright (C) Andrew Kozlov <ctatuct@gmail.com>.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <stdlib.h>

#include "format.h"
#include "xalloc.h"
#include "format-invalid.h"
#include "c-ctype.h"
#include "xvasprintf.h"
#include "gettext.h"

#define _(str) gettext (str)

/* Arma format strings are used in `format' and `formatText' SQF scripting
   commands. They are documented on these community wiki pages respectively:
   https://community.bistudio.com/wiki/format
   and
   https://community.bistudio.com/wiki/formatText
   A directive
     - starts with '%',
     - is followed by number indicating which argument to use on this position.
   %% directive to put a percent char does not work. One should pass "%"
   string as format argument and reference it by its number, e.g.
       format ["146%1", "%"];
*/

struct spec
{
  /* Number of format directives.  */
  unsigned int directives;

  /* Booleans telling which %n was seen.  */
  unsigned int arg_count;
  bool args_used[8192];
};


static void *
format_parse (const char *format, bool translated, char *fdi,
              char **invalid_reason)
{
  const char *const format_start = format;
  struct spec ss_spec;
  struct spec *result = NULL;

  ss_spec.directives = 0;
  ss_spec.arg_count = 0;

  for (; *format != '\0';)
    if (*format++ == '%')
      {
        const char *dir_start = format - 1;

        if (c_isdigit(*format))
          {
            /* A directive.  */
            unsigned int number;

            FDI_SET (dir_start, FMTDIR_START);
            ss_spec.directives++;

            number = (*format++) & 0x0F;
            while (c_isdigit(*format) && (10 * number + ((*format) & 0x0F)) < 8192)
              {
                number = 10 * number + ((*format) & 0x0F);
                format++;
              }

            while (ss_spec.arg_count <= number)
              ss_spec.args_used[ss_spec.arg_count++] = false;
            ss_spec.args_used[number] = true;

            FDI_SET (format-1, FMTDIR_END);
          }
        else
          {
            if (*format == '\0')
              {
                *invalid_reason = INVALID_UNTERMINATED_DIRECTIVE ();
                FDI_SET (format - 1, FMTDIR_ERROR);
              }
            else
              {
                *invalid_reason =
                INVALID_CONVERSION_SPECIFIER (ss_spec.arg_count + 1,
                                              *format);
                FDI_SET (format, FMTDIR_ERROR);
              }
          }
      }

  result = XMALLOC (struct spec);
  *result = ss_spec;
  return result;
}

static void
format_free (void *descr)
{
  struct spec *spec = (struct spec *) descr;

  free (spec);
}

static int
format_get_number_of_directives (void *descr)
{
  struct spec *spec = (struct spec *) descr;

  return spec->directives;
}

static bool
format_check (void *msgid_descr, void *msgstr_descr, bool equality,
              formatstring_error_logger_t error_logger,
              const char *pretty_msgid, const char *pretty_msgstr)
{
  struct spec *spec1 = (struct spec *) msgid_descr;
  struct spec *spec2 = (struct spec *) msgstr_descr;
  bool err = false;
  unsigned int i;

  for (i = 0; i < spec1->arg_count || i < spec2->arg_count; i++)
    {
      bool arg_used1 = (i < spec1->arg_count && spec1->args_used[i]);
      bool arg_used2 = (i < spec2->arg_count && spec2->args_used[i]);

      /* The translator cannot omit a %n from the msgstr because that would
         yield a "Argument missing" warning at runtime.  */
      if (arg_used1 != arg_used2)
        {
          if (error_logger)
            {
              if (arg_used1)
                error_logger (_("a format specification for argument %u doesn't exist in '%s'"),
                              i, pretty_msgstr);
              else
                error_logger (_("a format specification for argument %u, as in '%s', doesn't exist in '%s'"),
                              i, pretty_msgstr, pretty_msgid);
            }
          err = true;
          break;
        }
    }

  return err;
}

struct formatstring_parser formatstring_arma =
{
  format_parse,
  format_free,
  format_get_number_of_directives,
  NULL,
  format_check
};
