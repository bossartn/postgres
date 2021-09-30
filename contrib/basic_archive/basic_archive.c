/*-------------------------------------------------------------------------
 *
 * basic_archive.c
 *
 * This file demonstrates a basic archive_library implemenation that is
 * roughly equivalent to the following shell command:
 *
 * 		test ! -f /path/to/dest && cp /path/to/src /path/to/dest
 *
 * One notable difference between this module and the shell command above
 * is that this module first copies the file to a temporary destination,
 * syncs it to disk, and then durably moves it to the final destination.
 *
 * Copyright (c) 2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/basic_archive/basic_archive.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

void	_PG_init(void);
bool	_PG_archive(const char *path, const char *file);

static char *archive_directory = NULL;

static bool check_archive_directory(char **newval, void **extra, GucSource source);
static bool copy_file(const char *src, const char *dst, char *buf);

void
_PG_init(void)
{
	if (!process_archive_library_in_progress)
		ereport(ERROR,
				(errmsg("\"basic_archive\" can only be loaded via "
						"\"archive_library\"")));

	DefineCustomStringVariable("basic_archive.archive_directory",
							   gettext_noop("Archive file destination directory."),
							   NULL,
							   &archive_directory,
							   "",
							   PGC_POSTMASTER,
							   GUC_NOT_IN_SAMPLE,
							   check_archive_directory, NULL, NULL);
}

static bool
check_archive_directory(char **newval, void **extra, GucSource source)
{
	struct stat st;

	if (*newval == NULL || *newval[0] == '\0')
		return true;

	if (stat(*newval, &st) != 0 || !S_ISDIR(st.st_mode))
	{
		GUC_check_errdetail("specified archive directory does not exist");
		return false;
	}

	return true;
}

bool
_PG_archive(const char *path, const char *file)
{
	char destination[MAXPGPATH];
	char temp[MAXPGPATH];
	struct stat st;
	char *buf;

	if (archive_directory == NULL || archive_directory[0] == '\0')
	{
		ereport(WARNING,
				(errmsg("\"basic_archive.archive_directory\" not specified")));
		return false;
	}

#define TEMP_FILE_NAME ("archtemp")

	if (strlen(path) + Max(strlen(file), strlen(TEMP_FILE_NAME)) + 2 >= MAXPGPATH)
	{
		ereport(WARNING,
				(errmsg("archive destination path too long")));
		return false;
	}

	snprintf(destination, MAXPGPATH, "%s/%s", path, file);
	snprintf(temp, MAXPGPATH, "%s/%s", path, TEMP_FILE_NAME);

	/*
	 * First, check if the file has already been archived.  If it has,
	 * just fail, because something is wrong.
	 */
	if (stat(destination, &st) == 0)
	{
		ereport(WARNING,
				(errmsg("archive file \"%s\" already exists",
						destination)));
		return false;
	}
	else if (errno != ENOENT)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						destination)));
		return false;
	}

	/*
	 * Remove pre-existing temporary file, if one exists.
	 */
	if (unlink(temp) != 0 && errno != ENOENT)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not unlink file \"%s\": %m",
						temp)));
		return false;
	}

	/*
	 * Copy the file to its temporary destination.
	 */
#define COPY_BUF_SIZE (64 * 1024)

	buf = palloc(COPY_BUF_SIZE);

	if (!copy_file(path, temp, buf))
	{
		pfree(buf);
		return false;
	}

	pfree(buf);

	/*
	 * Sync the temporary file to disk and move it to its final
	 * destination.
	 */
	return (durable_rename(temp, destination, WARNING) == 0);
}

static bool
copy_file(const char *src, const char *dst, char *buf)
{
	int srcfd;
	int dstfd;
	int nbytes;

	srcfd = OpenTransientFile(src, O_RDONLY | PG_BINARY);
	if (srcfd < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", src)));
		return false;
	}

	dstfd = OpenTransientFile(dst, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (dstfd < 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", dst)));
		return false;
	}

	for (;;)
	{
		nbytes = read(srcfd, buf, COPY_BUF_SIZE);
		if (nbytes < 0)
		{
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", src)));
			return false;
		}

		if (nbytes == 0)
			break;

		errno = 0;
		if ((int) write(dstfd, buf, nbytes) != nbytes)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", dst)));
			return false;
		}
	}

	if (CloseTransientFile(dstfd) != 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", dst)));
		return false;
	}

	if (CloseTransientFile(srcfd) != 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", src)));
		return false;
	}

	return true;
}
