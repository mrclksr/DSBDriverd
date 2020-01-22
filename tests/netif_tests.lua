-- Install luaunit.lua under /usr/local/share/lua/VERSION/ from
-- https://github.com/bluebird75/luaunit
lu = require('luaunit')

-- Creates a file with the given filename and data
local function write_file(fname, data)
	local f, e = io.open(fname, 'w+')
	if f == nil then
		io.stderr:write(e)
		return nil
	end
	for _, d in pairs(data) do
		f:write(d .. '\n')
	end
	f:close()
end

-- Create a mock object to test functions using io.open, io.lines,
-- io.popen, and io.close
local IOMock = {}

function IOMock.new(data_lines)
	local mock = {}
	mock.__data = data_lines
	function mock.open()
		return mock
	end
	function mock.lines()
		local idx  = 0
		function iter()
			idx = idx + 1
			if idx > #mock.__data then
				return nil
			end
			return mock.__data[idx]
		end
		return iter
	end
	function mock.close()
	end
	return mock
end

TestNetif = {}
	function TestNetif:test_match_netif_type()
		local netif = require('netif')
		local found, type = netif.match_netif_type("if_rum")
		lu.assertTrue(found)
		lu.assertEquals(netif.NETIF_TYPE_WLAN, type)
	
		found, type = netif.match_netif_type("if_nf10bmac")
		lu.assertTrue(found)
		lu.assertEquals(netif.NETIF_TYPE_ETHER, type)
	
		found, type = netif.match_netif_type("if_rtwn_pci")
		lu.assertTrue(found)
		lu.assertEquals(netif.NETIF_TYPE_WLAN, type)
	end

	function TestNetif:test_kmod_to_dev()
		local netif = require('netif')
		local name = netif.kmod_to_dev("if_nf10bmac")
		lu.assertNotNil(name)
		lu.assertEquals("nf10bmac", name)
	
		name = netif.kmod_to_dev("if_rtwn_pci")
		lu.assertNotNil(name)
		lu.assertEquals("rtwn", name)
	
		name = netif.kmod_to_dev("foo_usb")
		lu.assertNotNil(name)
		lu.assertEquals("foo", name)
	
	end
	
	function TestNetif:test_find_netif()
		local netif = require('netif')
		local list = { "rtwn0", "alc1", "iwm2", "malo0" }
		local dev = netif.find_netif("alc", list)
		lu.assertNotNil(dev)
		lu.assertEquals("alc1", dev)
		-- Should be nil
		dev = netif.find_netif("mal", list)
		lu.assertNil(dev)
	end

	function TestNetif:test_in_rc_conf()
		local netif = require('netif')
		local rc_conf = {
			'foo="bar"',
			'ifconfig_wlan0="up scan WPA DHCP"',
			'ifconfig_alc1="up DHCP"',
			'ifconfig_alc0="up DHCP"'
		}
		netif.path_rc_conf = os.tmpname()
		write_file(netif.path_rc_conf, rc_conf)
		local found = netif.in_rc_conf("alc0")
		os.remove(netif.path_rc_conf)
		lu.assertTrue(found)
	end

	function TestNetif:test_get_wlan_region()
		local netif = require('netif')
		local zoneinfo = { 'America/Argentina/Buenos_Aires' }
		netif.path_zoneinfo = os.tmpname()
		write_file(netif.path_zoneinfo, zoneinfo)
		local region = netif.get_wlan_region()
		os.remove(netif.path_zoneinfo)
		lu.assertEquals('AR', region)
	end

	function TestNetif:test_link_status()
		local netif = require('netif')
		local ifconfig_output = {
			'alc0: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST> ' ..
			'metric 0 mtu 1500',
			'	options=c3198<VLAN_MTU,VLAN_HWTAGGING,VLAN_HWCSUM,TSO4,' ..
			'WOL_MCAST,WOL_MAGIC,VLAN_HWTSO,LINKSTATE>',
			'	ether 00:25:22:c7:0a:8b',
			'	inet 192.168.2.100 netmask 0xffffff00 broadcast 192.168.2.255',
			'	media: Ethernet autoselect (100baseTX <full-duplex>)',
			'	status: no carrier',
			'	nd6 options=29<PERFORMNUD,IFDISABLED,AUTO_LINKLOCAL>'
		}
		-- Mock get_ifconfig_if_info()
		netif.get_ifconfig_if_info = function()
			return ifconfig_output
		end
		local status = netif.link_status("alc0")
		lu.assertEquals('no carrier', status)
	end

	function TestNetif:test_get_inet_addr()
		local netif = require('netif')
		local ifconfig_output = {
			'lo0: flags=8049<UP,LOOPBACK,RUNNING,MULTICAST> metric 0 ' ..
			'mtu 16384',
			'	options=680003<RXCSUM,TXCSUM,LINKSTATE,RXCSUM_IPV6,' ..
			'TXCSUM_IPV6>',
			'	inet6 ::1 prefixlen 128',
			'	inet6 fe80::1%lo0 prefixlen 64 scopeid 0x2',
			'	inet 127.0.0.1 netmask 0xff000000',
			'	groups: lo',
			'	nd6 options=21<PERFORMNUD,AUTO_LINKLOCAL>'
		}
		-- Mock get_ifconfig_if_info()
		netif.get_ifconfig_if_info = function()
			return ifconfig_output
		end
		local ip4, ip6 = netif.get_inet_addr("lo0")
		lu.assertEquals('127.0.0.1', ip4)
		lu.assertEquals('::1', ip6)
	end

	function TestNetif:wlan_unit_from_parent()
		local netif = require('netif')
		local sysctl_output = {
			'net.wlan.devices: rtwn0 ath0 rtwn1',
			'net.wlan.1.%parent: ath0',
			'net.wlan.2.%parent: rtwn1',
			'net.wlan.0.%parent: rtwn0'
		}
		local mock = IOMock.new(sysctl_output)
		local io = require('io')
		io.popen = mock.open
		io.lines = mock.lines
		io.close = mock.close
		local unit = netif.wlan_unit_from_parent("rtwn0")
		lu.assertEquals(0, unit)
		local unit = netif.wlan_unit_from_parent("rtwn1")
		lu.assertEquals(2, unit)
		local unit = netif.wlan_unit_from_parent("ath0")
		lu.assertEquals(1, unit)
	end

	function TestNetif:test_media_type()
		local netif = require('netif')
		local ifconfig_output_ether = {
			'	media: Ethernet autoselect (100baseTX <full-duplex>)'
		}
		local ifconfig_output_wlan = {
			'	media: IEEE 802.11 Wireless Ethernet OFDM/54Mbps mode 11g'
		}
		-- Mock netif.get_ifconfig_if_info
		netif.get_ifconfig_if_info = function()
			return ifconfig_output_ether
		end
		local media = netif.media_type("unused")
		lu.assertEquals(netif.NETIF_TYPE_ETHER, media)

		-- Mock netif.get_ifconfig_if_info
		netif.get_ifconfig_if_info = function()
			return ifconfig_output_wlan
		end
		media = netif.media_type("unused")
		lu.assertEquals(netif.NETIF_TYPE_WLAN, media)
	end

	function TestNetif:test_wlans_from_rc_conf()
		local netif = require('netif')
		local rc_conf = {
			'foo="bar"',
			'ifconfig_wlan0="up scan WPA DHCP"',
			'wlans_rtwn0="wlan1"',
			'ifconfig_alc1="up DHCP"',
			'ifconfig_alc0="up DHCP"',
			'wlans_rtwn1="wlan0"',
			'wlans_ath0="wlan2"'
		}
		local expect_wlans = {
			{ ["parent"] = "rtwn0", ["child"] = 1 },
			{ ["parent"] = "rtwn1", ["child"] = 0 },
			{ ["parent"] = "ath0",  ["child"] = 2 }
		}
		netif.path_rc_conf = os.tmpname()
		write_file(netif.path_rc_conf, rc_conf)

		local wlans = netif.wlans_from_rc_conf()
		os.remove(netif.path_rc_conf)
		lu.assertEquals(expect_wlans, wlans)
	end
	
	function TestNetif:test_wait_for_new_wlan()
		local netif = require('netif')

		-- Mock netif.get_wlan_devs()
		function get_wlan_devs_mock(timeout)
			local n = timeout
			local wlist1 = {
				{ ['parent'] = 'ath0',  ['child'] = 0 }
			}
			local wlist2 = {
				{ ['parent'] = 'ath0',  ['child'] = 0 },
				{ ['parent'] = 'rtwn0', ['child'] = 1 }
			}
			function iter()
				n = n - 1
				if n < 0 then return wlist2 end
				return wlist1
			end
			return iter
		end
		-- Mock netif.sleep()
		netif.sleep = function() end
		netif.get_wlan_devs = get_wlan_devs_mock(2)

		local wlan = netif.wait_for_new_wlan("if_rtwn_pci", 3)
		lu.assertEquals({ ['parent'] = 'rtwn0', ['child'] = 1 }, wlan)
	end

	function TestNetif:test_set_rc_conf_var()
		local netif = require('netif')
		-- Mock run_sysrc()
		netif.run_sysrc = function(var) return var end
		local var = netif.set_rc_conf_var("ifconfig_wlan0", "up scan DHCP WPA")
		lu.assertEquals('ifconfig_wlan0="up scan DHCP WPA"', var)
	end
	
	function TestNetif:test_add_wlan_to_rc_conf()
		local netif = require('netif')
		local wlan = { ['parent'] = 'rtwn0', ['child'] = 1 }
		wlan_ifconfig_args = "up scan WPA DHCP"
		local expected = {
			'wlans_rtwn0="wlan1"',
			'ifconfig_wlan1="' .. wlan_ifconfig_args .. '"'
		}

		local expected_with_create_args = {
			'wlans_rtwn0="wlan1"',
			'create_args_wlan1="foo bar"',
			'ifconfig_wlan1="' .. wlan_ifconfig_args .. '"'
		}
		-- Mock run_sysrc()
		local lines = {}
		netif.run_sysrc = function(var)
			table.insert(lines, var)
		end
		wlan_create_args = nil
		netif.add_wlan_to_rc_conf(wlan)
		lu.assertEquals(expected, lines)
		
		-- Test with wlan_create_args set
		lines = {}
		wlan_create_args = 'foo bar'
		netif.add_wlan_to_rc_conf(wlan)
		lu.assertEquals(expected_with_create_args, lines)
	end

	function TestNetif:test_get_ifconfig_iflist()
		local netif = require('netif')
		local output = { 'alc0 em0 wlan0' }
		local mock = IOMock.new(output)
		local io = require('io')
		io.popen = mock.open
		io.lines = mock.lines
		io.close = mock.close
		local iflist = netif.get_ifconfig_iflist()
		lu.assertEquals({ 'alc0', 'em0', 'wlan0' }, iflist)
	end

os.exit(lu.LuaUnit.run())
