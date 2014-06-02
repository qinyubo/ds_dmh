#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "arpa/inet.h"
#include <getopt.h>
#include <stdio.h>
#include "unistd.h"
#include <math.h>

#include "debug.h"
#include "timer.h"
#include "list.h"
#include "dart.h"
#include "ds_gspace.h"
#include "dimes_server.h"

#include "hstaging_scheduler.h"
#include "hstaging_api.h"
#include "hstaging_def.h"
#include "mpi.h"

static int num_sp;
static int num_cp;
static char *conf;

static struct dimes_server *dimes_s = NULL;
static struct dart_server *ds = NULL;
static struct ds_gspace *dsg = NULL;

static struct hstaging_workflow *wf = NULL;
static struct timer tm; 
static double tm_st;
static double dag_tm_st, dag_tm_end;

struct job_id {
	int tid;
	int step;
};

struct job {
	struct list_head entry;
	struct task_instance *ti;
	int num_input_vars;
	struct job_id jid;
    // enum hstaging_location_type location_type;
    int bk_allocation_size; // number of buckets required for job execution
    int *bk_idx_tab; // array of idx to the buckets in bk_tab 
};

enum bucket_status {
    bk_none = 0, // not ready for task execution 
    bk_idle,
    bk_busy,
};

struct mpi_rank_range {
    int l, r;
};

struct bucket {
    int dart_id; // unique id
    int pool_id; // indicate which resource pool the bucket is from
    int origin_mpi_rank;
    enum bucket_status status;
    struct job_id current_jid; // TODO: do we still need this? 
    struct executor_topology_info topo_info;
};

#define MAX_NUM_BK_PER_NODE 32
struct compute_node {
    struct node_topology_info topo_info; 
    int num_bucket;
    int bk_idx_tab[MAX_NUM_BK_PER_NODE];
};

struct bucket_pool {
    struct list_head entry;
    int pool_id;
    int num_bucket;
    int bk_tab_size;
    struct bucket *bk_tab;
    int num_node;
    struct compute_node *node_tab;
    int f_bk_reg_done;
    int f_node_tab_done;
};

struct workflow_state {
	unsigned int f_done;
	unsigned int f_send_exit_msg;
};

struct pending_msg {
    struct list_head entry;
    struct rpc_cmd cmd;
};

static struct list_head bk_pool_list;
static struct list_head jobq;
static struct workflow_state state;
static struct list_head pending_msg_list;

static inline int equal_jid(struct job_id *l, struct job_id *r)
{
	return (l->tid == r->tid) && (l->step == r->step);
}

/*
static inline int is_master()
{
	return ( 0 == ds->rpc_s->ptlmap.id );
}
*/

static inline void init_bk_pool_list()
{
    INIT_LIST_HEAD(&bk_pool_list);
}

static struct bucket_pool* bk_pool_lookup(int pool_id)
{
    struct bucket_pool *bp;
    list_for_each_entry(bp, &bk_pool_list, struct bucket_pool, entry) {
        if (bp->pool_id == pool_id)
            return bp;
    }

    return NULL;    
}

static struct bucket_pool* bk_pool_create_new(int pool_id, int num_bucket)
{
    struct bucket_pool *bp;
    bp = malloc(sizeof(*bp));
    bp->pool_id = pool_id;
    bp->num_bucket = 0;
    bp->bk_tab_size = num_bucket;
    bp->bk_tab = malloc(sizeof(struct bucket) * num_bucket);
    bp->node_tab = NULL;
    bp->f_bk_reg_done = 0;
    bp->f_node_tab_done = 0;

    // Add to the list
    list_add_tail(&bp->entry, &bk_pool_list);

    return bp; 
}

static int bk_pool_add_bucket(struct bucket_pool *bp, int origin_mpi_rank, int dart_id,
                    int pool_id, struct executor_topology_info *topo_info)
{
    if (origin_mpi_rank < 0 || origin_mpi_rank >= bp->bk_tab_size) {
        uloga("ERROR %s(): origin_mpi_rank out of range\n", __func__);
        return -1;
    }

    struct bucket *bk = &(bp->bk_tab[origin_mpi_rank]);
    bk->dart_id = dart_id;
    bk->pool_id = pool_id;
    bk->origin_mpi_rank = origin_mpi_rank;
    bk->status = bk_none;
    if (topo_info) bk->topo_info = *topo_info; 

    bp->num_bucket++;
    return 0;
}

