/*
 * lar - LinuxBIOS archiver
 *
 * This includes code from previous versions of the LAR utility,
 * including create.c, list.c, extract.c and bootblock.c
 *
 * Copyright (C) 2006-2007 coresystems GmbH
 * (Written by Stefan Reinauer <stepan@coresystems.de> for coresystems GmbH)
 * Copyright (C) 2007 Patrick Georgi <patrick@georgi-clan.de>
 * Copyright (C) 2007 Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA, 02110-1301 USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <libgen.h>

#include "lib.h"
#include "lar.h"

/**
 * \def err(fmt,args...)
 * Abstract outputing error strings to avoid repetition
 */
#define err(fmt,args...) fprintf(stderr, fmt, ##args)

extern enum compalgo algo;

/**
 * Given a size, return the offset of the bootblock (including the
 * header)
 * @param size Size of the LAR archive
 * @return The offset of the bootblock header
 */
static inline u32 get_bootblock_offset(u32 size)
{
	return size - (BOOTBLOCK_SIZE + sizeof(struct lar_header)
		       + BOOTBLOCK_NAME_LEN);
}

/**
 * Return the expected offset of the next LAR header after the given one
 * @param header The current LAR header
 * @return The offset of the next possible LAR header
 */
static inline u32 get_next_offset(struct lar_header *header) {
	return ((ntohl(header->len) + ntohl(header->offset) - 1) & 0xFFFFFFF0)
		+ 16;
}

/**
 * Read the size of the LAR archive from the size embedded in the bootblock
 * @param lar The LAR archive to read from
 * @return The size as read from the bootblock header
 */
static int lar_read_size(struct lar *lar)
{
	u32 *ptr = (u32 *) (lar->map + (lar->size - 12));
	return ptr[0];
}

/**
 * Add the LAR archive size to the bootblock, and clean up some other params
 * in what we're loosely calling the "bootblockh header"
 * @param ptr Pointer to the start of the bootblock
 * @param size The size value to write to the bootblock header
 */
static void annotate_bootblock(u8 *ptr, u32 size)
{
	int i;
	u32 *p;

	memset(ptr + (BOOTBLOCK_SIZE - 13), 0, 13);
	p = (u32 *) (ptr + BOOTBLOCK_SIZE - 12);
	p[0] = size;
}

/**
 * Add a bootblock file to the LAR - the bootblock must be of a consistant
 * size, and always gets put in a special location in the LAR
 * @param lar The LAR archive to write to
 * @param bootblock The name of the bootblock file to insert
 * @return 0 on success, or -1 on failure
 */
int lar_add_bootblock(struct lar *lar, const char *bootblock)
{
	u8 *offset;
	struct lar_header *header;
	int ret;

	offset = lar->map + get_bootblock_offset(lar->size);
	header = (struct lar_header *) offset;

	memcpy(header->magic, MAGIC, 8);
	header->reallen = htonl(BOOTBLOCK_SIZE);
	header->len = htonl(BOOTBLOCK_SIZE);
	header->offset = htonl(sizeof(struct lar_header) + BOOTBLOCK_NAME_LEN);

	offset += sizeof(struct lar_header);
	strcpy((char *) offset, BOOTBLOCK_NAME);

	offset += BOOTBLOCK_NAME_LEN;

	/* If a file waas specified, then load it, and read it directly
	 * into place */

	if (bootblock != NULL) {
		struct stat s;
		int fd = open(bootblock, O_RDONLY);

		if (fd == -1) {
			err("Unable to read bootblock file %s\n", bootblock);
			return -1;
		}

	        ret = fstat(fd, &s);

		if (ret == -1) {
			err("Unable to stat %s\n", bootblock);
			close(fd);
			return -1;
		}

		if (s.st_size != BOOTBLOCK_SIZE) {
			err("Bootblock %s does not appear to be a bootblock.\n",
			    bootblock);
			close(fd);
			return -1;
		}

		ret = read(fd, offset, BOOTBLOCK_SIZE);
		close(fd);

		if (ret != BOOTBLOCK_SIZE) {
			err("Unable to read all the bytes in the bootblock file.\n");
			return -1;
		}
	}

	annotate_bootblock(offset, lar->size);
	return 0;
}

