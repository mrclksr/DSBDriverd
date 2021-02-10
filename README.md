
# ABOUT

**dsbdriverd**
is a daemon that automatically tries to find and load the
suitable driver for your PCI and USB hardware. On startup
**dsbdriverd**
scans the PCI and US(B) bus for all connected devices and looks up their
driver in a database and linker.hints files using information provided by
the hardware. The same applies to USB devices attached to the system later
at runtime.

# INSTALLATION

	# git clone https://github.com/mrclksr/DSBDriverd.git
	# cd DSBDriverd && make install

# USAGE

**dsbdriverd**
\[**-l** | **-c** *vendor:device*]
|
\[**-fn**]
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

> Just show what would be done, but do not load any drivers, or call any
> Lua functions.

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
variable.

in
*/etc/rc.conf*

