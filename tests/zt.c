//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "convey.h"
#include "trantest.h"

extern int         nng_zt_register(void);
extern const char *nng_opt_zt_home;
extern int         nng_optid_zt_home;

// zerotier tests.

// This network is an open network setup exclusively for nng testing.
// Do not attach to it in production.
#define NWID "a09acf02337b057b"

#ifdef _WIN32

int
mkdir(const char *path, int mode)
{
	CreateDirectory(path, NULL);
}
#else
#include <sys/stat.h>
#include <unistd.h>
#endif // WIN32

TestMain("ZeroTier Transport", {

	// trantest_test_all("tcp://127.0.0.1:%u");
	char     path1[NNG_MAXADDRLEN];
	char     path2[NNG_MAXADDRLEN];
	unsigned port;

	trantest_next_address(path1, "/tmp/zt_serv_%u");
	trantest_prev_address(path2, "/tmp/zt_clnt_%u");
	port = trantest_port - 1;

	atexit(nng_fini);

	Convey("We can register the zero tier transport", {
		So(nng_zt_register() == 0);
		printf("nng_zt_register done!\n");
	});

	Convey("We can create a zt listener", {
		nng_listener l;
		nng_socket   s;
		char         addr[NNG_MAXADDRLEN];
		int          rv;

		snprintf(addr, sizeof(addr), "zt://" NWID ":%u", port);

		So(nng_pair_open(&s) == 0);
		Reset({ nng_close(s); });

		So(nng_listener_create(&l, s, addr) == 0);

		Convey("We can lookup zerotier home option id", {
			So(nng_optid_zt_home > 0);
			So(nng_option_lookup(nng_opt_zt_home) ==
			    nng_optid_zt_home);
		});

		Convey("And it can be started...", {

			mkdir(path1, 0700);

			So(nng_listener_setopt(l, nng_optid_zt_home, path1,
			       strlen(path1) + 1) == 0);

			So(nng_listener_start(l, 0) == 0);

			nng_usleep(5000000);
		})
	});

	Convey("We can create a zt dialer", {
		nng_dialer d;
		nng_socket s;
		char       addr[NNG_MAXADDRLEN];
		int        rv;
		uint64_t   node = 0xb000072fa6ull; // my personal host for now

		snprintf(
		    addr, sizeof(addr), "zt://" NWID "/%llx:%u", node, port);

		So(nng_pair_open(&s) == 0);
		Reset({ nng_close(s); });

		printf("ADDR is %s\n", addr);
		So(nng_dialer_create(&d, s, addr) == 0);

		Convey("We can lookup zerotier home option id", {
			So(nng_optid_zt_home > 0);
			So(nng_option_lookup(nng_opt_zt_home) ==
			    nng_optid_zt_home);
		});

		Convey("And it can be started...", {
			mkdir(path2, 0700);

			So(nng_dialer_setopt(d, nng_optid_zt_home, path2,
			       strlen(path2) + 1) == 0);

			So(nng_dialer_start(d, 0) == NNG_ETIMEDOUT);
		})
	});

#if 0
	Convey("We cannot connect to wild cards", {
		nng_socket s;

		char       addr[NNG_MAXADDRLEN];

		So(nng_pair_open(&s) == 0);
		Reset({ nng_close(s); });
		trantest_next_address(addr, "zt://*:%u");
		So(nng_dial(s, addr, NULL, 0) == NNG_EADDRINVAL);
	});

	Convey("We can bind to wild card", {
		nng_socket s1;
		nng_socket s2;
		char       addr[NNG_MAXADDRLEN];

		So(nng_pair_open(&s1) == 0);
		So(nng_pair_open(&s2) == 0);
		Reset({
			nng_close(s2);
			nng_close(s1);
		});
		trantest_next_address(addr, "tcp://*:%u");
		So(nng_listen(s1, addr, NULL, 0) == 0);
		// reset port back one
		trantest_prev_address(addr, "tcp://127.0.0.1:%u");
		So(nng_dial(s2, addr, NULL, 0) == 0);
	});
#endif
})
