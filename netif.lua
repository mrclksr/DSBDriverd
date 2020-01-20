-- Copyright (c) 2019 Marcel Kaiser <mk@freeshell.de>
--
-- Redistribution and use in source and binary forms, with or without
-- modification, are permitted provided that the following conditions
-- are met:
-- 1. Redistributions of source code must retain the above copyright
--    notice, this list of conditions and the following disclaimer.
-- 2. Redistributions in binary form must reproduce the above copyright
--    notice, this list of conditions and the following disclaimer in the
--    documentation and/or other materials provided with the distribution.
--
-- THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
-- ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
-- IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
-- ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
-- FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
-- DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
-- OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
-- HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
-- LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
-- OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
-- SUCH DAMAGE.
--

local netif = {}

-- Constants for the match_netif_type() function
netif.NETIF_TYPE_WLAN  = 1
netif.NETIF_TYPE_ETHER = 2

netif.path_rc_conf  = '/etc/rc.conf'
netif.path_zoneinfo = '/var/db/zoneinfo'
netif.path_zone_tab = '/usr/share/zoneinfo/zone.tab'

-- Returns a pair, (true|false, NETIF_TYPE_WLAN|NETIF_TYPE_ETHER|nil),
-- if the given driver name matches an ethernet or wireless device driver.
function netif.match_netif_type(driver)
	local m
	local wlan_kmods = {
		"if_zyd",    "if_ath",      "if_bwi",      "if_bwn",
		"if_ipw",    "if_iwi",      "if_iwm",      "if_iwn",
		"if_malo",   "if_mwl",      "if_otus",     "if_ral",
		"if_rsu",    "if_rtwn_usb", "if_rtwn_pci", "if_rum",
		"if_run",    "if_uath",     "if_upgt",     "if_ural",
		"if_urtw",   "if_wi"
	}
	local ether_kmods = {
		"if_ae",     "if_age",      "if_alc",      "if_ale",
		"if_aue",    "if_axe",      "if_bce",      "if_bfe",
		"if_bge",    "if_bnxt",     "if_bxe",      "if_cas",
		"if_cdce",   "if_cue",      "if_cxgb",     "if_dc",
		"if_de",     "if_ed",       "if_edsc",     "if_em",
		"if_et",     "if_fwe",      "if_fxp",      "if_gem",
		"if_hme",    "if_ntb",      "if_ipheth",   "if_ix",
		"if_ixl",    "if_jme",      "if_kue",      "if_le",
		"if_lge",    "if_mos",      "if_msk",      "if_mxge",
		"if_my",     "if_nf10bmac", "if_nfe",      "if_nge",
		"if_pcn",    "if_ptnet",    "if_qlnxe",    "if_qlxgb",
		"if_qlxgbe", "if_qlxge",    "if_re",       "if_rl",
		"if_rue",    "if_sf",       "if_sfxge",    "if_sge",
		"if_sis",    "if_sk",       "if_smsc",     "if_sn",
		"if_ste",    "if_stge",     "if_tap",      "if_ti",
		"if_tl",     "if_tx",       "if_txp",      "if_udav",
		"if_ure",    "if_urndis",   "if_vge",      "if_vr",
		"if_vte",    "if_vtnet",    "if_wb",       "if_xe",
		"if_xl"
	}
	for _, m in pairs(wlan_kmods) do
		if driver == m then
			return true, netif.NETIF_TYPE_WLAN
		end
	end
	for _, m in pairs(ether_kmods) do
		if driver == m then
			return true, netif.NETIF_TYPE_ETHER
		end
	end
	return false, nil
end

-- Takes the name of a kernel module, and removes the "if_" prefix and
-- the "_pci" or "_usb" suffix. Returns the new string.
function netif.kmod_to_dev(kmod)
	local dev = kmod
	if string.match(kmod, ".*_pci$") or string.match(kmod, ".*_usb$") then
		dev = string.sub(kmod, 1, string.len(kmod) - 4)
	end
	if string.match(dev, "^if_.*") then
		dev = string.sub(dev, 4)
	end
	return dev
end

-- Takes a device name without unit number (e.g. ath, rtwn), and a list of
-- network devices. Returns the name of the interface if found, nil otherwise.
function netif.find_netif(dev, netifs)
	local d
	for _, d in pairs(netifs) do
		if d == dev or string.match(d, dev .. "[0-9]+") then
			return d
		end
	end
	return nil
end

