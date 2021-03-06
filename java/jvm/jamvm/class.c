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

#include "xi/xi_string.h"
#include "xi/xi_mem.h"
#include "xi/xi_file.h"
#include "xi/xi_env.h"

#include "jam.h"
#include "sig.h"
#include "thread.h"
#include "lock.h"
#include "hash.h"
#include "zip.h"
#include "class.h"
#include "interp/engine/interp.h"
#include "symbol.h"
#include "excep.h"

#define PREPARE(ptr) ptr
#define SCAVENGE(ptr) FALSE
#define FOUND(ptr1, ptr2) ptr2

static int verbose;
static char *bootpath = NULL;
static char *classpath = NULL;
static int max_cp_element_len;

/* Structures holding the boot loader classpath */
typedef struct bcp_entry {
	char *path;
	ZipFile *zip;
} BCPEntry;

static BCPEntry *bootclasspath;
static int bcp_entries;

/* Cached offsets of fields in java.lang.ref.Reference objects */
int ref_referent_offset = -1;
int ref_queue_offset;

/* Cached offset of vmdata field in java.lang.ClassLoader objects */
int ldr_vmdata_offset = -1;

/* Helper method to create a Package Object representing a
 package loaded by the boot loader */
static MethodBlock *vm_loader_create_package = NULL;
static Class *package_array_class;

/* hash table containing packages loaded by the boot loader */
#define PCKG_INITSZE 1<<6
static HashTable boot_packages;

/* Hashtable entry for each package defined by the boot loader */
typedef struct package_entry {
	int index;
	char name[0];
} PackageEntry;

static MethodBlock *ldr_new_unloader = NULL;
static int ldr_data_tbl_offset;

/* Instance of java.lang.Class for java.lang.Class */
Class *java_lang_Class = NULL;

/* Method table index of ClassLoader.loadClass - used when
 requesting a Java-level class loader to load a class.
 Cached on first use. */
static int loadClass_mtbl_idx = -1;

/* Method table index of finalizer method and ClassLoader.enqueue.
 Used by finalizer and reference handler threads */
int finalize_mtbl_idx;
int enqueue_mtbl_idx;

/* hash table containing classes loaded by the boot loader and
 internally created arrays */
#define CLASS_INITSZE 1<<8
static HashTable boot_classes;

/* Array large enough to hold all primitive classes -
 * access protected by boot_classes hash table lock */
#define MAX_PRIM_CLASSES 9
static Class *prim_classes[MAX_PRIM_CLASSES];

/* Bytecode for stub abstract method.  If it is invoked
 we'll get an abstract method error. */
static char abstract_method[] = { OPC_ABSTRACT_METHOD_ERROR };

static Class *addClassToHash(Class *class, Object *class_loader) {
	HashTable *table;
	Class *entry;

#define HASH(ptr) utf8Hash(CLASS_CB((Class *)ptr)->name)
#define COMPARE(ptr1, ptr2, hash1, hash2) (hash1 == hash2) && \
            CLASS_CB((Class *)ptr1)->name == CLASS_CB((Class *)ptr2)->name

	if (class_loader == NULL) {
		table = &boot_classes;
	} else {
		Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

		if (vmdata == NULL) {
			objectLock(class_loader);
			vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

			if (vmdata == NULL) {
				if ((vmdata = allocObject(ldr_new_unloader->class)) == NULL) {
					objectUnlock(class_loader);
					return NULL;
				}

				table = sysMalloc(sizeof(HashTable));
				initHashTable((*table), CLASS_INITSZE, TRUE);

				INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset) = table;
				INST_DATA(class_loader, Object*, ldr_vmdata_offset) = vmdata;

				objectUnlock(class_loader);
			}
		}

		table = INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset);
	}

	// Add if absent, no scavenge, locked
	findHashEntry((*table), class, entry, TRUE, FALSE, TRUE);

	return entry;
}

static void prepareClass(Class *class) {
	ClassBlock *cb = CLASS_CB(class);

	if (cb->name == SYMBOL(java_lang_Class)) {
		java_lang_Class = class->class = class;
		cb->flags |= CLASS_CLASS;
	} else {
		if (java_lang_Class == NULL)
			findSystemClass0(SYMBOL(java_lang_Class));
		class->class = java_lang_Class;
	}
}

