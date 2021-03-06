/*
 * Copyright (C) 2003, 2004, 2005, 2006, 2007, 2008, 2009
 * Robert Lougher <rob@jamvm.org.uk>.
 *
 * This file is part of JamVM.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "../../properties.h"

/* _GNU_SOURCE doesn't bring in C99 long long constants,
   but we do get the GNU constants */
#ifndef LLONG_MAX
#define LLONG_MAX XI_LLONG_MAX
#endif

#ifndef LLONG_MIN
#define LLONG_MIN XI_LLONG_MIN
#endif

#define FLOAT_1_BITS 0x3f800000
#define FLOAT_2_BITS 0x40000000

#define READ_U1_OP(p)    ((p)[1])
#define READ_U2_OP(p)    (((p)[1]<<8)|(p)[2])
#define READ_U4_OP(p)    (((p)[1]<<24)|((p)[2]<<16)|((p)[3]<<8)|(p)[4])
#define READ_S1_OP(p)    (signed char)READ_U1_OP(p)
#define READ_S2_OP(p)    (signed short)READ_U2_OP(p)
#define READ_S4_OP(p)    (signed int)READ_U4_OP(p)

/* Stack related macros */

#define STACK_float(offset)    *((float*)&ostack[offset] + IS_BE64)
#define STACK_xuint64(offset) *(xuint64*)&ostack[offset * 2]
#define STACK_xint64(offset)  *(xint64*)&ostack[offset * 2]
#define STACK_double(offset)   *(double*)&ostack[offset * 2]
#define STACK_xuint16(offset) (xuint16)ostack[offset]
#define STACK_xint16(offset)  (xint16)ostack[offset]
#define STACK_xint8(offset)   (xint8)ostack[offset]
#define STACK_int(offset)      (int)ostack[offset]

#define STACK(type, offset)  STACK_##type(offset)

#define SLOTS(type) (sizeof(type) + 3)/4

#define STACK_POP(type) ({       \
    ostack -= SLOTS(type);       \
    STACK(type, 0);              \
})

#define STACK_PUSH(type, val) {  \
    STACK(type, 0) = val;        \
    ostack += SLOTS(type);       \
}

/* Include the interpreter variant header */

#include "interp-direct.h"