static void bk_pool_free_bk_allocation(struct bucket_pool *bp, int *bk_idx_tab,
                                int allocation_size)
{
    if (allocation_size > bp->bk_tab_size) {
        uloga("ERROR %s(): allocation_size is larger than bk_tab_size\n", __func__);
        return;
    }

    int i;
    for (i = 0; i < allocation_size; i++) {
        bp->bk_tab[bk_idx_tab[i]].status = bk_idle;
    }
}


static int* bk_pool_request_bk_allocation(struct bucket_pool *bp, int allocation_size)
{
    if (allocation_size > bp->bk_tab_size) {
        uloga("ERROR %s(): allocation_size is larger than bk_tab_size\n", __func__);
        return NULL;
    }

    int *bk_idx_tab = (int*)malloc(allocation_size*sizeof(int));
    if (!bk_idx_tab) return NULL;

    int i, j;
    for (i = 0, j = 0; i < bp->bk_tab_size; i++) {
        if (bp->bk_tab[i].status == bk_idle) {
            bk_idx_tab[j++] = i;
        }
        if (j == allocation_size) break;
    }

    if (j < allocation_size) {
        // no sufficient idle buckets
        free(bk_idx_tab);
        return NULL;  
    }

    for (i = 0; i < allocation_size; i++) {
        // update status of the bucket
        bp->bk_tab[bk_idx_tab[i]].status = bk_busy;
    }
    return bk_idx_tab;
}

static struct bucket* bk_pool_get_bucket(struct bucket_pool *bp, int origin_mpi_rank)
{
    if (origin_mpi_rank < 0 || origin_mpi_rank >= bp->bk_tab_size) {
        uloga("ERROR %s(): origin_mpi_rank out of range\n", __func__);
        return NULL;
    }

    return &(bp->bk_tab[origin_mpi_rank]);
}

static void bk_pool_set_bucket_idle(struct bucket_pool *bp)
{
    int i;
    for (i = 0; i < bp->bk_tab_size; i++) {
        bp->bk_tab[i].status = bk_idle;
    }

    bp->f_bk_reg_done = 1;
    uloga("%s(): bucket resource pool %d is ready, num_bucket %d\n",
        __func__, bp->pool_id, bp->bk_tab_size);
}

int compare_bk_ptr(const void *p1, const void *p2)
{
    return (*(const struct bucket**)p1)->topo_info.nid -
           (*(const struct bucket**)p2)->topo_info.nid;
}

static void bk_pool_build_node_tab(struct bucket_pool *bp)
{
    struct bucket ** bk_ptr_tab = (struct bucket **)malloc(sizeof(struct bucket*)
                                    *bp->bk_tab_size);
    int i, j, k;
    // copy pointers
    for (i = 0; i < bp->bk_tab_size; i++) {
        bk_ptr_tab[i] = &bp->bk_tab[i];
    }

    // sort bk_ptr_tab by bk_ptr_tab[i]->topo_info.nid
    qsort(bk_ptr_tab, bp->bk_tab_size, sizeof(struct bucket *), compare_bk_ptr);

    bp->num_node = 0;
    uint32_t cur_nid = ~0; // assume node id can not be ~0.... 
    for (i = 0; i < bp->bk_tab_size; i++) {
        if (cur_nid != bk_ptr_tab[i]->topo_info.nid) {
            cur_nid = bk_ptr_tab[i]->topo_info.nid;
            bp->num_node++;
        }
    }    
        
    bp->node_tab = malloc(sizeof(struct compute_node)*bp->num_node);
    cur_nid = ~0;
    for (i = 0, j = -1, k = 0; i < bp->bk_tab_size; i++) {
        if (cur_nid != bk_ptr_tab[i]->topo_info.nid) {
            j++; 
            k = 0;
            cur_nid = bk_ptr_tab[i]->topo_info.nid;
            bp->node_tab[j].bk_idx_tab[k++] = bk_ptr_tab[i]->origin_mpi_rank;
            bp->node_tab[j].num_bucket = 1;
        } else {
            bp->node_tab[j].bk_idx_tab[k++] = bk_ptr_tab[i]->origin_mpi_rank;
            bp->node_tab[j].num_bucket += 1;
        }
    } 

    bp->f_node_tab_done = 1;

    // print it out
    uloga("bp->num_node= %d\n", bp->num_node);
    for (i = 0; i < bp->num_node; i++) {
        uloga("compute node id %u num_bucket %d:\n", bp->node_tab[i].topo_info.nid,
                bp->node_tab[i].num_bucket);
        for (k = 0; k < bp->node_tab[i].num_bucket; k++) {
            struct bucket *p = &(bp->bk_tab[bp->node_tab[i].bk_idx_tab[k]]);
            uloga("bucket dart_id %d pool_id %d origin_mpi_rank %d check nid %u\n",
                p->dart_id, p->pool_id, p->origin_mpi_rank, p->topo_info.nid);
        }
    }

    return;
}

