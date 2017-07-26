//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef NNZTAGENT_UTILS_H_
#define NNZTAGENT_UTILS_H_

#ifdef __cplusplus
#extern "C" {
#endif

extern void nnzt_agent_homedir(char *homedir, int size);
extern int nnzt_agent_get_file(const char *path, char **data, int *size);
extern int nnzt_agent_put_file(const char *path, char *data, int size);

#ifdef __cplusplus
}
#endif

#endif // NNZTAGENT_UTILS_H