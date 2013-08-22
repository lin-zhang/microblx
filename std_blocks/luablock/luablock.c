/*
 * A generic luajit based block.
 */

#define DEBUG

#include <luajit-2.0/lauxlib.h>
#include <luajit-2.0/lualib.h>
#include <luajit-2.0/lua.h>

#include <stdio.h>
#include <stdlib.h>

#include "ubx.h"

#define LUA_BLOCK_FILE 		"/home/mk/prog/c/microblx/std_blocks/luablock/luablock.lua"
#define EXEC_STR_BUFF_SIZE	16*1024*1024

ubx_port_t lua_ports[] = {
	{ .name="exec_str", .attrs=PORT_DIR_INOUT, .in_type_name="char", .out_type_name="int", .in_data_len=16777216 },
	{ NULL }
};

def_write_fun(write_int, int)

ubx_config_t lua_conf[] = {
	{ .name="lua_file", .type_name="char", .value = { .len=32 } },
	{ NULL }
};

char luablock_meta[] =
	"{ doc='A generic luajit based block',"
	"  license='MIT',"
	"  real-time=false,"
	"}";

struct luablock_info {
	struct ubx_node_info* ni;
	struct lua_State* L;
	ubx_data_t* exec_str_buff;
};



/**
 * @brief: call a hook with fname.
 *
 * @param block (is passed on a first arg)
 * @param fname name of function to call
 * @param require_fun raise an error if function fname does not exist.
 * @param require_res if 1, require a boolean valued result.
 * @return -1 in case of error, 0 otherwise.
 */
int call_hook(ubx_block_t* b, const char *fname, int require_fun, int require_res)
{

	int ret = 0;
	struct luablock_info* inf = (struct luablock_info*) b->private_data;
	int num_res = (require_res != 0) ? 1 : 0;

	lua_getglobal(inf->L, fname);

	if(lua_isnil(inf->L, -1)) {
		lua_pop(inf->L, 1);
		if(require_fun)
			ERR("%s: no (required) Lua function %s", b->name, fname);
		else
			goto out;
	}

	lua_pushlightuserdata(inf->L, (void*) b);

	if (lua_pcall(inf->L, 1, num_res, 0) != 0) {
		ERR("%s: error calling function %s: %s", b->name, fname, lua_tostring(inf->L, -1));
		ret = -1;
		goto out;
	}

	if(require_res) {
		if (!lua_isboolean(inf->L, -1)) {
			ERR("%s: %s must return a bool but returned a %s",
			    b->name, fname, lua_typename(inf->L, lua_type(inf->L, -1)));
			ret = -1;
			goto out;
		}
		ret = !(lua_toboolean(inf->L, -1)); /* back in C! */
		lua_pop(inf->L, 1); /* pop result */
	}
 out:
	return ret;
}

static int init_lua_state(struct luablock_info* inf, const char* lua_file)
{
	int ret=-1;

	if((inf->L=luaL_newstate())==NULL) {
		ERR("failed to alloc lua_State");
		goto out;
	}

	luaL_openlibs(inf->L);

	if(lua_file) {
		if((ret=luaL_dofile(inf->L, lua_file))!=0) {
			ERR("Failed to load ubx_lua.lua: %s\n", lua_tostring(inf->L, -1));
			goto out;
		}
	}
	ret=0;
 out:
	return ret;
}


static int luablock_init(ubx_block_t *b)
{
	char *lua_file;
	unsigned int lua_file_len;

	int ret = -EOUTOFMEM;
	struct luablock_info* inf;

	if((inf = calloc(1, sizeof(struct luablock_info)))==NULL)
		goto out;

	b->private_data = inf;

	lua_file = (char *) ubx_config_get_data_ptr(b, "lua_file", &lua_file_len);

	if((inf->exec_str_buff = ubx_data_alloc(b->ni, "char", EXEC_STR_BUFF_SIZE)) == NULL) {
		ERR("failed to allocate exec_str buffer");
		goto out_free1;
	}

	if(init_lua_state(inf, lua_file) != 0)
		goto out_free2;

	if((ret=call_hook(b, "init", 0, 1)) != 0)
		goto out_free2;

	/* Ok! */
	ret = 0;
	goto out;

 out_free2:
	free(inf->exec_str_buff);
 out_free1:
	free(inf);
 out:
	return ret;
}

static int luablock_start(ubx_block_t *b)
{
	return call_hook(b, "start", 0, 1);
}

/**
 * luablock_step - execute lua string and call step hook
 *
 * @param b
 */
static void luablock_step(ubx_block_t *b)
{
	int len=0, ret;
	struct luablock_info* inf = (struct luablock_info*) b->private_data;

	/* any lua code to execute */
	ubx_port_t* p_exec_str = ubx_port_get(b, "exec_str");

	if((len=__port_read(p_exec_str, inf->exec_str_buff))>0) {
		if ((ret=luaL_dostring(inf->L, inf->exec_str_buff->data)) != 0) {
			ERR("Failed to exec_str: %s", lua_tostring(inf->L, -1));
			write_int(p_exec_str, &ret);
			goto out;
		}
		write_int(p_exec_str, &ret);
	}
	call_hook(b, "step", 0, 0);
 out:
	return;
}

static void luablock_stop(ubx_block_t *b)
{
	call_hook(b, "stop", 0, 0);
}

static void luablock_cleanup(ubx_block_t *b)
{
	DBG(" ");
	struct luablock_info* inf = (struct luablock_info*) b->private_data;
	call_hook(b, "cleanup", 0, 0);
	lua_close(inf->L);
	ubx_data_free(b->ni, inf->exec_str_buff);
	free(b->private_data);
}


/* put everything together */
ubx_block_t lua_comp = {
	.name = "lua/luablock",
	.type = BLOCK_TYPE_COMPUTATION,
	.meta_data = luablock_meta,
	.configs = lua_conf,
	.ports = lua_ports,

	/* ops */
	.init = luablock_init,
	.start = luablock_start,
	.step = luablock_step,
	.stop = luablock_stop,
	.cleanup = luablock_cleanup,
};

static int lua_init(ubx_node_info_t* ni)
{
	DBG(" ");
	return ubx_block_register(ni, &lua_comp);
}

static void lua_cleanup(ubx_node_info_t *ni)
{
	DBG(" ");
	ubx_block_unregister(ni, "lua/luablock");
}

module_init(lua_init)
module_cleanup(lua_cleanup)