static void free_bk_pool_list()
{
    struct bucket_pool *bp, *t;
    list_for_each_entry_safe(bp, t, &bk_pool_list, struct bucket_pool, entry)
    {
        list_del(&bp->entry);
        if (bp->bk_tab) free(bp->bk_tab);
        if (bp->node_tab) free(bp->node_tab);
        free(bp);
    }
}

static void init_pending_msg_list()
{
    INIT_LIST_HEAD(&pending_msg_list);
}

static void free_pending_msg_list()
{
    int msg_cnt = 0;
    struct pending_msg *p, *t;
    list_for_each_entry_safe(p, t, &pending_msg_list, struct pending_msg, entry)
    {
        list_del(&p->entry);
        free(p);
        msg_cnt++;
    }
    uloga("%s(): msg_cnt= %d\n", __func__, msg_cnt);
}

/* operations on the jobq */
static inline void jobq_init()
{
	INIT_LIST_HEAD(&jobq);
}

static int idle_bk_count()
{
	int cnt = 0;

	return cnt;
}

static int busy_bk_count()
{
	int cnt = 0;

	return cnt;
}

static int jobq_count()
{
	int cnt = 0;
	struct job *j;
	list_for_each_entry(j, &jobq, struct job, entry) {
		cnt++;
	}

	return cnt;
}

static struct job * jobq_lookup(struct job_id *jid)
{
	struct job *j;
	list_for_each_entry(j,&jobq,struct job,entry) {
		if ( equal_jid(jid, &j->jid) )
			return j;
	}

	return NULL;
}

static struct job * jobq_create_job(struct task_instance *ti)
{
	struct job *j = malloc(sizeof(*j));
	j->jid.tid = ti->tid;
	j->jid.step = ti->step;
	j->ti = ti;
    j->num_input_vars = 0;
    j->bk_allocation_size = j->ti->size_hint;
    j->bk_idx_tab = NULL;
	struct var_instance *vi;
	list_for_each_entry(vi, &ti->input_vars_list, struct var_instance, entry) {
		j->num_input_vars++;
	}

    update_task_instance_status(ti, task_pending);
	list_add_tail(&j->entry, &jobq);	
	return j;
}

static inline int job_is_pending(struct job *j) {
	return j->ti->status == task_pending;
}

static inline int job_is_running(struct job *j) {
	return j->ti->status == task_running;
}

static inline int job_is_finish(struct job *j) {
	return j->ti->status == task_finish;
}

static void job_done(struct job *j)
{
	if (j->ti->status == task_running) {
        update_task_instance_status(j->ti, task_finish);
	}
}

static void free_job(struct job *j) {
    list_del(&j->entry);
    if (j->bk_idx_tab) free(j->bk_idx_tab);
    free(j);
}

static int job_notify_bk_completion(struct rpc_server *rpc_s, struct msg_buf *msg)
{
	if (msg->size > 0) {
		free(msg->msg_data);
	}
	free(msg);	

	return 0;
}