Class *defineClass(char *classname, char *data, int offset, int len,
		Object *class_loader) {

	u2 major_version, minor_version, this_idx, super_idx;
	unsigned char *ptr = (unsigned char *) data + offset;
	int cp_count, intf_count, i;
	u2 attr_count;
	u4 magic;

	ConstantPool *constant_pool;
	ClassBlock *classblock;
	Class *class, *found;
	Class **interfaces;

	READ_U4(magic, ptr, len);

	if (magic != 0xcafebabe) {
		signalException(java_lang_ClassFormatError, "bad magic");
		return NULL;
	}

	READ_U2(minor_version, ptr, len);
	READ_U2(major_version, ptr, len);

	if ((class = allocClass()) == NULL) {
		return NULL;
	}

	classblock = CLASS_CB(class);
	READ_U2(cp_count, ptr, len);

	constant_pool = &classblock->constant_pool;
	constant_pool->type = sysMalloc(cp_count);
	constant_pool->info = sysMalloc(cp_count * sizeof(ConstantPoolEntry));

	for (i = 1; i < cp_count; i++) {
		u1 tag;

		READ_U1(tag, ptr, len);
		CP_TYPE(constant_pool,i) = tag;

		switch (tag) {
		case CONSTANT_Class:
		case CONSTANT_String:
			READ_INDEX(CP_INFO(constant_pool,i), ptr, len);
			break;

		case CONSTANT_Fieldref:
		case CONSTANT_Methodref:
		case CONSTANT_NameAndType:
		case CONSTANT_InterfaceMethodref: {
			u2 idx1, idx2;

			READ_INDEX(idx1, ptr, len);
			READ_INDEX(idx2, ptr, len);
			CP_INFO(constant_pool,i) = (idx2 << 16) + idx1;
			break;
		}

		case CONSTANT_Float:
		case CONSTANT_Integer:
			READ_U4(CP_INFO(constant_pool,i), ptr, len);
			break;

		case CONSTANT_Long:
			READ_U8(*(u8 *)&(CP_INFO(constant_pool,i)), ptr, len);
			CP_TYPE(constant_pool,++i) = 0;
			break;

		case CONSTANT_Double:
			READ_DBL(*(u8 *)&(CP_INFO(constant_pool,i)), ptr, len);
			CP_TYPE(constant_pool,++i) = 0;
			break;

		case CONSTANT_Utf8: {
			int length;
			char *buff, *utf8;

			READ_U2(length, ptr, len);
			buff = sysMalloc(length + 1);

			xi_mem_copy(buff, ptr, length);
			buff[length] = '\0';
			ptr += length;

			CP_INFO(constant_pool,i) = (xuintptr) (utf8 = newUtf8(buff));

			if (utf8 != buff)
				sysFree(buff);

			break;
		}

		default:
			signalException(java_lang_ClassFormatError,
					"bad constant pool tag");
			return NULL;
		}
	}

	// Set count after constant pool has been initialized
	// -- it is now safe to be scanned by GC
	classblock->constant_pool_count = cp_count;

	READ_U2(classblock->access_flags, ptr, len);

	READ_TYPE_INDEX(this_idx, constant_pool, CONSTANT_Class, ptr, len);
	classblock->name
			= CP_UTF8(constant_pool, CP_CLASS(constant_pool, this_idx));

	if (classname && xi_strcmp(classblock->name, classname) != 0) {
		signalException(java_lang_NoClassDefFoundError,
				"class file has wrong name");
		return NULL;
	}

	prepareClass(class);

	if (classblock->name == SYMBOL(java_lang_Object)) {
		READ_U2(super_idx, ptr, len);
		if (super_idx) {
			signalException(java_lang_ClassFormatError, "Object has super");
			return NULL;
		}
		classblock->super_name = NULL;
	} else {
		READ_TYPE_INDEX(super_idx, constant_pool, CONSTANT_Class, ptr, len);
		classblock->super_name
				= CP_UTF8(constant_pool, CP_CLASS(constant_pool, super_idx));
	}

	classblock->class_loader = class_loader;

	READ_U2(intf_count = classblock->interfaces_count, ptr, len);
	interfaces = classblock->interfaces = sysMalloc(
			intf_count * sizeof(Class *));

	xi_mem_set(interfaces, 0, intf_count * sizeof(Class *));
	for (i = 0; i < intf_count; i++) {
		u2 index;
		READ_TYPE_INDEX(index, constant_pool, CONSTANT_Class, ptr, len);
		interfaces[i] = resolveClass(class, index, FALSE);
		if (exceptionOccurred()) {
			return NULL;
		}
	}

	READ_U2(classblock->fields_count, ptr, len);
	classblock->fields = sysMalloc(
			classblock->fields_count * sizeof(FieldBlock));

	for (i = 0; i < classblock->fields_count; i++) {
		u2 name_idx, type_idx;

		READ_U2(classblock->fields[i].access_flags, ptr, len);
		READ_TYPE_INDEX(name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
		READ_TYPE_INDEX(type_idx, constant_pool, CONSTANT_Utf8, ptr, len);
		classblock->fields[i].name = CP_UTF8(constant_pool, name_idx);
		classblock->fields[i].type = CP_UTF8(constant_pool, type_idx);
		classblock->fields[i].annotations = NULL;
		classblock->fields[i].signature = NULL;
		classblock->fields[i].constant = 0;

		READ_U2(attr_count, ptr, len);
		for (; attr_count != 0; attr_count--) {
			u2 attr_name_idx;
			char *attr_name;
			u4 attr_length;

			READ_TYPE_INDEX(attr_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
			attr_name = CP_UTF8(constant_pool, attr_name_idx);
			READ_U4(attr_length, ptr, len);

			if (attr_name == SYMBOL(ConstantValue)) {
				READ_INDEX(classblock->fields[i].constant, ptr, len);
			} else if (attr_name == SYMBOL(Signature)) {
				u2 signature_idx;
				READ_TYPE_INDEX(signature_idx, constant_pool, CONSTANT_Utf8, ptr, len);
				classblock->fields[i].signature
						= CP_UTF8(constant_pool, signature_idx);
			} else if (attr_name == SYMBOL(RuntimeVisibleAnnotations)) {
				classblock->fields[i].annotations = sysMalloc(
						sizeof(AnnotationData));
				classblock->fields[i].annotations->len = attr_length;
				classblock->fields[i].annotations->data
						= sysMalloc(attr_length);
				xi_mem_copy(classblock->fields[i].annotations->data, ptr,
						attr_length);
				ptr += attr_length;
			} else {
				ptr += attr_length;
			}
		}
	}

	READ_U2(classblock->methods_count, ptr, len);

	classblock->methods = sysMalloc(
			classblock->methods_count * sizeof(MethodBlock));

	xi_mem_set(classblock->methods, 0,
			classblock->methods_count * sizeof(MethodBlock));

	for (i = 0; i < classblock->methods_count; i++) {
		MethodBlock *method = &classblock->methods[i];
		MethodAnnotationData annos;
		u2 name_idx, type_idx;

		xi_mem_set(&annos, 0, sizeof(MethodAnnotationData));

		READ_U2(method->access_flags, ptr, len);
		READ_TYPE_INDEX(name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
		READ_TYPE_INDEX(type_idx, constant_pool, CONSTANT_Utf8, ptr, len);

		method->name = CP_UTF8(constant_pool, name_idx);
		method->type = CP_UTF8(constant_pool, type_idx);

		READ_U2(attr_count, ptr, len);
		for (; attr_count != 0; attr_count--) {
			u2 attr_name_idx;
			char *attr_name;
			u4 attr_length;

			READ_TYPE_INDEX(attr_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
			READ_U4(attr_length, ptr, len);
			attr_name = CP_UTF8(constant_pool, attr_name_idx);

			if (attr_name == SYMBOL(Code)) {
				u4 code_length;
				u2 code_attr_cnt;
				int j;

				READ_U2(method->max_stack, ptr, len);
				READ_U2(method->max_locals, ptr, len);

				READ_U4(code_length, ptr, len);
				method->code = sysMalloc(code_length);
				xi_mem_copy(method->code, ptr, code_length);
				ptr += code_length;

				method->code_size = code_length;

				READ_U2(method->exception_table_size, ptr, len);
				method->exception_table = sysMalloc(
						method->exception_table_size
								* sizeof(ExceptionTableEntry));

				for (j = 0; j < method->exception_table_size; j++) {
					ExceptionTableEntry *entry = &method->exception_table[j];

					READ_U2(entry->start_pc, ptr, len);
					READ_U2(entry->end_pc, ptr, len);
					READ_U2(entry->handler_pc, ptr, len);
					READ_U2(entry->catch_type, ptr, len);
				}

				READ_U2(code_attr_cnt, ptr, len);
				for (; code_attr_cnt != 0; code_attr_cnt--) {
					u2 attr_name_idx;
					u4 attr_length;

					READ_TYPE_INDEX(attr_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
					attr_name = CP_UTF8(constant_pool, attr_name_idx);
					READ_U4(attr_length, ptr, len);

					if (attr_name == SYMBOL(LineNumberTable)) {
						READ_U2(method->line_no_table_size, ptr, len);
						method->line_no_table = sysMalloc(
								method->line_no_table_size
										* sizeof(LineNoTableEntry));

						for (j = 0; j < method->line_no_table_size; j++) {
							LineNoTableEntry *entry = &method->line_no_table[j];

							READ_U2(entry->start_pc, ptr, len);
							READ_U2(entry->line_no, ptr, len);
						}
					} else
						ptr += attr_length;
				}
			} else if (attr_name == SYMBOL(Exceptions)) {
				int j;

				READ_U2(method->throw_table_size, ptr, len);
				method->throw_table = sysMalloc(
						method->throw_table_size * sizeof(u2));
				for (j = 0; j < method->throw_table_size; j++) {
					READ_U2(method->throw_table[j], ptr, len);
				}
			} else if (attr_name == SYMBOL(Signature)) {
				u2 signature_idx;
				READ_TYPE_INDEX(signature_idx, constant_pool, CONSTANT_Utf8, ptr, len);
				method->signature = CP_UTF8(constant_pool, signature_idx);
			} else if (attr_name == SYMBOL(RuntimeVisibleAnnotations)) {
				annos.annotations = sysMalloc(sizeof(AnnotationData));
				annos.annotations->len = attr_length;
				annos.annotations->data = sysMalloc(attr_length);
				xi_mem_copy(annos.annotations->data, ptr, attr_length);
				ptr += attr_length;
			} else if (attr_name == SYMBOL(RuntimeVisibleParameterAnnotations)) {
				annos.parameters = sysMalloc(sizeof(AnnotationData));
				annos.parameters->len = attr_length;
				annos.parameters->data = sysMalloc(attr_length);
				xi_mem_copy(annos.parameters->data, ptr, attr_length);
				ptr += attr_length;
			} else if (attr_name == SYMBOL(AnnotationDefault)) {
				annos.dft_val = sysMalloc(sizeof(AnnotationData));
				annos.dft_val->len = attr_length;
				annos.dft_val->data = sysMalloc(attr_length);
				xi_mem_copy(annos.dft_val->data, ptr, attr_length);
				ptr += attr_length;
			} else {
				ptr += attr_length;
			}
		}
		if (annos.annotations != NULL || annos.parameters != NULL
				|| annos.dft_val != NULL) {
			method->annotations = sysMalloc(sizeof(MethodAnnotationData));
			xi_mem_copy(method->annotations, &annos,
					sizeof(MethodAnnotationData));
		}
	}

	READ_U2(attr_count, ptr, len);
	for (; attr_count != 0; attr_count--) {
		u2 attr_name_idx;
		char *attr_name;
		u4 attr_length;

		READ_TYPE_INDEX(attr_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
		attr_name = CP_UTF8(constant_pool, attr_name_idx);
		READ_U4(attr_length, ptr, len);

		if (attr_name == SYMBOL(SourceFile)) {
			u2 file_name_idx;
			READ_TYPE_INDEX(file_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
			classblock->source_file_name
					= CP_UTF8(constant_pool, file_name_idx);
		} else if (attr_name == SYMBOL(InnerClasses)) {
			int j, size;
			READ_U2(size, ptr, len);
			{
				u2 inner_classes[size];
				for (j = 0; j < size; j++) {
					int inner, outer;
					READ_TYPE_INDEX(inner, constant_pool, CONSTANT_Class, ptr, len);
					READ_TYPE_INDEX(outer, constant_pool, CONSTANT_Class, ptr, len);

					if (inner == this_idx) {
						int inner_name_idx;

						/* A member class doesn't have an EnclosingMethod attribute, so set
						 the enclosing class to be the same as the declaring class */
						if (outer)
							classblock->declaring_class
									= classblock->enclosing_class = outer;

						READ_TYPE_INDEX(inner_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
						if (inner_name_idx == 0)
							classblock->flags |= ANONYMOUS;

						READ_U2(classblock->inner_access_flags, ptr, len);
					} else {
						ptr += 4;
						if (outer == this_idx)
							inner_classes[classblock->inner_class_count++]
									= inner;
					}
				}

				if (classblock->inner_class_count) {
					classblock->inner_classes = sysMalloc(
							classblock->inner_class_count * sizeof(u2));
					xi_mem_copy(classblock->inner_classes, &inner_classes[0],
							classblock->inner_class_count * sizeof(u2));
				}
			}
		} else if (attr_name == SYMBOL(EnclosingMethod)) {
			READ_TYPE_INDEX(classblock->enclosing_class, constant_pool, CONSTANT_Class, ptr, len);
			READ_TYPE_INDEX(classblock->enclosing_method, constant_pool, CONSTANT_NameAndType, ptr, len);
		} else if (attr_name == SYMBOL(Signature)) {
			u2 signature_idx;
			READ_TYPE_INDEX(signature_idx, constant_pool, CONSTANT_Utf8, ptr, len);
			classblock->signature = CP_UTF8(constant_pool, signature_idx);
		} else if (attr_name == SYMBOL(Synthetic)) {
			classblock->access_flags |= ACC_SYNTHETIC;
		} else if (attr_name == SYMBOL(RuntimeVisibleAnnotations)) {
			classblock->annotations = sysMalloc(sizeof(AnnotationData));
			classblock->annotations->len = attr_length;
			classblock->annotations->data = sysMalloc(attr_length);
			xi_mem_copy(classblock->annotations->data, ptr, attr_length);
			ptr += attr_length;
		} else {
			ptr += attr_length;
		}
	}

	classblock->super = super_idx ? resolveClass(class, super_idx, FALSE)
			: NULL;

	if (exceptionOccurred())
		return NULL;

	classblock->state = CLASS_LOADED;

	if ((found = addClassToHash(class, class_loader)) != class) {
		classblock->flags = CLASS_CLASH;
		if (class_loader != NULL) {
			signalException(java_lang_LinkageError, "duplicate class definition");
			return NULL;
		}
		return found;
	}

	return class;
}

Class *createArrayClass(char *classname, Object *class_loader) {
	ClassBlock *elem_cb, *classblock;
	Class *class, *found = NULL;
	int len = xi_strlen(classname);

	if ((class = allocClass()) == NULL) {
		return NULL;
	}

	classblock = CLASS_CB(class);

	classblock->name = copyUtf8(classname);
	classblock->super_name = SYMBOL(java_lang_Object);
	classblock->super = findSystemClass0(SYMBOL(java_lang_Object));
	classblock->method_table = CLASS_CB(classblock->super)->method_table;

	classblock->interfaces_count = 2;
	classblock->interfaces = sysMalloc(sizeof(Class*) * 2);
	classblock->interfaces[0] = findSystemClass0(SYMBOL(java_lang_Cloneable));
	classblock->interfaces[1] = findSystemClass0(SYMBOL(java_io_Serializable));

	classblock->state = CLASS_ARRAY;

	/* Find the array element class and the dimension --
	 this is used to speed up type checking (instanceof) */

	if (classname[1] == '[') {
		Class *comp_class = findArrayClassFromClassLoader(classname + 1,
				class_loader);

		if (comp_class == NULL) {
			goto error;
		}

		classblock->element_class = CLASS_CB(comp_class)->element_class;
		classblock->dim = CLASS_CB(comp_class)->dim + 1;
	} else {
		if (classname[1] == 'L') {
			char element_name[len - 2];

			xi_mem_copy(element_name, classname + 2, len - 3);
			element_name[len - 3] = '\0';

			classblock->element_class = findClassFromClassLoader(element_name,
					class_loader);
		} else {
			classblock->element_class = findPrimitiveClass(classname[1]);
		}

		if (classblock->element_class == NULL) {
			goto error;
		}

		classblock->dim = 1;
	}

	elem_cb = CLASS_CB(classblock->element_class);

	/* The array's classloader is the loader of the element class */
	classblock->class_loader = elem_cb->class_loader;

	/* The array's visibility (i.e. public, etc.) is that of the element */
	classblock->access_flags = (elem_cb->access_flags & ~ACC_INTERFACE)
			| ACC_FINAL | ACC_ABSTRACT;

	prepareClass(class);

	if ((found = addClassToHash(class, classblock->class_loader)) == class) {
		if (verbose) {
			jam_printf("[Created array class %s]\n", classname);
		}
		return class;
	}

	error: // ERROR GOTO LABEL
	classblock->flags = CLASS_CLASH;

	return found;
}

Class *createPrimClass(char *classname, int index) {
	Class *class;
	ClassBlock *classblock;

	if ((class = allocClass()) == NULL)
		return NULL;

	classblock = CLASS_CB(class);
	classblock->name = classname;
	classblock->state = CLASS_PRIM + index;
	classblock->access_flags = ACC_PUBLIC | ACC_FINAL | ACC_ABSTRACT;

	prepareClass(class);

	lockHashTable(boot_classes);
	if (prim_classes[index] == NULL) {
		prim_classes[index] = class;
	}
	unlockHashTable(boot_classes);

	if (verbose) {
		jam_printf("[Created primitive class %s]\n", classname);
	}

	return prim_classes[index];
}

/* Layout the instance data.

 The object layout places 64-bit fields on a double-word boundary
 as on some architectures this leads to better performance (and
 misaligned loads/store may cause traps).

 Reference fields are also placed together as these must be scanned
 during GC, and placing them together reduces the number of entries
 required in the reference offsets list.

 Double/long fields are layed out first, then references and finally
 int-sized fields.  When padding is needed for 64-bit fields we try
 to place an int-sized field, and only leave a hole when no int-sized
 fields are available */

void prepareFields(Class *class) {
	ClassBlock *cb = CLASS_CB(class);
	Class *super = (cb->access_flags & ACC_INTERFACE) ? NULL : cb->super;

	RefsOffsetsEntry *spr_rfs_offsts_tbl = NULL;
	int spr_rfs_offsts_sze = 0;

	FieldBlock *ref_head = NULL;
	FieldBlock *int_head = NULL;
	FieldBlock *dbl_head = NULL;

	int field_offset = sizeof(Object);
	int refs_start_offset = 0;
	int refs_end_offset = 0;
	int i;

	if (super != NULL) {
		field_offset = CLASS_CB(super)->object_size;
		spr_rfs_offsts_sze = CLASS_CB(super)->refs_offsets_size;
		spr_rfs_offsts_tbl = CLASS_CB(super)->refs_offsets_table;
	}

	/* Initialise static fields to default value, and separate
	 instance fields into three linked lists, holding
	 int-sized fields, double-sized fields and reference
	 fields */

	for (i = 0; i < cb->fields_count; i++) {
		FieldBlock *fb = &cb->fields[i];

		if (fb->access_flags & ACC_STATIC) {
			fb->u.static_value.l = 0;
		} else {
			FieldBlock **list;

			if (fb->type[0] == 'L' || fb->type[0] == '[') {
				list = &ref_head;
			} else if (fb->type[0] == 'J' || fb->type[0] == 'D') {
				list = &dbl_head;
			} else {
				list = &int_head;
			}

			fb->u.static_value.p = *list;
			*list = fb;
		}

		fb->class = class;
	}

	/* Layout the double-sized fields.  If padding is required,
	 use the first int-sized field (int_list head), or leave
	 a hole if no int-fields */

	if (dbl_head != NULL) {
		if (field_offset & 0x7) {
			if (int_head != NULL) {
				FieldBlock *fb = int_head;
				int_head = int_head->u.static_value.p;
				fb->u.offset = field_offset;
			}
			field_offset += 4;
		}

		do {
			FieldBlock *fb = dbl_head;
			dbl_head = dbl_head->u.static_value.p;
			fb->u.offset = field_offset;
			field_offset += 8;
		} while (dbl_head != NULL);
	}

	/* Layout the reference fields.  If padding is required,
	 use an int-sized field (int_list head), or leave
	 a hole if no int-fields remaining */

	if (ref_head != NULL) {
		if (sizeof(Object*) == 8 && field_offset & 0x7) {
			if (int_head != NULL) {
				FieldBlock *fb = int_head;
				int_head = int_head->u.static_value.p;
				fb->u.offset = field_offset;
			}
			field_offset += 4;
		}

		refs_start_offset = field_offset;

		do {
			FieldBlock *fb = ref_head;
			ref_head = ref_head->u.static_value.p;
			fb->u.offset = field_offset;
			field_offset += sizeof(Object*);
		} while (ref_head != NULL);

		refs_end_offset = field_offset;
	}

	/* Layout the remaining int-sized fields */

	while (int_head != NULL) {
		FieldBlock *fb = int_head;
		int_head = int_head->u.static_value.p;
		fb->u.offset = field_offset;
		field_offset += 4;
	}

	cb->object_size = field_offset;

	/* Construct the reference offsets list.  This is used to speed up
	 scanning of an objects references during the mark phase of GC.
	 If possible, merge the entry with the previous entry */

	if (refs_start_offset) {
		if (spr_rfs_offsts_sze > 0
				&& spr_rfs_offsts_tbl[spr_rfs_offsts_sze - 1].end
						== refs_start_offset) {
			cb->refs_offsets_size = spr_rfs_offsts_sze;
			refs_start_offset
					= spr_rfs_offsts_tbl[spr_rfs_offsts_sze - 1].start;
		} else {
			cb->refs_offsets_size = spr_rfs_offsts_sze + 1;
		}

		cb->refs_offsets_table = sysMalloc(
				cb->refs_offsets_size * sizeof(RefsOffsetsEntry));

		xi_mem_copy(cb->refs_offsets_table, spr_rfs_offsts_tbl,
				spr_rfs_offsts_sze * sizeof(RefsOffsetsEntry));

		cb->refs_offsets_table[cb->refs_offsets_size - 1].start
				= refs_start_offset;
		cb->refs_offsets_table[cb->refs_offsets_size - 1].end = refs_end_offset;
	} else {
		cb->refs_offsets_size = spr_rfs_offsts_sze;
		cb->refs_offsets_table = spr_rfs_offsts_tbl;
	}
}

#define MRNDA_CACHE_SZE 10

#define resizeMTable(method_table, method_table_size, miranda, count)  \
{                                                                      \
    method_table = (MethodBlock**)sysRealloc(method_table,             \
                  (method_table_size + count) * sizeof(MethodBlock*)); \
                                                                       \
    xi_mem_copy(&method_table[method_table_size], miranda,             \
                               count * sizeof(MethodBlock*));          \
    method_table_size += count;                                        \
}

#define fillinMTable(method_table, methods, methods_count)             \
{                                                                      \
    int i;                                                             \
    for(i = 0; i < methods_count; i++, methods++) {                    \
        if((methods->access_flags & (ACC_STATIC | ACC_PRIVATE)) ||     \
               (methods->name[0] == '<'))                              \
            continue;                                                  \
        method_table[methods->method_table_index] = methods;           \
    }                                                                  \
}

void linkClass(Class *class) {
	static MethodBlock *obj_fnlzr_mthd = NULL;

	ClassBlock *cb = CLASS_CB(class);
	Class *super = (cb->access_flags & ACC_INTERFACE) ? NULL : cb->super;

	ITableEntry *spr_imthd_tbl = NULL;
	MethodBlock **method_table = NULL;
	MethodBlock **spr_mthd_tbl = NULL;
	MethodBlock *finalizer;
	MethodBlock *mb;
	FieldBlock *fb;

	int new_methods_count = 0;
	int spr_imthd_tbl_sze = 0;
	int itbl_offset_count = 0;
	int spr_mthd_tbl_sze = 0;
	int method_table_size;
	int new_itable_count;
	int itbl_idx, i, j;
	int spr_flags = 0;

	if (cb->state >= CLASS_LINKED)
		return;

	objectLock(class);

	if (cb->state >= CLASS_LINKED)
		goto unlock;

	if (verbose)
		jam_printf("[Linking class %s]\n", cb->name);

	if (super) {
		ClassBlock *super_cb = CLASS_CB(super);
		if (super_cb->state < CLASS_LINKED)
			linkClass(super);

		spr_flags = super_cb->flags;
		spr_mthd_tbl = super_cb->method_table;
		spr_imthd_tbl = super_cb->imethod_table;
		spr_mthd_tbl_sze = super_cb->method_table_size;
		spr_imthd_tbl_sze = super_cb->imethod_table_size;
	}

	/* Calculate object layout */

	prepareFields(class);

	/* prepare methods */

	for (mb = cb->methods, i = 0; i < cb->methods_count; i++, mb++) {
		/* calculate argument count from signature */

		int count = 0;
		char *sig = mb->type;
		SCAN_SIG(sig, count+=2, count++);

		if (mb->access_flags & ACC_STATIC) {
			mb->args_count = count;
		} else {
			mb->args_count = count + 1;
		}

		mb->class = class;

		/* Set abstract method to stub */
		if (mb->access_flags & ACC_ABSTRACT) {
			mb->code_size = sizeof(abstract_method);
			mb->code = abstract_method;
		}

		if (mb->access_flags & ACC_NATIVE) {
			/* set up native invoker to wrapper to resolve function
			 on first invocation */
			mb->native_invoker = &resolveNativeWrapper;

			/* native methods have no code attribute so these aren't filled
			 in at load time - as these values are used when creating frame
			 set to appropriate values */
			mb->max_locals = mb->args_count;
			mb->max_stack = 0;
		} else {
			/* Set the bottom bit of the pointer to indicate the
			 method is unprepared */
			mb->code = ((char*) mb->code) + 1;
		}

		/* Static, private or init methods aren't dynamically invoked, so
		 don't stick them in the table to save space */

		if ((mb->access_flags & (ACC_STATIC | ACC_PRIVATE)) || (mb->name[0]
				== '<')) {
			continue;
		}

		/* if it's overriding an inherited method, replace in method table */

		for (j = 0; j < spr_mthd_tbl_sze; j++)
			if (mb->name == spr_mthd_tbl[j]->name && mb->type
					== spr_mthd_tbl[j]->type && checkMethodAccess(
					spr_mthd_tbl[j], class)) {
				mb->method_table_index = spr_mthd_tbl[j]->method_table_index;
				break;
			}

		if (j == spr_mthd_tbl_sze) {
			mb->method_table_index = spr_mthd_tbl_sze + new_methods_count++;
		}
	}

	/* construct method table */

	method_table_size = spr_mthd_tbl_sze + new_methods_count;

	if (!(cb->access_flags & ACC_INTERFACE)) {
		method_table = sysMalloc(method_table_size * sizeof(MethodBlock*));

		/* Copy parents method table to the start */
		xi_mem_copy(method_table, spr_mthd_tbl,
				spr_mthd_tbl_sze * sizeof(MethodBlock*));

		/* fill in the additional methods -- we use a
		 temporary because fillinMtable alters mb */
		mb = cb->methods;
		fillinMTable(method_table, mb, cb->methods_count);
	}

	/* setup interface method table */

	/* number of interfaces implemented by this class is those implemented by
	 * parent, plus number of interfaces directly implemented by this class,
	 * and the total number of their superinterfaces */

	new_itable_count = cb->interfaces_count;
	for (i = 0; i < cb->interfaces_count; i++) {
		new_itable_count += CLASS_CB(cb->interfaces[i])->imethod_table_size;
	}

	cb->imethod_table_size = spr_imthd_tbl_sze + new_itable_count;
	cb->imethod_table = sysMalloc(cb->imethod_table_size * sizeof(ITableEntry));

	/* copy parent's interface table - the offsets into the method table won't change */

	xi_mem_copy(cb->imethod_table, spr_imthd_tbl,
			spr_imthd_tbl_sze * sizeof(ITableEntry));

	/* now run through the extra interfaces implemented by this class,
	 * fill in the interface part, and calculate the number of offsets
	 * needed (i.e. the number of methods defined in the interfaces) */

	itbl_idx = spr_imthd_tbl_sze;
	for (i = 0; i < cb->interfaces_count; i++) {
		Class *intf = cb->interfaces[i];
		ClassBlock *intf_cb = CLASS_CB(intf);

		cb->imethod_table[itbl_idx++].interface = intf;
		itbl_offset_count += intf_cb->method_table_size;

		for (j = 0; j < intf_cb->imethod_table_size; j++) {
			Class *spr_intf = intf_cb->imethod_table[j].interface;

			cb->imethod_table[itbl_idx++].interface = spr_intf;
			itbl_offset_count += CLASS_CB(spr_intf)->method_table_size;
		}
	}

	/* if we're an interface all finished - offsets aren't used */

	if (!(cb->access_flags & ACC_INTERFACE)) {
		int *offsets_pntr = sysMalloc(itbl_offset_count * sizeof(int));
		int old_mtbl_size = method_table_size;
		MethodBlock *miranda[MRNDA_CACHE_SZE];
		int miranda_count = 0;
		int mtbl_idx;

		/* run through table again, this time filling in the offsets array -
		 * for each new interface, run through it's methods and locate
		 * each method in this classes method table */

		for (i = spr_imthd_tbl_sze; i < cb->imethod_table_size; i++) {
			ClassBlock *intf_cb = CLASS_CB(cb->imethod_table[i].interface);
			cb->imethod_table[i].offsets = offsets_pntr;

			for (j = 0; j < intf_cb->methods_count; j++) {
				MethodBlock *intf_mb = &intf_cb->methods[j];

				if ((intf_mb->access_flags & (ACC_STATIC | ACC_PRIVATE))
						|| (intf_mb->name[0] == '<')) {
					continue;
				}

				/* We scan backwards so that we find methods defined in sub-classes
				 before super-classes.  This ensures we find non-overridden
				 methods before the inherited non-accessible method */
				for (mtbl_idx = method_table_size - 1; mtbl_idx >= 0; mtbl_idx--)
					if (intf_mb->name == method_table[mtbl_idx]->name
							&& intf_mb->type == method_table[mtbl_idx]->type) {
						*offsets_pntr++ = mtbl_idx;
						break;
					}

				if (mtbl_idx < 0) {

					/* didn't find it - add a dummy abstract method (a so-called
					 miranda method).  If it's invoked we'll get an abstract
					 method error */

					int k;
					for (k = 0; k < miranda_count; k++)
						if (intf_mb->name == miranda[k]->name && intf_mb->type
								== miranda[k]->type)
							break;

					*offsets_pntr++ = method_table_size + k;

					if (k == miranda_count) {
						if (miranda_count == MRNDA_CACHE_SZE) {
							resizeMTable(method_table, method_table_size, miranda, MRNDA_CACHE_SZE);
							miranda_count = 0;
						}
						miranda[miranda_count++] = intf_mb;
					}
				}
			}
		}

		if (miranda_count > 0) {
			resizeMTable(method_table, method_table_size, miranda, miranda_count);
		}

		if (old_mtbl_size != method_table_size) {
			/* We've created some abstract methods */
			int num_mirandas = method_table_size - old_mtbl_size;

			mb = (MethodBlock *) sysRealloc(cb->methods,
					(cb->methods_count + num_mirandas) * sizeof(MethodBlock));

			/* If the realloc of the method list gave us a new pointer, the pointers
			 to them in the method table are now wrong. */
			if (mb != cb->methods) {
				/*  mb will be left pointing to the end of the methods */
				cb->methods = mb;
				fillinMTable(method_table, mb, cb->methods_count);
			} else {
				mb += cb->methods_count;
			}

			cb->methods_count += num_mirandas;

			/* Now we've expanded the method list, replace pointers to
			 the interface methods. */

			for (i = old_mtbl_size; i < method_table_size; i++, mb++) {
				xi_mem_copy(mb, method_table[i], sizeof(MethodBlock));
				mb->access_flags |= ACC_MIRANDA;
				mb->method_table_index = i;
				mb->class = class;
				method_table[i] = mb;
			}
		}
	}

	cb->method_table = method_table;
	cb->method_table_size = method_table_size;

	/* Handle finalizer */

	/* If this is Object find the finalize method.  All subclasses will
	 have it in the same place in the method table.  Note, Object
	 should always have a valid finalizer -- but check just in case */

	if (cb->super == NULL) {
		finalizer = findMethod(class, SYMBOL(finalize), SYMBOL(___V));
		if (finalizer
				&& !(finalizer->access_flags & (ACC_STATIC | ACC_PRIVATE))) {
			finalize_mtbl_idx = finalizer->method_table_index;
			obj_fnlzr_mthd = finalizer;
		}
	}

	cb->flags |= spr_flags;

	/* Store the finalizer only if it's overridden Object's.  We don't
	 want to finalize every object, and Object's imp is empty */

	if (super && obj_fnlzr_mthd && (finalizer
			= method_table[obj_fnlzr_mthd->method_table_index])
			!= obj_fnlzr_mthd) {
		cb->flags |= FINALIZED;
	}

	/* Handle reference classes */

	if (ref_referent_offset == -1 && cb->name
			== SYMBOL(java_lang_ref_Reference)) {
		FieldBlock *ref_fb = findField(class, SYMBOL(referent),
				SYMBOL(sig_java_lang_Object));
		FieldBlock *queue_fb = findField(class, SYMBOL(queue),
				SYMBOL(sig_java_lang_ref_ReferenceQueue));
		MethodBlock *enqueue_mb = findMethod(class, SYMBOL(enqueue),
				SYMBOL(___Z));

		if (ref_fb == NULL || queue_fb == NULL || enqueue_mb == NULL) {
			log_error(XDLOG, "Expected fields/methods missing in java.lang.ref.Reference\n");
			exitVM(1);
		}

		for (fb = cb->fields, i = 0; i < cb->fields_count; i++, fb++) {
			if (fb->u.offset > ref_fb->u.offset) {
				fb->u.offset -= sizeof(Object*);
			}
		}

		ref_referent_offset = ref_fb->u.offset = cb->object_size
				- sizeof(Object*);
		cb->refs_offsets_table[cb->refs_offsets_size - 1].end
				-= sizeof(Object*);

		enqueue_mtbl_idx = enqueue_mb->method_table_index;
		ref_queue_offset = queue_fb->u.offset;

		cb->flags |= REFERENCE;
	}

	if (spr_flags & REFERENCE) {
		if (cb->name == SYMBOL(java_lang_ref_SoftReference)) {
			cb->flags |= SOFT_REFERENCE;
		} else if (cb->name == SYMBOL(java_lang_ref_WeakReference)) {
			cb->flags |= WEAK_REFERENCE;
		} else if (cb->name == SYMBOL(java_lang_ref_PhantomReference)) {
			cb->flags |= PHANTOM_REFERENCE;
		}
	}

	/* Handle class loader classes */

	if (ldr_vmdata_offset == -1 && cb->name == SYMBOL(java_lang_ClassLoader)) {
		FieldBlock *ldr_fb = findField(class, SYMBOL(vmdata),
				SYMBOL(sig_java_lang_Object));

		if (ldr_fb == NULL) {
			log_error(XDLOG, "Expected vmdata field missing in java.lang.ClassLoader\n");
			exitVM(1);
		}

		ldr_vmdata_offset = ldr_fb->u.offset;
		cb->flags |= CLASS_LOADER;
	}

	cb->state = CLASS_LINKED;

	unlock: objectUnlock(class);
}

Class *initClass(Class *class) {
	ClassBlock *cb = CLASS_CB(class);
	ConstantPool *cp = &cb->constant_pool;
	FieldBlock *fb = cb->fields;
	MethodBlock *mb;
	Object *excep;
	int state, i;

	if (cb->state >= CLASS_INITED) {
		return class;
	}

	linkClass(class);
	objectLock(class);

	while (cb->state == CLASS_INITING)
		if (cb->initing_tid == threadSelf()->id) {
			objectUnlock(class);
			return class;
		} else {
			/* FALSE means this wait is non-interruptible.
			 An interrupt will appear as if the initialiser
			 failed (below), and clearing will lose the
			 interrupt status */
			objectWait0(class, 0, 0, FALSE);
		}

	if (cb->state >= CLASS_INITED) {
		objectUnlock(class);
		return class;
	}

	if (cb->state == CLASS_BAD) {
		objectUnlock(class);
		signalException(java_lang_NoClassDefFoundError, cb->name);
		return NULL;
	}

	cb->state = CLASS_INITING;
	cb->initing_tid = threadSelf()->id;

	objectUnlock(class);

	if (!(cb->access_flags & ACC_INTERFACE) && cb->super
			&& (CLASS_CB(cb->super)->state != CLASS_INITED)) {
		initClass(cb->super);
		if (exceptionOccurred()) {
			state = CLASS_BAD;
			goto set_state_and_notify;
		}
	}

	/* Never used to bother with this as only static finals use it and
	 the constant value's copied at compile time.  However, separate
	 compilation can result in a getstatic to a (now) constant field,
	 and the VM didn't initialise it... */

	for (i = 0; i < cb->fields_count; i++, fb++)
		if ((fb->access_flags & ACC_STATIC) && fb->constant) {
			if ((*fb->type == 'J') || (*fb->type == 'D')) {
				fb->u.static_value.l = *(u8*) &(CP_INFO(cp, fb->constant));
			} else {
				fb->u.static_value.u = resolveSingleConstant(class,
						fb->constant);
			}
		}

	if ((mb = findMethod(class, SYMBOL(class_init), SYMBOL(___V))) != NULL) {
		executeStaticMethod(class, mb);
	}

	if ((excep = exceptionOccurred())) {
		Class *error, *eiie;

		clearException();

		/* Don't wrap exceptions of type java.lang.Error... */
		if ((error = findSystemClass0(SYMBOL(java_lang_Error)))
				&& !isInstanceOf(error, excep->class)
				&& (eiie = findSystemClass(
						SYMBOL(java_lang_ExceptionInInitializerError))) && (mb
				= findMethod(eiie, SYMBOL(object_init),
						SYMBOL(_java_lang_Throwable__V)))) {

			Object *ob = allocObject(eiie);

			if (ob != NULL) {
				executeMethod(ob, mb, excep);
				setException(ob);
			}
		} else {
			setException(excep);
		}

		state = CLASS_BAD;
	} else {
		state = CLASS_INITED;
	}

	set_state_and_notify: objectLock(class);
	cb->state = state;

	objectNotifyAll(class);
	objectUnlock(class);

	return state == CLASS_BAD ? NULL : class;
}

char *findFileEntry(char *path, int *file_len) {
	int read_len;
	char *data;
	int fd;

	fd = xi_file_open(path, XI_FILE_MODE_READ, 0777);
	if (fd < 0) {
		return NULL;
	}

	*file_len = (xint32) xi_file_seek(fd, 0, XI_FILE_SEEK_END);
	xi_file_seek(fd, 0, XI_FILE_SEEK_SET);

	data = sysMalloc(*file_len);
	read_len = xi_file_read(fd, data, *file_len);
	xi_file_close(fd);

	if (read_len == *file_len) {
		return data;
	} else {
		sysFree(data);
		return NULL;
	}
}

void defineBootPackage(char *classname, int index) {
	char *last_slash = xi_strrchr(classname, XI_SEP_FILE_C);

	if (last_slash != NULL && last_slash != classname) {
		int len = last_slash - classname + 1;
		PackageEntry *package = sysMalloc(sizeof(PackageEntry) + len);
		PackageEntry *hashed;

		package->index = index;
		slash2dots2buff(classname, package->name, len);

#undef HASH
#undef COMPARE
#define HASH(ptr) utf8Hash(((PackageEntry*)ptr)->name)
#define COMPARE(ptr1, ptr2, hash1, hash2) (hash1 == hash2 && \
            utf8Comp(((PackageEntry*)ptr1)->name, ((PackageEntry*)ptr2)->name))

		// Add if absent, no scavenge, locked
		findHashEntry(boot_packages, package, hashed, TRUE, FALSE, TRUE);

		if (package != hashed) {
			sysFree(package);
		}
	}
}

Class *loadSystemClass(char *classname) {
	int file_len, fname_len = xi_strlen(classname) + 8;
	char buff[max_cp_element_len + fname_len];
	char filename[fname_len];
	Class *class = NULL;
	char *data = NULL;
	int i;

	filename[0] = XI_SEP_FILE_C;
	xi_strcat(xi_strcpy(&filename[1], classname), ".class");

	for (i = 0; i < bcp_entries && data == NULL; i++) {
		if (bootclasspath[i].zip) {
			data = findArchiveEntry(filename + 1, bootclasspath[i].zip,
					&file_len);
		} else {
			data
					= findFileEntry(
							xi_strcat(xi_strcpy(buff, bootclasspath[i].path),
									filename), &file_len);
		}
	}

	if (data == NULL) {
		// by jshwang
		if (verbose) {
			log_debug(XDLOG, "########################################################\n");
			log_debug(XDLOG, "########## %s is not in system class. Try to find it elsewhere\n", filename + 1);
			log_debug(XDLOG, "#########################################################\n\n");
		}
		signalException(java_lang_NoClassDefFoundError, classname);
		return NULL;
	}

	defineBootPackage(classname, i - 1);

	class = defineClass(classname, data, 0, file_len, NULL);
	sysFree(data);

	if (verbose && class) {
		jam_printf("[Loaded %s from %s]\n", classname, bootclasspath[i-1].path);
	}

	return class;
}

void addInitiatingLoaderToClass(Object *class_loader, Class *class) {
	ClassBlock *cb = CLASS_CB(class);

	/* The defining class loader is automatically an initiating
	 loader so don't add again */
	if (cb->class_loader != class_loader) {
		addClassToHash(class, class_loader);
	}
}

Class *findHashedClass(char *classname, Object *class_loader) {
	HashTable *table;
	Class *class;
	char *name;

	/* If the class name is not in the utf8 table it can't
	 have been loaded */
	if ((name = findUtf8(classname)) == NULL) {
		if (verbose) {
			log_debug(XDLOG, "      ---> Not found in HashedClass: %s\n", classname);
		}
		return NULL;
	}

	if (class_loader == NULL) {
		table = &boot_classes;
	} else {
		Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

		if (vmdata == NULL) {
			return NULL;
		}

		table = INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset);
	}

#undef HASH
#undef COMPARE
#define HASH(ptr) utf8Hash(ptr)
#define COMPARE(ptr1, ptr2, hash1, hash2) (hash1 == hash2) && \
            (ptr1 == CLASS_CB((Class *)ptr2)->name)

	// Do not add if absent, no scavenge, locked
	findHashEntry((*table), name, class, FALSE, FALSE, TRUE);

	return class;
}

Class *findSystemClass0(char *classname) {
	Class *class = findHashedClass(classname, NULL);

	if (class == NULL) {
		class = loadSystemClass(classname);
	}

	if (!exceptionOccurred()) {
		linkClass(class);
	}

	return class;
}

Class *findSystemClass(char *classname) {
	Class *class = findSystemClass0(classname);

	if (!exceptionOccurred()) {
		initClass(class);
	}

	return class;
}

Class *findArrayClassFromClassLoader(char *classname, Object *class_loader) {
	Class *class = findHashedClass(classname, class_loader);

	if (class == NULL) {
		if ((class = createArrayClass(classname, class_loader)) != NULL) {
			addInitiatingLoaderToClass(class_loader, class);
		}
	}
	return class;
}

Class *findPrimitiveClass(char prim_type) {
	int index;
	Class *prim;
	char *classname;

	switch (prim_type) {
	case 'Z':
		classname = SYMBOL(boolean);
		index = PRIM_IDX_BOOLEAN;
		break;
	case 'B':
		classname = SYMBOL(byte);
		index = PRIM_IDX_BYTE;
		break;
	case 'C':
		classname = SYMBOL(char);
		index = PRIM_IDX_CHAR;
		break;
	case 'S':
		classname = SYMBOL(short);
		index = PRIM_IDX_SHORT;
		break;
	case 'I':
		classname = SYMBOL(int);
		index = PRIM_IDX_INT;
		break;
	case 'F':
		classname = SYMBOL(float);
		index = PRIM_IDX_FLOAT;
		break;
	case 'J':
		classname = SYMBOL(long);
		index = PRIM_IDX_LONG;
		break;
	case 'D':
		classname = SYMBOL(double);
		index = PRIM_IDX_DOUBLE;
		break;
	case 'V':
		classname = SYMBOL(void);
		index = PRIM_IDX_VOID;
		break;
	default:
		signalException(java_lang_NoClassDefFoundError, NULL);
		return NULL;
		break;
	}

	prim = prim_classes[index];
	return prim ? prim : createPrimClass(classname, index);
}

Class *findNonArrayClassFromClassLoader(char *classname, Object *loader) {
	Class *class = findHashedClass(classname, loader);

	if (class == NULL) {
		char *dot_name = slash2dots(classname);
		Object *string = createString(dot_name);
		Object *excep;

		sysFree(dot_name);
		if (string == NULL) {
			return NULL;
		}

		if (loadClass_mtbl_idx == -1) {
			MethodBlock *mb = lookupMethod(loader->class, SYMBOL(loadClass),
					SYMBOL(_java_lang_String__java_lang_Class));
			if (mb == NULL) {
				return NULL;
			}

			loadClass_mtbl_idx = mb->method_table_index;
		}

		/* The public loadClass is not synchronized.
		 Lock the class-loader to be thread-safe */
		objectLock(loader);
		class
				= *(Class**) executeMethod(loader,
						CLASS_CB(loader->class)->method_table[loadClass_mtbl_idx], string);
		objectUnlock(loader);

		if ((excep = exceptionOccurred()) || class == NULL) {
			clearException();
			signalChainedException(java_lang_NoClassDefFoundError, classname, excep);
			return NULL;
		}

		addInitiatingLoaderToClass(loader, class);

		if (verbose && (CLASS_CB(class)->class_loader == loader)) {
			jam_printf("[Loaded %s]\n", classname);
		}
	}
	return class;
}

Class *findClassFromClassLoader(char *classname, Object *loader) {
	if (*classname == '[') {
		return findArrayClassFromClassLoader(classname, loader);
	}

	if (loader != NULL) {
		return findNonArrayClassFromClassLoader(classname, loader);
	}

	return findSystemClass0(classname);
}

Object *getSystemClassLoader() {
	Class *class_loader = findSystemClass(SYMBOL(java_lang_ClassLoader));

	if (verbose) {
		log_debug(XDLOG, "- - - - - - - > <getSystemClassLoader> [BEGIN]\n");
	}

	if (!exceptionOccurred()) {
		MethodBlock *mb;

		if ((mb = findMethod(class_loader, SYMBOL(getSystemClassLoader),
				SYMBOL(___java_lang_ClassLoader))) != NULL) {
			Object *system_loader;

			if (verbose) {
				log_debug(XDLOG, "- - - - - - - > <getSystemClassLoader> Finding system class loader ...\n");
			}
			system_loader = *(Object**) executeStaticMethod(class_loader, mb);
			if (verbose) {
				log_debug(XDLOG, "- - - - - - - > <getSystemClassLoader> %p\n", system_loader);
			}

			if (!exceptionOccurred()) {
				if (verbose) {
					log_debug(XDLOG, "- - - - - - - > Success in getSystemClassLoader!\n");
				}
				return system_loader;
			}
		}
	}
	if (verbose) {
		log_debug(XDLOG, "- - - - - - - > Fail to find SystemClassLoader!!\n");
	}
	return NULL;
}

Object *createBootPackage(PackageEntry *package_entry) {
	Object *name = createString(package_entry->name);

	if (name != NULL) {
		Object *package = *(Object**) executeStaticMethod(
				vm_loader_create_package->class,
				vm_loader_create_package, name,
				package_entry->index);

		if (!exceptionOccurred()) {
			return package;
		}
	}

	return NULL;
}

Object *bootPackage(char *package_name) {
	PackageEntry *hashed;

#undef HASH
#undef COMPARE
#define HASH(ptr) utf8Hash(ptr)
#define COMPARE(ptr1, ptr2, hash1, hash2) (hash1 == hash2 && \
                                 utf8Comp(ptr1, ((PackageEntry*)ptr2)->name))

	// Do not add if absent, no scavenge, locked
	findHashEntry(boot_packages, package_name, hashed, FALSE, FALSE, TRUE);

	if (hashed != NULL) {
		return createBootPackage(hashed);
	}

	return NULL;
}

#define ITERATE(ptr)                                       \
    if((data[--count] = createBootPackage(ptr)) == NULL) { \
        array = NULL;                                      \
        goto error;                                        \
    }

Object *bootPackages() {
	Object **data, *array;
	int count;

	lockHashTable(boot_packages);

	count = hashTableCount(boot_packages);
	if ((array = allocArray(package_array_class, count, sizeof(Object*)))
			== NULL) {
		goto error;
	}

	data = ARRAY_DATA(array, Object*);
	hashIterate(boot_packages);

	error: // ERROR GOTO LABEL
	unlockHashTable(boot_packages);

	return array;
}

/* gc support for marking classes */

#undef ITERATE
#define ITERATE(ptr) markRoot(ptr)

void markBootClasses() {
	int i;

	hashIterate(boot_classes);

	for (i = 0; i < MAX_PRIM_CLASSES; i++) {
		if (prim_classes[i] != NULL) {
			markRoot((Object*) prim_classes[i]);
		}
	}
}

#undef ITERATE
#define ITERATE(ptr)  threadReference((Object**)ptr)

void threadBootClasses() {
	int i;

	hashIterateP(boot_classes);

	for (i = 0; i < MAX_PRIM_CLASSES; i++) {
		if (prim_classes[i] != NULL) {
			threadReference((Object**) &prim_classes[i]);
		}
	}
}

#undef ITERATE
#define ITERATE(ptr)                                          \
    if(CLASS_CB((Class *)ptr)->class_loader == class_loader)  \
        markObject(ptr, mark)                                 \


void markLoaderClasses(Object *class_loader, int mark) {
	Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

	if (vmdata != NULL) {
		HashTable *table = INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset);
		hashIterate((*table));
	}
}

#undef ITERATE
#define ITERATE(ptr) threadReference((Object**)ptr)

void threadLoaderClasses(Object *class_loader) {
	Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

	if (vmdata != NULL) {
		HashTable *table = INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset);
		hashIterateP((*table));
	}
}

void freeClassData(Class *class) {
	ClassBlock *cb = CLASS_CB(class);
	int i;

	if (IS_ARRAY(cb)) {
		gcPendingFree(cb->interfaces);
		return;
	}

	gcPendingFree((void*) cb->constant_pool.type);
	gcPendingFree(cb->constant_pool.info);
	gcPendingFree(cb->interfaces);

	for (i = 0; i < cb->fields_count; i++) {
		FieldBlock *fb = &cb->fields[i];

		if (fb->annotations != NULL) {
			gcPendingFree(fb->annotations->data);
			gcPendingFree(fb->annotations);
		}
	}

	gcPendingFree(cb->fields);

	for (i = 0; i < cb->methods_count; i++) {
		MethodBlock *mb = &cb->methods[i];

		if (!((xuintptr) mb->code & 0x3)) {
			gcPendingFree(mb->code);
		} else {
			if (!(mb->access_flags & ACC_ABSTRACT)) {
				gcPendingFree((void*) ((xuintptr) mb->code & ~3));
			}
		}

		gcPendingFree(mb->exception_table);
		gcPendingFree(mb->line_no_table);
		gcPendingFree(mb->throw_table);

		if (mb->annotations != NULL) {
			if (mb->annotations->annotations != NULL) {
				gcPendingFree(mb->annotations->annotations->data);
				gcPendingFree(mb->annotations->annotations);
			}
			if (mb->annotations->parameters != NULL) {
				gcPendingFree(mb->annotations->parameters->data);
				gcPendingFree(mb->annotations->parameters);
			}
			if (mb->annotations->dft_val != NULL) {
				gcPendingFree(mb->annotations->dft_val->data);
				gcPendingFree(mb->annotations->dft_val);
			}
			gcPendingFree(mb->annotations);
		}
	}

	gcPendingFree(cb->methods);
	gcPendingFree(cb->inner_classes);

	if (cb->annotations != NULL) {
		gcPendingFree(cb->annotations->data);
		gcPendingFree(cb->annotations);
	}

	if (cb->state >= CLASS_LINKED) {
		ClassBlock *super_cb = CLASS_CB(cb->super);

		/* interfaces do not have a method table, or
		 imethod table offsets */
		if (!IS_INTERFACE(cb)) {
			int spr_imthd_sze = super_cb->imethod_table_size;

			gcPendingFree(cb->method_table);
			if (cb->imethod_table_size > spr_imthd_sze) {
				gcPendingFree(cb->imethod_table[spr_imthd_sze].offsets);
			}
		}

		gcPendingFree(cb->imethod_table);

		if (cb->refs_offsets_table != super_cb->refs_offsets_table) {
			gcPendingFree(cb->refs_offsets_table);
		}
	}
}

void freeClassLoaderData(Object *class_loader) {
	Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

	if (vmdata != NULL) {
		HashTable *table = INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset);
		gcFreeHashTable((*table));
		gcPendingFree(table);
	}
}

/* Add a library unloader object to the class loader for the
 library contained within entry.  The library has an unload
 function, which will be called from the unloader finalizer
 when the class loader is garbage collected */
void newLibraryUnloader(Object *class_loader, void *entry) {
	Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

	if (vmdata != NULL) {
		executeMethod(vmdata, ldr_new_unloader, (long long)(xuintptr)entry);
	}
}

int parseBootClassPath(char *cp_var) {
	char *cp;
	char *pntr;
	char *start;
	int i, j, len;
	int max = 0;

	xi_file_stat_t info;

	cp = sysMalloc(xi_strlen(cp_var) + 1);
	xi_strcpy(cp, cp_var);

	for (i = 0, start = pntr = cp; *pntr; pntr++) {
		if (*pntr == ':' || *pntr == ';') {
			if (start != pntr) {
				*pntr = '\0';
				i++;
			}
			start = pntr + 1;
		}
	}
	if (start != pntr) {
		i++;
	}

	bootclasspath = sysMalloc(sizeof(BCPEntry) * i);

	for (j = 0, pntr = cp; i > 0; i--) {
		while (*pntr == ':' || *pntr == ';') {
			pntr++;
		}

		start = pntr;
		pntr += (len = xi_strlen(pntr)) + 1;

		if (verbose) {
			log_trace(XDLOG, "Parsing bootclass : %s\n", start);
		}

		if (xi_file_stat(start, &info) == 0) {
			if ((info.type & XI_FILE_TYPE_DIR) == XI_FILE_TYPE_DIR) {
				bootclasspath[j].zip = NULL;
				if (len > max) {
					max = len;
				}
			} else {
				if (verbose) {
					log_trace(XDLOG, "Enter processing archive = %s\n", start);
				}
				if ((bootclasspath[j].zip = processArchive(start)) == NULL) {
					continue;
				}
			}
			bootclasspath[j++].path = start;
		}
	}

	max_cp_element_len = max;

	if (verbose) {
		log_trace(XDLOG, "bootclass entries = %d\n", j);
	}

	return bcp_entries = j;
}

void setClassPath(char *cmdlne_cp) {
	xchar cpath[1024];
	xint32 ret;

	if (classpath) {
		sysFree(classpath);
	}

	if (cmdlne_cp) {
		classpath = sysMalloc(xi_strlen(cmdlne_cp) + 1);
		xi_strcpy(classpath, cmdlne_cp);
	} else {
		xi_mem_set(cpath, 0, sizeof(cpath));
		ret = xi_env_get("CLASSPATH", cpath, sizeof(cpath));
		if (ret > 0) {
			classpath = sysMalloc(xi_strlen(cpath) + 1);
			xi_strcpy(classpath, cpath);
		} else {
			classpath = sysMalloc(2);
			xi_strcpy(classpath, ".");
		}
	}
}

char *getClassPath() {
	if (verbose) {
		log_trace(XDLOG, "classpath = %s\n", classpath);
	}
	return classpath;
}

void scanDirForJars(char *dir) {
	int bootpathlen;
	int dirlen;

	xi_dir_t *xdir;
	xi_file_stat_t sstat;

	bootpathlen = xi_strlen(bootpath) + 1;
	dirlen = xi_strlen(dir);

	xdir = xi_dir_open(dir);
	if (xdir == NULL) {
		return;
	}

	while (xi_dir_read(xdir, &sstat) > 0) {
		int nlen;
		char *ext;

		nlen = xi_strlen(sstat.filename);
		if (nlen < 4) {
			continue;
		}

		ext = (char*) &(sstat.filename[nlen - 4]);
		if (xi_strcasecmp(ext, ".zip") == 0 || xi_strcasecmp(ext, ".jar") == 0) {
			char *buff;
			bootpathlen += nlen + dirlen + 2;
			buff = sysMalloc(bootpathlen);

			xi_strcat(
					xi_strcat(
							xi_strcat(
									xi_strcat(xi_strcpy(buff, dir),
											XI_SEP_FILE_S), sstat.filename),
							XI_SEP_PATH_S), bootpath);

			sysFree(bootpath);
			bootpath = buff;
		}
	}
}

void scanDirsForJars(char *directories) {
	int dirslen;
	char *pntr;
	char *start;
	char *end;
	char *dirs;

	dirslen = xi_strlen(directories);
	dirs = sysMalloc(dirslen + 1);

	xi_strcpy(dirs, directories);
	for (end = pntr = &dirs[dirslen]; pntr != dirs; pntr--) {
		if (*pntr == ':') {
			start = pntr + 1;
			if (start != end) {
				scanDirForJars(start);
			}

			*(end = pntr) = '\0';
		}
	}

	if (end != dirs) {
		scanDirForJars(dirs);
	}

	sysFree(dirs);
}

char *setBootClassPath(char *cmdlne_bcp, char bootpathopt) {
	char *endorsed_dirs;

	if (verbose) {
		log_trace(XDLOG, "%s / %s\n", cmdlne_bcp, bootpathopt);
	}
	if (cmdlne_bcp) {
		switch (bootpathopt) {
		case 'a':
		case 'p':
			bootpath = sysMalloc(
					xi_strlen(DFLT_BCP) + xi_strlen(cmdlne_bcp) + 2);
			if (bootpathopt == 'a') {
				xi_strcat(
						xi_strcat(xi_strcpy(bootpath, DFLT_BCP), XI_SEP_PATH_S),
						cmdlne_bcp);
			} else {
				xi_strcat(
						xi_strcat(xi_strcpy(bootpath, cmdlne_bcp),
								XI_SEP_PATH_S), DFLT_BCP);
			}
			break;

		case 'c':
			bootpath = sysMalloc(
					xi_strlen(JAMVM_CLASSES) + xi_strlen(cmdlne_bcp) + 2);
			xi_strcat(
					xi_strcat(xi_strcpy(bootpath, JAMVM_CLASSES), XI_SEP_PATH_S),
					cmdlne_bcp);
			break;

		case 'v':
			bootpath = sysMalloc(
					xi_strlen(CLASSPATH_CLASSES) + xi_strlen(cmdlne_bcp) + 2);
			xi_strcat(
					xi_strcat(xi_strcpy(bootpath, cmdlne_bcp), XI_SEP_PATH_S),
					CLASSPATH_CLASSES);
			break;

		default:
			bootpath = sysMalloc(xi_strlen(cmdlne_bcp) + 1);
			xi_strcpy(bootpath, cmdlne_bcp);
		}
	} else {
		xint32 ret;
		xchar bcpath[1024];
		xchar *path = NULL;

		ret = xi_env_get("BOOTCLASSPATH", bcpath, sizeof(bcpath));
		path = (ret > 0) ? bcpath : DFLT_BCP;
		if (verbose) {
			log_trace(XDLOG, "Enter here : %s\n", path);
		}

		bootpath = sysMalloc(xi_strlen(path) + 1);
		xi_strcpy(bootpath, path);
	}

	endorsed_dirs = getCommandLineProperty("java.endorsed.dirs");
	if (endorsed_dirs == NULL) {
		endorsed_dirs = ".."XI_SEP_FILE_S"lib"XI_SEP_FILE_S"endorsed";
		//endorsed_dirs = INSTALL_DIR"/share/jamvm/endorsed";
	}

	scanDirsForJars(endorsed_dirs);

	return bootpath;
}

char *getBootClassPath() {
	return bootpath;
}

int bootClassPathSize() {
	return bcp_entries;
}

Object *bootClassPathResource(char *filename, int index) {
	Object *res = NULL;

	if (index < bcp_entries) {
		char *buff, *path = bootclasspath[index].path;
		int path_len = xi_strlen(path);

		if (path[0] != XI_SEP_FILE_C) {
			char *cwd = getCwd();
			path_len += xi_strlen(cwd) + 1;
			path = xi_strcat(
					xi_strcat(xi_strcpy(sysMalloc(path_len + 1), cwd),
							XI_SEP_FILE_S), path);
		}

		/* Alloc enough space for Jar file URL -- jar:file://<path>!/<filename> */
		buff = sysMalloc(xi_strlen(filename) + path_len + 14);

		if (bootclasspath[index].zip != NULL) {
			while (*filename == XI_SEP_FILE_C) {
				filename++;
			}

			if (findArchiveDirEntry(filename, bootclasspath[index].zip) == NULL) {
				goto out;
			}

			xi_sprintf(buff, "jar:file://%s!/%s", path, filename);
		} else {
			xi_file_stat_t info;

			xi_sprintf(buff, "file://%s/%s", path, filename);
			if (xi_file_stat(&buff[7], &info) != 0 || (info.type
					& XI_FILE_TYPE_DIR) == XI_FILE_TYPE_DIR) {
				goto out;
			}
		}

		res = createString(buff);

		out: if (path != bootclasspath[index].path) {
			sysFree(path);
		}
		sysFree(buff);
	}

	return res;
}

void initialiseClass(InitArgs *args) {
	FieldBlock *hashtable = NULL;
	Class *loader_data_class;
	Class *vm_loader_class;
	char *bcp = setBootClassPath(args->bootpath, args->bootpathopt);

	if (verbose) {
		log_trace(XDLOG, "bootclasspath = %s\n", bcp);
	}
	if (!(bcp && parseBootClassPath(bcp))) {
		log_error(XDLOG, "bootclasspath is empty!\n");
		exitVM(1);
	}

	verbose = args->verboseclass;
	setClassPath(args->classpath);

	/* Init hash table, and create lock */initHashTable(boot_classes, CLASS_INITSZE, TRUE);

	initHashTable(boot_packages, PCKG_INITSZE, TRUE);

	loader_data_class = findSystemClass0(
			SYMBOL(jamvm_java_lang_VMClassLoaderData));
	if (loader_data_class != NULL) {
		ldr_new_unloader = findMethod(loader_data_class,
				SYMBOL(newLibraryUnloader), SYMBOL(_J__V));
		hashtable = findField(loader_data_class, SYMBOL(hashtable), SYMBOL(J));
	}

	if (hashtable == NULL || ldr_new_unloader == NULL) {
		log_error(XDLOG, "Fatal error: Bad VMClassLoaderData (missing or corrupt)\n");
		exitVM(1);
	}
	ldr_data_tbl_offset = hashtable->u.offset;

	vm_loader_class = findSystemClass0(SYMBOL(java_lang_VMClassLoader));
	if (vm_loader_class != NULL) {
		vm_loader_create_package = findMethod(vm_loader_class,
				SYMBOL(createBootPackage),
				SYMBOL(_java_lang_String_I__java_lang_Package));
	}

	if (vm_loader_create_package == NULL) {
		log_error(XDLOG, "Fatal error: Bad java.lang.VMClassLoader (missing or corrupt)\n");
		exitVM(1);
	}

	package_array_class = findArrayClass(SYMBOL(array_java_lang_Package));
	registerStaticClassRef(&package_array_class);

	if (package_array_class == NULL) {
		log_error(XDLOG, "Fatal error: missing java.lang.Package\n");
		exitVM(1);
	}

	// Register the address of where the java.lang.Class ref _will_ be
	registerStaticClassRef(&java_lang_Class);
}

