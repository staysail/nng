//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <cstring>
#include <iostream>
#include <queue>
#include <string>
#include <vector>

#include "nnztagent_utils.h"

#include "nnztagent_getopt.hpp"

#include "nng.h"

int
main(int argc, char *argv[])
{
	auto        opts     = nnztagent::GetOpt(argc, argv, "hlk:d:u:");
	std::string authfile = "";
	std::string homedir  = "";
	std::string url      = "";
	std::string optarg;
	char *      authdata;
	int         authsize;
	char        optc;
	int         rv;

	while (opts.Next(optc, optarg)) {
		switch (optc) {
		case '?':
			std::cerr << opts.Error() << std::endl;
			return (1);
		case 'h':
			std::cout << "Help message here!" << std::endl;
			break;
		case 'l':
			std::cout << "What does 'l' do?" << std::endl;
			break;
		case 'd':
			homedir = optarg;
			break;
		case 'a':
			authfile = optarg;
			break;
		case 'u':
			url = optarg;
			break;
		default:
			std::cerr << "Default switch!" << std::endl;
			return (1);
		}
	}

	for (auto s : opts.Arguments()) {
		std::cout << "Trailing argument: '" << s << "'" << std::endl;
	}

	// If we were not given a home directory, get a default one.
	if (homedir == "") {
		char hdir[256];
		nnzt_agent_homedir(hdir, sizeof(hdir));
		homedir = hdir;
	}

	if (authfile == "") {
		authfile = homedir + "/agentauth";
	}

	if ((rv = nnzt_agent_get_file(
	         authfile.c_str(), &authdata, &authsize)) != 0) {
		// Failed to get the file... if it did not exist, then
		// let's create one. We create 64-bits which is adequate
		// for our needs.
		if (rv == NNG_ENOENT) {
			authsize = 8;
			if ((authdata = new char[authsize]) == NULL) {
				std::cerr << "Out of memory!" << std::endl;
				return (1);
			}

			// Fill the data with random stuff.
			for (int i = 0; i < authsize; i += sizeof(uint64_t)) {
				uint64_t r = nnzt_agent_random();
				memcpy(authdata + i, &r, sizeof(r));
			}

			if (nnzt_agent_put_file(
			        authfile.c_str(), authdata, authsize) != 0) {
				std::cerr << "Unable to create auth file"
				          << std::endl;
				return (1);
			}
			std::cout << "Created initial random auth file"
			          << std::endl;
		} else {
			std::cerr << "Unable to open auth file" << std::endl;
			return (1);
		}
	} else {
		std::cout << "Opened auth file" << std::endl;
	}

	std::cout << "Home: " << homedir << std::endl;
	std::cout << "AuthFile: " << authfile << std::endl;
	std::cout << "URL: " << url << std::endl;
	return (0);
}