static int job_notify_bk(struct job *j, struct bucket_pool *bp, struct bucket *bk,
    int rank_hint, int nproc_hint)
{
	int err = -ENOMEM;
	struct msg_buf *msg;
	struct node_id *peer;
	peer = ds_get_peer(ds, bk->dart_id);
	msg = msg_buf_alloc(ds->rpc_s, peer, 1);
	if (!msg)
		goto err_out;
    
    size_t mpi_rank_tab_size = j->bk_allocation_size*sizeof(int);
    size_t input_var_tab_size = j->num_input_vars*sizeof(struct var_descriptor);
    msg->size = mpi_rank_tab_size + input_var_tab_size;
    msg->msg_data = malloc(msg->size); 
    msg->cb = job_notify_bk_completion;

    int i;
    int *mpi_rank_tab = (int*)msg->msg_data;
    for (i = 0; i < j->bk_allocation_size; i++) {
        mpi_rank_tab[i] = bp->bk_tab[j->bk_idx_tab[i]].origin_mpi_rank; 
    }
    // Copy the input var information
    struct var_descriptor *vars = (struct var_descriptor*)(msg->msg_data+mpi_rank_tab_size);
    struct var_instance *vi;
    i = 0;
    list_for_each_entry(vi, &j->ti->input_vars_list, struct var_instance,
            entry) {
        strcpy(vars[i].var_name, vi->var->name);
        vars[i].step = j->ti->step;
        vars[i].bb = vi->bb;
        vars[i].size = vi->size;
        i++;
    }	

	msg->msg_rpc->cmd = hs_exec_task_msg;
	msg->msg_rpc->id = ds->rpc_s->ptlmap.id;
	struct hdr_exec_task *hdr =
		(struct hdr_exec_task *)msg->msg_rpc->pad;
	hdr->tid = j->ti->tid;
	hdr->step = j->ti->step;
	hdr->rank_hint = rank_hint; 
	hdr->nproc_hint = nproc_hint;
	hdr->num_input_vars = j->num_input_vars;

	err = rpc_send(ds->rpc_s, peer, msg);
	if (err < 0) {
		free(msg->msg_data);
		free(msg);
		goto err_out;
	}

	return 0;
err_out:
	ERROR_TRACE();
}

static int job_allocate_bk(struct job *j)
{
	if (j->ti->status != task_pending) {
		return 0;
	}

    // Try to determine placement location
/*
    enum hstaging_location_type preferred_loc;
    switch (j->ti->placement_hint) {
    case hint_insitu:
        preferred_loc = loc_insitu;
        break;
    case hint_intransit:
        preferred_loc = loc_intransit;
        break;
    case hint_none:
        preferred_loc = loc_intransit;
        break;
    default:
        preferred_loc = loc_intransit;
        uloga("%s(): unknown placement hint information\n", __func__);
        break;
    }
*/

    // try to allocate buckets in a simple first-fit manner
    struct bucket_pool *bp;
    list_for_each_entry(bp, &bk_pool_list, struct bucket_pool, entry) {
        j->bk_idx_tab = bk_pool_request_bk_allocation(bp, j->bk_allocation_size);
        if (j->bk_idx_tab) break;
    }    
    
    if (!j->bk_idx_tab) {
        return 0;
    }

    // notify the allocated buckets
    int i;
    for (i = 0; i < j->bk_allocation_size; i++) {
        struct bucket *bk = bk_pool_get_bucket(bp, j->bk_idx_tab[i]);
        int rank_hint = i;
        int nproc_hint = j->bk_allocation_size;
		if (job_notify_bk(j, bp, bk, rank_hint, nproc_hint) < 0) {
			return -1;
		}
	}

	// Update job state
    update_task_instance_status(j->ti, task_running);

    uloga("%s(): assign job (%d,%d) to buckets timestamp %lf\n",
        __func__, j->jid.tid, j->jid.step, timer_read(&tm)-tm_st);
	return 0;	
}

static int jobq_free()
{
	struct job *j, *t;
	list_for_each_entry_safe(j,t,&jobq,struct job,entry) {
		list_del(&j->entry);
		free(j);
	}
}

/************/
static int timestamp_ = 0;
static void print_rr_count()
{
	int num_job_in_queue = 0;
	int num_idle_bucket = 0;
	int num_busy_bucket = 0;

	num_job_in_queue = jobq_count();
	num_idle_bucket = idle_bk_count();
	num_busy_bucket = busy_bk_count();

	timestamp_++;
	fprintf(stderr, "EVAL: %d num_job_in_queue= %d num_idle_bucket= %d num_busy_bucket= %d\n",
		timestamp_, num_job_in_queue, num_idle_bucket, num_busy_bucket);
}