/**
 * mmap() the LAR archive
 * @param lar The LAR archive information to map
 * @param u32 size Size of the mapped region
 * @return 0 on success, or -1 on failure
 */
static int _map_lar(struct lar *lar, u32 size)
{
	if (lar == NULL)
		return -1;

	lar->map = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			lar->fd, 0);

	lar->size = size;

	if (lar->map == MAP_FAILED)
		return -1;

	return 0;
}

/**
 * Close the LAR archive and free all structures
 * @param lar The LAR archive to close
 */
void lar_close_archive(struct lar *lar)
{
	if (lar == NULL)
		return;

	if (lar->map != MAP_FAILED)
		munmap(lar->map, lar->size);

	if (lar->fd >= 0)
		close(lar->fd);

	free(lar);
}

/**
 * Create a new LAR archive - the archive must not exist on disk.
 * @param archive The filename of the new archive
 * @param size The intended size of the new archive
 * @return A LAR archive structure for the new archive
 */
struct lar * lar_new_archive(const char *archive, u32 size)
{
	struct lar *lar;
	int i;

	if (!access(archive, F_OK)) {
		err("Archive file %s already exists\n", archive);
		return NULL;
	}

	lar = calloc(sizeof(*lar), 1);

	if (lar == NULL) {
		err("Unable to allocate memory.\n");
		return NULL;
	}

	lar->fd = open(archive, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);

	if (lar->fd == -1) {
		err("Couldn't open the archive %s\n", archive);

		free(lar);
		return NULL;
	}

	/* Expand the file to the correct size */

	if (lseek(lar->fd, size - 1, SEEK_SET) == -1) {
		err("Unable to write the archive %s\n", archive);
		goto err;
	}

	if (write(lar->fd, "", 1) != 1) {
		err("Unable to write the file %s\n", archive);
		goto err;
	}

	if (_map_lar(lar, size)) {
		err("Unable to map the archive %s\n", archive);
		goto err;
	}

	/* Fill the whole thing with flash friendly 0xFFs */
	memset(lar->map, 0xFF, lar->size);

	/* Make a dummy bootblock */

	if (lar_add_bootblock(lar, NULL)) {
		err("Couldn't add a bootblock to %s\n", archive);
		goto err;
	}

	return lar;
 err:
	lar_close_archive(lar);

	/* Don't leave a halfbaked LAR laying around */

	unlink(archive);
	return NULL;
}

/**
 * Open an existing LAR archive
 * @param The archive filename to open
 * @return A LAR archive structure for the opened archive
 */
struct lar * lar_open_archive(const char *archive)
{
	struct lar *lar;
	int ret;
	u32 romlen;
	struct stat s;

	lar = calloc(sizeof(*lar), 1);

	if (lar == NULL) {
		err("Unable to allocate memory.\n");
		return NULL;
	}

	lar->fd = open(archive, O_RDWR);

	ret = fstat(lar->fd, &s);

	if (ret == -1) {
		err("Unable to stat %s\n", archive);
		goto err;
	}

	if (_map_lar(lar, s.st_size)) {
		err("Unable to map the archive %s\n", archive);
		goto err;
	}

	/* Sanity check - make sure the bootblock header is the same length
	 * as the LAR archive
	 */

	romlen = lar_read_size(lar);

	if (romlen != s.st_size) {
		err("Size mismatch - the header says %d but the file is %d bytes long.\n",
		    romlen, (int) s.st_size);
		goto err;
	}

	return lar;

 err:
	lar_close_archive(lar);
	return NULL;
}

/**
 * Return the offset of the first chunk of empty space in the LAR
 * @param lar the LAR archive structure
 * @return The offset of the first chunk of empty space
 */
static int lar_empty_offset(struct lar *lar)
{
	u32 offset = 0;
	struct lar_header *header;

	while (offset < get_bootblock_offset(lar->size)) {
		header = (struct lar_header *) (lar->map + offset);

		/* We interpet the absence of the magic as empty space */

		if (memcmp(header->magic, MAGIC, 8))
			break;

		offset += get_next_offset(header);
	}

	if (offset >= get_bootblock_offset(lar->size))
		return -1;

	return offset;
}

