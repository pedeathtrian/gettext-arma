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

#include <stdio.h>

#include "message.h"
#include "xgettext.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define EXTENSIONS_ARMA \
  { "cpp",    "Arma"   },                                                \
  { "ext",    "Arma"   },                                                \
  { "fsm",    "Arma"   },                                                \
  { "hpp",    "Arma"   },                                                \
  { "inc",    "Arma"   },                                                \
  { "sqf",    "Arma"   },                                                \
  { "sqm",    "Arma"   },                                                \
  { "sqs",    "Arma"   },                                                \

#define SCANNERS_ARMA \
  { "Arma",          extract_arma,                                       \
                     &flag_table_arma, &formatstring_arma, NULL, NULL }, \

  /* Scan an Arma file and add its translatable strings to mdlp.  */
  extern void extract_arma (FILE * fp, const char *real_filename,
                           const char *logical_filename,
                           flag_context_list_table_ty * flag_table,
                           msgdomain_list_ty * mdlp);

  extern void x_arma_keyword (const char *keyword);
  extern void x_arma_extract_all (void);

  extern void init_flag_table_arma (void);

#ifdef __cplusplus
}
#endif
