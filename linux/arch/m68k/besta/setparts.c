/*
 * setparts.c -- Program to write hard disk partition info.
 *		 For Bestas only. Used by Linux only, non-visible by sysv.
 *		 To read use `getparts.c' .
 *
 * Copyright 1996, 1997	    Dmitry K. Butskoy
 *			    <buc@citadel.stu.neva.ru>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "xscsi.h"

struct hard_disk_partition hdps;
char buf[1024];

main (int argc, char *argv[]) {
	FILE *fp;
	int fd, i, n, area_to_check;
	unsigned int *lp, checksum = 0;
	unsigned int root_part = 0;
	struct hd_partition *hdp;
	char ch;
	char c[4];

	if (argc < 3) {
		fprintf (stderr, "Usage: %s file device\n", argv[0]);
		exit (2);
	}

	fp = fopen (argv[1], "r");
	if (fp == NULL) {
		fprintf (stderr, "%s: Cannot open file %s\n", argv[0], argv[1]);
		exit (2);
	}

	/*  read specifications   */
	i = 0;
	hdp = hdps.parts;

	while (fgets (buf, sizeof (buf), fp) != NULL) {

	    n = sscanf (buf, " %i %i ", &hdp[i].start_sect, &hdp[i].nr_sects);

	    if (n < 2) {
		if (sscanf (buf, " %c", &ch) == 1 &&
		    (ch == '#' || ch == '|')
		)  continue;  /*  ignore commented line   */

		fprintf (stderr, "%s: Bad line:\n%s in file %s\n",
						argv[0], buf, argv[1]);
		exit (2);
	    }

	    if (!root_part &&
		sscanf (buf, " %*i %*i %c%c%c%c ", c, c+1, c+2, c+3) == 4 &&
		(!strncmp (c, "root", 4) || !strncmp (c, "ROOT", 4))
	    )  root_part = i + 1;       /*  convert index to number   */

	    i++;
	    if (i >= sizeof (hdps.parts) / sizeof (hdps.parts[0]))  break;
	}

	fclose (fp);

	if (i <= 0) {
	    fprintf (stderr, "%s: Cannot find partition info in file %s\n",
							    argv[0], argv[1]);
	    exit (2);
	}

	/*  open device   */
	fd = open (argv[2], 2);
	if (fd < 0) {
		fprintf (stderr, "%s: Cannot open %s for read/write\n",
							argv[0], argv[2]);
		exit (2);
	}


	/*  generate hard_disk_partition struct contents   */

	memset (hdps.misc_info, 0, sizeof (hdps.misc_info));
	hdps.magic = HDP_MAGIC;
	hdps.mtime = time ((long *) 0);
	hdps.num_parts = i;
	hdps.blksize = 1024;
	hdps.root_part = root_part;

	/*  generate checksum   */
	area_to_check = hdps.num_parts * sizeof (struct hd_partition) +
			(((int) &((struct hard_disk_partition *) 0)->parts) -
			 ((int) &((struct hard_disk_partition *) 0)->magic));
	area_to_check /= sizeof (*lp);

	lp = &hdps.magic;
	for (i = 0; i < area_to_check; i++)  checksum ^= *lp++;
	*lp++ = checksum;     /*  store the checksum  */

	i = ((char *) lp) - ((char *) &hdps);   /*  useful data size   */

	/*  put generated info into hard disk...   */

	n = read (fd, buf, 1024);
	if (n < 1024) {
		fprintf (stderr, "%s: %s: read error\n", argv[0], argv[2]);
		exit (1);
	}

	if (lseek (fd, 0, 0) != 0) {
		fprintf (stderr, "%s: %s: lseek error\n", argv[0], argv[2]);
		exit (1);
	}

	memcpy (buf, &hdps, i);

	n = write (fd, buf, 1024);
	if (n < 1024) {
		fprintf (stderr, "%s: %s: write error\n", argv[0], argv[2]);
		exit (1);
	}

	close (fd);
	sync();

	exit (0);
}
