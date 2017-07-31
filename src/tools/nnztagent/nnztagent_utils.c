//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/nng_impl.h"

void
nnzt_agent_homedir(char *homedir, int size)
{
	char *home;

	// Always honor the NNZTAGENTHOME first.  No underscores for
	// maximum portability.
	if ((home = getenv("NNZTAGENTHOME")) != NULL) {
		(void) snprintf(homedir, size, "%s", home);
		return;
	}

	// HOME is required by POSIX.
	if ((home = getenv("HOME")) != NULL) {
		(void) snprintf(homedir, size, "%s/.nnztagent", home);
		return;
	}
	(void) snprintf(homedir, size, "/.nnztagent");
}

int
nnzt_agent_get_file(const char *path, char **datap, int *sizep)
{
	char *      buf;
	int         sz;
	int         fd;
	int         rv;
	int         n;
	struct stat st;

	if ((fd = open(path, O_RDONLY)) < 0) {
		return (nni_plat_errno(errno));
	}

	if (fstat(fd, &st) < 0) {
		rv = errno;
		(void) close(fd);
		return (nni_plat_errno(rv));
	}

	// 2MB limit - there is no reason for any key file to be larger
	// than this.
	if ((sz = st.st_size) > (1U << 21)) {
		(void) close(fd);
		return (NNG_EMSGSIZE);
	}

	if ((buf = nni_alloc(sz)) == NULL) {
		(void) close(fd);
		return (NNG_ENOMEM);
	}
	*sizep = sz;
	*datap = buf;

	while (sz > 0) {
		if ((n = read(fd, buf, sz)) < 0) {
			rv = errno;
			(void) close(fd);
			free(buf);
			*sizep = 0;
			*datap = NULL;
			return (nni_plat_errno(rv));
		}
		sz -= n;
		buf += n;
	}
	(void) close(fd);
	return (0);
}

int
nnzt_agent_put_file(const char *path, char *data, int size)
{
	// Use with caution -- this will overwrite any existing file.
	FILE * f;
	int    rv;
	int    fd;
	int    n;
	mode_t mode = S_IRUSR | S_IWUSR; // mode 0600

	rv = 0;
	if ((fd = open(path, O_CREAT | O_EXCL | O_WRONLY, mode)) < 0) {
		return (nni_plat_errno(errno));
	}

	while (size > 0) {
		if ((n = write(fd, data, size)) < 0) {
			rv = nni_plat_errno(rv);
			(void) close(fd);
			return (nni_plat_errno(rv));
		}
		size -= n;
		data += n;
	}
	NNI_ASSERT(size == 0);

	if (close(fd) < 0) {
		return (nni_plat_errno(errno));
	}
	return (0);
}

int
nnzt_agent_mkhome(const char *homedir)
{
	int rv;
	if (mkdir(homedir, S_IRUSR | S_IWUSR | S_IXUSR) < 0) {
		if ((rv = errno) == EEXIST) {
			// Already exists!  This is good.
			return (0);
		}
		return (nni_plat_errno(rv));
	}
	return (0);
}

#endif // _WIN32