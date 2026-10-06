/*
GUEST_STRING.C

memcmp for the guest, in place of musl's (tools/ios_guest_build.py leaves
string/memcmp.c out). musl's compares a byte at a time, and every guest load
also pays the arena's address rebasing, so the front end's per-draw state
compares (the pixel shader key, the uniform inputs: about 600 bytes a draw)
took several percent of the game thread. This one compares 8 bytes at a time
and finds the first differing byte only in the word where they differ, so it
returns what musl's does: the difference of the first unequal bytes.
*/

#include <stddef.h>
#include <stdint.h>
#include <string.h>

__attribute__((no_builtin)) int memcmp(const void *left, const void *right, size_t size)
{
	const unsigned char *l = left, *r = right;

	while (size >= 8)
	{
		uint64_t a, b;

		__builtin_memcpy(&a, l, 8);
		__builtin_memcpy(&b, r, 8);
		if (a != b)
			break;
		l += 8;
		r += 8;
		size -= 8;
	}
	for (; size; size--, l++, r++)
	{
		if (*l != *r)
			return *l - *r;
	}
	return 0;
}
