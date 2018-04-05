/**********************************************************************
 * pdump.c
 *
 * Debugging dump.
 */

#include <stdio.h>
#include <zephyr.h>
#include "pdump.h"
#include <ctype.h>

#define PRINTABLE(x) (isprint(x) ? (x) : '.')

void
pdump(const void *ibuffer, int length)
{
	static char line[80];
	static char ascii[18];
	int offset = 0;
	int ascii_offset = 0;
	unsigned char *buffer = (unsigned char *)ibuffer;
	int i;

	offset = 0;

	if (length == 0)
		return;

	ascii_offset = 0;

	for (i = 0; i < length; i++) {
		if ((i & 15) == 0) {
			if (i > 0) {
				ascii[ascii_offset] = 0;
				printk("%-60s %s\n", line, ascii);
				offset = 0;
				ascii_offset = 0;
			}
			offset += sprintf(line+offset, "%08x", i);
		} else if ((i & 7) == 0) {
			// offset += sprintf(line+offset, " -");
			// ascii[ascii_offset++] = ' ';
		}
		offset += sprintf(line+offset, " %02x", buffer[i] & 0xFF);
		ascii[ascii_offset++] = PRINTABLE (buffer[i] & 0xFF);
	}
	ascii[ascii_offset] = 0;
	printk("%-60s %s\n", line, ascii);
	offset = 0;
}