static int process_dag_state()
{
    if (wf == NULL) return 0;
    if (!is_workflow_finished(wf)) return 0;

    // reply to the submitter of the dag
    struct msg_buf *msg;
    struct node_id *peer;
    int err = -ENOMEM;
    int dart_id = wf->submitter_dart_id;

    // free workflow
    free_workflow(wf);
    wf = NULL;
    dag_tm_end = timer_read(&tm);

    peer = ds_get_peer(ds, dart_id); 
    msg = msg_buf_alloc(ds->rpc_s, peer, 1);
    if (!msg)
        goto err_out;

    msg->msg_rpc->cmd = hs_finish_dag_msg;
    msg->msg_rpc->id = ds->rpc_s->ptlmap.id;
    struct hdr_finish_dag *hdr= (struct hdr_finish_dag *)msg->msg_rpc->pad;
    hdr->dag_execution_time = (dag_tm_end-dag_tm_st);

    err = rpc_send(ds->rpc_s, peer, msg);
    if (err < 0) {
        free(msg);
        goto err_out;
    }

    return 0;
 err_out:
    ERROR_TRACE();
}

static int process_workflow_state()
{
	int err = -ENOMEM;

	if (state.f_send_exit_msg) return 0;

	if (state.f_done && 0 == busy_bk_count()) {
        struct msg_buf *msg;
        struct node_id *peer;
        struct bucket_pool *bp;
        struct bucket *bk;
        list_for_each_entry(bp, &bk_pool_list, struct bucket_pool, entry)
        {
            int mpi_rank;
            for (mpi_rank = 0; mpi_rank < bp->bk_tab_size; mpi_rank++) {
                bk = bk_pool_get_bucket(bp, mpi_rank);
                peer = ds_get_peer(ds, bk->dart_id);
                msg = msg_buf_alloc(ds->rpc_s, peer, 1);
                if (!msg)
                    goto err_out;

                msg->msg_rpc->cmd = hs_stop_executor_msg;
                msg->msg_rpc->id = ds->rpc_s->ptlmap.id;

                err = rpc_send(ds->rpc_s, peer, msg);
                if (err < 0) {
                    free(msg);
                    goto err_out;
                }
            }
		}

		state.f_send_exit_msg = 1;
	}

	return 0;
 err_out:
	ERROR_TRACE();
}

static int process_hs_build_staging(struct pending_msg *p)
{
    int err;
    int submitter_dart_id = p->cmd.id;
    struct hdr_build_staging *hdr = (struct hdr_build_staging*)p->cmd.pad;
    int pool_id = hdr->pool_id;
  
    struct bucket_pool *bp = bk_pool_lookup(pool_id);
    if (!bp) { 
        return -1;
    }
    if (!bp->f_bk_reg_done || !bp->f_node_tab_done) {
        return -1;
    }

    uloga("%s(): build multi-level staging for bucket pool %d based on config file '%s'\n",
        __func__, pool_id, hdr->staging_conf_file);

    // TODO: build multi-level staging 

    // reply to the submitter of the dag
    struct msg_buf *msg;
    struct node_id *peer;

    peer = ds_get_peer(ds, submitter_dart_id);
    msg = msg_buf_alloc(ds->rpc_s, peer, 1);
    if (!msg)
        goto err_out;

    msg->msg_rpc->cmd = hs_build_staging_done_msg;
    msg->msg_rpc->id = ds->rpc_s->ptlmap.id;
    err = rpc_send(ds->rpc_s, peer, msg);
    if (err < 0) {
        free(msg);
        goto err_out;
    }

    return 0;
 err_out:
    err = -1;
    ERROR_TRACE();
}

static int process_pending_msg()
{
    // process one msg at a time
    struct pending_msg *p, *t;
    int err;
    list_for_each_entry_safe(p, t, &pending_msg_list, struct pending_msg, entry)
    {
        switch (p->cmd.cmd) {
        case hs_build_staging_msg:
            err = process_hs_build_staging(p);
            if (0 == err) {
                // remove msg from list
                list_del(&p->entry);
                free(p);
            }
            break;
        default:
            uloga("%s(): unknown message type\n", __func__);
            break;
        }
    }

    return 0;
}

/*
*/
static int process_jobq()
{
    if (wf == NULL) return 0;

	struct job *j, *t;
	int err = -ENOMEM;

	// 1. Add ready jobs (if any) in the queue
	struct task_instance *tasks[MAX_NUM_TASKS];
	int num_tasks;
	get_ready_tasks(wf, tasks, &num_tasks);
	if (num_tasks > 0) {
		int i;
		for (i = 0; i < num_tasks; i++) {
			jobq_create_job(tasks[i]);
		}
	}

	// 2. process ready jobs (if any) in the queue
	list_for_each_entry_safe(j, t, &jobq, struct job, entry) {
		if (job_is_pending(j)) {
			job_allocate_bk(j);
		}
	}	

	// 3. process finish jobs (if any) in the queue
	list_for_each_entry_safe(j, t, &jobq, struct job, entry) {
		if (job_is_finish(j)) {
			list_del(&j->entry);
			free(j);
		}
	}

    // 4. clear finished task instances
    clear_finished_tasks(wf);

	return 0;
}

