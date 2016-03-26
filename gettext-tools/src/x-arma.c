/* xgettext Arma (SQF and config files) backend.
   Copyright (C) 2016 Andrew Kozlov <ctatuct@gmail.com>.

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
#include "config.h"
#endif

/* Specification.  */
#include "x-arma.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "message.h"
#include "xgettext.h"
#include "error.h"
#include "xalloc.h"
#include "gettext.h"
#include "po-charset.h"

#define _(s) gettext(s)

#define SIZEOF(a) (sizeof(a) / sizeof(a[0]))

/* Basic info on the SQF syntax can be found in its community wiki
   page: https://community.bistudio.com/wiki/SQF_syntax
   Syntax of config files is somewhat similar to C++ files */

/* This SQF/Config syntax parser is based on xgettext's C parser and
   defines following phases of translation:

   1. Terminate line by \n, regardless of the external representation
      of a text line.  Stdio does this for us.

// Arma files don't use trigraphs
// 2. Convert trigraphs to their single character equivalents.

   3. Concatenate each line ending in backslash (\) with the following
      line.

   4. Replace each comment with a space character.

   5. Parse each resulting logical line as preprocessing tokens a
      white space.

   6. Recognize and carry out directives (it also expands macros on
      non-directive lines, which we do not do here).

// Arma files don't use escape sequences
// 7. Replaces escape sequences within character strings with their
//    single character equivalents (we do this in step 5, because we
//    don't have to worry about the #include argument).

   8. Concatenates adjacent string literals to form single string
      literals (because we don't expand macros, there are a few things
      we will miss).
      We only drop whitespace tokens in this step.

   9. Converts the remaining preprocessing tokens to C tokens and
      discards any white space from the translation unit.

   This lexer implements the above, and presents the scanner (in
   xgettext.c) with a stream of C tokens.  The comments are
   accumulated in a buffer, and given to xgettext when asked for.  */


/* ====================== Keyword set customization.  ====================== */

/* If true extract all strings.  */
static bool extract_all = false;

static hash_table arma_keywords;
static bool default_keywords = true;


void
x_arma_extract_all ()
{
  extract_all = true;
}


static void
add_keyword (const char *name, hash_table *keywords)
{
  if (name == NULL)
    default_keywords = false;
  else
    {
      const char *end;
      struct callshape shape;
      const char *colon;

      if (keywords->table == NULL)
        hash_init (keywords, 100);

      split_keywordspec (name, &end, &shape);

      /* The characters between name and end should form a valid C identifier.
         A colon means an invalid parse in split_keywordspec().  */
      colon = strchr (name, ':');
      if (colon == NULL || colon >= end)
        insert_keyword_callshape (keywords, name, end - name, &shape);
    }
}

void
x_arma_keyword (const char *name)
{
  add_keyword (name, &arma_keywords);
}

/* Finish initializing the keywords hash table.
   Called after argument processing, before each file is processed.  */
static void
init_keywords ()
{
  if (default_keywords)
    {
      /* When adding new keywords here, also update the documentation in
         xgettext.texi!  */
      x_arma_keyword ("localize");
      default_keywords = false;
    }
}

void
init_flag_table_arma ()
{
  xgettext_record_flag ("localize:1:pass-arma-format");
  xgettext_record_flag ("format:1:arma-format");
  xgettext_record_flag ("formatText:1:arma-format");
}

/* ======================== Reading of characters.  ======================== */

/* Real filename, used in error messages about the input file.  */
static const char *real_file_name;

/* Logical filename and line number, used to label the extracted messages.  */
static char *logical_file_name;
static int line_number;

/* The input file stream.  */
static FILE *fp;


/* 0. Terminate line by \n, regardless whether the external representation of
   a line terminator is LF (Unix), CR (Mac) or CR/LF (DOS/Windows).
   The so-called "text mode" in stdio on DOS/Windows translates CR/LF to \n
   automatically, but here we also need this conversion on Unix.  As a side
   effect, on DOS/Windows we also parse CR/CR/LF into a single \n, but this
   is not a problem.  */


static int
phase0_getc ()
{
  int c;

  c = getc (fp);
  if (c == EOF)
    {
      if (ferror (fp))
        error (EXIT_FAILURE, errno, _("error while reading \"%s\""),
               real_file_name);
      return EOF;
    }

  if (c == '\r')
    {
      int c1 = getc (fp);

      if (c1 != EOF && c1 != '\n')
        ungetc (c1, fp);

      /* Seen line terminator CR or CR/LF.  */
      return '\n';
    }

  return c;
}


/* Supports only one pushback character, and not '\n'.  */
static inline void
phase0_ungetc (int c)
{
  if (c != EOF)
    ungetc (c, fp);
}


/* 1. line_number handling.  Combine backslash-newline to nothing.  */

static unsigned char phase1_pushback[2];
static int phase1_pushback_length;