-- Returns the the unit (X) of a "wlanX" device name to a given
-- parent device, or nil
function wlan_unit_from_parent(pdev)
	local l
	local proc, e = io.popen("sysctl net.wlan")
	if proc == nil then
		io.stderr:write(e)
		return nil
	end
	for l in proc:lines() do
		if string.match(l, "%%parent") and string.match(l, pdev) then
			return tonumber(string.match(l, "net.wlan.([0-9]+)."))
		end
	end
	return nil
end

-- We define a wlan device object which has the following fields:
--	parent ::= parent device name (e.g., "ath0", "rtwn0")
--	child  ::= child device unit number ("wlan0" -> unit = 0). If there is no
--  	child device yet, this field is nil.
--
-- This function returns a list of available wlan device objects, or an empty
-- list if there are none.
function netif.get_wlan_devs()
	local i, l, parent
	local pdevs = {}
	local wlans = {}
	local proc, e = io.popen("sysctl -n net.wlan.devices")
	if proc == nil then
		io.stderr:write(e)
		return nil
	end
	i = 1
	for l in proc:lines() do
		for w in string.gmatch(l, "%w+") do
			pdevs[i] = w
			i = i + 1
		end
	end
	proc:close()
	i = 1
	for _, parent in pairs(pdevs) do
		child = wlan_unit_from_parent(parent)
		wlans[i] = {}
		wlans[i]["parent"] = parent
		wlans[i]["child"] = child
		i = i + 1
	end
	return wlans
end

-- Returns the wlan device object from the given list matching the given
-- parent device pattern, or nil if there was no match.
function netif.find_wlan(parent, wlans)
	local w
	for _, w in pairs(wlans) do
		if string.match(w.parent, parent .. "[0-9]+") then
			return w
		end
	end
	return nil
end

function netif.get_ifconfig_if_info(ifname)
	local info = {}
	local proc, e = io.popen("ifconfig " .. ifname)
	if proc == nil then
		io.stderr:write(e)
		return nil
	end
	local l
	for l in proc:lines() do
		table.insert(info, l)
	end
	proc:close()
	return info
end

-- Returns the network interface's media type or nil
function netif.media_type(ifname)
	local l
	local info = netif.get_ifconfig_if_info(ifname)
	if info == nil then
		return nil
	end
	for _, l in pairs(info) do
		local type = string.match(l, "^%s+media:%s([%g,%s]+)$")
		if type then
			if string.match(type, "%s[wW]ireless%s") or
			   string.match(type, "%s802.11%s") then
				return netif.NETIF_TYPE_WLAN
			elseif string.match(type, "^[Ee]thernet") then
				return netif.NETIF_TYPE_ETHER
			else
				return nil
			end
		end
	end
	return nil
end

function netif.get_ifconfig_iflist()
	local l
	local iflist = {}
	local proc, e = io.popen("ifconfig -l")
	if proc == nil then
		io.stderr:write(e)
		return nil
	end
	for l in proc:lines() do
		for w in string.gmatch(l, "%w+") do
			table.insert(iflist, w)
		end
	end
	proc:close()
	return iflist
end

-- Returns the list of network interfaces from the output of "ifconfig" as
-- array.
function netif.get_netifs()
	local iflist = {}
	local all_ifs = netif.get_ifconfig_iflist()
	if all_ifs == nil then
		return nil
	end
	for _, i in pairs(all_ifs) do
		type = netif.media_type(i)
		if type ~= nil then
			table.insert(iflist, i)
		end
	end
	return iflist
end

-- Returns the given network interface's status
function netif.link_status(ifname)
	local i, status
	local info = netif.get_ifconfig_if_info(ifname)
	if info == nil then
		return nil
	end
	for _, i in pairs(info) do
		if string.match(i, "^[ \t]*status: (%w+)") then
			status = string.gsub(i, "^[ \t]*status: ([%w, ]+)$", "%1")
			if status ~= nil then
				return status
			end
		end
	end
	return nil
end

-- Returns "true" if the given network interface was configured
-- via /etc/rc.conf
function netif.in_rc_conf(ifname)
	local l
	local f, e = io.open(netif.path_rc_conf)
	if f == nil then
		io.stderr:write(e)
		return nil
	end
	for l in f:lines() do
		if string.match(l, "^[ \t]*ifconfig_" .. ifname) then
			f:close()
			return true
		end
	end
	f:close()
	return false
end