static int callback_hs_finish_task(struct rpc_server *rpc_s, struct rpc_cmd *cmd)
{
    struct hdr_finish_task *hdr = (struct hdr_finish_task*)cmd->pad;
    struct bucket_pool *bp = bk_pool_lookup(hdr->pool_id);
    if (bp == NULL) {
        uloga("ERROR %s(): should not happen bp == NULL\n", __func__);
        return 0;
    }

    struct job_id jid;
    jid.tid = hdr->tid;
    jid.step = hdr->step;
    struct job *j = jobq_lookup(&jid);
    if (j) {
        uloga("%s(): finish job (%d,%d) timestamp %lf\n",
            __func__, hdr->tid, hdr->step, timer_read(&tm)-tm_st);

        // mark job as done
        job_done(j);
        // free bucket allocation
        bk_pool_free_bk_allocation(bp, j->bk_idx_tab, j->bk_allocation_size);
        // free job
        free_job(j); 
    } else {
        uloga("ERROR %s(): failed to find job (%d,%d) in jobq\n",
            __func__, jid.tid, jid.step);
    } 

    return 0;
}

static int callback_hs_reg_resource(struct rpc_server *rpc_s, struct rpc_cmd *cmd)
{
    struct hdr_register_resource *hdr = (struct hdr_register_resource*)cmd->pad;
    int pool_id = hdr->pool_id;
    int num_bucket = hdr->num_bucket;
    int mpi_rank = hdr->mpi_rank;
    int dart_id = cmd->id; // TODO: do we need to store dart id explicitly in hdr?

#ifdef HAVE_UGNI
    uloga("%s(): get msg from peer #%d mpi_rank %d num_bucket %d pool_id %d nid %u "
            "mesh_coord (%u,%u,%u)\n",
            __func__, dart_id, mpi_rank, num_bucket, pool_id,
            hdr->topo_info.nid, hdr->topo_info.mesh_coord.mesh_x,
            hdr->topo_info.mesh_coord.mesh_y, hdr->topo_info.mesh_coord.mesh_z);
#endif

    struct bucket_pool *bp = bk_pool_lookup(pool_id);
    if (bp == NULL) {
        bp = bk_pool_create_new(pool_id, num_bucket);
        if (bp == NULL) {
            uloga("ERROR %s(): bk_pool_create_new() failed\n", __func__);
            return -1;
        }
    }

    bk_pool_add_bucket(bp, mpi_rank, dart_id, pool_id, &hdr->topo_info);
    // Check if all peers of the resource pool have registered
    if (bp->num_bucket == bp->bk_tab_size) {
        bk_pool_set_bucket_idle(bp);
        bk_pool_build_node_tab(bp);
    }

    return 0;
}

static int callback_hs_finish_workflow(struct rpc_server *rpc_s,
    struct rpc_cmd *cmd)
{
	state.f_done = 1;
	return 0;
}

static int callback_hs_update_var(struct rpc_server *rpc_s, struct rpc_cmd *cmd)
{
	struct hdr_update_var *hdr;
	hdr = (struct hdr_update_var*)cmd->pad;

	uloga("%s(): update variable '%s' step %d "
		"dims %d bbox {(%d,%d,%d),(%d,%d,%d)}\n", 
			__func__, hdr->var_name, hdr->step, hdr->bb.num_dims,
			hdr->bb.lb.c[0], hdr->bb.lb.c[1], hdr->bb.lb.c[2],
			hdr->bb.ub.c[0], hdr->bb.ub.c[1], hdr->bb.ub.c[2]);
	if (wf != NULL) {
		struct var_descriptor var_desc;
		strcpy(var_desc.var_name, hdr->var_name);
		var_desc.step = hdr->step;
		var_desc.bb = hdr->bb;
		var_desc.size = hdr->size; 
		evaluate_dataflow_by_available_var(wf, &var_desc);
	}

	return 0;
}

