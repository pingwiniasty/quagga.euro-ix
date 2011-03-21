/* Pseudo Random Sequence  -- Header
 * Copyright (C) 2010 Chris Hall (GMCH), Highwayman
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2, or (at your
 * option) any later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef _ZEBRA_QRAND_H
#define _ZEBRA_QRAND_H

#include "misc.h"

/*==============================================================================
 * Simple 32 bit random sequence.
 *
 * Produces 32-bit signed integers in 0..0x7FFF_FFFF.
 *
 * Object of the exercise is to be able to produce repeatable sequence, so can
 * debug !
 */

struct qrand_seq
{
  uint  last ;
} ;

typedef struct qrand_seq* qrand_seq ;
typedef struct qrand_seq  qrand_seq_t[1] ;

#define QRAND_SEQ_INIT(s) { { s } }

/*==============================================================================
 * Functions
 */

extern int qrand(qrand_seq seq, int range) ;

#endif /* _ZEBRA_QRAND_H */