static int
phase1_getc ()
{
  int c;

  if (phase1_pushback_length)
    {
      c = phase1_pushback[--phase1_pushback_length];
      if (c == '\n')
        ++line_number;
      return c;
    }
  for (;;)
    {
      c = phase0_getc ();
      switch (c)
        {
        case '\n':
          ++line_number;
          return '\n';

        case '\\':
          c = phase0_getc ();
          if (c != '\n')
            {
              phase0_ungetc (c);
              return '\\';
            }
          ++line_number;
          break;

        default:
          return c;
        }
    }
}


/* Supports 2 characters of pushback.  */
static void
phase1_ungetc (int c)
{
  switch (c)
    {
    case EOF:
      break;

    case '\n':
      --line_number;
      /* FALLTHROUGH */

    default:
      if (phase1_pushback_length == SIZEOF (phase1_pushback))
        abort ();
      phase1_pushback[phase1_pushback_length++] = c;
      break;
    }
}


/* 2. Convert trigraphs to their single character equivalents.  Most
   sane human beings vomit copiously at the mention of trigraphs, which
   is why they are an option.  */

static unsigned char phase2_pushback[1];
static int phase2_pushback_length;


static int
phase2_getc ()
{
  int c;

  if (phase2_pushback_length)
    return phase2_pushback[--phase2_pushback_length];

  return phase1_getc ();
}


/* Supports only one pushback character.  */
static void
phase2_ungetc (int c)
{
  if (c != EOF)
    {
      if (phase2_pushback_length == SIZEOF (phase2_pushback))
        abort ();
      phase2_pushback[phase2_pushback_length++] = c;
    }
}


/* 3. Concatenate each line ending in backslash (\) with the following
   line.  Basically, all you need to do is elide "\\\n" sequences from
   the input.  */

static unsigned char phase3_pushback[2];
static int phase3_pushback_length = 0;


static int
phase3_getc ()
{
  if (phase3_pushback_length)
    return phase3_pushback[--phase3_pushback_length];
  for (;;)
    {
      int c = phase2_getc ();
      if (c != '\\')
        return c;
      c = phase2_getc ();
      if (c != '\n')
        {
          phase2_ungetc (c);
          return '\\';
        }
    }
}


/* Supports 2 characters of pushback.  */
static void
phase3_ungetc (int c)
{
  if (c != EOF)
    {
      if (phase3_pushback_length == SIZEOF (phase3_pushback))
        abort ();
      phase3_pushback[phase3_pushback_length++] = c;
    }
}


/* Accumulating comments.  */

static char *buffer;
static size_t bufmax;
static size_t buflen;

static inline void
comment_start ()
{
  buflen = 0;
}

static inline void
comment_add (int c)
{
  if (buflen >= bufmax)
    {
      bufmax = 2 * bufmax + 10;
      buffer = xrealloc (buffer, bufmax);
    }
  buffer[buflen++] = c;
}

static inline void
comment_line_end (size_t chars_to_remove)
{
  buflen -= chars_to_remove;
  while (buflen >= 1
         && (buffer[buflen - 1] == ' ' || buffer[buflen - 1] == '\t'))
    --buflen;
  if (chars_to_remove == 0 && buflen >= bufmax)
    {
      bufmax = 2 * bufmax + 10;
      buffer = xrealloc (buffer, bufmax);
    }
  buffer[buflen] = '\0';
  savable_comment_add (buffer);
}


/* These are for tracking whether comments count as immediately before
   keyword.  */
static int last_comment_line;
static int last_non_comment_line;
static int newline_count;


/* 4. Replace each comment that is not inside a character constant or
   string literal with a space character.  We need to remember the
   comment for later, because it may be attached to a keyword string.
   We also optionally understand C++ comments.  */

static int
phase4_getc ()
{
  int c;
  bool last_was_star;

  c = phase3_getc ();
  if (c != '/')
    return c;
  c = phase3_getc ();
  switch (c)
    {
    default:
      phase3_ungetc (c);
      return '/';

    case '*':
      /* C comment.  */
      comment_start ();
      last_was_star = false;
      for (;;)
        {
          c = phase3_getc ();
          if (c == EOF)
            break;
          /* We skip all leading white space, but not EOLs.  */
          if (!(buflen == 0 && (c == ' ' || c == '\t')))
            comment_add (c);
          switch (c)
            {
            case '\n':
              comment_line_end (1);
              comment_start ();
              last_was_star = false;
              continue;

            case '*':
              last_was_star = true;
              continue;

            case '/':
              if (last_was_star)
                {
                  comment_line_end (2);
                  break;
                }
              /* FALLTHROUGH */

            default:
              last_was_star = false;
              continue;
            }
          break;
        }
      last_comment_line = newline_count;
      return ' ';

    case '/':
      /* C++ or ISO C 99 comment.  */
      comment_start ();
      for (;;)
        {
          c = phase3_getc ();
          if (c == '\n' || c == EOF)
            break;
          /* We skip all leading white space, but not EOLs.  */
          if (!(buflen == 0 && (c == ' ' || c == '\t')))
            comment_add (c);
        }
      comment_line_end (0);
      last_comment_line = newline_count;
      return '\n';
    }
}