/**
 * Return a 1 if filename is in the list of files - if files is NULL, then
 * act as if all files are in the list
 * @param files A list of files to check
 * @param filename The filename to check against the list
 * @return 1 if the file is in the list or if the list is NULL, 0 otherwise
 */
static int file_in_list(struct file *files, char *filename)
{
	struct file *p;

	if (files == NULL)
		return 1;

	for(p = files ; p; p = p->next) {
		if (!strcmp(p->name, filename))
			return 1;
	}

	return 0;
}

/**
 * List the files in a LAR archive
 * @param lar The LAR archive to list
 * @param files A list of files to display- if this is NULL, then the default
 * is to list all files
 */
void lar_list_files(struct lar *lar, struct file *files)
{
	u8 *ptr = lar->map;
	char *filename;

	struct lar_header *header;
	struct file *fp;

	while (ptr < (lar->map + get_bootblock_offset(lar->size))) {
		header = (struct lar_header *) ptr;

		/* We interpet the absence of the magic as empty space */

		if (strncmp(header->magic, MAGIC, 8))
			break;

		filename = (char *) (ptr + sizeof(struct lar_header));

		if (file_in_list(files, filename)) {
			printf("  %s ", filename);

			if (ntohl(header->compression) == none) {
				printf("(%d bytes @0x%lx)\n",
				       ntohl(header->len),
				       (unsigned long)(ptr - lar->map) +
				       ntohl(header->offset));
			} else {
				printf("(%d bytes, %s compressed to %d bytes "
				       "@0x%lx)\n",
				       ntohl(header->reallen),
				       algo_name[ntohl(header->compression)],
				       ntohl(header->len),
				       (unsigned long)(ptr - lar->map) +
				       ntohl(header->offset));
			}
		}

		ptr += get_next_offset(header);
	}

	/* Show the bootblock */

	if (file_in_list(files, BOOTBLOCK_NAME)) {
		header = (struct lar_header *)
			(lar->map + get_bootblock_offset(lar->size));

		printf("  bootblock (%d bytes @0x%x)\n",
		       ntohl(header->len),
		       get_bootblock_offset(lar->size) +
		       ntohl(header->offset));
	}
}

/**
 * Write a buffer to a file - this is used to write blobs from a LAR archive
 * @param filename The file to write
 * @param buffer The source buffer
 * @param len The length of the buffer
 * @return 0 on success , or -1 on failure
 */
static int _write_file(char *filename, u8 *buffer, u32 len)
{
	char *path = strdup(filename);
	int fd, ret;

	if (path == NULL) {
		err("Out of memory.\n");
		return -1;
	}

	mkdirp((const char *) dirname(path), 0755);
	free(path);

	fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd == -1) {
		err("Error creating file %s\n", filename);
		return -1;
	}

	ret = write(fd, buffer, len);

	if (ret != len)
		err("Error writingthe file %s\n", filename);

	close(fd);
	return (ret == len) ? 0 : -1;
}

/**
 * Extract files from a LAR archive.  If files is not NULL, then only
 * extract the filenames listed within
 * @param lar The LAR archive to extract from
 * @param files A list of files to extract.  If NULL then all files are
 * extracted
 * @return 0 on success, or -1 on failure
 */
int lar_extract_files(struct lar *lar, struct file *files)
{
	u8 *ptr = lar->map;
	char *filename;
	struct lar_header *header;
	int ret = 0;

	while (ptr < (lar->map + get_bootblock_offset(lar->size))) {

		header = (struct lar_header *) ptr;

		if (strncmp(header->magic, MAGIC, 8))
			break;

		filename = (char *) (ptr + sizeof(struct lar_header));

		if (file_in_list(files, filename)) {

			if (ntohl(header->compression) == none) {
				ret = _write_file(filename,
						  (u8 *) (ptr + ntohl(header->offset)),
						  (u32) ntohl(header->len));
			}
			else {
				char *buf =
					malloc(ntohl(header->reallen));

				if (buf == NULL) {
					err("Out of memory.\n");
					return -1;
				}

				uncompress_functions[ntohl(header->compression)](
					(char*) buf,
					(char *) ptr + ntohl(header->offset),
					ntohl(header->len));

				ret = _write_file(filename, (u8 *) buf,
						  ntohl(header->reallen));

				free(buf);
			}

			if (ret == -1)
				return -1;
		}

		ptr += get_next_offset(header);
	}

	if (file_in_list(files, BOOTBLOCK_NAME)) {
		header = (struct lar_header *)
			(lar->map + get_bootblock_offset(lar->size));

		ret = _write_file((char *) BOOTBLOCK_NAME,
				  lar->map + (get_bootblock_offset(lar->size) + ntohl(header->offset)),
				  BOOTBLOCK_SIZE);
	}

	return ret;
}

