/*-
 * Copyright (c) 2020 Marcel Kaiser. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "log.h"
#include "config.h"

static void setint_tbl_field(lua_State *, const char *, int);
static void setstr_tbl_field(lua_State *, const char *, const char *);
static void add_interface_tbl(lua_State *, const iface_t *);
static void dev_to_tbl(lua_State *, const devinfo_t *dev);
static char **getstrarr(lua_State *, const char *, size_t *);

static char **
getstrarr(lua_State *L, const char *var, size_t *len)
{
	int  i;
	char **arr;

	lua_getglobal(L, var);
	if (lua_isnil(L, -1))
		return (NULL);
	if (lua_type(L, -1) != LUA_TTABLE) {
		logprintx("Syntax error: '%s' is not a string list", var);
		return (NULL);
	}
	*len = lua_rawlen(L, -1);
	if ((arr = malloc(*len * sizeof(char *))) == NULL)
		return (NULL);
	for (i = 0; i < *len; i++) {
		lua_rawgeti(L, -1, i + 1);
		if ((arr[i] = strdup(lua_tostring(L, -1))) == NULL) {
			free(arr);
			return (NULL);
		}
		lua_pop(L, 1);
	}
	return (arr);
}

static void
setint_tbl_field(lua_State *L, const char *name, int val)
{
	lua_pushnumber(L, val);
	lua_setfield(L, -2, name);
}

static void
setstr_tbl_field(lua_State *L, const char *name, const char *val)
{
	if (val == NULL)
		lua_pushnil(L);
	else
		lua_pushstring(L, val);
	lua_setfield(L, -2, name);
}

static void
add_interface_tbl(lua_State *L, const iface_t *iface)
{
	lua_newtable(L);
	setint_tbl_field(L, "class", iface->class);
	setint_tbl_field(L, "subclass", iface->subclass);
	setint_tbl_field(L, "protocol", iface->protocol);
}

/*
 * Create a Lua table from the given devinfo_t * object, and push it on the
 * Lua stack.
 */
static void
dev_to_tbl(lua_State *L, const devinfo_t *dev)
{
	int i;

	lua_newtable(L);
	setint_tbl_field(L, "bus", dev->bus);
	setint_tbl_field(L, "vendor", dev->vendor);
	setint_tbl_field(L, "device", dev->device);
	setint_tbl_field(L, "subvendor", dev->subvendor);
	setint_tbl_field(L, "subdevice", dev->subdevice);
	setint_tbl_field(L, "class", dev->class);
	setint_tbl_field(L, "subclass", dev->subclass);
	setint_tbl_field(L, "revision", dev->revision);
	setint_tbl_field(L, "nifaces", dev->nifaces);
	setstr_tbl_field(L, "descr", dev->descr);
	setint_tbl_field(L, "ndrivers", dev->ndrivers);

	lua_newtable(L);
	for (i = 0; i < dev->ndrivers; i++) {
		lua_pushstring(L, dev->drivers[i]);
		lua_rawseti(L, -2, i + 1);
	}
	lua_setfield(L, -2, "drivers");
	lua_newtable(L);
	for (i = 0; i < dev->nifaces; i++) {
		add_interface_tbl(L, &dev->iface[i]);
		lua_rawseti(L, -2, i + 1);
	}
	lua_setfield(L, -2, "iface");
}

int
call_cfg_function(config_t *cfg, const char *fname, const devinfo_t *dev,
	const char *kmod)
{
	int error, nargs = 0;

	error = -1;
	if (cfg->luastate == NULL)
		return (-1);
	lua_getglobal(cfg->luastate, fname);
	if (lua_isnil(cfg->luastate, -1))
		goto out;
	if (lua_type(cfg->luastate, -1) != LUA_TFUNCTION) {
		logprintx("Syntax error: '%s' is not a function", fname);
		goto out;
	}
	if (dev != NULL) {
		/* Push the given devinfo_t * object onto the Lua stack. */
		dev_to_tbl(cfg->luastate, dev);
	}
	if (strcmp(fname, "on_load_kmod") == 0 ||
	    strcmp(fname, "affirm") == 0) {
		lua_pushstring(cfg->luastate, kmod);
		nargs = 2;
	} else if (strcmp(fname, "init") == 0)
		nargs = 0;
	else
		nargs = 1;
	if (lua_pcall(cfg->luastate, nargs, 1, 0) != 0) {
		logprintx("%s(): %s", fname, lua_tostring(cfg->luastate, -1));
		goto out;
	}
	error = lua_tointeger(cfg->luastate, -1);
out:
	lua_settop(cfg->luastate, 0);

	return (error);
}

config_t *
open_cfg(const char *path, bool init)
{
	config_t *cfg;
	
	errno = 0;
	if (access(path, R_OK) == -1) {
		if (errno == ENOENT)
			return (NULL);
		die("access(%s)", path);
	}
	if ((cfg = malloc(sizeof(config_t))) == NULL)
		die("malloc()");
	(void)memset(cfg, 0, sizeof(config_t));

	cfg->luastate = luaL_newstate();
	luaL_openlibs(cfg->luastate);
	if (luaL_dofile(cfg->luastate, PATH_CFG_FILE) != 0)
		diex("%s", lua_tostring(cfg->luastate, -1));
	if (init)
		call_cfg_function(cfg, "init", NULL, NULL);
	cfg->exclude = getstrarr(cfg->luastate, "exclude_kmods",
	    &cfg->exclude_len);

	return (cfg);
}