/* Supports only one pushback character.  */
static void
phase4_ungetc (int c)
{
  phase3_ungetc (c);
}


/* ========================== Reading of tokens.  ========================== */

enum token_type_ty
{
  token_type_eof,
  token_type_eoln,
  token_type_eoln_explicit,             /* \n (not within string literals) */
  token_type_hash,                      /* # */
  token_type_lparen,                    /* ( */
  token_type_rparen,                    /* ) */
  token_type_lsqbr,                     /* [ */
  token_type_rsqbr,                     /* [ */
  token_type_comma,                     /* , */
  token_type_colon,                     /* : */
  token_type_name,                      /* abc */
  token_type_number,                    /* 2.7 */
  token_type_string_literal,            /* "abc", 'abc' */
  token_type_dollar_literal,            /* $STR_myTag_strName */
  token_type_symbol,                    /* < > = etc. */
  token_type_white_space
};
typedef enum token_type_ty token_type_ty;

typedef struct token_ty token_ty;
struct token_ty
{
  token_type_ty type;
  char *string;   /* for token_type_name, token_type_string_literal,
                     token_type_dollar_literal and token_type_eoln_explicit */
  refcounted_string_list_ty *comment;    /* for token_type_string_literal and
                                            token_type_dollar_literal */
  enum literalstring_escape_type escape; /* for token_type_string_literal */
  long number;
  int line_number;
};


/* Free the memory pointed to by a 'struct token_ty'.  */
static inline void
free_token (token_ty *tp)
{
  if (tp->type == token_type_name || tp->type == token_type_string_literal ||
      tp->type == token_type_dollar_literal || tp->type == token_type_eoln_explicit)
    free (tp->string);
  if (tp->type == token_type_string_literal || tp->type == token_type_dollar_literal) {
    drop_reference (tp->comment);
  }
}


static char *
literalstring_parse (const char *string, lex_pos_ty *pos,
                     // type is always assumed LET_NONE
                     enum literalstring_escape_type type)
{
  struct mixed_string_buffer *bp;
  const char *p;

  /* Start accumulating the string.  */
  bp = mixed_string_buffer_alloc (lc_string,
                                  logical_file_name,
                                  line_number);

  for (p = string; ; )
    {
      int c = *p++;
      if (c == '\0')
        break;

      mixed_string_buffer_append_char (bp, c);
    }

  return mixed_string_buffer_done (bp);
}

struct literalstring_parser literalstring_arma =
  {
    literalstring_parse
  };


/* 5. Parse each resulting logical line as preprocessing tokens and
   white space.  Preprocessing tokens and C tokens don't always match.  */

static token_ty phase5_pushback[1];
static int phase5_pushback_length;


