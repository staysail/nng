//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef NNZTAGENT_GETOPT_HPP_
#define NNZTAGENT_GETOPT_HPP_

#include <cstring>
#include <iostream>
#include <queue>
#include <string>
#include <vector>

namespace nnztagent {

/// C++ class encapsulating a reasonable getopt(1) style parser.
/// No support for long getopts, but clustering and putting the argument
/// next to the option without intervening space is supported.  This should
/// be reasonably POSIX compliant.
class GetOpt {
    public:
	/// Constructor, initializes from the main() arguments and
	/// a getopt(3c) style option string.  The argv array is
	/// assumed to contain the program name in argv[0].
	/// @param argc number of elements in argv
	/// @param argv program arguments
	/// @param opts getopt style string (e.g. "hvk:")
	/// @param index number of elements in argv to skip past
	GetOpt(int argc, char *argv[], std::string opts, int index = 1)
	{
		for (int i = 0; i < argc; i++) {
			args.push_back(argv[i]);
			work.push(argv[i]);
		}
		while ((index > 0) && (!work.empty())) {
			work.pop();
			index--;
		}
		optstring = opts;
	}

	/// Constructor, may be useful for other uses where an
	/// array of options can be parsed.
	GetOpt(std::vector<std::string> argv, std::string opts, int index = 1)
	{
		args = argv;
		for (auto s : argv) {
			work.push(s);
		}
		while ((index > 0) && (!work.empty())) {
			work.pop();
			index--;
		}
		optstring = opts;
	}

	/// Reset the option parser, for multiple passes.
	/// @param index the number elements in the arguments to skip past
	void Reset(int index = 1)
	{
		while (!work.empty()) {
			work.pop();
		}
		for (auto s : args) {
			work.push(s);
		}
		while ((index > 0) && (!work.empty())) {
			work.pop();
			index--;
		}
	}

	/// Get the next option flag, and argument string.
	/// (Maybe we should look at an iterator pattern here?)
	/// @param optc [out] the option character found
	/// @param optarg [out] a string associated with the option
	/// @return true if an option was parsed, false if no more options
	bool Next(char &optc, std::string &optarg)
	{
		if (work.empty()) {
			return false;
		}
		auto str = work.front();
		if (str == "") {
			return false;
		}
		if (str == "--") {
			// End of options.
			work.pop();
			return (false);
		}
		if ((str[0] != '-') || (str == "-")) {
			// Lone '-' is an arg not an option (e.g. stdin).
			// If not starting with a -, then its not an
			// option either.
			return (false);
		}

		optc     = str[1];
		auto pos = optstring.find_first_of(optc);
		if (pos == std::string::npos) {
			// No matching option flag.
			optarg = "";
			errstr = "Unknown option '";
			errstr += optc;
			errstr += "'";
			optc = '?';
			if (str.length() > 2) {
				// Option cluster, move to next option.
				work.front().erase(1, 1);
			} else {
				// Just pop the entire argument.
				work.pop();
			}
			return true;
		}
		if ((optstring.size() > pos) && (optstring[pos + 1] == ':')) {
			// option takes an argument.
			if (str.length() > 2) {
				// It is attached...
				optarg = str.substr(2);
				work.pop();
				return true;
			}

			work.pop();
			if (work.empty()) {
				// Missing opt arg!
				errstr = "Option '";
				errstr += optc;
				errstr += "' missing argument";
				optc   = '?';
				optarg = "";
				return true;
			}
			optarg = work.front();
			work.pop();
			return true;
		}

		optarg = "";
		if (str.length() > 2) {
			// Option cluster, move to next option.
			work.front().erase(1, 1);
		} else {
			work.pop();
		}
		return true;
	}

	/// Error returns a string describing the last parse error.
	std::string Error() { return errstr; }

	/// After Next() returns false, Arguments() can be used to
	/// return the array of arguments remaining.
	std::vector<std::string> Arguments()
	{
		std::vector<std::string> v;
		while (!work.empty()) {
			v.push_back(work.front());
			work.pop();
		}
		return (v);
	}

    private:
	std::vector<std::string> args;
	std::queue<std::string>  work;
	std::string              optstring;
	std::string              errstr;
};

} // namespace nnztagent

#endif // NNZTAGENT_GETOPT_HPP