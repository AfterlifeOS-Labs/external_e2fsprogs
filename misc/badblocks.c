/*
 * badblocks.c		- Bad blocks checker
 *
 * Copyright (C) 1992, 1993, 1994  Remy Card <card@masi.ibp.fr>
 *                                 Laboratoire MASI, Institut Blaise Pascal
 *                                 Universite Pierre et Marie Curie (Paris VI)
 *
 * Copyright 1995, 1996, 1997 by Theodore Ts'o
 * Copyright 1999 by David Beattie
 *
 * This file is based on the minix file system programs fsck and mkfs
 * written and copyrighted by Linus Torvalds <Linus.Torvalds@cs.helsinki.fi>
 * 
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

/*
 * History:
 * 93/05/26	- Creation from e2fsck
 * 94/02/27	- Made a separate bad blocks checker
 * 99/06/30...99/07/26 - Added non-destructive write-testing,
 *                       and a whole host of other features.
 */

#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#if HAVE_LINUX_FS_H
#include <linux/fd.h>
#include <linux/fs.h>
#endif

#include "et/com_err.h"
#include "ext2fs/ext2_io.h"
#include <linux/ext2_fs.h>
#include "ext2fs/ext2fs.h"

const char * program_name = "badblocks";
const char * done_string = "done                        \n";

int v_flag = 0;			/* verbose */
int w_flag = 0;			/* do r/w test: 0=no, 1=yes, 2=non-destructive */
int s_flag = 0;			/* show progress of test */

static void usage(void)
{
	fprintf (stderr, "Usage: %s [-b block_size] [-i input_file] [-o output_file] [-svwn]\n [-c blocks_at_once] [-p num_passes] device blocks_count [start_count]\n",
		 program_name);
	exit (1);
}

static unsigned long currently_testing = 0;
static unsigned long num_blocks = 0;
static ext2_badblocks_list bb_list = NULL;
static FILE *out;
static blk_t next_bad = 0;
static ext2_badblocks_iterate bb_iter = NULL;

static void bb_output (unsigned long bad)
{
	errcode_t errcode;

	fprintf (out, "%lu\n", bad);

	errcode = ext2fs_badblocks_list_add (bb_list, bad);
	if (errcode) {
		com_err (program_name, errcode, "adding to in-memory bad block list");
		exit (1);
	}

	/* kludge:
	   increment the iteration through the bb_list if 
	   an element was just added before the current iteration
	   position.  This should not cause next_bad to change. */
	if (bb_iter && bad < next_bad)
		ext2fs_badblocks_list_iterate (bb_iter, &next_bad);
}