static int callback_hs_exec_dag(struct rpc_server *rpc_s, struct rpc_cmd *cmd)
{
    struct hdr_exec_dag *hdr;
    hdr = (struct hdr_exec_dag*)cmd->pad;

    uloga("%s(): dag config file %s\n", __func__, hdr->dag_conf_file);

    wf = read_workflow_conf_file(hdr->dag_conf_file);
    if (wf == NULL) {
        uloga("ERROR %s(): failed to read workflow config file\n", __func__);
        return 0;
    }
        
    wf->submitter_dart_id = cmd->id;
    state.f_done = 0;
    state.f_send_exit_msg = 0;

    print_workflow(wf);
    dag_tm_st = timer_read(&tm);

    return 0;
}

static int callback_hs_build_staging(struct rpc_server *rpc_s, struct rpc_cmd *cmd)
{
    uloga("%s(): get request from peer #%d\n", __func__, cmd->id);

    // copy message to the application-level message list 
    struct pending_msg *p = malloc(sizeof(*p));
    memcpy(&p->cmd, cmd, sizeof(struct rpc_cmd));        
    list_add_tail(&p->entry, &pending_msg_list);

    return 0;
}

int hstaging_scheduler_init()
{
	rpc_add_service(hs_finish_workflow_msg, callback_hs_finish_workflow);
	rpc_add_service(hs_update_var_msg, callback_hs_update_var); 
    rpc_add_service(hs_reg_resource_msg, callback_hs_reg_resource);
    rpc_add_service(hs_finish_task_msg, callback_hs_finish_task);
    rpc_add_service(hs_exec_dag_msg, callback_hs_exec_dag);
    rpc_add_service(hs_build_staging_msg, callback_hs_build_staging);

	dimes_s = dimes_server_alloc(num_sp, num_cp, conf);
	if (!dimes_s) {
		return -1;
	}
	
	dsg = dimes_s->dsg;
	ds = dimes_s->dsg->ds;

	// Init
	jobq_init();
    init_bk_pool_list();
    init_pending_msg_list();

	state.f_done = 0;
	state.f_send_exit_msg = 0;

    timer_init(&tm, 1);
    timer_start(&tm);
    tm_st = timer_read(&tm);

	return 0;
}

int hstaging_scheduler_run()
{
	int err;

	while (!dimes_server_complete(dimes_s)) {
		err = dimes_server_process(dimes_s);
		if (err < 0) {
			/* If there is an error on the execution path,
			   I should stop the server. */
			dimes_server_free(dimes_s);

			/* TODO:  implement an  exit method  to signal
			   other servers to stop. */
			printf("Server exits due to error %d.\n", err);

			return err;
		}

        process_pending_msg();
        process_jobq();
        process_dag_state();
        process_workflow_state();
	}

    free_pending_msg_list();
    free_bk_pool_list();
	jobq_free();

	return 0;
}

int hstaging_scheduler_finish()
{
	dimes_server_barrier(dimes_s);
	dimes_server_free(dimes_s);

	return 0;
}


void hstaging_scheduler_usage()
{
	printf("Usage: server OPTIONS\n"
			"OPTIONS: \n"
			"--server, -s    Number of server instance/staging nodes\n"
			"--cnodes, -c    Number of compute nodes\n"
			"--conf, -f      Define configuration file\n");
}

int hstaging_scheduler_parse_args(int argc, char *argv[])
{
	const char opt_short[] = "s:c:f:";
	const struct option opt_long[] = {
			{"server",      1,      NULL,   's'},
			{"cnodes",      1,      NULL,   'c'},
			{"conf",        1,      NULL,   'f'},
			{NULL,          0,      NULL,   0}
	};

	int opt;

	while ((opt = getopt_long(argc, argv, opt_short, opt_long, NULL)) != -1) {
		switch (opt) {
		case 's':
			num_sp = (optarg) ? atoi(optarg) : -1;
			break;

		case 'c':
			num_cp = (optarg) ? atoi(optarg) : -1;
			break;

		case 'f':
			conf = (optarg) ? optarg : NULL;
			break;

		default:
			printf("Unknown argument \n");
		}
	}

	if (num_sp <= 0)
		num_sp = 1;
	if (num_cp == 0)
		num_cp = 0;
	if (!conf)
		conf = "dataspaces.conf";
	return 0;
}
