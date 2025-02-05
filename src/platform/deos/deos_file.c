//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

#ifdef NNG_PLATFORM_DEOS

// FACE technically allows some of these APIs, but we don't need them,
// and won't use them in this environment, so just stub them out.

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

// File support.

int
nni_plat_file_put(const char *name, const void *data, size_t len)
{
	NNI_ARG_UNUSED(name);
	NNI_ARG_UNUSED(data);
	NNI_ARG_UNUSED(size);
	return (NNG_ENOTSUP);
}

int
nni_plat_file_get(const char *name, void **datap, size_t *lenp)
{
	NNI_ARG_UNUSED(name);
	NNI_ARG_UNUSED(datap);
	NNI_ARG_UNUSED(lenp);
	return (NNG_ENOTSUP);
}

int
nni_plat_file_delete(const char *name)
{
	NNI_ARG_UNUSED(name);
	return (NNG_ENOTSUP);
}

int
nni_plat_file_type(const char *name, int *typep)
{
	NNI_ARG_UNUSED(name);
	NNI_ARG_UNUSED(typep);
	return (NNG_ENOTSUP);
}

int
nni_plat_file_walk(
    const char *name, nni_plat_file_walker walkfn, void *arg, int flags)
{
	NNI_ARG_UNUSED(name);
	NNI_ARG_UNUSED(walkfn);
	NNI_ARG_UNUSED(arg);
	NNI_ARG_UNUSED(flags);
	return (NNG_ENOTSUP);
}

const char *
nni_plat_file_basename(const char *path)
{
	const char *end;
	if ((end = strrchr(path, '/')) != NULL) {
		return (end + 1);
	}
	return (path);
}

int
nni_plat_file_lock(const char *path, nni_plat_flock *lk)
{
	NNI_ARG_UNUSED(path);
	NNI_ARG_UNUSED(lk);
	return (NNG_ENOTSUP);
}

void
nni_plat_file_unlock(nni_plat_flock *lk)
{
	NNI_ARG_UNUSED(lk);
	return (NNG_ENOTSUP);
}

char *
nni_plat_temp_dir(void)
{
	return (NULL);
}

char *
nni_plat_join_dir(const char *prefix, const char *suffix)
{
	NNI_ARG_UNUSED(prefix);
	NNI_ARG_UNUSED(suffix);
	return (NULL);
}

#endif // NNG_PLATFORM_DEOS
