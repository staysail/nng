//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifdef PLATFORM_POSIX
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

uint64_t
nnzt_agent_random(void)
{
	uint64_t val;
#if defined(NNG_USE_GETRANDOM)
	// Latest Linux has a nice API here.
	(void) getrandom(&val, sizeof(val), 0);
#elif defined(NNG_USE_GETENTROPY)
	// Modern BSD systems prefer this, but can only generate 256 bytes
	(void) getentropy(&val, sizeof(val));
#elif defined(NNG_USE_ARC4RANDOM)
	// This uses BSD style pRNG seeded from the kernel in libc.
	(void) arc4random_buf(&val, sizeof(val));
#elif defined(NNG_USE_DEVURANDOM)
	// The historic /dev/urandom device.  This is not as a good as
	// a system call, since file descriptor attacks are possible,
	// and it may need special permissions.  We choose /dev/urandom
	// over /dev/random to avoid diminishing the system entropy.
	int fd;

	if ((fd = open("/dev/urandom", O_RDONLY)) >= 0) {
		(void) read(fd, &val, sizeof(val));
		(void) close(fd);
	}
#endif

	// Let's mixin a few other things...  There are a lot of other
	// things we could mix in depending on the platform, but we're
	// really hoping we already got reasonable random data this far.
	val ^= time(NULL);
	val ^= getpid();
	val ^= getuid();
	val ^= (uint64_t)(time(NULL)); // Mix up the high order bits too
	val ^= (uintptr_t) &val;       // Take advantage of ASLR if in use!

	return (val);
}

#endif // PLATFORM_POSIX