-- Returns the inet (v4) address of the given interface from the dhclient
-- lease file
function netif.inet_from_lease(ifname)
	local l, inet, _inet
	local f, e, errno = io.open("/var/db/dhclient.leases." .. ifname)
	if f == nil then
		if errno == 2 then -- ENOENT
			return nil
		end
		io.stderr:write(e)
		return nil
	end
	inet = nil
	-- Get IP address of the last lease record
	for l in f:lines() do
		_inet = string.match(l, "^%s*fixed.address%s*(.+);$")
		if _inet ~= nil then
			inet = _inet
		end
	end
	f:close()
	return inet
end

-- Get the inet v4 and v6 addresses of the given interface
function netif.get_inet_addr(ifname)
	local i, inet4, inet6
	local info = netif.get_ifconfig_if_info(ifname)
	if info == nil then
		return nil, nil
	end
	for _, i in pairs(info) do
		if inet4 == nil then
			inet4 = string.match(i, "^%s+inet%s+(%g+)")
		end
		if inet6 == nil then
			inet6 = string.match(i, "^%s+inet6%s+([%x:]+)")
		end
		if inet4 ~= nil and inet6 ~= nil then
			break
		end
	end
	return inet4, inet6
end

-- Returns a list of wlan device objects configured via /etc/rc.conf
function netif.wlans_from_rc_conf()
	local i, l
	local wlans = {}
	local f, e = io.open(netif.path_rc_conf)
	if f == nil then
		io.stderr:write(e)
		return nil
	end
	i = 1
	for l in f:lines() do
		local p, c = string.match(l, "^[ \t]*wlans_(%w+)=\"?(%w+)\"?")
		if p ~= nil and c ~= nil then
			wlans[i] = {}
			wlans[i].parent = p
			wlans[i].child = tonumber(string.match(c, "wlan(%d+)"))
			i = i + 1
		end
	end
	f:close()
	return wlans
end

-- Returns "true" if the given wlan device object was configured via
-- /etc/rc.conf, else "false".
function netif.wlan_rc_configured(wlan)
	local w
	local wlans = netif.wlans_from_rc_conf()
	for _, w in pairs(wlans) do
		if w.parent == wlan.parent and w.child == wlan.child then
			return true
		end
	end
	return false
end

-- Returns the country code of the region defined in /var/db/zoneinfo,
-- or nil if not found
function netif.get_wlan_region()
	local l, zone, code
	local f, e = io.open(netif.path_zoneinfo)
	if f == nil then
		io.stderr:write(e)
		return nil
	end
	for l in f:lines() do
		if string.match(l, "/") then
			zone = l
			break
		end
	end
	f:close()
	if not zone then
		return nil
	end
	f, e = io.open(netif.path_zone_tab)
	if f == nil then
		io.stderr:write(e)
		return nil
	end
	for l in f:lines() do
		code = string.match(l, "^(%u+)%s+[0-9+-]+%s+" .. zone)
		if code then
			break
		end
	end
	f:close()
	return code
end

-- Sleeps n seconds
function netif.sleep(n)
	os.execute("sleep " .. tonumber(n))
end

-- Takes a driver name (e.g. if_ath) and waits for not more than "timeout"
-- seconds for the parent device matching the driver to appear in
-- net.wlan.devices. If found, the corresponding wlan device object is
-- returned, else nil.
function netif.wait_for_new_wlan(driver, timeout)
	-- Get the corresponding device name without unit number from the
	-- given driver (e.g., if_rtwn_usb -> rtwn)
	local devname = netif.kmod_to_dev(driver)

	local tries = 1
	while true do
		-- Periodically check for the parent device to appear
		-- in net.wlan.devices. After loading the driver it
		-- can take a moment for the device to appear.
		local wlans = netif.get_wlan_devs()
		local w = netif.find_wlan(devname, wlans)
		if w == nil then
			if tries >= timeout then
				-- Give up
				return nil
			end
			netif.sleep(1)
		else
			return w
		end
		tries = tries + 1
	end
end

local function add_wlan_regdomain_args()
	local country = netif.get_wlan_region()
	if country == nil then
		return
	end
	local args = "down country " .. country
	if wlan_create_args ~= nil then
		wlan_create_args = args .. " " .. wlan_create_args
	else
		wlan_create_args = args
	end
end

function netif.restart_netif(ifname)
	return os.execute("service netif restart " .. ifname)
end

function netif.run_sysrc(var)
	return os.execute("sysrc " .. var)
end

function netif.set_rc_conf_var(var, val)
	local rc_var = string.format('%s="%s"', var, val)
	return netif.run_sysrc(rc_var)
end

function netif.create_wlan_child_dev(parent, child_unit)
	local cmd = string.format("ifconfig wlan%d create wlandev %s",
	    child_unit, parent)
	return os.execute(cmd)
