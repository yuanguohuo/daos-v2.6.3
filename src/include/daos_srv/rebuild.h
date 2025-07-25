/**
 * (C) Copyright 2017-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * rebuild Server API
 */

#ifndef __DAOS_SRV_REBUILD_H__
#define __DAOS_SRV_REBUILD_H__

#include <daos_types.h>
#include <daos_srv/pool.h>

#define REBUILD_ENV            "DAOS_REBUILD"
#define REBUILD_ENV_DISABLED   "no"

/**
 * Enum values to indicate the rebuild operation that should be applied to the
 * associated targets
 */
typedef enum {
	RB_OP_REBUILD,
	RB_OP_RECLAIM,
	RB_OP_FAIL_RECLAIM,
	RB_OP_UPGRADE,
	RB_OP_NONE	= 0xffff,
} daos_rebuild_opc_t;

#define RB_OP_STR(rb_op) ((rb_op) == RB_OP_REBUILD ? "Rebuild" : \
			  (rb_op) == RB_OP_RECLAIM ? "Reclaim" : \
			  (rb_op) == RB_OP_FAIL_RECLAIM ? "Reclaim fail" : \
			  (rb_op) == RB_OP_UPGRADE ? "Upgrade" : \
			  (rb_op) == RB_OP_NONE ? "None" : \
			  "Unknown")

/* Common rebuild identifying information for INFO/DEBUG logging:
 * rb=<pool_uuid>/<rebuild_ver>/<rebuild_gen>/<opstring>
 */
#define DF_RB  "rb=" DF_UUID "/%u/%u/%s"

/* Full rebuild identifying information includes <leader_rank>/<term>
 * Instead of this, use DF_RB most of the time (use this for leader change scenarios, etc.)
 */
#define DF_RBF DF_RB " ld=%u/" DF_U64

/* arguments for log rebuild identifier given a struct rebuild_global_pool_tracker * */
#define DP_RB_RGT(rgt)                                                                             \
	DP_UUID((rgt)->rgt_pool_uuid), (rgt)->rgt_rebuild_ver, (rgt)->rgt_rebuild_gen,             \
	    RB_OP_STR((rgt)->rgt_opc)

/* arguments for log rebuild identifier given a struct rebuild_tgt_pool_tracker *rpt */
#define DP_RB_RPT(rpt)                                                                             \
	DP_UUID((rpt)->rt_pool_uuid), (rpt)->rt_rebuild_ver, (rpt)->rt_rebuild_gen,                \
	    RB_OP_STR((rpt)->rt_rebuild_op)
#define DP_RBF_RPT(rpt) DP_RB_RPT(rpt), (rpt)->rt_leader_rank, (rpt)->rt_leader_term

int ds_rebuild_schedule(struct ds_pool *pool, uint32_t map_ver,
			daos_epoch_t stable_eph, uint32_t layout_version,
			struct pool_target_id_list *tgts,
			daos_rebuild_opc_t rebuild_op, uint64_t delay_sec);
void ds_rebuild_restart_if_rank_wip(uuid_t pool_uuid, d_rank_t rank);
int ds_rebuild_query(uuid_t pool_uuid,
		     struct daos_rebuild_status *status);
void ds_rebuild_running_query(uuid_t pool_uuid, uint32_t opc, uint32_t *rebuild_ver,
			      daos_epoch_t *current_eph, uint32_t *rebuild_gen);
int ds_rebuild_regenerate_task(struct ds_pool *pool, daos_prop_t *prop);
void ds_rebuild_leader_stop_all(void);
void ds_rebuild_abort(uuid_t pool_uuid, unsigned int version, uint32_t rebuild_gen,
		      uint64_t term);
#endif
