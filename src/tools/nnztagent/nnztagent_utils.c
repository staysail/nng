//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

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

#ifdef _WIN32
	(void) snprintf(homedir, size, "%s%s/.nnztagent", getenv("HOMEDRIVE"),
	    getenv("HOMEPATH"));
#else
	// HOME is required by POSIX.
	if ((home = getenv("HOME")) != NULL) {
		(void) snprintf(homedir, size, "%s/.nnztagent", home);
		return;
	}
	(void) snprintf(homedir, size, "/.nnztagent");
#endif
}

int
nnzt_agent_get_file(const char *path, char **data, int *size)
{
	FILE *f;
	char *buf;
	int   sz;

	if ((f = fopen(path, "rb")) == NULL) {
		// This could be EPERM too...
		switch (errno) {
		case ENOENT:
			return (NNG_ENOENT);
		case ENOMEM:
			return (NNG_ENOMEM);
		case EPERM:
			return (NNG_EPERM);
		default:
			return (NNG_ESYSERR + errno);
		}
	}

	// NB: We are kind of assuming that the files we are interested
	// in are small!  If a long won't hold the size, you don't want
	// this anyway.
	if (fseek(f, 0L, SEEK_END) < 0) {
		(void) fclose(f);
		return (NNG_ESYSERR + errno);
	}
	sz = (int) ftell(f);

	// If its bigger than 2 MB, we reject it.
	if (sz > 1U << 21) {
		(void) fclose(f);
		return (NNG_EMSGSIZE);
	}

	if ((buf = nni_alloc(sz)) == NULL) {
		(void) fclose(f);
		return (NNG_ENOMEM);
	}
	if (fread(buf, 1, sz, f) != sz) {
		(void) fclose(f);
		nni_free(buf, sz);
		return (NNG_EMSGSIZE);
	}
	(void) fclose(f);

	*data = buf;
	*size = sz;
	return (0);
}

int
nnzt_agent_put_file(const char *path, char *data, int size)
{
	// Use with caution -- this will overwrite any existing file.
	FILE *f;

	if ((f = fopen(path, "wb")) == NULL) {
		switch (errno) {
		case ENOENT:
			return (NNG_ENOENT);
		case EEXIST:
			return (NNG_EBUSY);
		case ENOMEM:
			return (NNG_ENOMEM);
		case EPERM:
			return (NNG_EPERM);
		default:
			return (NNG_ESYSERR + errno);
		}
	}
	if (fwrite(data, 1, size, f) != size) {
		(void) fclose(f);
		return (NNG_ESYSERR + errno);
	}
	(void) fclose(f);
	return (0);
}

// We only support connecting to looopback or IPC addresses; others are
// subject to observation or tampering or both.
#define TCP4_LOOP "tcp://127.0.0.1:"
#define TCP6_LOOP "tcp://::1:"
#define IPC_ADDR "ipc://"
