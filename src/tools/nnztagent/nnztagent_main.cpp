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

int
main(int argc, char *argv[])
{
	auto        opts    = nnztagent::GetOpt(argc, argv, "hlk:d:u:");
	std::string keyfile = "";
	std::string homedir = "";
	std::string url     = "";
	std::string optarg;
	char        optc;

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
		case 'k':
			keyfile = optarg;
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

	if (keyfile == "") {
		keyfile = homedir + "/agentkey";
	}

	std::cout << "Home: " << homedir << std::endl;
	std::cout << "KeyFile: " << keyfile << std::endl;
	std::cout << "URL: " << url << std::endl;
	return (0);
}