static void
phase5_get (token_ty *tp)
{
  static char *buffer;
  static int bufmax;
  int bufpos;
  int c;
  int cquot;

  if (phase5_pushback_length)
    {
      *tp = phase5_pushback[--phase5_pushback_length];
      return;
    }
  tp->string = NULL;
  tp->number = 0;
  tp->line_number = line_number;
  c = phase4_getc ();
  switch (c)
    {
    case EOF:
      tp->type = token_type_eof;
      return;

    case '\n':
      tp->type = token_type_eoln;
      return;

    case ' ':
    case '\f':
    case '\t':
      for (;;)
        {
          c = phase4_getc ();
          switch (c)
            {
            case ' ':
            case '\f':
            case '\t':
              continue;

            default:
              phase4_ungetc (c);
              break;
            }
          break;
        }
      tp->type = token_type_white_space;
      return;

    case 'A': case 'B': case 'C': case 'D': case 'E': case 'F': case 'G':
    case 'H': case 'I': case 'J': case 'K': case 'L': case 'M': case 'N':
    case 'O': case 'P': case 'Q': case 'R': case 'S': case 'T': case 'U':
    case 'V': case 'W': case 'X': case 'Y': case 'Z':
    case '_':
    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g':
    case 'h': case 'i': case 'j': case 'k': case 'l': case 'm': case 'n':
    case 'o': case 'p': case 'q': case 'r': case 's': case 't': case 'u':
    case 'v': case 'w': case 'x': case 'y': case 'z':
      bufpos = 0;
      for (;;)
        {
          if (bufpos >= bufmax)
            {
              bufmax = 2 * bufmax + 10;
              buffer = xrealloc (buffer, bufmax);
            }
          buffer[bufpos++] = c;
          c = phase4_getc ();
          switch (c)
            {
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
            case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
            case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
            case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
            case 'Y': case 'Z':
            case '_':
            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
            case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
            case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
            case 's': case 't': case 'u': case 'v': case 'w': case 'x':
            case 'y': case 'z':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
              continue;

            default:
              phase4_ungetc (c);
              break;
            }
          break;
        }
      if (bufpos >= bufmax)
        {
          bufmax = 2 * bufmax + 10;
          buffer = xrealloc (buffer, bufmax);
        }
      buffer[bufpos] = 0;
      tp->string = xstrdup (buffer);
      tp->type = token_type_name;
      return;

    case '$':
      bufpos = 0;
      c = phase4_getc ();
      switch (c)
        {
          case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
          case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
          case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
          case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
          case 'Y': case 'Z':
          case '_':
          case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
          case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
          case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
          case 's': case 't': case 'u': case 'v': case 'w': case 'x':
          case 'y': case 'z': // not letting digit as first char in literal
            if (bufpos >= bufmax)
              {
                bufmax = 2 * bufmax + 10;
                buffer = xrealloc (buffer, bufmax);
              }
            buffer[bufpos++] = c;
            for (;;)
              {
                c = phase4_getc ();
                switch (c)
                  {
                    case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
                    case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
                    case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
                    case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
                    case 'Y': case 'Z':
                    case '_':
                    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
                    case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
                    case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
                    case 's': case 't': case 'u': case 'v': case 'w': case 'x':
                    case 'y': case 'z':
                    case '0': case '1': case '2': case '3': case '4':
                    case '5': case '6': case '7': case '8': case '9':
                      if (bufpos >= bufmax)
                        {
                          bufmax = 2 * bufmax + 10;
                          buffer = xrealloc (buffer, bufmax);
                        }
                      buffer[bufpos++] = c;
                      continue;

                    default:
                      phase4_ungetc (c);
                      break;
                  }
                break;
              }
            break;

          default:
            phase4_ungetc (c);
            break;
        }
      if (bufpos > 0)
        {
          // if we have more than just '$' char
          if (bufpos >= bufmax)
            {
              bufmax = 2 * bufmax + 10;
              buffer = xrealloc (buffer, bufmax);
            }
          buffer[bufpos] = 0;
          if (bufpos > 2 && 0 == strncasecmp("str", buffer, 3))
            {
              // Force lowercase STR_ prefix
              strncpy(buffer, "str", 3);
            }
          tp->string = xstrdup (buffer);
          tp->type = token_type_dollar_literal;
          // No escaping in Arma string literals
          tp->escape = LET_NONE;
          tp->comment = add_reference (savable_comment);
        } else {
          // otherwise act like it was a symbol
          tp->type = token_type_symbol;
        }
      return;

    case '.':
      c = phase4_getc ();
      phase4_ungetc (c);
      switch (c)
        {
        default:
          tp->type = token_type_symbol;
          return;

        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
          c = '.';
          break;
        }
      /* FALLTHROUGH */

    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
      /* The preprocessing number token is more "generous" than the C
         number tokens.  This is mostly due to token pasting (another
         thing we can ignore here).  */
      bufpos = 0;
      for (;;)
        {
          if (bufpos >= bufmax)
            {
              bufmax = 2 * bufmax + 10;
              buffer = xrealloc (buffer, bufmax);
            }
          buffer[bufpos++] = c;
          c = phase4_getc ();
          switch (c)
            {
            case 'e':
            case 'E':
              if (bufpos >= bufmax)
                {
                  bufmax = 2 * bufmax + 10;
                  buffer = xrealloc (buffer, bufmax);
                }
              buffer[bufpos++] = c;
              c = phase4_getc ();
              if (c != '+' && c != '-')
                {
                  phase4_ungetc (c);
                  break;
                }
              continue;

            case 'A': case 'B': case 'C': case 'D':           case 'F':
            case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
            case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
            case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
            case 'Y': case 'Z':
            case 'a': case 'b': case 'c': case 'd':           case 'f':
            case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
            case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
            case 's': case 't': case 'u': case 'v': case 'w': case 'x':
            case 'y': case 'z':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
            case '.':
              continue;

            default:
              phase4_ungetc (c);
              break;
            }
          break;
        }
      if (bufpos >= bufmax)
        {
          bufmax = 2 * bufmax + 10;
          buffer = xrealloc (buffer, bufmax);
        }
      buffer[bufpos] = 0;
      tp->type = token_type_number;
      tp->number = atol (buffer);
      return;

    case '"': case '\'':
      {
      string:
        bufpos = 0;
        cquot = c;
        for (;;)
          {
            c = phase3_getc ();
            if (c == cquot)
              {
                // Arma string literal could start with both single and
                // double quote mark. Unescaped single quotes are ok in
                // double quoted string literals and vice versa. So we
                // only concerned with the same char as the one that
                // has started the literal. If it appears twice inside
                // literal, that means it appears once in the actual
                // string.
                c = phase3_getc ();
                if (c == cquot)
                  {
                    if (bufpos >= bufmax)
                      {
                        bufmax = 2 * bufmax + 10;
                        buffer = xrealloc (buffer, bufmax);
                      }
                    buffer[bufpos++] = c;
                    continue;
                  }
                else
                  {
                    phase3_ungetc (c);
                  }
                break;
              }
            else if (c == EOF)
              break;
            else // default
              {
                if (c == '\n')
                  {
                    error_with_progname = false;
                    error (0, 0,
                           _("%s:%d: warning: unterminated string literal"),
                           logical_file_name, line_number - 1);
                    error_with_progname = true;
                    phase3_ungetc ('\n');
                    break;
                  }
                if (bufpos >= bufmax)
                  {
                    bufmax = 2 * bufmax + 10;
                    buffer = xrealloc (buffer, bufmax);
                  }
                buffer[bufpos++] = c;
                continue;
              }
            break;
          }
        if (bufpos >= bufmax)
          {
            bufmax = 2 * bufmax + 10;
            buffer = xrealloc (buffer, bufmax);
          }
        buffer[bufpos] = 0;
        tp->type = token_type_string_literal;
        tp->string = xstrdup (buffer);
        // No escaping in Arma string literals
        tp->escape = LET_NONE;
        tp->comment = add_reference (savable_comment);
        return;
      }

    case '(':
      tp->type = token_type_lparen;
      return;

    case ')':
      tp->type = token_type_rparen;
      return;

    case '[':
      tp->type = token_type_lsqbr;
      return;

    case ']':
      tp->type = token_type_rsqbr;
      return;

    case ',':
      tp->type = token_type_comma;
      return;

    case '#':
      tp->type = token_type_hash;
      return;

    case ':':
      tp->type = token_type_colon;
      return;

    case '\\':
      c = phase4_getc ();
      if (c == 'n')
        {
          tp->string = xstrdup("\n");
          tp->type = token_type_eoln_explicit;
          tp->escape = LET_NONE;
          tp->comment = add_reference (savable_comment);
          return;
        }
      else
        {
          phase4_ungetc (c);
        }
      /* FALLTHROUGH */

    default:
      /* We could carefully recognize each of the 2 and 3 character
         operators, but it is not necessary, as we only need to recognize
         gettext invocations.  Don't bother.  */
      tp->type = token_type_symbol;
      return;
    }
}