static void print_status (void)
{
	fprintf(stderr, "%9ld/%9ld", currently_testing, num_blocks);
	fprintf(stderr, "\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
	fflush (stderr);
}

static void alarm_intr (int alnum)
{
	signal (SIGALRM, alarm_intr);
	alarm(1);
	if (!num_blocks)
		return;
	fprintf(stderr, "%9ld/%9ld", currently_testing, num_blocks);
	fprintf(stderr, "\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
	fflush (stderr);
}

static void *terminate_addr = NULL;

static void terminate_intr (int signo)
{
	if (terminate_addr)
		longjmp(terminate_addr,1);
	exit(1);
}

static void capture_terminate (jmp_buf term_addr)
{
	terminate_addr = term_addr;
	signal (SIGHUP, terminate_intr);
	signal (SIGINT, terminate_intr);
	signal (SIGPIPE, terminate_intr);
	signal (SIGTERM, terminate_intr);
	signal (SIGUSR1, terminate_intr);
	signal (SIGUSR2, terminate_intr);
}

/*
 * Perform a read of a sequence of blocks; return the number of blocks
 *    successfully sequentially read.
 */
static long do_read (int dev, char * buffer, int try, unsigned long block_size,
		     unsigned long current_block)
{
	long got;

	if (v_flag > 1)
		print_status();

	/* Seek to the correct loc. */
	if (ext2fs_llseek (dev, (ext2_loff_t) current_block * block_size,
			 SEEK_SET) != (ext2_loff_t) current_block * block_size)
		com_err (program_name, errno, "during seek");

	/* Try the read */
	got = read (dev, buffer, try * block_size);
	if (got < 0)
		got = 0;	
	if (got & 511)
		fprintf (stderr,
			 "Weird value (%ld) in do_read\n", got);
	got /= block_size;
	return got;
}

/*
 * Perform a write of a sequence of blocks; return the number of blocks
 *    successfully sequentially written.
 */
static long do_write (int dev, char * buffer, int try, unsigned long block_size,
		     unsigned long current_block)
{
	long got;

	if (v_flag > 1)
		print_status();

	/* Seek to the correct loc. */
	if (ext2fs_llseek (dev, (ext2_loff_t) current_block * block_size,
			 SEEK_SET) != (ext2_loff_t) current_block * block_size)
		com_err (program_name, errno, "during seek");

	/* Try the write */
	got = write (dev, buffer, try * block_size);
	if (got < 0)
		got = 0;	
	if (got & 511)
		fprintf (stderr,
			 "Weird value (%ld) in do_write\n", got);
	got /= block_size;
	return got;
}

static int host_dev;

static void flush_bufs (int dev, int sync)
{
  if (v_flag
#if !defined (BLKFLSBUF) && !defined (FDFLUSH)
      && sync
#endif
      )
    fprintf (stderr, "Flushing buffers\n");

  if (sync && fdatasync (dev) == -1)
    com_err (program_name, errno, "during fsync");

#ifdef BLKFLSBUF
  ioctl (host_dev, BLKFLSBUF, 0);   /* In case this is a HD */
#endif
#ifdef FDFLUSH
  ioctl (host_dev, FDFLUSH, 0);   /* In case this is floppy */
#endif
}

static unsigned int test_ro (int dev, unsigned long blocks_count,
		     unsigned long block_size,
		     unsigned long from_count, unsigned long blocks_at_once)
{
	char * blkbuf;
	int try;
	long got;
	unsigned int bb_count = 0;
	errcode_t errcode;

	errcode = ext2fs_badblocks_list_iterate_begin(bb_list,&bb_iter);
	if (errcode) {
		com_err (program_name, errcode, "while beginning bad block list iteration");
		exit (1);
	}
	do {
		ext2fs_badblocks_list_iterate (bb_iter, &next_bad);
	} while (next_bad && next_bad < from_count);

	blkbuf = malloc (blocks_at_once * block_size);
	if (!blkbuf)
	{
		com_err (program_name, ENOMEM, "while allocating buffers");
		exit (1);
	}
	flush_bufs (dev, 0);
	if (v_flag) {
	    fprintf (stderr,
		     "Checking for bad blocks in read-only mode\n");
	    fprintf (stderr, "From block %lu to %lu\n", from_count, blocks_count);
	}
	try = blocks_at_once;
	currently_testing = from_count;
	num_blocks = blocks_count;
	if (s_flag || v_flag > 1) {
		fprintf(stderr, "Checking for bad blocks (read-only test): ");
		if (v_flag <= 1)
			alarm_intr(SIGALRM);
	}
	while (currently_testing < blocks_count)
	{
		if (next_bad) {
			if (currently_testing == next_bad) {
				/* fprintf (out, "%lu\n", nextbad); */
				ext2fs_badblocks_list_iterate (bb_iter, &next_bad);
				currently_testing++;
				continue;
			}
			else if (currently_testing + try > next_bad)
				try = next_bad - currently_testing;
		}
		if (currently_testing + try > blocks_count)
			try = blocks_count - currently_testing;
		got = do_read (dev, blkbuf, try, block_size, currently_testing);
		currently_testing += got;
		if (got == try) {
			try = blocks_at_once;
			continue;
		}
		else
			try = 1;
		if (got == 0) {
			bb_output (currently_testing++), ++bb_count;
		}
	}
	num_blocks = 0;
	alarm(0);
	if (s_flag || v_flag > 1)
		fprintf(stderr, done_string);

	fflush (stderr);
	free (blkbuf);

	ext2fs_badblocks_list_iterate_end(bb_iter);

	return bb_count;
}

static unsigned int test_rw (int dev, unsigned long blocks_count,
		     unsigned long block_size,
		     unsigned long from_count, unsigned long blocks_at_once)
{
	int i;
	char * buffer;
	unsigned char pattern[] = {0xaa, 0x55, 0xff, 0x00};
	unsigned int bb_count = 0;

	buffer = malloc (2 * block_size);
	if (!buffer)
	{
		com_err (program_name, ENOMEM, "while allocating buffers");
		exit (1);
	}

	flush_bufs (dev, 0);

	if (v_flag) {
		fprintf(stderr,
			"Checking for bad blocks in read-write mode\n");
		fprintf(stderr, "From block %lu to %lu\n",
			 from_count, blocks_count);
	}
	for (i = 0; i < sizeof (pattern); i++) {
		memset (buffer, pattern[i], block_size);
		if (s_flag | v_flag)
			fprintf (stderr, "Writing pattern 0x%08x: ",
				 *((int *) buffer));
		num_blocks = blocks_count;
		currently_testing = from_count;
		if (s_flag && v_flag <= 1)
			alarm_intr(SIGALRM);
		for (;
		     currently_testing < blocks_count;
		     currently_testing++)
		{
			if (ext2fs_llseek (dev, (ext2_loff_t) currently_testing *
					 block_size, SEEK_SET) !=
			    (ext2_loff_t) currently_testing * block_size)
				com_err (program_name, errno,
					 "during seek on block %d",
					 currently_testing);
			if (v_flag > 1)
				print_status();
			write (dev, buffer, block_size);
		}
		num_blocks = 0;
		alarm (0);
		if (s_flag | v_flag)
			fprintf(stderr, done_string);
		flush_bufs (dev, 1);
		if (s_flag | v_flag)
			fprintf (stderr, "Reading and comparing: ");
		num_blocks = blocks_count;
		currently_testing = from_count;
		if (s_flag && v_flag <= 1)
			alarm_intr(SIGALRM);
		for (;
		     currently_testing < blocks_count;
		     currently_testing++)
		{
			if (ext2fs_llseek (dev, (ext2_loff_t) currently_testing *
					 block_size, SEEK_SET) !=
			    (ext2_loff_t) currently_testing * block_size)
				com_err (program_name, errno,
					 "during seek on block %d",
					 currently_testing);
			if (v_flag > 1)
				print_status();
			if (read (dev, buffer + block_size, block_size) < block_size)

				ext2fs_badblocks_list_test (bb_list, currently_testing)
				|| ( bb_output (currently_testing), ++bb_count );

			else if (memcmp (buffer, buffer + block_size, block_size))

				ext2fs_badblocks_list_test (bb_list, currently_testing)
				|| ( bb_output (currently_testing), ++bb_count );

		}
		num_blocks = 0;
		alarm (0);
		if (s_flag | v_flag)
			fprintf(stderr, done_string);
		flush_bufs (dev, 0);
	}

	return bb_count;
}

static unsigned int test_nd (int dev, unsigned long blocks_count,
		     unsigned long block_size,
		     unsigned long from_count, unsigned long blocks_at_once)
{
	char * blkbuf;
	char * ptr;
	int try;
	long got;
	long buf_used;
	unsigned long bufblk[blocks_at_once];
	unsigned long bufblks[blocks_at_once];
	jmp_buf terminate_env;
	unsigned int bb_count = 0;
	errcode_t errcode;

	errcode = ext2fs_badblocks_list_iterate_begin(bb_list,&bb_iter);
	if (errcode) {
		com_err (program_name, errcode, "while beginning bad block list iteration");
		exit (1);
	}
	do {
		ext2fs_badblocks_list_iterate (bb_iter, &next_bad);
	} while (next_bad && next_bad < from_count);

	blkbuf = malloc (3 * blocks_at_once * block_size);
	if (!blkbuf)
	{
		com_err (program_name, ENOMEM, "while allocating buffers");
		exit (1);
	}

	/* inititalize the test data randomly: */
	if (v_flag) {
		fprintf (stderr, "Initializing random test data\n");
	}
	for(ptr = blkbuf + blocks_at_once * block_size;
	       ptr < blkbuf + 2 * blocks_at_once * block_size;
	          ++ptr) {
		(*ptr) = random() % (1 << sizeof(char));
	}

	flush_bufs (dev, 0);
	if (v_flag) {
	    fprintf (stderr,
		     "Checking for bad blocks in non-destructive read-write mode\n");
	    fprintf (stderr, "From block %lu to %lu\n", from_count, blocks_count);
	}
	buf_used = 0;
	currently_testing = from_count;
	num_blocks = blocks_count;
	if (s_flag || v_flag > 1) {
		fprintf (stderr, "Checking for bad blocks (non-destructive read-write test): ");
		if (v_flag <= 1)
			alarm_intr(SIGALRM);
	}
	if (! setjmp(terminate_env)) {
		/* set up abend handler */
		capture_terminate(terminate_env);

		while (currently_testing < blocks_count)
		{
			try = blocks_at_once - buf_used;
			if (next_bad) {
				if (currently_testing == next_bad) {
					/* fprintf (out, "%lu\n", nextbad); */
					ext2fs_badblocks_list_iterate (bb_iter, &next_bad);
					bufblk[buf_used] = currently_testing++;
					goto test_full_buf;
				}
				else if (currently_testing + try > next_bad)
					try = next_bad - currently_testing;
			}
			if (currently_testing + try > blocks_count)
				try = blocks_count - currently_testing;
			got = do_read (dev, blkbuf + buf_used * block_size, try,
			                                block_size, currently_testing);

			/* if reading succeeded, write the test data */
			if (got) {
				long written;

				written = do_write (dev, blkbuf + (buf_used + blocks_at_once) * block_size,
				                             got, block_size, currently_testing);
				if (written != got)
					com_err (program_name, errno, "during test data write, block %lu",
					                                  currently_testing + written);
			}

			bufblk[buf_used] = currently_testing;
			bufblks[buf_used] = got;
			buf_used += got;
			currently_testing += got;
			if (got != try)
				bb_output (currently_testing++), ++bb_count;

			test_full_buf:

			if (buf_used == blocks_at_once || currently_testing == blocks_count) {
				if (s_flag || v_flag > 1)
					fprintf(stderr, "\n");
				flush_bufs (dev, 1);

				for (buf_used = 0, currently_testing = bufblk[0];
				        buf_used < blocks_at_once &&
				        currently_testing < blocks_count &&
                    (currently_testing = bufblk[buf_used]) < blocks_count;
				           buf_used += bufblks[buf_used])
					{
						/* for each contiguous block that we read into the buffer
						   (and wrote test data into afterwards), read it back
						   (looping if necessary, to get past newly discovered
						   unreadable blocks, of which there should be none, but with
						   a hard drive which is unreliable, it has happened), and
						   compare with the test data that was written; output to
						   the bad block list if it doesn't match. */

						int offset;

						while ((offset = currently_testing - bufblk[buf_used]) <
						                                          bufblks[buf_used]) {
							int i;

							try = bufblks[buf_used] - offset;
							got = do_read (dev, blkbuf + (2 * blocks_at_once + buf_used + offset) * block_size,
						                       try, block_size, currently_testing);

							/* test the comparison between all the blocks successfully read */
							for (i = 0; i < got; ++i)
								if (memcmp (blkbuf + (blocks_at_once + buf_used + offset + i) * block_size,
								            blkbuf + (2 * blocks_at_once + buf_used + offset + i) * block_size,
								            block_size))
									bb_output (currently_testing + i), ++bb_count;

							currently_testing += got;
							if (got != try)
								bb_output (currently_testing++), ++bb_count;
						}

						/* when done, write back original data */
						do_write (dev, blkbuf + buf_used * block_size,
						               bufblks[buf_used], block_size, bufblk[buf_used]);
					}

				/* empty the buffer so it can be reused */
				buf_used = 0;
			}
		}
		num_blocks = 0;
		alarm(0);
		if (s_flag || v_flag > 1)
			fprintf(stderr, "done               \n");

	} else {
		/* abnormal termination by a signal is handled here */
		/* logic is: write back the data that is in the buffer,
		   so that we can maintain data integrity on disk.  Then
		   abort. */
		long buf_written;

		fprintf(stderr, "Interrupt caught, cleaning up\n");

		for (buf_written = 0;
		        buf_written < buf_used;
		           buf_written += bufblks[buf_written])
			do_write (dev, blkbuf + buf_written * block_size,
			               bufblks[buf_written], block_size, bufblk[buf_written]);

		fflush (out);

		abort ();
	}

	fflush (stderr);
	free (blkbuf);

	ext2fs_badblocks_list_iterate_end(bb_iter);

	return bb_count;
}

int main (int argc, char ** argv)
{
	int c;
	char * tmp;
	char * device_name;
	char * host_device_name = NULL;
	char * input_file = NULL;
	char * output_file = NULL;
	FILE * in = NULL;
	unsigned long block_size = 1024;
	unsigned long blocks_at_once = 16;
	unsigned long blocks_count, from_count;
	int num_passes = 0;
	int passes_clean = 0;
	int dev;
	errcode_t errcode;

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	if (argc && *argv)
		program_name = *argv;
	while ((c = getopt (argc, argv, "b:i:o:svwnc:p:h:")) != EOF) {
		switch (c) {
		case 'b':
			block_size = strtoul (optarg, &tmp, 0);
			if (*tmp || block_size > 4096) {
				com_err (program_name, 0,
					 "bad block size - %s", optarg);
				exit (1);
			}
			break;
		case 'i':
			input_file = optarg;
			break;
		case 'o':
			output_file = optarg;
			break;
		case 's':
			s_flag = 1;
			break;
		case 'v':
			v_flag++;
			break;
		case 'w':
			if (! w_flag)
				w_flag = 1;
			break;
		case 'n':
			w_flag = 2;
			break;
		case 'c':
			blocks_at_once = strtoul (optarg, &tmp, 0);
			if (*tmp) {
				com_err (program_name, 0,
					 "bad simultaneous block count - %s", optarg);
				exit (1);
			}
			break;
		case 'p':
			num_passes = strtoul (optarg, &tmp, 0);
			if (*tmp) {
				com_err (program_name, 0,
				    "bad number of clean passes - %s", optarg);
				exit (1);
			}
			break;
		case 'h':
			host_device_name = optarg;
			break;
		default:
			usage();
		}
	}
	if (optind > argc - 1)
		usage();
	device_name = argv[optind++];
	if (optind > argc - 1)
		usage();
	blocks_count = strtoul (argv[optind], &tmp, 0);
	if (*tmp)
	{
		com_err (program_name, 0, "bad blocks count - %s", argv[optind]);
		exit (1);
	}
	if (++optind <= argc-1) {
		from_count = strtoul (argv[optind], &tmp, 0);
	} else from_count = 0;
	if (from_count >= blocks_count) {
	    com_err (program_name, 0, "bad blocks range: %lu-%lu",
		     from_count, blocks_count);
	    exit (1);
	}
	dev = open (device_name, w_flag ? O_RDWR : O_RDONLY);
	if (dev == -1)
	{
		com_err (program_name, errno, "while trying to open %s",
			 device_name);
		exit (1);
	}
	if (host_device_name) {
		host_dev = open (host_device_name, O_RDONLY);
		if (host_dev == -1)
		{
			com_err (program_name, errno, "while trying to open %s",
			    host_device_name);
			exit (1);
		}
	} else
		host_dev = dev;
	if (input_file)
		if (strcmp (input_file, "-") == 0)
			in = stdin;
		else {
			in = fopen (input_file, "r");
			if (in == NULL)
			{
				com_err (program_name, errno,"while trying to open %s",
					 input_file);
				exit (1);
			}
		}
	if (output_file && strcmp (output_file, "-") != 0)
	{
		out = fopen (output_file, "w");
		if (out == NULL)
		{
			com_err (program_name, errno,"while trying to open %s",
				 output_file);
			exit (1);
		}
	}
	else
		out = stdout;

	errcode = ext2fs_badblocks_list_create(&bb_list,0);
	if (errcode) {
		com_err (program_name, errcode, "creating in-memory bad blocks list");
		exit (1);
	}

	if (in) {
		for(;;) {
			switch(fscanf (in, "%lu\n", &next_bad)) {
				case 0:
					com_err (program_name, 0, "input file - bad format");
					exit (1);
				case EOF:
					break;
				default:
					errcode = ext2fs_badblocks_list_add(bb_list,next_bad);
					if (errcode) {
						com_err (program_name, errcode, "adding to in-memory bad block list");
						exit (1);
					}
					continue;
			}
			break;
		}

		if (in != stdin)
			fclose (in);
	}

	do {
		unsigned int bb_count;

		(bb_count =

		(w_flag == 2 ? test_nd
		    : w_flag ? test_rw
		             : test_ro)
		                        (dev, blocks_count, block_size, from_count,
		                              blocks_at_once)
		)	? passes_clean = 0 : ++passes_clean;

		if (v_flag)
			fprintf(stderr,"Pass completed, %u bad blocks found.\n", bb_count);

	} while (passes_clean < num_passes);

	close (dev);
	if (out != stdout)
		fclose (out);
	return 0;
}
