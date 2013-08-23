/*
 * A pthread based trigger block
 */

#define DEBUG 1

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "ubx.h"

#include "types/ptrig_config.h"
#include "types/ptrig_config.h.hexarr"

/* ptrig metadata */
char ptrig_meta[] =
	"{ doc='pthread based trigger',"
	"  license='MIT',"
	"  real-time=false,"
	"}";

/* types defined by ptrig block */
ubx_type_t ptrig_types[] = {
	def_struct_type("std_triggers", struct ptrig_config, &ptrig_config_h),
	{ NULL },
};

/* configuration */
ubx_config_t ptrig_config[] = {
	{ .name="stacksize", .type_name = "size_t" },
	{ .name="sched_priority", .type_name = "int" },
	{ .name="sched_policy", .type_name = "char", .value = { .len=12 } },
	{ .name="trig_blocks", .type_name = "std_triggers/struct ptrig_config" },
	{ NULL },
};

/* instance state */
struct ptrig_inf {
	pthread_t tid;
	pthread_attr_t attr;
	uint32_t state;
	pthread_mutex_t mutex;
	pthread_cond_t active_cond;
	struct ptrig_config *trig_list;
	unsigned int trig_list_len;
};

/* trigger the configured blocks */
int trigger_steps(struct ptrig_inf *inf)
{
	int i, steps, res=-1;

	for(i=0; i<inf->trig_list_len; i++) {
		for(steps=0; steps<inf->trig_list[i].num_steps; steps++) {
			if(ubx_cblock_step(inf->trig_list[i].b)!=0)
				goto out;
		}
	}

	res=0;
 out:
	return res;
}

/* thread entry */
static void* thread_startup(void *arg)
{
	ubx_block_t *b;
	struct ptrig_inf *inf;

	b = (ubx_block_t*) arg;
	inf = (struct ptrig_inf*) b->private_data;

	while(1) {
		pthread_mutex_lock(&inf->mutex);

		while(inf->state != BLOCK_STATE_ACTIVE) {
			pthread_cond_wait(&inf->active_cond, &inf->mutex);
		}
		pthread_mutex_unlock(&inf->mutex);

		trigger_steps(inf);
	}

	/* block on cond var that signals block is running */
	pthread_exit(NULL);
}

/* init */
static int ptrig_init(ubx_block_t *b)
{
	int ret = EOUTOFMEM;
	struct ptrig_inf* inf;

	if((b->private_data=calloc(1, sizeof(struct ptrig_inf)))==NULL) {
		ERR("failed to alloc");
		goto out;
	}

	inf=(struct ptrig_inf*) b->private_data;

	inf->state=BLOCK_STATE_INACTIVE;

	pthread_cond_init(&inf->active_cond, NULL);
	pthread_mutex_init(&inf->mutex, NULL);
	pthread_attr_init(&inf->attr);
	pthread_attr_setdetachstate(&inf->attr, PTHREAD_CREATE_JOINABLE);
	
	if((ret=pthread_create(&inf->tid, &inf->attr, thread_startup, b))!=0) {
		ERR2(ret, "pthread_create failed");
		goto out_err;
	}

	/* OK */
	ret=0;
	goto out;

 out_err:
	free(b->private_data);
 out:
	return ret;
}

static int ptrig_start(ubx_block_t *b)
{
	DBG(" ");

	struct ptrig_inf *inf;
	ubx_data_t* trig_list_data;

	inf = (struct ptrig_inf*) b->private_data;

	trig_list_data = ubx_config_get_data(b, "trig_blocks");

	/* make a copy? */
	inf->trig_list = trig_list_data->data;
	inf->trig_list_len = trig_list_data->len;

	pthread_mutex_lock(&inf->mutex);
	inf->state=BLOCK_STATE_ACTIVE;
	pthread_cond_signal(&inf->active_cond);
	pthread_mutex_unlock(&inf->mutex);
	return 0;
}

static void ptrig_stop(ubx_block_t *b)
{
	DBG(" ");
	struct ptrig_inf *inf;
	inf = (struct ptrig_inf*) b->private_data;
	
	pthread_mutex_lock(&inf->mutex);
	inf->state=BLOCK_STATE_INACTIVE;
	pthread_mutex_unlock(&inf->mutex);
}

static void ptrig_cleanup(ubx_block_t *b)
{
	int ret;
	struct ptrig_inf* inf;
	inf=(struct ptrig_inf*) b->private_data;

	inf->state=BLOCK_STATE_PREINIT;

	if((ret=pthread_cancel(inf->tid))!=0)
		ERR2(ret, "pthread_cancel failed");
	
	/* join */
	if((ret=pthread_join(inf->tid, NULL))!=0)
		ERR2(ret, "pthread_join failed");

	free(b->private_data);
}

/* put everything together */
ubx_block_t ptrig_comp = {
	.name = "std_triggers/ptrig",
	.type = BLOCK_TYPE_COMPUTATION,
	.meta_data = ptrig_meta,
	.configs = ptrig_config,

	.init = ptrig_init,
	.start = ptrig_start,
	.stop = ptrig_stop,
	.cleanup = ptrig_cleanup
};

static int ptrig_mod_init(ubx_node_info_t* ni)
{
	int ret;
	ubx_type_t *tptr;

	for(tptr=ptrig_types; tptr->name!=NULL; tptr++) {
		if((ret=ubx_type_register(ni, tptr))!=0) {
			ERR("failed to register type %s", tptr->name);
			goto out;
		}
	}
	ret=ubx_block_register(ni, &ptrig_comp);
 out:
	return ret;
}

static void ptrig_mod_cleanup(ubx_node_info_t *ni)
{
	ubx_type_t *tptr;

	for(tptr=ptrig_types; tptr->name!=NULL; tptr++)
		ubx_type_unregister(ni, tptr->name);

	ubx_block_unregister(ni, "std_triggers/ptrig");
}

module_init(ptrig_mod_init)
module_cleanup(ptrig_mod_cleanup)