/* Supports only one pushback token.  */
static void
phase5_unget (token_ty *tp)
{
  if (tp->type != token_type_eof)
    {
      if (phase5_pushback_length == SIZEOF (phase5_pushback))
        abort ();
      phase5_pushback[phase5_pushback_length++] = *tp;
    }
}


/* X. Recognize a leading # symbol.  Leave leading hash as a hash, but
   turn hash in the middle of a line into a plain symbol token.  This
   makes the phase 6 easier.  */

static void
phaseX_get (token_ty *tp)
{
  static bool middle;  /* false at the beginning of a line, true otherwise.  */

  phase5_get (tp);

  if (tp->type == token_type_eoln || tp->type == token_type_eof)
    middle = false;
  else
    {
      if (middle)
        {
          /* Turn hash in the middle of a line into a plain symbol token.  */
          if (tp->type == token_type_hash)
            tp->type = token_type_symbol;
        }
      else
        {
          /* When we see leading whitespace followed by a hash sign,
             discard the leading white space token.  The hash is all
             phase 6 is interested in.  */
          if (tp->type == token_type_white_space)
            {
              token_ty next;

              phase5_get (&next);
              if (next.type == token_type_hash)
                *tp = next;
              else
                phase5_unget (&next);
            }
          middle = true;
        }
    }
}


/* 6. Recognize and carry out directives (it also expands macros on
   non-directive lines, which we do not do here).  The only directive
   we care about are the #line and #define directive.  We throw all the
   others away.  */

// If you need macro expansion in Arma files you will probably want to
// preprocess them with external tool like GNU cpp or such before
// processing it with xgettext. #line directive is taken into account,
// so output PO files will contain correct locations of localized
// strings in source files. Arma's __EXEC and __EVAL preprocessor
// macros can break stuff when preprocessed with cpp, so you might also
// want to redefine them to empty macros, hoping it will not interfere
// with localization strings/macros.

static token_ty phase6_pushback[2];
static int phase6_pushback_length;


