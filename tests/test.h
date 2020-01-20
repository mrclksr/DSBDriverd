ATF_TC_WITHOUT_HEAD(parse_devd_event);
ATF_TC_BODY(parse_devd_event, tc)
{
	char *ev1 = strdup("!system=USB subsystem=DEVICE type=ATTACH "	    \
			   "ugen=ugen4.3 cdev=ugen4.3 vendor=0x8564 "	    \
			   "product=0x1000 devclass=0x00 devsubclass=0x00 " \
			   "sernum=\"15H0FJ69EWI876TT\" release=0x1100 "    \
			   "mode=host port=3 parent=ugen4.1");

	char *ev2 = strdup("!system=USB subsystem=INTERFACE type=ATTACH "   \
			   "ugen=ugen4.3 cdev=ugen4.3 vendor=0x8564 "	    \
			   "product=0x1000 devclass=0x00 devsubclass=0x00 " \
			   "sernum=\"15H0FJ69EWI876TT\" release=0x1100 "    \
			   "mode=host interface=0 endpoints=2 "		    \
			   "intclass=0x08 intsubclass=0x06 intprotocol=0x50");

	ATF_REQUIRE(ev1 != NULL);
	ATF_REQUIRE(ev2 != NULL);

	parse_devd_event(ev1);
	ATF_CHECK_EQ(DEVD_SYSTEM_USB, devdevent.system);
	ATF_CHECK_EQ(DEVD_TYPE_ATTACH, devdevent.type);
	ATF_CHECK_STREQ("ugen4.3", devdevent.cdev);
	ATF_CHECK_STREQ("DEVICE", devdevent.subsystem);

	parse_devd_event(ev2);
	ATF_CHECK_EQ(DEVD_SYSTEM_USB, devdevent.system);
	ATF_CHECK_EQ(DEVD_TYPE_ATTACH, devdevent.type);
	ATF_CHECK_STREQ("ugen4.3", devdevent.cdev);
	ATF_CHECK_STREQ("INTERFACE", devdevent.subsystem);
}

ATF_TC_WITHOUT_HEAD(find_driver);
ATF_TC_BODY(find_driver, tc)
{
	char	  *testdriver1, *testdriver2, *testdriver3, *testdriver4;
	iface_t	  testdev4_iface;
	devinfo_t testdev1, testdev2, testdev3, testdev4;
	
	open_dbs();

	bzero(&testdev1, sizeof(testdev1));
	bzero(&testdev2, sizeof(testdev2));
	bzero(&testdev3, sizeof(testdev3));
	bzero(&testdev4, sizeof(testdev4));

	/*
	 * Test that matches against vendor, device, subvendor, and
	 * subdevice
	 */
	testdev1.vendor    = 0x14e4;
	testdev1.device    = 0x16aa;
	testdev1.subvendor = 0x103c;
	testdev1.subdevice = 0x3102;

	testdriver1 = find_driver(&testdev1);
	ATF_REQUIRE(testdriver1 != NULL);
	ATF_CHECK_STREQ_MSG("if_bce", testdriver1, "drivername is %s",
	    testdriver1);

	/*
	 * Test returning multiple driver names
	 */
	testdev2.vendor = 0x14e4;
	testdev2.device = 0x4306;

	testdriver2 = find_driver(&testdev2);
	ATF_REQUIRE(testdriver2 != NULL);
	ATF_CHECK_STREQ("if_bwn", testdriver2);
	testdriver2 = find_driver(NULL);
	ATF_REQUIRE(testdriver2 != NULL);
	ATF_CHECK_STREQ("bwn_v4_ucode", testdriver2);

	/*
	 * Test which matches against vendor, device, and "revision"
	 * keyword.
	 */
	testdev3.vendor   = 0x108e;
	testdev3.device   = 0xabba;
	testdev3.revision = 0x10;

	testdriver3 = find_driver(&testdev3);
	ATF_REQUIRE(testdriver3 != NULL);
	ATF_CHECK_STREQ("if_cas", testdriver3);

	/*
	 * Test matching against vendor, device wildcard, and "ifclass",
	 * "ifsubclass", and "protocol" keyword.
	 */
	testdev4.vendor = 0x5ac;
	testdev4.device = 0x1234; /* Can be anything */
	testdev4.nifaces = 1;
	testdev4.iface  = &testdev4_iface;
	testdev4.iface[0].class    = 0x255;
	testdev4.iface[0].subclass = 0x253;
	testdev4.iface[0].protocol = 0x1;

	testdriver4 = find_driver(&testdev4);
	ATF_REQUIRE(testdriver4 != NULL);
	ATF_CHECK_STREQ("if_ipheth", testdriver4);
}

ATF_TC_WITHOUT_HEAD(match_kmod_name);
ATF_TC_BODY(match_kmod_name, tc)
{
	/* Should match */
	ATF_CHECK(match_kmod_name("uhub/uaudio", "uaudio"));
	ATF_CHECK(match_kmod_name("uhub/uaudio.ko", "uaudio"));
	ATF_CHECK(match_kmod_name("uaudio.ko", "uaudio"));
	ATF_CHECK(match_kmod_name("snd_emu10kx_pcm", "snd_emu10kx_pcm"));

	/* Should not match */
	ATF_CHECK(!match_kmod_name("alc/miibus", "alc"));
	ATF_CHECK(!match_kmod_name("uhub/uaudio", "uaudi"));
	ATF_CHECK(!match_kmod_name("", "foo"));
	ATF_CHECK(!match_kmod_name("foo", ""));
	ATF_CHECK(!match_kmod_name("foo", "fo"));

}

ATF_TC_WITHOUT_HEAD(get_devdescr);
ATF_TC_BODY(get_devdescr, tc)
{
	char	  *descr1, *descr2;
	devinfo_t testdev_pci1, testdev_pci2;

	open_dbs();
	bzero(&testdev_pci1, sizeof(testdev_pci1));
	bzero(&testdev_pci2, sizeof(testdev_pci2));

	/*
	 * Test with vendor, device, subvendor, and subdevice
	 */
	testdev_pci1.vendor    = 0x0e11;
	testdev_pci1.device    = 0xb178;
	testdev_pci1.subvendor = 0x0e11;
	testdev_pci1.subdevice = 0x4082;

	descr1 = get_devdescr(pcidb, &testdev_pci1);
	ATF_REQUIRE(descr1 != NULL);
	ATF_CHECK_STREQ_MSG(descr1,
	    "Compaq Computer Corporation Smart Array 5i/532 Smart Array 532",
	    "descr1 == \"%s\"", descr1);

	/*
	 * Test with a vendor-and-device-only record.
	 */
	testdev_pci2.vendor = 0x1023;
	testdev_pci2.device = 0x9350;

	descr2 = get_devdescr(pcidb, &testdev_pci2);
	ATF_REQUIRE(descr2 != NULL);
	ATF_CHECK_STREQ_MSG(descr2, "Trident Microsystems GUI Accelerator",
	    "descr2 == \"%s\"", descr2);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, parse_devd_event);
	ATF_TP_ADD_TC(tp, find_driver);
	ATF_TP_ADD_TC(tp, match_kmod_name);
	ATF_TP_ADD_TC(tp, get_devdescr);
	return atf_no_error();
}
