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

extern int nng_zt_register(void);

// zerotier tests.

// This network is an open network setup exclusively for nng testing.
// Do not attach to it in production.
#define NWID "a09acf02337b057b"

TestMain("ZeroTier Transport", {

	// trantest_test_all("tcp://127.0.0.1:%u");

	Convey("We can register the zero tier transport", {
		So(nng_zt_register() == 0);
		printf("nng_zt_register done!\n");
	});

	Convey("We can create a zt listener", {
		nng_listener l;
		nng_socket   s;
		char         addr[NNG_MAXADDRLEN];
		int          rv;

		trantest_next_address(addr, "zt://" NWID ":%u");
		printf("ADDRESS is %s\n", addr);

		So(nng_pair_open(&s) == 0);
		Reset({ nng_close(s); });

		rv = nng_listener_create(&l, s, addr);
		printf("RV is %d %s\n", rv, nng_strerror(rv));
		So(rv == 0);
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

	nng_fini();
})