static void
phase6_get (token_ty *tp)
{
  static token_ty *buf;
  static int bufmax;
  int bufpos;
  int j;

  if (phase6_pushback_length)
    {
      *tp = phase6_pushback[--phase6_pushback_length];
      return;
    }
  for (;;)
    {
      /* Get the next token.  If it is not a '#' at the beginning of a
         line (ignoring whitespace), return immediately.  */
      phaseX_get (tp);
      if (tp->type != token_type_hash)
        return;

      /* Accumulate the rest of the directive in a buffer, until the
         "define" keyword is seen or until end of line.  */
      bufpos = 0;
      for (;;)
        {
          phaseX_get (tp);
          if (tp->type == token_type_eoln || tp->type == token_type_eof)
            break;

          /* Before the "define" keyword and inside other directives
             white space is irrelevant.  So just throw it away.  */
          if (tp->type != token_type_white_space)
            {
              /* If it is a #define directive, return immediately,
                 thus treating the body of the #define directive like
                 normal input.  */
              if (bufpos == 0
                  && tp->type == token_type_name
                  && strcmp (tp->string, "define") == 0)
                return;

              /* Accumulate.  */
              if (bufpos >= bufmax)
                {
                  bufmax = 2 * bufmax + 10;
                  buf = xrealloc (buf, bufmax * sizeof (buf[0]));
                }
              buf[bufpos++] = *tp;
            }
        }

      /* If it is a #line directive, with no macros to expand, act on
         it.  Ignore all other directives.  */
      if (bufpos >= 3 && buf[0].type == token_type_name
          && strcmp (buf[0].string, "line") == 0
          && buf[1].type == token_type_number
          && buf[2].type == token_type_string_literal)
        {
          logical_file_name = xstrdup (buf[2].string);
          line_number = buf[1].number;
        }
      if (bufpos >= 2 && buf[0].type == token_type_number
          && buf[1].type == token_type_string_literal)
        {
          logical_file_name = xstrdup (buf[1].string);
          line_number = buf[0].number;
        }

      /* Release the storage held by the directive.  */
      for (j = 0; j < bufpos; ++j)
        free_token (&buf[j]);

      /* We must reset the selected comments.  */
      savable_comment_reset ();
    }
}


/* Supports 2 tokens of pushback.  */
static void
phase6_unget (token_ty *tp)
{
  if (tp->type != token_type_eof)
    {
      if (phase6_pushback_length == SIZEOF (phase6_pushback))
        abort ();
      phase6_pushback[phase6_pushback_length++] = *tp;
    }
}

/* 8a. Drop whitespace.  */
static void
phase8a_get (token_ty *tp)
{
  for (;;)
    {
      phase6_get (tp);

      if (tp->type == token_type_white_space)
        continue;
      if (tp->type == token_type_eoln)
        {
          /* We have to track the last occurrence of a string.  One
             mode of xgettext allows to group an extracted message
             with a comment for documentation.  The rule which states
             which comment is assumed to be grouped with the message
             says it should immediately precede it.  Our
             interpretation: between the last line of the comment and
             the line in which the keyword is found must be no line
             with non-white space tokens.  */
          ++newline_count;
          if (last_non_comment_line > last_comment_line)
            savable_comment_reset ();
          continue;
        }
      break;
    }
}

/* Supports 2 tokens of pushback.  */
static inline void
phase8a_unget (token_ty *tp)
{
  phase6_unget (tp);
}

/* 8. Concatenate adjacent string literals to form single string
   literals (because we don't expand macros, there are a few things we
   will miss). */

static void
phase8_get (token_ty *tp)
{
  phase8a_get (tp);
  if (tp->type != token_type_string_literal &&
      tp->type != token_type_eoln_explicit)
    return;
  for (;;)
    {
      token_ty tmp;
      size_t len;

      phase8a_get (&tmp);
      if (tmp.type != token_type_string_literal &&
          tmp.type != token_type_eoln_explicit)
        {
          phase8a_unget (&tmp);
          if (tp->type == token_type_eoln_explicit)
            {
              tp->type = token_type_string_literal;
            }
          return;
        }
      len = strlen (tp->string);
      tp->string = xrealloc (tp->string, len + strlen (tmp.string) + 1);
      // if it was token_type_eoln_explicit, convert it to
      // token_type_string_literal
      tp->type = token_type_string_literal;
      strcpy (tp->string + len, tmp.string);
      free_token (&tmp);
    }
}


/* ===================== Reading of high-level tokens.  ==================== */

enum xgettext_token_type_ty
{
  xgettext_token_type_eof,
  xgettext_token_type_keyword,
  xgettext_token_type_symbol,
  xgettext_token_type_lparen,
  xgettext_token_type_rparen,
  xgettext_token_type_lsqbr,
  xgettext_token_type_rsqbr,
  xgettext_token_type_comma,
  xgettext_token_type_colon,
  xgettext_token_type_string_literal,
  xgettext_token_type_dollar_literal,
  xgettext_token_type_other
};
typedef enum xgettext_token_type_ty xgettext_token_type_ty;

struct xgettext_token_ty
{
  xgettext_token_type_ty type;

  /* This field is used only for xgettext_token_type_keyword.  */
  const struct callshapes *shapes;

