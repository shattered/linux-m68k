/*
 * getparts.c -- Program to read hard disk partition info,
 *		 for Bestas only. Used by Linux only, non-visible for sysv.
 *		 To write onto disk use `setparts.c' .
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

main (int argc, char *argv[]) {
	int fd, i, n, area_to_check;
	unsigned int *lp, checksum = 0;
	unsigned int root_part = 0;
	struct hd_partition *hdp;

	if (argc < 2) {
		fprintf (stderr, "Usage: %s device\n", argv[0]);
		exit (2);
	}

	fd = open (argv[1], 0);
	if (fd < 0) {
		fprintf (stderr, "%s: Cannot open %s for read\n",
						    argv[0], argv[1]);
		exit (1);
	}

	n = read (fd, &hdps, sizeof (hdps));
	if (n < sizeof (hdps)) {
		fprintf (stderr, "%s: %s: read error\n", argv[0], argv[1]);
		exit (1);
	}

	close (fd);


	if (hdps.magic != HDP_MAGIC) {
		fprintf (stderr, "%s: %s: bad magic in partition area\n",
							    argv[0], argv[1]);
		exit (2);
	}

	if (hdps.num_parts == 0 ||
	    hdps.num_parts > sizeof (hdps.parts) / sizeof (hdps.parts[0])
	) {
		fprintf (stderr, "%s: %s: Bad num_partitions value %d "
			"(should be no more than %d)\n", argv[0], argv[1],
			hdps.num_parts,
			sizeof (hdps.parts) / sizeof (hdps.parts[0]));
		exit (2);
	}

	/*  compute a check sum...  */
	area_to_check = hdps.num_parts * sizeof (struct hd_partition) +
			(((int) &((struct hard_disk_partition *) 0)->parts) -
			 ((int) &((struct hard_disk_partition *) 0)->magic));
	area_to_check /= sizeof (*lp);

	lp = &hdps.magic;
	for (i = 0; i < area_to_check; i++)  checksum ^= *lp++;

	/*  compare with stored check sum...  */
	checksum ^= *lp;

	if (checksum) {
		fprintf (stderr, "%s: %s: non-zero checksum value 0x%08x\n",
						argv[0], argv[1], checksum);
		exit (2);
	}


	printf ("# Partition info extracted from %s \n", argv[1]);
	if (hdps.root_part)
		printf ("# root partition index: %d\n", hdps.root_part - 1);
	printf ("# number of partitions: %d\n", hdps.num_parts);
	printf ("# in %d byte`s blocks\n", hdps.blksize);

	for (i = 0; i < hdps.num_parts; i++) {
	    int start = hdps.parts[i].start_sect;
	    int size = hdps.parts[i].nr_sects;

	    if (!(start % 0x100) && (start % 100))  printf ("0x%-8x\t", start);
	    else  printf ("%8d\t", start);

	    if (!(size % 0x100) && (size % 100))  printf ("0x%-8x\t", size);
	    else  printf ("%8d\t", size);

	    if (i == hdps.root_part - 1)  printf ("root \n");
	    else  printf ("\n");
	}

	exit (0);
}
