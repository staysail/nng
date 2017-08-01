//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifdef PLATFORM_WINDOWS
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

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
	FILE *             f;
	char *             buf;
	LARGE_INTEGER      sz64;
	DWORD              sz;
	DWORD              nread;
	HANDLE             h;
	int                rv;
	FILE_STANDARD_INFO info;

	// Note that we assume the default security descriptor for the user
	// only grants access to that user.  We could perhaps enforce this
	// by explicitly managing the security descriptor, but that might
	// prove to be limiting for some use cases.
	h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	    OPEN_EXISTING, FILE_FLAG_NORMAL, NULL);

	if (h == INVALID_HANDLE_VALUE) {
		return (nni_win_error(GetLastError()));
	}

	// NB: Simpler GetFileSizeEx is not supported for Windows store apps.
	if (GetFileInformationByHandleEx(
	        h, FileStandardInfo, &info, sizeof(nfo)) != 0) {
		rv = GetLastError();
		CloseHandle(h);
		return (nni_win_error(rv));
	}

	// File too large (2MB)?
	if (info.AllocationSize.QuadPart > (1U << 21)) {
		CloseHandle(h);
		return (NNG_EMSGSIZE);
	}
	sz = info.AllocationSize.LowPart;

	if ((buf = nni_alloc(sz)) == NULL) {
		CloseHandle(h);
		return (NNG_ENOMEM);
	}

	if (ReadFile(h, buf, sz, &nread, NULL) != 0) {
		rv = GetLastError();
		CloseHandle(h);
		nni_free(buf, sz);
		return (nni_win_error(rv));
	}

	if (nread != sz) {
		CloseHandle(h);
		nni_free(buf, sz);
		// This should not happen -- the file got truncated!
		return (NNG_EINTERNAL);
	}

	(void) CloseHandle(h);

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

#endif // PLATFORM_WINDOWS