  /* This field is used only for xgettext_token_type_string_literal,
     xgettext_token_type_keyword, xgettext_token_type_symbol and
     xgettext_token_type_dollar_literal */
  char *string;

  /* This field is used only for xgettext_token_type_string_literal.  */
  enum literalstring_escape_type escape;

  /* This field is used only for xgettext_token_type_string_literal and
     xgettext_token_type_dollar_literal.  */
  refcounted_string_list_ty *comment;

  /* These fields are only for
       xgettext_token_type_keyword,
       xgettext_token_type_string_literal,
       xgettext_token_type_dollar_literal.  */
  lex_pos_ty pos;
};
typedef struct xgettext_token_ty xgettext_token_ty;


/* 9. Convert the remaining preprocessing tokens to C tokens and
   discards any white space from the translation unit.  */

static void
x_arma_lex (xgettext_token_ty *tp)
{
  for (;;)
    {
      token_ty token;
      void *keyword_value;

      phase8_get (&token);
      switch (token.type)
        {
        case token_type_eof:
          tp->type = xgettext_token_type_eof;
          return;

        case token_type_name:
          last_non_comment_line = newline_count;

          if (hash_find_entry (&arma_keywords, token.string,
                               strlen (token.string),
                               &keyword_value)
              == 0)
            {
              tp->type = xgettext_token_type_keyword;
              tp->shapes = (const struct callshapes *) keyword_value;
              tp->pos.file_name = logical_file_name;
              tp->pos.line_number = token.line_number;
            }
          else
            tp->type = xgettext_token_type_symbol;
          tp->string = token.string;
          return;

        case token_type_lparen:
          last_non_comment_line = newline_count;

          tp->type = xgettext_token_type_lparen;
          return;

        case token_type_rparen:
          last_non_comment_line = newline_count;

          tp->type = xgettext_token_type_rparen;
          return;

        case token_type_lsqbr:
          last_non_comment_line = newline_count;

          tp->type = xgettext_token_type_lsqbr;
          return;

        case token_type_rsqbr:
          last_non_comment_line = newline_count;

          tp->type = xgettext_token_type_rsqbr;
          return;

        case token_type_comma:
          last_non_comment_line = newline_count;

          tp->type = xgettext_token_type_comma;
          return;

        case token_type_colon:
          last_non_comment_line = newline_count;

          tp->type = xgettext_token_type_colon;
          return;

        case token_type_string_literal:
          last_non_comment_line = newline_count;

          tp->type = xgettext_token_type_string_literal;
          tp->string = token.string;
          tp->escape = token.escape;
          tp->comment = token.comment;
          tp->pos.file_name = logical_file_name;
          tp->pos.line_number = token.line_number;
          return;

        case token_type_dollar_literal:
          last_non_comment_line = newline_count;

          tp->type = xgettext_token_type_dollar_literal;
          tp->string = token.string;
          tp->escape = token.escape;
          tp->comment = token.comment;
          tp->pos.file_name = logical_file_name;
          tp->pos.line_number = token.line_number;
          tp->shapes = NULL;
          return;

        default:
          last_non_comment_line = newline_count;

          tp->type = xgettext_token_type_other;
          return;
        }
    }
}


/* ========================= Extracting strings.  ========================== */


/* Context lookup table.  */
static flag_context_list_table_ty *flag_context_list_table;


/* The file is broken into tokens.  Scan the token stream, looking for
   a keyword, followed by a left paren, followed by a string.  When we
   see this sequence, we have something to remember.  We assume we are
   looking at a valid C or C++ program, and leave the complaints about
   the grammar to the compiler.

     Normal handling: Look for
       keyword ( ... msgid ... )
     Plural handling: Look for
       keyword ( ... msgid ... msgid_plural ... )

   We use recursion because the arguments before msgid or between msgid
   and msgid_plural can contain subexpressions of the same form.  */


/* Extract messages until the next balanced closing parenthesis.
   Extracted messages are added to MLP.
   Return true upon eof, false upon closing parenthesis.  */
static bool
extract_parenthesized (message_list_ty *mlp,
                       flag_context_ty outer_context,
                       flag_context_list_iterator_ty context_iter,
                       struct arglist_parser *argparser)
{
  /* Current argument number.  */
  int arg = 1;
  /* 0 when no keyword has been seen.  1 right after a keyword is seen.  */
  int state;
  /* Parameters of the keyword just seen.  Defined only in state 1.  */
  const struct callshapes *next_shapes = NULL;
  /* Context iterator that will be used if the next token is a '('.  */
  flag_context_list_iterator_ty next_context_iter =
    passthrough_context_list_iterator;
  /* Current context.  */
  flag_context_ty inner_context =
    inherited_context (outer_context,
                       flag_context_list_iterator_advance (&context_iter));

  /* Start state is 0.  */
  state = 0;