/**
 * Add a new file to the LAR archive
 * @param lar The LAR archive to write into
 * @param name The name of the file to add
 * @return  0 on success, or -1 on failure
 */
int lar_add_file(struct lar *lar, const char *name)
{
	char *filename, *ptr, *temp;
	char *pathname;

	enum compalgo thisalgo;
	struct lar_header *header;
	u32 offset;
	int ret, fd, hlen;
	u32 complen;
	int pathlen;
	struct stat s;
	u32 *walk,  csum;

	/* Find the beginning of the available space in the LAR */
	offset = lar_empty_offset(lar);

	thisalgo = algo;

	filename = (char *) name;

	if (!strncmp(name, "nocompress:",11)) {
		filename += 11;
		thisalgo = none;
	}

	if (filename[0] == '.' && filename[1] == '/')
		filename += 2;

	pathname = strchr(filename, ':');

	if (pathname != NULL) {
	  *pathname = '\0';
	  pathname++;

	  if (!strlen(pathname)) {
	    err("Invalid pathname specified.\n");
	    return -1;
	  }
	}
	else
		pathname = filename;

	/* Open the file */
	fd = open(filename, O_RDONLY);

	if (fd == -1) {
		err("Unable to open %s\n", filename);
		return -1;
	}

	if (fstat(fd, &s)) {
		err("Unable to stat the file %s\n", filename);
		close(fd);
		return -1;
	}

	/* Allocate a temporary buffer to compress into - this is unavoidable,
	   because we need to make sure that the compressed data will fit in
	   the LAR, and we won't know the size of the compressed data until
	   we actually compress it */

	temp = calloc(s.st_size, 1);

	if (temp == NULL) {
		err("Out of memory.\n");
		return -1;
	}

	ptr = mmap(0, s.st_size, PROT_READ, MAP_SHARED, fd, 0);

	if (ptr == MAP_FAILED) {
		err("Unable to map %s\n", filename);
		close(fd);
		free(temp);
		return -1;
	}


	/* Do the compression step */
	compress_functions[thisalgo](ptr, s.st_size, temp, &complen);

	if (complen >= s.st_size && (thisalgo != none)) {
		thisalgo = none;
		compress_functions[thisalgo](ptr, s.st_size, temp, &complen);
	}

	munmap(ptr, s.st_size);
	close(fd);

	pathlen = strlen(pathname) + 1 > MAX_PATHLEN ? MAX_PATHLEN : strlen(pathname) + 1;

	/* Figure out how big our header will be */
	hlen = sizeof(struct lar_header) + pathlen;
	hlen = (hlen + 15) & 0xFFFFFFF0;

	if (offset + hlen + complen >= get_bootblock_offset(lar->size)) {
		err("Not enough room in the LAR to add the file.\n");
		free(temp);
		return -1;
	}

	/* Lets do this thing */

	/* Zero out the header area */
	memset(lar->map + offset, 0, hlen);

	header = (struct lar_header *) (lar->map + offset);

	memcpy(header, MAGIC, 8);
	header->compression = htonl(thisalgo);
	header->reallen = htonl(s.st_size);
	header->len = htonl(complen);
	header->offset = htonl(hlen);

	/* Copy the path name */
	strncpy((char *) (lar->map + offset + sizeof(struct lar_header)),
		pathname, pathlen - 1);

	/* Copy in the data */
	memcpy(lar->map + (offset + hlen), temp, complen);

	/* Figure out the checksum */

	csum = 0;
	for (walk = (u32 *) (lar->map + offset);
	     walk < (u32 *) (temp + complen + hlen);
	     walk++) {
		csum += ntohl(*walk);
	}
	header->checksum = htonl(csum);

	free(temp);
	return 0;
}