end

function netif.add_wlan_to_rc_conf(wlan)
	netif.set_rc_conf_var("wlans_" .. wlan.parent, "wlan" .. wlan.child)
	if wlan_create_args then
		netif.set_rc_conf_var("create_args_wlan" .. wlan.child,
		  wlan_create_args)
	end
	netif.set_rc_conf_var("ifconfig_wlan" .. wlan.child, wlan_ifconfig_args)
end

-- Creates and configures a new wlan child device (wlanX) for each wlan
-- device object which doesn't have a child device yet.
function netif.create_wlan_devs()
	local w, max_unit
	local wlans = netif.get_wlan_devs()

	if wlan_set_country then
		add_wlan_regdomain_args()
	end
	if wlan_ifconfig_args == nil then
		wlan_ifconfig_args = "up scan WPA DHCP"
	end

	-- Calculate the next available unit number for the child device
	max_unit = -1
	for _, w in pairs(wlans) do
		if w.child ~= nil then
			if w.child > max_unit then
				max_unit = w.child
			end
		end
	end
	-- Create a child device for each parent device which doesn't have
	-- a child ("wlanX"), and wasn't configured via /etc/rc.conf with
	-- 'wlans_parent="wlan<max_unit>"'
	for _, w in pairs(wlans) do
		if ignore_netifs == nil or
		   netif.find_netif(w.parent, ignore_netifs) == nil then
			if not netif.wlan_rc_configured(w) or w.child == nil then
				-- If w.child is nil, we create a new wlanX device
				-- (X = max_unit + 1). Else, we are here because a wlan
				-- device was created via /etc/rc.conf, but the wlanX device
				-- doesn't match w.child, so we have to correct that.
				if w.child == nil then
					max_unit = max_unit + 1
					w.child = max_unit
					netif.create_wlan_child_dev(w.parent, max_unit)
				end
				netif.add_wlan_to_rc_conf(w)
				local child = "wlan" .. w.child
				local status = netif.link_status(child)
				-- Only restart the interface if is isn't already associated
				if status == nil or status ~= "associated" then
					netif.restart_netif(child)
				end
			end
		end
	end
end

-- Takes a driver name (e.g. if_alc) and waits for not more than "timeout"
-- seconds for the device matching the driver to appear in the list of
-- network interfaces. If found, the interface name is returned, else nil.
function netif.wait_for_new_ether(driver, timeout)
	-- Get the corresponding device name without unit number from the
	-- given driver (e.g., if_rtwn_usb -> rtwn)
	local devname = netif.kmod_to_dev(driver)

	local tries = 1
	while true do
		-- Periodically check for the device to appear in the network
		-- interface list. After loading the driver it can take a moment
		-- for the device to appear.
		local iflist = netif.get_netifs()
		local ifname = netif.find_netif(devname, iflist)
		if ifname == nil then
			if tries >= timeout then
				-- Give up
				return nil
			end
			netif.sleep(1)
		else
			return ifname
		end
		tries = tries + 1
	end
end

-- Starts DHCP on each ethernet device.
function netif.setup_ether_devs()
	local i
	local iflist = netif.get_netifs()
	if ether_ifconfig_args == nil then
		ether_ifconfig_args = "DHCP"
	end
	for _, i in pairs(iflist) do
		if ignore_netifs == nil or
		  netif.find_netif(w.parent, ignore_netifs) == nil then
			if not string.match(i, "wlan") then
				if not netif.in_rc_conf(i) then
					netif.set_rc_conf_var('ifconfig_' .. i, ether_ifconfig_args)
				end
				local inet4, inet6 = netif.get_inet_addr(i)
				if inet6 == nil and inet4 == nil then
					netif.restart_netif(i)
				end
			end
		end
	end
end

-- Configures and starts all network interfaces based on the given driver/kmod
-- name.
function netif.config_netif(kmod)
	if netif_wait_max == nil then
		netif_wait_max = 1
	end
	local is_netif, iftype = netif.match_netif_type(kmod)
	if is_netif and iftype == netif.NETIF_TYPE_WLAN then
		if netif.wait_for_new_wlan(kmod, netif_wait_max) ~= nil then
			netif.create_wlan_devs()
		end
	elseif is_netif and iftype == netif.NETIF_TYPE_ETHER then
		if netif.wait_for_new_ether(kmod, netif_wait_max) ~= nil then
			netif.setup_ether_devs()
		end
	end
end

return netif