  for (;;)
    {
      xgettext_token_ty token;

      x_arma_lex (&token);
      switch (token.type)
        {
        case xgettext_token_type_keyword:
          next_shapes = token.shapes;
          state = 1;
          goto keyword_or_symbol;

        case xgettext_token_type_symbol:
          state = 0;
        keyword_or_symbol:
          next_context_iter =
            flag_context_list_iterator (
              flag_context_list_table_lookup (
                flag_context_list_table,
                token.string, strlen (token.string)));
          free (token.string);
          continue;

        case xgettext_token_type_lparen:
        // We don't make any difference between parentheses and square
        // brackets here, they should be balanced anyway.
        case xgettext_token_type_lsqbr:
          if (extract_parenthesized (mlp, inner_context, next_context_iter,
                                     arglist_parser_alloc (
                                       mlp, state ? next_shapes : NULL)))
            {
              arglist_parser_done (argparser, arg);
              return true;
            }
          next_context_iter = null_context_list_iterator;
          state = 0;
          continue;

        case xgettext_token_type_rparen:
        case xgettext_token_type_rsqbr:
          arglist_parser_done (argparser, arg);
          return false;

        case xgettext_token_type_comma:
          arg++;
          inner_context =
            inherited_context (outer_context,
                               flag_context_list_iterator_advance (
                                 &context_iter));
          next_context_iter = passthrough_context_list_iterator;
          state = 0;
          continue;

        case xgettext_token_type_colon:
          next_context_iter = null_context_list_iterator;
          state = 0;
          continue;

        case xgettext_token_type_string_literal:
          if (extract_all)
            {
              char *string;
              refcounted_string_list_ty *comment;
              const char *encoding;

              string = literalstring_parse (token.string, &token.pos,
                                            token.escape);
              free (token.string);
              token.string = string;

              if (token.comment != NULL)
                {
                  comment = savable_comment_convert_encoding (token.comment,
                                                              &token.pos);
                  drop_reference (token.comment);
                  token.comment = comment;
                }

              /* token.string and token.comment are already converted
                 to UTF-8.  Prevent further conversion in
                 remember_a_message.  */
              encoding = xgettext_current_source_encoding;
              xgettext_current_source_encoding = po_charset_utf8;
              remember_a_message (mlp, NULL, token.string, inner_context,
                                  &token.pos, NULL, token.comment);
              xgettext_current_source_encoding = encoding;
            }
          else
            {
              if (state)
                {
                  // A string immediately after a keyword means a
                  // function call.
                  struct arglist_parser *tmp_argparser;
                  tmp_argparser = arglist_parser_alloc (mlp, next_shapes);

                  arglist_parser_remember (tmp_argparser, 1, token.string,
                                           inner_context, token.pos.file_name,
                                           token.pos.line_number,
                                           token.comment);
                  arglist_parser_done (tmp_argparser, 1);
                }
              else
                {
                  arglist_parser_remember_literal (argparser, arg,
                                                   token.string,
                                                   inner_context,
                                                   token.pos.file_name,
                                                   token.pos.line_number,
                                                   token.comment,
                                                   token.escape);
                }
            }
          drop_reference (token.comment);
          next_context_iter = null_context_list_iterator;
          state = 0;
          continue;

        case xgettext_token_type_dollar_literal:
          remember_a_message (mlp, NULL, token.string, inner_context,
                              &token.pos, NULL, token.comment);
          drop_reference (token.comment);
          next_context_iter = null_context_list_iterator;
          state = 0;
          continue;

        case xgettext_token_type_other:
          next_context_iter = null_context_list_iterator;
          state = 0;
          continue;

        case xgettext_token_type_eof:
          arglist_parser_done (argparser, arg);
          return true;

        default:
          abort ();
        }
    }
}


static void
extract_whole_file (FILE *f,
                    const char *real_filename, const char *logical_filename,
                    flag_context_list_table_ty *flag_table,
                    msgdomain_list_ty *mdlp)
{
  message_list_ty *mlp = mdlp->item[0]->messages;

  fp = f;
  real_file_name = real_filename;
  logical_file_name = xstrdup (logical_filename);
  line_number = 1;

  newline_count = 0;
  last_comment_line = -1;
  last_non_comment_line = -1;

  flag_context_list_table = flag_table;

  init_keywords ();

  /* Eat tokens until eof is seen.  When extract_parenthesized returns
     due to an unbalanced closing parenthesis, just restart it.  */
  while (!extract_parenthesized (mlp, null_context, null_context_list_iterator,
                                 arglist_parser_alloc (mlp, NULL)))
    ;

  /* Close scanner.  */
  fp = NULL;
  real_file_name = NULL;
  logical_file_name = NULL;
  line_number = 0;
}


void
extract_arma (FILE *f,
              const char *real_filename, const char *logical_filename,
              flag_context_list_table_ty *flag_table,
              msgdomain_list_ty *mdlp)
{
  extract_whole_file (f, real_filename, logical_filename, flag_table, mdlp);
}
