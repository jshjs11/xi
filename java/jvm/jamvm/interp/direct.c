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

/* Must be included first to get configure options */
#include "../jam.h"

#include "../thread.h"
#include "engine/interp.h"
#include "../symbol.h"

#include "shared.h"

#ifdef TRACEDIRECT
#define TRACE(fmt, ...) jam_printf(fmt, ## __VA_ARGS__)
#else
#define TRACE(fmt, ...)
#endif

#define REWRITE_OPERAND(index) \
    quickened = TRUE;          \
    operand.uui.u1 = index;    \
    operand.uui.u2 = opcode;   \
    operand.uui.i = ins_cache;

/* Used to indicate that stack depth
   has not been calculated yet */
#define DEPTH_UNKNOWN -1

/* Method preparation states */
#define PREPARED   0
#define UNPREPARED 1
#define PREPARING  2

/* Global lock for method preparation */
static VMWaitLock prepare_lock;

#define MACRO_NTOHL(x) \
	((((x) & 0xff000000u) >> 24) | (((x) & 0x00ff0000u) >>  8) | \
	(((x) & 0x0000ff00u) <<  8) | (((x) & 0x000000ffu) << 24))

void initialiseDirect(InitArgs *args) {
    initVMWaitLock(prepare_lock);
}

void prepare(MethodBlock *mb, const void ***handlers) {
    int code_len = mb->code_size;
    Instruction *new_code = NULL;
    unsigned char *code;
    short map[code_len];
    int ins_count = 0;
    int pass;
    int i;

    /* The bottom bits of the code pointer are used to
       indicate whether the method has been prepared.  The
       check in the interpreter is unsynchronised, so grab
       the lock and recheck.  If another thread tries to
       prepare the method, they will wait on the lock.  This
       lock is global, but methods are only prepared once,
       and contention to prepare a method is unlikely. */

    Thread *self = threadSelf();

    disableSuspend(self);
    lockVMWaitLock(prepare_lock, self);

retry:
    code = mb->code;

    switch((xuintptr)code & 0x3) {
        case PREPARED:
            unlockVMWaitLock(prepare_lock, self);
            enableSuspend(self);
            return;

        case UNPREPARED:
            mb->code = (void*)PREPARING;
            break;

        case PREPARING:
            waitVMWaitLock(prepare_lock, self);
            goto retry;
    }

    unlockVMWaitLock(prepare_lock, self);

    TRACE("Preparing %s.%s%s\n", CLASS_CB(mb->class)->name, mb->name, mb->type);

    /* Method is unprepared, so bottom bit of pntr will be set */
    code--;

    for(pass = 0; pass < 2; pass++) {
        //int block_quickened = FALSE;
        int cache = 0;
        int pc;

        if(pass == 1)
            new_code = sysMalloc((ins_count + 1) * sizeof(Instruction));

        for(ins_count = 0, pc = 0; pc < code_len; ins_count++) {
            int quickened = FALSE;
            Operand operand;
            int ins_cache;
            int opcode;

            /* Load the opcode -- we change it if it's WIDE, or ALOAD_0
               and the method is an instance method */
            opcode = code[pc];

            /* On pass one we calculate the cache depth and the mapping
               between bytecode and instruction numbering */

            if(pass == 0) {
                    TRACE("%d : pc %d opcode %d\n", ins_count, pc, opcode);
                    map[pc] = ins_count;
            }

            /* For instructions without an operand */
            operand.i = 0;
            ins_cache = cache;

            switch(opcode) {
                default:
                    jam_printf("Unrecognised bytecode %d found while preparing %s.%s%s\n",
                               opcode, CLASS_CB(mb->class)->name, mb->name, mb->type);
                    exitVM(1);

                case OPC_ALOAD_0:
                {
                    FieldBlock *fb;

                    /* If the next instruction is GETFIELD, this is an instance method
                       and the field is not 2-slots rewrite it to GETFIELD_THIS.  We
                       can safely resolve the field because as an instance method
                       the class must be initialised */

                    if((code[++pc] == OPC_GETFIELD) && !(mb->access_flags & ACC_STATIC)
                                    && (fb = resolveField(mb->class, READ_U2_OP(code + pc)))
                                    && !((*fb->type == 'J') || (*fb->type == 'D'))) {
                        if(*fb->type == 'L' || *fb->type == '[')
                            opcode = OPC_GETFIELD_THIS_REF;
                        else
                            opcode = OPC_GETFIELD_THIS;

                        operand.i = fb->u.offset;
                        pc += 3;
                    } else
                        opcode = OPC_ILOAD_0;
                    break;
                }
                case OPC_SIPUSH:
                    operand.i = READ_S2_OP(code + pc);
                    pc += 3;
                    break;
        
                case OPC_LDC_W:
                    REWRITE_OPERAND(READ_U2_OP(code + pc));
                    opcode = OPC_LDC;
                    pc += 3;
                    break;
    
                case OPC_LDC2_W:
                    operand.i = READ_U2_OP(code + pc);
                    pc += 3;
                    break;
        
                case OPC_LDC:
                    REWRITE_OPERAND(READ_U1_OP(code + pc));
                    pc += 2;
                    break;
    
                case OPC_BIPUSH:
                    operand.i = READ_S1_OP(code + pc);
                    pc += 2;
                    break;

                case OPC_ILOAD:
                case OPC_FLOAD:
                case OPC_ALOAD:
                case OPC_LLOAD:
                case OPC_DLOAD:
                    operand.i = READ_U1_OP(code + pc);
                    pc += 2;
                    break;
        
                case OPC_RETURN: case OPC_IRETURN:  case OPC_ARETURN:
                case OPC_FRETURN: case OPC_LRETURN: case OPC_DRETURN:
                case OPC_ATHROW:
                case OPC_ACONST_NULL: case OPC_ICONST_M1: case OPC_ICONST_0:
                case OPC_FCONST_0: case OPC_ICONST_1: case OPC_ICONST_2:
                case OPC_ICONST_3: case OPC_ICONST_4: case OPC_ICONST_5:
                case OPC_FCONST_1: case OPC_FCONST_2: case OPC_ILOAD_0:
                case OPC_FLOAD_0: case OPC_ILOAD_1: case OPC_FLOAD_1:
                case OPC_ALOAD_1: case OPC_ILOAD_2: case OPC_FLOAD_2:
                case OPC_ALOAD_2: case OPC_ILOAD_3: case OPC_FLOAD_3:
                case OPC_ALOAD_3: case OPC_DUP:
                case OPC_LCONST_0: case OPC_DCONST_0: case OPC_DCONST_1:
                case OPC_LCONST_1: case OPC_LLOAD_0: case OPC_DLOAD_0:
                case OPC_LLOAD_1: case OPC_DLOAD_1: case OPC_LLOAD_2:
                case OPC_DLOAD_2: case OPC_LLOAD_3: case OPC_DLOAD_3:
                case OPC_LALOAD: case OPC_DALOAD: case OPC_DUP_X1:
                case OPC_DUP_X2: case OPC_DUP2: case OPC_DUP2_X1:
                case OPC_DUP2_X2: case OPC_SWAP: case OPC_LADD:
                case OPC_LSUB: case OPC_LMUL: case OPC_LDIV:
                case OPC_LREM: case OPC_LAND: case OPC_LOR:
                case OPC_LXOR: case OPC_LSHL: case OPC_LSHR:
                case OPC_LUSHR: case OPC_F2L: case OPC_D2L:
                case OPC_I2L:
                case OPC_NOP: case OPC_LSTORE_0: case OPC_DSTORE_0:
                case OPC_LSTORE_1: case OPC_DSTORE_1: case OPC_LSTORE_2:
                case OPC_DSTORE_2: case OPC_LSTORE_3: case OPC_DSTORE_3:
                case OPC_IASTORE: case OPC_FASTORE: case OPC_AASTORE:
                case OPC_LASTORE: case OPC_DASTORE: case OPC_BASTORE:
                case OPC_CASTORE: case OPC_SASTORE: case OPC_POP2:
                case OPC_FADD: case OPC_DADD: case OPC_FSUB:
                case OPC_DSUB: case OPC_FMUL: case OPC_DMUL:
                case OPC_FDIV: case OPC_DDIV: case OPC_I2F:
                case OPC_I2D: case OPC_L2F: case OPC_L2D:
                case OPC_F2D: case OPC_D2F: case OPC_FREM:
                case OPC_DREM: case OPC_LNEG: case OPC_FNEG:
                case OPC_DNEG: case OPC_MONITORENTER:
                case OPC_MONITOREXIT: case OPC_ABSTRACT_METHOD_ERROR:
                case OPC_ISTORE_0: case OPC_ASTORE_0: case OPC_FSTORE_0:
                case OPC_ISTORE_1: case OPC_ASTORE_1: case OPC_FSTORE_1:
                case OPC_ISTORE_2: case OPC_ASTORE_2: case OPC_FSTORE_2:
                case OPC_ISTORE_3: case OPC_ASTORE_3: case OPC_FSTORE_3:
                case OPC_POP:
                case OPC_INEG:
                case OPC_IALOAD: case OPC_AALOAD: case OPC_FALOAD:
                case OPC_BALOAD: case OPC_CALOAD: case OPC_SALOAD:
                case OPC_IADD: case OPC_IMUL: case OPC_ISUB:
                case OPC_IDIV: case OPC_IREM: case OPC_IAND:
                case OPC_IOR: case OPC_IXOR: case OPC_F2I:
                case OPC_D2I: case OPC_I2B: case OPC_I2C:
                case OPC_I2S: case OPC_ISHL: case OPC_ISHR:
                case OPC_IUSHR: case OPC_LCMP: case OPC_DCMPG:
                case OPC_DCMPL: case OPC_FCMPG: case OPC_FCMPL:
                case OPC_ARRAYLENGTH: case OPC_L2I:
                    pc += 1;
                    break;
    
                case OPC_LSTORE:
                case OPC_DSTORE:
                case OPC_ISTORE:
                case OPC_FSTORE:
                case OPC_ASTORE:
                    operand.i = READ_U1_OP(code + pc);
                    pc += 2;
                    break;
        
                case OPC_IINC:
                    operand.ii.i1 = READ_U1_OP(code + pc); 
                    operand.ii.i2 = READ_S1_OP(code + pc + 1);
                    pc += 3;
                    break;
    
                case OPC_IFEQ: case OPC_IFNULL: case OPC_IFNE:
                case OPC_IFNONNULL: case OPC_IFLT: case OPC_IFGE:
                case OPC_IFGT: case OPC_IFLE: case OPC_IF_ACMPEQ:
                case OPC_IF_ICMPEQ: case OPC_IF_ACMPNE: case OPC_IF_ICMPNE:
                case OPC_IF_ICMPLT: case OPC_IF_ICMPGE: case OPC_IF_ICMPGT:
                case OPC_IF_ICMPLE:
                {
                    int dest = pc + READ_S2_OP(code + pc);
                        pc += 3;
    
                    if(pass == 1) {
                        TRACE("IF old dest %d new dest %d\n", dest, map[dest]);
                        operand.pntr = &new_code[map[dest]];
                    }

                    break;
                }

                case OPC_PUTFIELD: case OPC_INVOKEVIRTUAL: case OPC_INVOKESPECIAL:
                case OPC_INVOKESTATIC: case OPC_CHECKCAST: case OPC_INSTANCEOF:
                    REWRITE_OPERAND(READ_U2_OP(code + pc));
                    pc += 3;
                    break;
        
                case OPC_GOTO_W:
                case OPC_GOTO:
                {
                    int delta, dest;
                    
                    if(opcode == OPC_GOTO)
                        delta = READ_S2_OP(code + pc);
                    else
                        delta = READ_S4_OP(code + pc);

                    dest = pc + delta;
                    pc += opcode == OPC_GOTO ? 3 : 5;

                    if(pass == 1) {
                        TRACE("GOTO old dest %d new dest %d\n", dest, map[dest]);
                        operand.pntr = &new_code[map[dest]];
                    }

                    opcode = OPC_GOTO;
                    break;
                }
        
                case OPC_TABLESWITCH:
                {
                    int *aligned_pc = (int*)(code + ((pc + 4) & ~0x3));
                    int deflt = MACRO_NTOHL(aligned_pc[0]);
                    int low   = MACRO_NTOHL(aligned_pc[1]);
                    int high  = MACRO_NTOHL(aligned_pc[2]);
                    int i;
    
                    if(pass == 0) {
                        i = high - low + 4;
                    } else {
                        SwitchTable *table = sysMalloc(sizeof(SwitchTable));

                        table->low = low;
                        table->high = high; 
                        table->deflt = &new_code[map[pc + deflt]];
                        table->entries = sysMalloc((high - low + 1) * sizeof(Instruction *));

                        for(i = 3; i < (high - low + 4); i++)
                            table->entries[i - 3] = &new_code[map[pc + MACRO_NTOHL(aligned_pc[i])]];

                        operand.pntr = table;
                    }

                    pc = (unsigned char*)&aligned_pc[i] - code;
                    break;
                }
        
                case OPC_LOOKUPSWITCH:
                {
                    int *aligned_pc = (int*)(code + ((pc + 4) & ~0x3));
                    int deflt  = MACRO_NTOHL(aligned_pc[0]);
                    int npairs = MACRO_NTOHL(aligned_pc[1]);
                    int i, j;
    
                    if(pass == 0) {
                        i = npairs*2+2;
                    } else {
                        LookupTable *table = sysMalloc(sizeof(LookupTable));
   
                        table->num_entries = npairs;
                        table->deflt = &new_code[map[pc + deflt]];
                        table->entries = sysMalloc(npairs * sizeof(LookupEntry));
                            
                        for(i = 2, j = 0; j < npairs; i += 2, j++) {
                            table->entries[j].key = MACRO_NTOHL(aligned_pc[i]);
                            table->entries[j].handler = &new_code[map[pc + MACRO_NTOHL(aligned_pc[i+1])]];
                        }
                        operand.pntr = table;
                    }

                    pc = (unsigned char*)&aligned_pc[i] - code;
                    break;
                }
    
                case OPC_GETSTATIC:
                case OPC_PUTSTATIC: 
                case OPC_GETFIELD:
                {
                    int idx = READ_U2_OP(code + pc);
                    REWRITE_OPERAND(idx);
                    pc += 3;
                    break;
                }
    
                case OPC_INVOKEINTERFACE:
                    REWRITE_OPERAND(READ_U2_OP(code + pc));
                    pc += 5;
                    break;
    
                case OPC_JSR_W:
                case OPC_JSR:
                {
                    int delta, dest;
                    
                    if(opcode == OPC_JSR)
                        delta = READ_S2_OP(code + pc);
                    else
                        delta = READ_S4_OP(code + pc);

                    dest = pc + delta;
                        pc += opcode == OPC_JSR ? 3 : 5;
                    if(pass == 1) {
                        TRACE("JSR old dest %d new dest %d\n", dest, map[dest]);
                        operand.pntr = &new_code[map[dest]];
                    }
                    opcode = OPC_JSR;
                    break;
                }
    
                case OPC_RET:
                    operand.i = READ_U1_OP(code + pc);
                    pc += 2;
                    break;

                case OPC_NEWARRAY:
                    operand.i = READ_U1_OP(code + pc);
                    pc += 2;
                    break;
        
                case OPC_NEW:
                case OPC_ANEWARRAY:
                    REWRITE_OPERAND(READ_U2_OP(code + pc));
                    pc += 3;
                    break;
    
                case OPC_MULTIANEWARRAY:
                    operand.uui.u1 = READ_U2_OP(code + pc);
                    operand.uui.u2 = READ_U1_OP(code + pc + 2);
                    operand.uui.i = cache;
                    quickened = TRUE;
                    pc += 4;
                    break;
    
                /* All instructions are "wide" in the direct interpreter --
                   rewrite OPC_WIDE to the actual widened instruction */
                case OPC_WIDE:
                {
                    opcode = code[pc + 1];
        
                    switch(opcode) {
                        case OPC_ILOAD:
                        case OPC_FLOAD:
                        case OPC_ALOAD:
                        case OPC_LLOAD:
                        case OPC_DLOAD:
                        case OPC_ISTORE:
                        case OPC_FSTORE:
                        case OPC_ASTORE:
                        case OPC_LSTORE:
                        case OPC_DSTORE:
                        case OPC_RET:
                            operand.i = READ_U2_OP(code + pc + 1);
                            pc += 4;
                            break;
    
                        case OPC_IINC:
                            operand.ii.i1 = READ_U2_OP(code + pc + 1); 
                            operand.ii.i2 = READ_S2_OP(code + pc + 3);
                            pc += 6;
                            break;
                    }
                }
            }


            if(pass == 0) {
            } else {
                /* Store the new instruction */
                new_code[ins_count].handler = handlers[ins_cache][opcode];
                new_code[ins_count].operand = operand;
            }
        }
    }

    /* Update the method's line number and exception tables
      with the new instruction offsets */

    for(i = 0; i < mb->line_no_table_size; i++) {
        LineNoTableEntry *entry = &mb->line_no_table[i];
        entry->start_pc = map[entry->start_pc];
    }

    for(i = 0; i < mb->exception_table_size; i++) {
        ExceptionTableEntry *entry = &mb->exception_table[i];
        entry->start_pc = map[entry->start_pc];
        entry->end_pc = map[entry->end_pc];
        entry->handler_pc = map[entry->handler_pc];
    }

    /* Update the method with the new code.  This
       also marks the method as being prepared. */

    lockVMWaitLock(prepare_lock, self);
    mb->code = new_code;
    mb->code_size = ins_count;
    notifyAllVMWaitLock(prepare_lock, self);
    unlockVMWaitLock(prepare_lock, self);
    enableSuspend(self);

    /* We don't need the old bytecode stream anymore */
    if(!(mb->access_flags & ACC_ABSTRACT))
        sysFree(code);
}
