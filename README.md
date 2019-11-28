
# ABOUT

**dsbdriverd**
is a daemon that automatically tries to find and load the
suitable driver for your PCI and USB hardware. On startup
**dsbdriverd**
scans the PCI and US(B) bus for all connected devices and looks up their
driver in a database using information provided by the hardware. The same
applies to USB devices attached to the system later at runtime.

# INSTALLATION

	# git clone https://github.com/mrclksr/DSBDriverd.git
	# cd DSBDriverd && make install

# USAGE

**dsbdriverd**
\[**-l** | **-c** *vendor:device*]
|
\[**-fnu**]
\[**-x** *driver,...*]

# OPTIONS

**-c**

> Check if there is a driver for the given
> *vendor*
> and
> *device*
> ID.

**-f**

> Run in foreground.

**-l**

> List installed devices and their corresponding driver.

**-n**

> Just show what would be done, but do not load any drivers.

**-u**

> Start
> dhclient(8)
> on Ethernet devices that appeared after loading the corresponding driver.

**-x**

> Exclude every
> *driver*
> in the comma separated list from loading.

# SETUP

In oder to start
**dsbdriverd**
at boot time, add the following line to
*/etc/rc.conf*:

	dsbdriverd_enable="YES"

In addtion you can specify flags using the
*dsbdriverd\_flags*
variable. If you want
**dsbdriverd**
to automatically start
dhclient(8)
on your Ethernet devices for which it loaded the corresponding drivers,
you can set

	dsbdriverd_flags="-u"

in
*/etc/rc.conf*

