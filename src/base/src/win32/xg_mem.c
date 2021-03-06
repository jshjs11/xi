/*
 * Copyright 2013 Cheolmin Jo (webos21@gmail.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * File : xg_memory.c
 */

#include <windows.h>
#include <memory.h>

#include "xi/xi_mem.h"

// ----------------------------------------------
// XI Functions
// ----------------------------------------------

xvoid *xi_mem_alloc(xsize size) {
	return LocalAlloc(LMEM_FIXED, size);
}

xvoid *xi_mem_calloc(xsize nmemb, xsize size) {
	return LocalAlloc(LPTR, nmemb * size);
}

xvoid *xi_mem_realloc(xvoid *ptr, xsize size) {
	if (ptr == NULL) {
		return LocalAlloc(LMEM_FIXED, size);
	} else {
		return LocalReAlloc(ptr, size, LMEM_MOVEABLE);
	}
}

xvoid xi_mem_free(xvoid *ptr) {
	LocalFree(ptr);
}

xvoid *xi_mem_set(xvoid *ptr, xint32 val, xsize len) {
	return memset(ptr, val, len);
}

xvoid *xi_mem_copy(xvoid *dest, const xvoid *src, xsize len) {
	return memcpy(dest, src, len);
}

xvoid *xi_mem_move(xvoid *dest, const xvoid *src, xsize len) {
	return memmove(dest, src, len);
}

xvoid *xi_mem_chr(const xvoid *s, xint32 c, xsize n) {
	return memchr(s, c, n);
}

xint32 xi_mem_cmp(const xvoid *s1, const xvoid *s2, xsize n) {
	return memcmp(s1, s2, n);
}

xssize xi_mem_read(xvoid *addr, xsize size, xvoid *buf) {
	xssize bytes_read;
	BOOL result;

	if (!buf || !addr) {
		return -1;
	}

	if (size == 0) {
		return 0;
	}

	result = ReadProcessMemory(GetCurrentProcess(), addr, buf, size,
			(xsize*)&bytes_read);

	if (!result || bytes_read != (xssize)size) {
		return -1;
	}

	return bytes_read;
}

xssize xi_mem_write(xvoid *addr, xsize size, xvoid *buf) {
	xssize bytes_written;
	BOOL result;

	if (!buf || !addr) {
		return -1;
	}

	if (size == 0) {
		return 0;
	}

	result = WriteProcessMemory(GetCurrentProcess(), addr, buf, size,
			(xsize*)&bytes_written);

	if (!result || bytes_written != (xssize)size) {
		return -1;
	}

	FlushInstructionCache(GetCurrentProcess(), addr, size);

	return bytes_written;
}
