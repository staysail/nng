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

#ifdef _WIN32
#include <direct.h>

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

	(void) snprintf(homedir, size, "%s%s/.nnztagent", getenv("HOMEDRIVE"),
	    getenv("HOMEPATH"));
}

int
nnzt_agent_get_file(const char *path, char **data, int *size)
{
	FILE * f;
	char * buf;
	int    sz;
	HANDLE h;

	// Note that we assume the default security descriptor for the user
	// only grants access to that user.  We could perhaps enforce this
	// by explicitly managing the security descriptor, but that might
	// prove to be limiting for some use cases.
	h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	    OPEN_EXISTING, FILE_FLAG_NORMAL, NULL);

	if (h == INVALID_HANDLE_VALUE) {
		int err;
		switch ((err = GetLastError())) {
		case ERROR_ACCESS_DENIED:
		case ERROR_SHARING_VIOLATION:
		case ERROR_LOCK_VIOLATION:
			return (NNG_EPERM);
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
			return (NNG_ENOENT);
		case ERROR_NOT_ENOUGH_MEMORY:
		case ERROR_OUTOFMEMORY:
			return (NNG_ENOMEM);
		case ERROR_TOO_MANY_OPEN_FILES:
		// XXX we want an NNG_ERR for this
		default:
			return (NNG_ESYSERR + err);
		}
	}

	// XXX: ReadFile...
	// XXX: CloseHandle ...

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
	int   rv;

	if ((f = fopen(path, "wb")) == NULL) {
		switch (errno) {
		case ENOENT:
			rv = NNG_ENOENT;
			break;
		case EEXIST:
		case EPERM:
			rv = NNG_EPERM;
			break;
		case ENOMEM:
			rv = NNG_ENOMEM;
			break;
		default:
			rv = NNG_ESYSERR + errno;
			break;
		}
	}
	if ((rv == 0) && (fwrite(data, 1, size, f) != size)) {
		rv = NNG_ESYSERR + errno;
	}
	if (f != NULL) {
		(void) fclose(f);
	}

	return (rv);
}

int
nnzt_agent_mkhome(const char *homedir)
{
	(void) _mkdir(homedir);
	return (0);
}

#endif