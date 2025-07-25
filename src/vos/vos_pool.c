/**
 * (C) Copyright 2016-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/**
 * Implementation for pool specific functions in VOS
 *
 * vos/vos_pool.c
 *
 * Author: Vishwanath Venkatesan <vishwanath.venkatesan@intel.com>
 */
#define D_LOGFAC	DD_FAC(vos)

#include <daos/common.h>
#include <daos_srv/vos.h>
#include <daos_srv/ras.h>
#include <daos_errno.h>
#include <gurt/hash.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "vos_layout.h"
#include "vos_internal.h"
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include <daos_pool.h>

static void
vos_iod2bsgl(struct umem_store *store, struct umem_store_iod *iod, struct bio_sglist *bsgl)
{
	struct bio_iov	*biov;
	bio_addr_t	 addr = { 0 };
	uint32_t	 off_bytes;
	int		 i;

	off_bytes = store->stor_hdr_blks * store->stor_blk_size;
	for (i = 0; i < iod->io_nr; i++) {
		biov = &bsgl->bs_iovs[i];

		bio_addr_set(&addr, DAOS_MEDIA_NVME, iod->io_regions[i].sr_addr + off_bytes);
		bio_iov_set(biov, addr, iod->io_regions[i].sr_size);
	}
	bsgl->bs_nr_out = bsgl->bs_nr;
}

static int
vos_meta_rwv(struct umem_store *store, struct umem_store_iod *iod, d_sg_list_t *sgl, bool update)
{
	struct bio_sglist	bsgl;
	struct bio_iov		local_biov;
	int			rc;

	D_ASSERT(store && store->stor_priv != NULL);
	D_ASSERT(iod->io_nr > 0);
	D_ASSERT(sgl->sg_nr > 0);

	if (update) {
		rc = bio_meta_clear_empty(store->stor_priv);
		if (rc)
			return rc;
	} else if (bio_meta_is_empty(store->stor_priv)) {
		return 0;
	}

    //Yuanguo:
    //  - iod 描述spdk blob上的一块或多块区间(iod->io_nr块)；每一块通过一个 struct umem_store_region 对象描述；
    //  - 函数vos_iod2bsgl()把iod->io_nr个struct umem_store_region对象 "一对一翻译" 成struct bio_iov对象 (结果存放于bsgl.bs_iovs)；
    //
    // struct umem_store_region对象: 描述storage (spdk blob)上的region(地址与size)；
    //
    // struct bio_iov对象是IO(DMA操作)需要的参数形态：
    //        void		     *bi_buf;        --> 指向DMA region (对于flush操作，将来会把数据写到这里，再进行DMA)
    //        size_t		 bi_data_len;    --> IO的size；
    //        bio_addr_t	 bi_addr;        --> storage (spdk blob)上的地址；
	if (iod->io_nr == 1) {
		bsgl.bs_iovs = &local_biov;
		bsgl.bs_nr = 1;
	} else {
		rc = bio_sgl_init(&bsgl, iod->io_nr);
		if (rc)
			return rc;
	}
	vos_iod2bsgl(store, iod, &bsgl);

    //Yuanguo: 注意，现在struct bio_iov对象的`bi_buf`还都为NULL；
    //  对于flush(BIO_IOD_TYPE_UPDATE):
    //          bio_writev ->
    //          bio_rwv ->
    //                1. bio_iod_prep -> iod_prep_internal -> iod_map_iovs -> dma_map_one   //赋值bi_buf，指向DMA region;
    //                2. bio_iod_copy   //把数据从heap rgions (sgl) 拷贝到DMA regions;
    //                3. bio_iod_post   //调用dma_rw(), 发起DMA，并release DMA buffers;
    //  对于read (BIO_IOD_TYPE_FETCH):
    //          bio_readv ->
    //          bio_rwv ->
    //                1. bio_iod_prep -> iod_prep_internal
    //                                        a. iod_map_iovs -> dma_map_one   //赋值bi_buf，指向DMA region;
    //                                        b. 调用dma_rw(), 发起DMA；
    //                                        c. 等待完成；
    //                2. bio_iod_copy   //把数据从DMA regions拷贝到heap rgions (sgl);
    //                3. bio_iod_post   //release DMA buffers;
	if (update)
		rc = bio_writev(bio_mc2ioc(store->stor_priv, SMD_DEV_TYPE_META), &bsgl, sgl);
	else
		rc = bio_readv(bio_mc2ioc(store->stor_priv, SMD_DEV_TYPE_META), &bsgl, sgl);

	if (iod->io_nr > 1)
		bio_sgl_fini(&bsgl);

	return rc;
}

static inline int
vos_meta_readv(struct umem_store *store, struct umem_store_iod *iod, d_sg_list_t *sgl)
{
	return vos_meta_rwv(store, iod, sgl, false);
}


#define META_READ_BATCH_SIZE (1024 * 1024)

struct meta_load_control {
	ABT_mutex		mlc_lock;
	ABT_cond		mlc_cond;
	int			mlc_rc;
	int			mlc_inflights;
	int			mlc_wait_finished;
};

struct meta_load_arg {
	daos_off_t		  mla_off;
	uint64_t		  mla_read_size;
	char		         *mla_start;
	struct umem_store        *mla_store;
	struct meta_load_control *mla_control;
};

#define META_READ_QD_NR	4

static inline void
vos_meta_load_fn(void *arg)
{
	struct meta_load_arg	  *mla = arg;
	struct umem_store_iod	  iod;
	d_sg_list_t		  sgl;
	d_iov_t			  iov;
	int			  rc;
	struct meta_load_control *mlc = mla->mla_control;

	D_ASSERT(mla != NULL);
    //Yuanguo: iod 描述spdk blob上的一块或多块区间！
    //         sr_addr从blob header之后开始算，见vos_iod2bsgl函数，翻译时加上blob header的size (stor_hdr_blks * stor_blk_size, 1*4k)
	iod.io_nr             = 1;
	iod.io_regions        = &iod.io_region;
	iod.io_region.sr_addr = mla->mla_off;
	iod.io_region.sr_size = mla->mla_read_size;

    //Yuanguo: sgl 描述内存(mmap映射的)上的一块或多块区间！
	sgl.sg_iovs   = &iov;
	sgl.sg_nr     = 1;
	sgl.sg_nr_out = 0;
	d_iov_set(&iov, mla->mla_start, mla->mla_read_size);

	rc = vos_meta_rwv(mla->mla_store, &iod, &sgl, false);
	if (!mlc->mlc_rc && rc)
		mlc->mlc_rc = rc;

	D_FREE(mla);
	mlc->mlc_inflights--;
	if (mlc->mlc_wait_finished && mlc->mlc_inflights == 0)
		ABT_cond_signal(mlc->mlc_cond);
	else if (!mlc->mlc_wait_finished && mlc->mlc_inflights == META_READ_QD_NR)
		ABT_cond_signal(mlc->mlc_cond);
}

//Yuanguo:
//  1. 对于MD-on-SSD场景 (其实也只有MD-on-SSD场景才会调用本函数，只有src/common/dav/dav_iface.c中调用)
//           /mnt/daos0/c090c2fc-8d83-45de-babe-104bad165593/vos-0
//                                (mmaped in memory)
//  start = hdl->do_base  +---------------------------------+ 0
//                        |                                 |
//                        |         struct dav_phdr         |
//                        |              (4k)               |
//                        |                                 |
//              heap_base +---------------------------------+ 4k  ---
//                        | +-----------------------------+ |      |
//                        | | struct heap_header (1k)     | |      |
//                        | +-----------------------------+ |      |
//                        | | struct zone_header (64B)    | |      |
//                        | | struct chunk_header(8B)     | |      |
//                        | | struct chunk_header(8B)     | |      |
//                        | | ...... 65528个,不一定都用   | |      |
//                        | | struct chunk_header(8B)     | |      |
//                        | | chunk (256k)                | |      |
//                        | | chunk (256k)                | |      |
//                        | | ...... 最多65528个          | |      |
//                        | | chunk (256k)                | |      |
//                        | +-----------------------------+ |      |
//                        | | struct zone_header (64B)    | |      |
//                        | | struct chunk_header(8B)     | |      |
//                        | | struct chunk_header(8B)     | |  heap_size = path文件大小 - blob-header-size(4k) - sizeof(struct dav_phdr)(4k)
//                        | | ...... 65528个,不一定都用   | |      |                       (什么是blob-header?)
//                        | | struct chunk_header(8B)     | |      |
//                        | | chunk (256k)                | |      |
//                        | | chunk (256k)                | |      |
//                        | | ...... 最多65528个          | |      |
//                        | | chunk (256k)                | |      |
//                        | +-----------------------------+ |      |
//                        | |                             | |      |
//                        | |     ... more zones ...      | |      |
//                        | |                             | |      |
//                        | |  除了最后一个，前面的zone   | |      |
//                        | |  都是65528个chunk,接近16G   | |      |
//                        | |                             | |      |
//                        | +-----------------------------+ |      |
//                        +---------------------------------+     ---
//
//    问：daos_engine进程重启的时候，tmpfs是新建的吗？
//        若不是新建的，/mnt/daos0/c090c2fc-8d83-45de-babe-104bad165593/vos-0中还保留着原来的内容。
//        否则，若是新建的，文件就是空的（虽然内存空间分配了）
//
//    应该是后者，文件(内存)是空的，就要从spdk blob (nvme盘)读取，就是本函数的工作！
static inline int
vos_meta_load(struct umem_store *store, char *start)
{
	uint64_t		 read_size;
    //Yuanguo:
    //  对于MD-on-SSD场景
    //     store->stor_size是文件大小 - blob-header-size(4k) (注意不是struct dav_phdr的4k)
    //     就是说，store->stor_size包含struct dav_phdr的4k空间；
    //     那个blob header在哪呢？
	uint64_t		 remain_size = store->stor_size;
	daos_off_t		 off = 0;
	int			 rc = 0;
    //Yuanguo: 下面的逻辑，用golang的思想去理解，就是启动多个routine，每个routine load一块数据；
    //         meta_load_control和meta_load_arg用来控制routine的并发，以及传递参数给routine。
	struct meta_load_arg	*mla;
	struct meta_load_control mlc;

	mlc.mlc_inflights = 0;
	mlc.mlc_rc = 0;
	mlc.mlc_wait_finished = 0;
	rc = ABT_mutex_create(&mlc.mlc_lock);
	if (rc != ABT_SUCCESS) {
		rc = dss_abterr2der(rc);
		D_ERROR("Failed to create ABT mutex: %d\n", rc);
		return rc;
	}

	rc = ABT_cond_create(&mlc.mlc_cond);
	if (rc != ABT_SUCCESS) {
		rc = dss_abterr2der(rc);
		D_ERROR("Failed to create ABT cond: %d\n", rc);
		goto destroy_lock;
	}

	while (remain_size) {
        //Yuanguo: 一个routine读取1MiB的数据；
		read_size =
		    (remain_size > META_READ_BATCH_SIZE) ? META_READ_BATCH_SIZE : remain_size;

		D_ALLOC_PTR(mla);
		if (mla == NULL) {
			rc = -DER_NOMEM;
			break;
		}
        //Yuanguo:
        //    mla_off       : spdk meta blob内source的偏移；
        //                    从blob header之后开始算，见vos_iod2bsgl函数，翻译时加上blob header的size (stor_hdr_blks * stor_blk_size, 1*4k)
        //    mla_start     : mmap空间内destination的地址；
        //    mla_read_size : 数据长度；
		mla->mla_off = off;
		mla->mla_read_size = read_size;
		mla->mla_start = start;
		mla->mla_store = store;
		mla->mla_control = &mlc;

		mlc.mlc_inflights++;
        //Yuanguo: 类比golang可以理解为 go vos_meta_load_fn(mal);
		rc = vos_exec(vos_meta_load_fn, (void *)mla);
		if (rc || mlc.mlc_rc) {
			if (rc) {
				D_FREE(mla);
				mlc.mlc_inflights--;
			}
			break;
		}

		if (mlc.mlc_inflights > META_READ_QD_NR) {
			ABT_cond_wait(mlc.mlc_cond, mlc.mlc_lock);
			D_ASSERT(mlc.mlc_inflights <= META_READ_QD_NR);
		}

		off += read_size;
		start += read_size;
		remain_size -= read_size;
	}

	mlc.mlc_wait_finished = 1;
	if (mlc.mlc_inflights > 0) {
		ABT_cond_wait(mlc.mlc_cond, mlc.mlc_lock);
		D_ASSERT(mlc.mlc_inflights == 0);
	}
	ABT_cond_free(&mlc.mlc_cond);

destroy_lock:
	ABT_mutex_free(&mlc.mlc_lock);
	return rc ? rc : mlc.mlc_rc;
}

static inline int
vos_meta_writev(struct umem_store *store, struct umem_store_iod *iod, d_sg_list_t *sgl)
{
	return vos_meta_rwv(store, iod, sgl, true);
}

static int
vos_meta_flush_prep(struct umem_store *store, struct umem_store_iod *iod, daos_handle_t *fh)
{
	struct bio_desc		*biod;
	struct bio_sglist	*bsgl;
	int			 rc;

	D_ASSERT(store && store->stor_priv != NULL);
	D_ASSERT(iod->io_nr > 0);
	D_ASSERT(fh != NULL);

	biod = bio_iod_alloc(bio_mc2ioc(store->stor_priv, SMD_DEV_TYPE_META),
			     NULL, 1, BIO_IOD_TYPE_UPDATE);
	if (biod == NULL)
		return -DER_NOMEM;

	bsgl = bio_iod_sgl(biod, 0);
    //Yuanguo:
    // - iod->io_regions是一个列表，有iod->io_nr项，每一项描述spdk meta blob上的一个region；
    //   对于flush来说，它们是IO的destinations;
    // - struct umem_checkpoint_data的cd_sg_list也是一个列表，和iod->io_regions一一对应，
    //   它们描述的是heap regions；对于flush来说，这些heap regions是IO的sources;
    //   (这里还用不到它们，vos_meta_flush_copy的时候才用到，cd_sg_list作为参数`sgl`传给它)
    //
    // struct bio_desc对象是bio的核心，它的bd_sgls是一个"列表的数组"，我们只考虑单个列表的情况，
    // 即`bd_sgls[0]`是一个列表，也就是这里的`bsgl`指向的列表；它的项数和sources/destinations
    // 项数一样(一一对应)；每一项包含IO操作的DMA region 和 meta blob上的region；
    //
    //  struct umem_checkpoint_data的cd_sg_list                              iod->io_regions
    //              (heap regions)                (DMA regions)               (meta blob)
    //
    //         +---------------------+      +---------------------+       +---------------------+
    //         |      region0        |      |                     |       |                     |
    //         +---------------------+      +---------------------+       +---------------------+
    //
    //         +---------------------+      +---------------------+       +---------------------+
    //         |                     |      |                     |       |                     |
    //         |      region1        |      |                     |       |                     |
    //         |                     |      |                     |       |                     |
    //         +---------------------+      +---------------------+       +---------------------+
    //
    //         +---------------------+      +---------------------+       +---------------------+
    //         |      region2        |      |                     |       |                     |
    //         +---------------------+      +---------------------+       +---------------------+
    //
    // `bd_sgls[0]/bsgl`里的元素是struct bio_iov对象:
    //      void*        bi_buf        ---> 指向DMA region；
    //      bio_addr_t	 bi_addr       ---> 指向meta blob内的区间；
	//      size_t       bi_data_len
    //
    // 这里初始化`bd_sgls[0]/bsgl`，即分配一个struct bio_iov对象数组，个数是iod->io_nr；
	rc = bio_sgl_init(bsgl, iod->io_nr);
	if (rc)
		goto free;

    //Yuanguo: 把destinations地址(meta blob上的region地址)转换成bio_iov对象需要的形式，存到bsgl的各项(struct bio_iov)的bi_addr；
	vos_iod2bsgl(store, iod, bsgl);

    //Yuanguo: 准备DMA region，设置到bio_iov对象的bi_buf；
    //  (sources数据(heap中的regions) 在vos_meta_flush_copy中才用的上)
	rc = bio_iod_try_prep(biod, BIO_CHK_TYPE_LOCAL, NULL, 0);
	if (rc) {
		DL_CDEBUG(rc == -DER_AGAIN, DB_TRACE, DLOG_ERR, rc, "Failed to prepare DMA buffer");
		goto free;
	}

	fh->cookie = (uint64_t)biod;
	return 0;
free:
	bio_iod_free(biod);
	return rc;
}

//Yuanguo:
//  - 参数fh->cookie是指向struct bio_desc对象的指针，见vos_meta_flush_prep()函数；
//  - 参数sgl指向heap中的source数据；
//
// struct bio_desc对象是bio的核心，它的核心数据成员是bd_sgls；注意，它是一个"列表的数组"，我们只考虑
// 单个列表的情况，即`bd_sgls[0]`是一个列表；列表的每一个元素是一个struct bio_iov对象；
//
// vos_meta_flush_prep()函数为每个struct bio_iov对象
//   - 填好了destination，即bi_addr/bi_data_len字段，指向nvme meta blob中的区域；
//   - 准备好了DMA内存region，即bi_buf字段；
//
//  本函数把数据拷贝到DMA regions；(将来由vos_meta_flush_post函数开始发起DMA操作)
static int
vos_meta_flush_copy(daos_handle_t fh, d_sg_list_t *sgl)
{
	struct bio_desc	*biod = (struct bio_desc *)fh.cookie;

	D_ASSERT(sgl->sg_nr > 0);
    //Yuanguo: 参数'1'表示只有1个列表；
    //  但列表有多个项，每个项是一个struct bio_iov对象，表示一个region的IO(source到destination的flush);
	return bio_iod_copy(biod, sgl, 1);
}

//Yuanguo: 发起DMA操作；
static int
vos_meta_flush_post(daos_handle_t fh, int err)
{
	struct bio_desc	*biod = (struct bio_desc *)fh.cookie;

	err = bio_iod_post(biod, err);
	bio_iod_free(biod);
	if (err) {
		DL_ERROR(err, "Checkpointing flush failed.");
		/* See the comment in vos_wal_commit() */
		if (err != -DER_NVME_IO) {
			D_ERROR("Checkpointing flush hit fatal error, kill engine...\n");
			err = kill(getpid(), SIGKILL);
			if (err != 0)
				D_ERROR("Failed to raise SIGKILL: %d\n", errno);
		}
	}

	return err;
}

#define VOS_WAL_DIR	"vos_wal"

void
vos_wal_metrics_init(struct vos_wal_metrics *vw_metrics, const char *path, int tgt_id)
{
	int	rc;

	/* Initialize metrics for WAL stats */
	rc = d_tm_add_metric(&vw_metrics->vwm_wal_sz, D_TM_STATS_GAUGE, "WAL tx size",
			     "bytes", "%s/%s/wal_sz/tgt_%d", path, VOS_WAL_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create WAL size telemetry: "DF_RC"\n", DP_RC(rc));

	rc = d_tm_add_metric(&vw_metrics->vwm_wal_qd, D_TM_STATS_GAUGE, "WAL tx QD",
			     "commits", "%s/%s/wal_qd/tgt_%d", path, VOS_WAL_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create WAL QD telemetry: "DF_RC"\n", DP_RC(rc));

	rc = d_tm_add_metric(&vw_metrics->vwm_wal_waiters, D_TM_STATS_GAUGE, "WAL waiters",
			     "transactions", "%s/%s/wal_waiters/tgt_%d", path, VOS_WAL_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create WAL waiters telemetry: "DF_RC"\n", DP_RC(rc));

	rc = d_tm_add_metric(&vw_metrics->vwm_wal_dur, D_TM_DURATION, "WAL commit duration", NULL,
			     "%s/%s/wal_dur/tgt_%d", path, VOS_WAL_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create WAL commit duration telemetry: " DF_RC "\n", DP_RC(rc));

	/* Initialize metrics for WAL replay */
	rc = d_tm_add_metric(&vw_metrics->vwm_replay_count, D_TM_COUNTER, "Number of WAL replays",
			     NULL, "%s/%s/replay_count/tgt_%u", path, VOS_WAL_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'replay_count' telemetry: "DF_RC"\n", DP_RC(rc));

	rc = d_tm_add_metric(&vw_metrics->vwm_replay_size, D_TM_GAUGE, "WAL replay size", "bytes",
			     "%s/%s/replay_size/tgt_%u", path, VOS_WAL_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'replay_size' telemetry: "DF_RC"\n", DP_RC(rc));

	rc = d_tm_add_metric(&vw_metrics->vwm_replay_time, D_TM_GAUGE, "WAL replay time", "us",
			     "%s/%s/replay_time/tgt_%u", path, VOS_WAL_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'replay_time' telemetry: "DF_RC"\n", DP_RC(rc));

	rc = d_tm_add_metric(&vw_metrics->vwm_replay_tx, D_TM_COUNTER,
			     "Number of replayed transactions", NULL,
			     "%s/%s/replay_transactions/tgt_%u", path, VOS_WAL_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'replay_transactions' telemetry: "DF_RC"\n", DP_RC(rc));


	rc = d_tm_add_metric(&vw_metrics->vwm_replay_ent, D_TM_COUNTER,
			     "Number of replayed log entries", NULL,
			     "%s/%s/replay_entries/tgt_%u", path, VOS_WAL_DIR, tgt_id);
	if (rc)
		D_WARN("Failed to create 'replay_entries' telemetry: "DF_RC"\n", DP_RC(rc));
}

static inline int
vos_wal_reserve(struct umem_store *store, uint64_t *tx_id)
{
	struct bio_wal_info	wal_info;
	struct vos_pool		*pool;
	struct bio_wal_stats	ws = { 0 };
	struct vos_wal_metrics	*vwm;
	int			rc;

	pool = store->vos_priv;

	if (unlikely(pool == NULL))
		goto reserve; /** In case there is any race for checkpoint init. */

	/** Update checkpoint state before reserve to ensure we activate checkpointing if there
	 *  is any space pressure in the WAL.
	 */
	bio_wal_query(store->stor_priv, &wal_info);

    //Yuanguo: 检查wal是不是过大，从而触发checkpoint
    //   vos_pool:  ./src/pool/srv_pool_chkpt.c : update_cb()
    //   rbd_pool:  src/rdb/rdb.c               : rdb_chkpt_update()
	pool->vp_update_cb(pool->vp_chkpt_arg, wal_info.wi_commit_id, wal_info.wi_used_blks,
			   wal_info.wi_tot_blks);

reserve:
	D_ASSERT(store && store->stor_priv != NULL);
	vwm = (struct vos_wal_metrics *)store->stor_stats;
    //Yuanguo: 预留transaction id (tx_id)，实际上是在wal中预留空间；
	rc = bio_wal_reserve(store->stor_priv, tx_id, (vwm != NULL) ? &ws : NULL);
	if (rc == 0 && vwm != NULL)
		d_tm_set_gauge(vwm->vwm_wal_waiters, ws.ws_waiters);

	return rc;
}

static inline int
vos_wal_commit(struct umem_store *store, struct umem_wal_tx *wal_tx, void *data_iod)
{
	struct bio_wal_info     wal_info;
	struct vos_pool        *pool;
	struct bio_wal_stats    ws = {0};
	struct vos_wal_metrics *vwm;
	int                     rc;

	D_ASSERT(store && store->stor_priv != NULL);
	vwm = (struct vos_wal_metrics *)store->stor_stats;
	if (vwm != NULL)
		d_tm_mark_duration_start(vwm->vwm_wal_dur, D_TM_CLOCK_REALTIME);
	rc = bio_wal_commit(store->stor_priv, wal_tx, data_iod, (vwm != NULL) ? &ws : NULL);
	if (vwm != NULL)
		d_tm_mark_duration_end(vwm->vwm_wal_dur);
	if (rc) {
		DL_ERROR(rc, "WAL commit failed.");
		/*
		 * WAL commit could fail due to faulty NVMe or other fatal errors like ENOMEM
		 * or software bug.
		 *
		 * On NVMe I/O error, the NVMe device should have been marked as faulty (in
		 * the BIO module), and a series actions will be automatically triggered to
		 * take DOWN all impacted pool targets. We just suppress the error here since
		 * the caller (DAV) can't cope with commit error.
		 *
		 * On other fatal error, the best we can do is killing the engine...
		 */
		if (rc != -DER_NVME_IO) {
			D_ERROR("WAL commit hit fatal error, kill engine...\n");
			rc = kill(getpid(), SIGKILL);
			if (rc != 0)
				D_ERROR("Failed to raise SIGKILL: %d\n", errno);
		}
		store->store_faulty = true;
	} else if (vwm != NULL) {
		d_tm_set_gauge(vwm->vwm_wal_sz, ws.ws_size);
		d_tm_set_gauge(vwm->vwm_wal_qd, ws.ws_qd);
	}

	pool = store->vos_priv;
	if (unlikely(pool == NULL))
		return 0; /** In case there is any race for checkpoint init. */

	/** Update checkpoint state after commit in case there is an active checkpoint waiting
	 *  for this commit to finish.
	 */
	bio_wal_query(store->stor_priv, &wal_info);

    //Yuanguo: 检查wal是不是过大，从而触发checkpoint
    //   vos_pool:  ./src/pool/srv_pool_chkpt.c : update_cb()
    //   rbd_pool:  src/rdb/rdb.c               : rdb_chkpt_update()
	pool->vp_update_cb(pool->vp_chkpt_arg, wal_info.wi_commit_id, wal_info.wi_used_blks,
			   wal_info.wi_tot_blks);

	return 0;
}

static inline int
vos_wal_replay(struct umem_store *store,
	       int (*replay_cb)(uint64_t tx_id, struct umem_action *act, void *arg),
	       void *arg)
{
	struct bio_wal_rp_stats wrs;
	int rc;

	D_ASSERT(store && store->stor_priv != NULL);
	rc = bio_wal_replay(store->stor_priv,
			    (store->stor_stats != NULL) ? &wrs : NULL,
			    replay_cb, arg);

	/* VOS file rehydration metrics */
	if (store->stor_stats != NULL && rc >= 0) {
		struct vos_wal_metrics *vwm = (struct vos_wal_metrics *)store->stor_stats;

		d_tm_inc_counter(vwm->vwm_replay_count, 1);
		d_tm_set_gauge(vwm->vwm_replay_size, wrs.wrs_sz);
		d_tm_set_gauge(vwm->vwm_replay_time, wrs.wrs_tm);
		d_tm_inc_counter(vwm->vwm_replay_tx, wrs.wrs_tx_cnt);
		d_tm_inc_counter(vwm->vwm_replay_ent, wrs.wrs_entries);
	}
	return rc;
}

static inline int
vos_wal_id_cmp(struct umem_store *store, uint64_t id1, uint64_t id2)
{
	D_ASSERT(store && store->stor_priv != NULL);
	return bio_wal_id_cmp(store->stor_priv, id1, id2);
}

struct umem_store_ops vos_store_ops = {
	.so_load	= vos_meta_load,
	.so_read	= vos_meta_readv,
	.so_write	= vos_meta_writev,
	.so_flush_prep	= vos_meta_flush_prep,
	.so_flush_copy	= vos_meta_flush_copy,
	.so_flush_post	= vos_meta_flush_post,
	.so_wal_reserv	= vos_wal_reserve,
	.so_wal_submit	= vos_wal_commit,
	.so_wal_replay	= vos_wal_replay,
	.so_wal_id_cmp	= vos_wal_id_cmp,
};

#define	CHKPT_TELEMETRY_DIR	"checkpoint"

void
vos_chkpt_metrics_init(struct vos_chkpt_metrics *vc_metrics, const char *path, int tgt_id)
{
	int rc;

	rc = d_tm_add_metric(&vc_metrics->vcm_duration, D_TM_DURATION, "Checkpoint duration", NULL,
			     "%s/%s/duration/tgt_%d", path, CHKPT_TELEMETRY_DIR, tgt_id);
	if (rc)
		D_WARN("failed to create checkpoint_duration metric: "DF_RC"\n", DP_RC(rc));

	rc = d_tm_add_metric(&vc_metrics->vcm_dirty_pages, D_TM_STATS_GAUGE,
			     "Number of dirty page blocks checkpointed", "16MiB",
			     "%s/%s/dirty_pages/tgt_%d", path, CHKPT_TELEMETRY_DIR, tgt_id);
	if (rc)
		D_WARN("failed to create checkpoint_dirty_pages metric: "DF_RC"\n", DP_RC(rc));

	rc = d_tm_add_metric(&vc_metrics->vcm_dirty_chunks, D_TM_STATS_GAUGE,
			     "Number of umem chunks checkpointed", "4KiB",
			     "%s/%s/dirty_chunks/tgt_%d", path, CHKPT_TELEMETRY_DIR, tgt_id);
	if (rc)
		D_WARN("failed to create checkpoint_dirty_chunks metric: "DF_RC"\n", DP_RC(rc));

	rc = d_tm_add_metric(&vc_metrics->vcm_iovs_copied, D_TM_STATS_GAUGE,
			     "Number of sgl iovs used to copy dirty chunks", NULL,
			     "%s/%s/iovs_copied/tgt_%d", path, CHKPT_TELEMETRY_DIR, tgt_id);
	if (rc)
		D_WARN("failed to create checkpoint_iovs_copied metric: "DF_RC"\n", DP_RC(rc));

	rc = d_tm_add_metric(&vc_metrics->vcm_wal_purged, D_TM_STATS_GAUGE,
			     "Size of WAL purged by the checkpoint", "4KiB",
			     "%s/%s/wal_purged/tgt_%d", path, CHKPT_TELEMETRY_DIR, tgt_id);
	if (rc)
		D_WARN("failed to create checkpoint_wal_purged metric: "DF_RC"\n", DP_RC(rc));

}

void
vos_pool_checkpoint_init(daos_handle_t poh, vos_chkpt_update_cb_t update_cb,
			 vos_chkpt_wait_cb_t wait_cb, void *arg, struct umem_store **storep)
{
	struct vos_pool      *pool;
	struct umem_instance *umm;
	struct umem_store    *store;
	struct bio_wal_info   wal_info;

	pool = vos_hdl2pool(poh);
	D_ASSERT(pool != NULL);

	umm   = vos_pool2umm(pool);
	store = &umm->umm_pool->up_store;

	pool->vp_update_cb = update_cb;
	pool->vp_wait_cb   = wait_cb;
	pool->vp_chkpt_arg = arg;
	store->vos_priv    = pool;

	*storep = store;

	bio_wal_query(store->stor_priv, &wal_info);

	/** Set the initial values */
	update_cb(arg, wal_info.wi_commit_id, wal_info.wi_used_blks, wal_info.wi_tot_blks);
}

void
vos_pool_checkpoint_fini(daos_handle_t poh)
{
	struct vos_pool      *pool;
	struct umem_instance *umm;
	struct umem_store    *store;

	pool = vos_hdl2pool(poh);
	D_ASSERT(pool != NULL);

	umm   = vos_pool2umm(pool);
	store = &umm->umm_pool->up_store;

	pool->vp_update_cb = NULL;
	pool->vp_wait_cb   = NULL;
	pool->vp_chkpt_arg = NULL;
	store->vos_priv    = NULL;
}

bool
vos_pool_needs_checkpoint(daos_handle_t poh)
{
	struct vos_pool *pool;

	pool = vos_hdl2pool(poh);
	D_ASSERT(pool != NULL);

	/** TODO: Revisit. */
	return bio_nvme_configured(SMD_DEV_TYPE_META);
}

int
vos_pool_checkpoint(daos_handle_t poh)
{
	struct vos_pool               *pool;
	uint64_t                       tx_id;
	struct umem_instance          *umm;
	struct umem_store             *store;
	struct bio_wal_info            wal_info;
	int                            rc;
	uint64_t                       purge_size = 0;
	struct umem_cache_chkpt_stats  stats;
	struct vos_chkpt_metrics      *chkpt_metrics = NULL;

	pool = vos_hdl2pool(poh);
	D_ASSERT(pool != NULL);

	umm   = vos_pool2umm(pool);
	store = &umm->umm_pool->up_store;

	if (pool->vp_metrics != NULL)
		chkpt_metrics = &pool->vp_metrics->vp_chkpt_metrics;

	if (chkpt_metrics != NULL)
		d_tm_mark_duration_start(chkpt_metrics->vcm_duration, D_TM_CLOCK_REALTIME);

	bio_wal_query(store->stor_priv, &wal_info);
	tx_id = wal_info.wi_commit_id;
	if (tx_id == wal_info.wi_ckp_id) {
		D_DEBUG(DB_TRACE, "No checkpoint needed for "DF_UUID"\n", DP_UUID(pool->vp_id));
		return 0;
	}

	D_DEBUG(DB_MD, "Checkpoint started pool=" DF_UUID ", committed_id=" DF_X64 "\n",
		DP_UUID(pool->vp_id), tx_id);

	rc = bio_meta_clear_empty(store->stor_priv);
	if (rc)
		return rc;

	rc = umem_cache_checkpoint(store, pool->vp_wait_cb, pool->vp_chkpt_arg, &tx_id, &stats);

	if (rc == 0)
		rc = bio_wal_checkpoint(store->stor_priv, tx_id, &purge_size);

	bio_wal_query(store->stor_priv, &wal_info);

	/* Update the used block info post checkpoint */
	pool->vp_update_cb(pool->vp_chkpt_arg, wal_info.wi_commit_id, wal_info.wi_used_blks,
			   wal_info.wi_tot_blks);

	D_DEBUG(DB_MD,
		"Checkpoint finished pool=" DF_UUID ", committed_id=" DF_X64 ", rc=" DF_RC "\n",
		DP_UUID(pool->vp_id), tx_id, DP_RC(rc));

	if (chkpt_metrics != NULL) {
		d_tm_mark_duration_end(chkpt_metrics->vcm_duration);
		if (!rc) {
			d_tm_set_gauge(chkpt_metrics->vcm_dirty_pages, stats.uccs_nr_pages);
			d_tm_set_gauge(chkpt_metrics->vcm_dirty_chunks, stats.uccs_nr_dchunks);
			d_tm_set_gauge(chkpt_metrics->vcm_iovs_copied, stats.uccs_nr_iovs);
			d_tm_set_gauge(chkpt_metrics->vcm_wal_purged, purge_size);
		}
	}
	return rc;
}

int
vos_pool_settings_init(bool md_on_ssd)
{
	return umempobj_settings_init(md_on_ssd);
}

static inline enum bio_mc_flags
vos2mc_flags(unsigned int vos_flags)
{
	enum bio_mc_flags mc_flags = 0;

	if (vos_flags & VOS_POF_RDB)
		mc_flags |= BIO_MC_FL_RDB;

	if (vos_flags & VOS_POF_FOR_RECREATE)
		mc_flags |= BIO_MC_FL_RECREATE;

	return mc_flags;
}

//Yuanguo:
//  MD-on-SSD情况下，创建pool:
//
//     dmg pool create --scm-size=200G --nvme-size=3000G --properties rd_fac:2 ensblock
//
//     上述200G,3000G都是单个engine的配置；
//     假设单engine的target数是18
//     假设pool的uuid是c090c2fc-8d83-45de-babe-104bad165593 (可以通过dmg pool list -v查看)
//
//     - vos_db pool
//         - path = "/var/daos/config/daos_control/engine0/daos_sys/sys_db"   (NOT on tmpfs)
//
//     - 数据pool
//         - path = "/mnt/daos0/NEWBORNS/c090c2fc-8d83-45de-babe-104bad165593/vos-0"  (on tmpfs)
//                  ...
//                  这些文件已经被创建(可能是dao_server创建的)
//                  它们的size都是20G/18
//
//         - pool_id = c090c2fc-8d83-45de-babe-104bad165593
//         - layout = "vos_pool_layout"
//         - scm_sz = 0     注意不是200G/18 (可以获取path文件的size)
//         - nvme_sz = 3000G/18
//         - wal_sz = 0
static int
vos_pmemobj_create(const char *path, uuid_t pool_id, const char *layout,
		   size_t scm_sz, size_t nvme_sz, size_t wal_sz, unsigned int flags,
		   struct umem_pool **ph)
{
	struct bio_xs_context	*xs_ctxt = vos_xsctxt_get();
	struct umem_store	 store = { 0 };
	struct bio_meta_context	*mc;
	struct umem_pool	*pop = NULL;
	enum bio_mc_flags	 mc_flags = vos2mc_flags(flags);
	size_t			 meta_sz = scm_sz;
	int			 rc, ret;

	*ph = NULL;
	/* always use PMEM mode for SMD */
    //Yuanguo: 对于MD-on-SSD，store.store_type = DAOS_MD_BMEM
	store.store_type = umempobj_get_backend_type();
	if (flags & VOS_POF_SYSDB) {
		store.store_type = DAOS_MD_PMEM;
		store.store_standalone = true;
	}

	/* No NVMe is configured or current xstream doesn't have NVMe context */
	if (!bio_nvme_configured(SMD_DEV_TYPE_MAX) || xs_ctxt == NULL)
		goto umem_create;

    //Yuanguo: 通过stat(path)文件，获取scm_sz
	if (!scm_sz) {
		struct stat lstat;

		rc = stat(path, &lstat);
		if (rc != 0)
			return daos_errno2der(errno);
		meta_sz = lstat.st_size;
	}

	D_DEBUG(DB_MGMT, "Create BIO meta context for xs:%p pool:"DF_UUID" "
		"meta_sz: %zu, nvme_sz: %zu wal_sz:%zu\n",
		xs_ctxt, DP_UUID(pool_id), meta_sz, nvme_sz, wal_sz);

    //Yuanguo: 创建data/meta/wal spdk-blob
	rc = bio_mc_create(xs_ctxt, pool_id, meta_sz, wal_sz, nvme_sz, mc_flags);
	if (rc != 0) {
		D_ERROR("Failed to create BIO meta context for xs:%p pool:"DF_UUID". "DF_RC"\n",
			xs_ctxt, DP_UUID(pool_id), DP_RC(rc));
		return rc;
	}

	rc = bio_mc_open(xs_ctxt, pool_id, mc_flags, &mc);
	if (rc != 0) {
		D_ERROR("Failed to open BIO meta context for xs:%p pool:"DF_UUID". "DF_RC"\n",
			xs_ctxt, DP_UUID(pool_id), DP_RC(rc));

		ret = bio_mc_destroy(xs_ctxt, pool_id, mc_flags);
		if (ret)
			D_ERROR("Failed to destroy BIO meta context. "DF_RC"\n", DP_RC(ret));

		return rc;
	}

    //Yuanguo:
    //  - 对于MD-on-SSD
    //       scm_sz               : 0
    //       meta_sz              : 文件/mnt/daos0/NEWBORNS/c090c2fc-8d83-45de-babe-104bad165593/vos-0的size；
    //       store.stor_size      : meta_sz - 4k. 即meta_sz扣除header-size(默认是1个block,4k)；
    //       store.stor_blk_size  : 默认4k. 见bio_mc_create() --> meta_format()
    //       store.stor_hdr_blks  : 默认1. 见bio_mc_create() --> meta_format()
	bio_meta_get_attr(mc, &store.stor_size, &store.stor_blk_size, &store.stor_hdr_blks);
	store.stor_priv = mc;
	store.stor_ops = &vos_store_ops;

umem_create:
	pop = umempobj_create(path, layout, UMEMPOBJ_ENABLE_STATS, scm_sz, 0600, &store);
	if (pop != NULL) {
		*ph = pop;
		return 0;
	}
	rc = daos_errno2der(errno);
	D_ASSERT(rc != 0);

	if (store.stor_priv != NULL) {
		ret = bio_mc_close(store.stor_priv);
		if (ret) {
			D_ERROR("Failed to close BIO meta context. "DF_RC"\n", DP_RC(ret));
			return rc;
		}
		ret = bio_mc_destroy(xs_ctxt, pool_id, mc_flags);
		if (ret)
			D_ERROR("Failed to destroy BIO meta context. "DF_RC"\n", DP_RC(ret));
	}

	return rc;
}

static int
vos_pmemobj_open(const char *path, uuid_t pool_id, const char *layout, unsigned int flags,
		 void *metrics, struct umem_pool **ph)
{
	struct bio_xs_context	*xs_ctxt = vos_xsctxt_get();
	struct umem_store	 store = { 0 };
	struct bio_meta_context	*mc;
	struct umem_pool	*pop;
	enum bio_mc_flags	 mc_flags = vos2mc_flags(flags);
	int			 rc, ret;

	*ph = NULL;
	/* always use PMEM mode for SMD */
	store.store_type = umempobj_get_backend_type();
	if (flags & VOS_POF_SYSDB) {
		store.store_type = DAOS_MD_PMEM;
		store.store_standalone = true;
	}

	/* No NVMe is configured or current xstream doesn't have NVMe context */
	if (!bio_nvme_configured(SMD_DEV_TYPE_MAX) || xs_ctxt == NULL)
		goto umem_open;

	D_DEBUG(DB_MGMT, "Open BIO meta context for xs:%p pool:"DF_UUID"\n",
		xs_ctxt, DP_UUID(pool_id));

	rc = bio_mc_open(xs_ctxt, pool_id, mc_flags, &mc);
	if (rc) {
		D_ERROR("Failed to open BIO meta context for xs:%p pool:"DF_UUID", "DF_RC"\n",
			xs_ctxt, DP_UUID(pool_id), DP_RC(rc));
		return rc;
	}

	bio_meta_get_attr(mc, &store.stor_size, &store.stor_blk_size, &store.stor_hdr_blks);
	store.stor_priv = mc;
	store.stor_ops = &vos_store_ops;
	if (metrics != NULL) {
		struct vos_pool_metrics	*vpm = (struct vos_pool_metrics *)metrics;

		store.stor_stats = &vpm->vp_wal_metrics;
	}

umem_open:
	pop = umempobj_open(path, layout, UMEMPOBJ_ENABLE_STATS, &store);
	if (pop != NULL) {
		*ph = pop;
		return 0;
	}
	rc = daos_errno2der(errno);
	D_ASSERT(rc != 0);

	if (store.stor_priv != NULL) {
		ret = bio_mc_close(store.stor_priv);
		if (ret)
			D_ERROR("Failed to close BIO meta context. "DF_RC"\n", DP_RC(ret));
	}

	return rc;
}

static inline void
vos_pmemobj_close(struct umem_pool *pop)
{
	struct umem_store	store;
	int			rc;

	store = pop->up_store;

	umempobj_close(pop);

	if (store.stor_priv != NULL) {
		rc = bio_mc_close(store.stor_priv);
		if (rc)
			D_ERROR("Failed to close BIO meta context. "DF_RC"\n", DP_RC(rc));
	}
}

static inline struct vos_pool_df *
vos_pool_pop2df(struct umem_pool *pop)
{
	return (struct vos_pool_df *)
		umempobj_get_rootptr(pop, sizeof(struct vos_pool_df));
}

static struct vos_pool *
pool_hlink2ptr(struct d_ulink *hlink)
{
	D_ASSERT(hlink != NULL);
	return container_of(hlink, struct vos_pool, vp_hlink);
}

static void
vos_delete_blob(uuid_t pool_uuid, unsigned int flags)
{
	struct bio_xs_context	*xs_ctxt = vos_xsctxt_get();
	enum bio_mc_flags	 mc_flags = vos2mc_flags(flags);
	int			 rc;

	/* NVMe device isn't configured */
	if (!bio_nvme_configured(SMD_DEV_TYPE_MAX) || xs_ctxt == NULL)
		return;

	D_DEBUG(DB_MGMT, "Deleting blob for xs:%p pool:"DF_UUID"\n",
		xs_ctxt, DP_UUID(pool_uuid));

	rc = bio_mc_destroy(xs_ctxt, pool_uuid, mc_flags);
	if (rc)
		D_ERROR("Destroying meta context blob for xs:%p pool="DF_UUID" failed: "DF_RC"\n",
			xs_ctxt, DP_UUID(pool_uuid), DP_RC(rc));

	return;
}

static void
pool_hop_free(struct d_ulink *hlink)
{
	struct vos_pool		*pool = pool_hlink2ptr(hlink);
	int			 rc;

	D_ASSERT(pool->vp_opened == 0);
	D_ASSERT(!gc_have_pool(pool));

	if (pool->vp_vea_info != NULL)
		vea_unload(pool->vp_vea_info);

	if (daos_handle_is_valid(pool->vp_cont_th))
		dbtree_close(pool->vp_cont_th);

	if (pool->vp_size != 0) {
		rc = munlock((void *)pool->vp_umm.umm_base, pool->vp_size);
		if (rc != 0)
			D_WARN("Failed to unlock pool memory at "DF_X64": errno=%d (%s)\n",
			       pool->vp_umm.umm_base, errno, strerror(errno));
		else
			D_DEBUG(DB_MGMT, "Unlocked VOS pool memory: "DF_U64" bytes at "DF_X64"\n",
				pool->vp_size, pool->vp_umm.umm_base);
	}

	if (pool->vp_uma.uma_pool)
		vos_pmemobj_close(pool->vp_uma.uma_pool);

	vos_dedup_fini(pool);

	if (pool->vp_dummy_ioctxt) {
		rc = bio_ioctxt_close(pool->vp_dummy_ioctxt);
		if (rc != 0)
			D_WARN("Failed to close dummy ioctxt: rc=%d\n", rc);
	}

	if (pool->vp_dying)
		vos_delete_blob(pool->vp_id, pool->vp_rdb ? VOS_POF_RDB : 0);

	D_FREE(pool);
}

static struct d_ulink_ops   pool_uuid_hops = {
	.uop_free       = pool_hop_free,
};

/** allocate DRAM instance of vos pool */
static int
pool_alloc(uuid_t uuid, struct vos_pool **pool_p)
{
	struct vos_pool		*pool;

	D_ALLOC_PTR(pool);
	if (pool == NULL)
		return -DER_NOMEM;

	d_uhash_ulink_init(&pool->vp_hlink, &pool_uuid_hops);
	D_INIT_LIST_HEAD(&pool->vp_gc_link);
	D_INIT_LIST_HEAD(&pool->vp_gc_cont);
	uuid_copy(pool->vp_id, uuid);

	*pool_p = pool;
	return 0;
}

static int
pool_link(struct vos_pool *pool, struct d_uuid *ukey, daos_handle_t *poh)
{
	int	rc;

	rc = d_uhash_link_insert(vos_pool_hhash_get(pool->vp_sysdb), ukey, NULL,
				 &pool->vp_hlink);
	if (rc) {
		D_ERROR("uuid hash table insert failed: "DF_RC"\n", DP_RC(rc));
		D_GOTO(failed, rc);
	}
	*poh = vos_pool2hdl(pool);
	return 0;
failed:
	return rc;
}

static int
pool_lookup(struct d_uuid *ukey, struct vos_pool **pool)
{
	struct d_ulink *hlink;
	bool		is_sysdb = uuid_compare(ukey->uuid, *vos_db_pool_uuid()) == 0 ?
					true : false;

	hlink = d_uhash_link_lookup(vos_pool_hhash_get(is_sysdb), ukey, NULL);
	if (hlink == NULL) {
		D_DEBUG(DB_MGMT, "can't find "DF_UUID"\n", DP_UUID(ukey->uuid));
		return -DER_NONEXIST;
	}

	*pool = pool_hlink2ptr(hlink);
	return 0;
}

static int
vos_blob_format_cb(void *cb_data)
{
	struct bio_blob_hdr	*blob_hdr = cb_data;
	struct bio_xs_context	*xs_ctxt = vos_xsctxt_get();
	struct bio_io_context	*ioctxt;
	int			 rc;

	/* Create a bio_io_context to get the blob */
	rc = bio_ioctxt_open(&ioctxt, xs_ctxt, blob_hdr->bbh_pool, false);
	if (rc) {
		D_ERROR("Failed to create an I/O context for writing blob "
			"header: "DF_RC"\n", DP_RC(rc));
		return rc;
	}

	/* Write the blob header info to blob offset 0 */
	rc = bio_write_blob_hdr(ioctxt, blob_hdr);
	if (rc)
		D_ERROR("Failed to write header for blob:"DF_U64" : "DF_RC"\n",
			blob_hdr->bbh_blob_id, DP_RC(rc));

	rc = bio_ioctxt_close(ioctxt);
	if (rc)
		D_ERROR("Failed to free I/O context: "DF_RC"\n", DP_RC(rc));

	return rc;
}

/**
 * Unmap (TRIM) the extent being freed
 */
static int
vos_blob_unmap_cb(d_sg_list_t *unmap_sgl, uint32_t blk_sz, void *data)
{
	struct bio_io_context	*ioctxt = data;
	int			 rc;

	/* unmap unused pages for NVMe media to perform more efficiently */
	rc = bio_blob_unmap_sgl(ioctxt, unmap_sgl, blk_sz);
	if (rc)
		D_ERROR("Blob unmap SGL failed. "DF_RC"\n", DP_RC(rc));

	return rc;
}

static int pool_open(void *ph, struct vos_pool_df *pool_df,
		     unsigned int flags, void *metrics, daos_handle_t *poh);

//Yuanguo:
//  MD-on-SSD情况下，创建pool:
//
//     dmg pool create --scm-size=200G --nvme-size=3000G --properties rd_fac:2 ensblock
//
//     上述200G,3000G都是单个engine的配置；
//     假设单engine的target数是18
//     假设pool的uuid是c090c2fc-8d83-45de-babe-104bad165593 (可以通过dmg pool list -v查看)
//
//     - vos_db pool
//         - path = "/var/daos/config/daos_control/engine0/daos_sys/sys_db"   (NOT on tmpfs)
//
//     - 数据pool
//         - path = "/mnt/daos0/NEWBORNS/c090c2fc-8d83-45de-babe-104bad165593/vos-0"  (on tmpfs)
//                  "/mnt/daos0/NEWBORNS/c090c2fc-8d83-45de-babe-104bad165593/vos-1"  (on tmpfs)
//                  ...
//                  "/mnt/daos0/NEWBORNS/c090c2fc-8d83-45de-babe-104bad165593/vos-17" (on tmpfs)
//
//                  这些文件已经被创建(可能是dao_server创建的)
//                  它们的size都是20G/18
//
//         - uuid = c090c2fc-8d83-45de-babe-104bad165593
//         - scm_sz = 0     注意不是200G/18 (可以获取path文件的size)
//         - nvme_sz = 3000G/18
//         - wal_sz = 0
int
vos_pool_create_ex(const char *path, uuid_t uuid, daos_size_t scm_sz, daos_size_t nvme_sz,
		   daos_size_t wal_sz, unsigned int flags, uint32_t version, daos_handle_t *poh)
{
	struct umem_pool	*ph;
	struct umem_attr	 uma = {0};
	struct umem_instance	 umem = {0};
	struct vos_pool_df	*pool_df;
	struct bio_blob_hdr	 blob_hdr;
	uint32_t		 vea_compat = 0;
	daos_handle_t		 hdl;
	struct d_uuid		 ukey;
	struct vos_pool		*pool = NULL;
	int			 rc = 0;

	if (!path || uuid_is_null(uuid) || daos_file_is_dax(path))
		return -DER_INVAL;

	if (version == 0)
		version = POOL_DF_VERSION;
	else if (version < POOL_DF_VER_1 || version > POOL_DF_VERSION)
		return -DER_INVAL;

	D_DEBUG(DB_MGMT,
		"Pool Path: %s, size: " DF_U64 ":" DF_U64 ", "
		"UUID: " DF_UUID ", version: %u\n",
		path, scm_sz, nvme_sz, DP_UUID(uuid), version);

	if (flags & VOS_POF_SMALL)
		flags |= VOS_POF_EXCL;

	uuid_copy(ukey.uuid, uuid);
	rc = pool_lookup(&ukey, &pool);
	if (rc == 0) {
		D_ASSERT(pool != NULL);
		D_ERROR("Found already opened(%d) pool:%p dying(%d)\n",
			pool->vp_opened, pool, pool->vp_dying);
		vos_pool_decref(pool);
		return -DER_EXIST;
	}

	/* Path must be a file with a certain size when size argument is 0 */
	if (!scm_sz && access(path, F_OK) == -1) {
		D_ERROR("File not accessible (%d) when size is 0\n", errno);
		return daos_errno2der(errno);
	}

	rc = vos_pmemobj_create(path, uuid, VOS_POOL_LAYOUT, scm_sz, nvme_sz, wal_sz, flags, &ph);
	if (rc) {
		D_ERROR("Failed to create pool %s, scm_sz="DF_U64", nvme_sz="DF_U64". "DF_RC"\n",
			path, scm_sz, nvme_sz, DP_RC(rc));
		return rc;
	}

    //Yuanguo:
    //  对于persistent memory(MD-on-SSD是模拟的persistent memory)，机器重启之后，内存上存的数据在哪呢？
    //  要访问内存上的数据，就需要一个root，通过这个root可达重启之前的所有内存数据(有点类似与Java虚拟
    //  机管理内存)。这个root存在于“已知的固定的位置”，相对于persistent memory的起始地址(起始地址每次
    //  重启可能mmap到不同地址)
    //  root处存储的结构体类型是应用程序开发者设计的，对于DAOS vos pool，是struct vos_pool_df类型；
	pool_df = vos_pool_pop2df(ph);

	/* If the file is fallocated separately we need the fallocated size
	 * for setting in the root object.
	 */
	if (!scm_sz) {
		struct stat lstat;

		rc = stat(path, &lstat);
		if (rc != 0)
			D_GOTO(close, rc = daos_errno2der(errno));
		scm_sz = lstat.st_size;
	}

	uma.uma_id = umempobj_backend_type2class_id(ph->up_store.store_type);
	uma.uma_pool = ph;

    //Yuanguo:
    //  全局数组umem_class_defined定义了一些内存class (pmem, bmem, ...)；可以理解为C++的class(包含成员函数)
    //  这里根据class id (uma.uma_id)，以及数据成员，实例化一个struct umem_instance (可以理解为C++对象)；
    //  它(umem)描述了本vos pool的内存池(heap)信息；
	rc = umem_class_init(&uma, &umem);
	if (rc != 0)
		goto close;

    //Yuanguo: 开始一个transaction.
    //  对于pmem: pmem_tx_begin()
    //  对于bmem: bmem_tx_begin()
	rc = umem_tx_begin(&umem, NULL);
	if (rc != 0)
		goto close;

    //Yuanguo:
    //  对于pmem: pmem_tx_add_ptr()
    //  对于bmem: bmem_tx_add_ptr()
	rc = umem_tx_add_ptr(&umem, pool_df, sizeof(*pool_df));
	if (rc != 0)
		goto end;

	memset(pool_df, 0, sizeof(*pool_df));
	rc = dbtree_create_inplace(VOS_BTR_CONT_TABLE, 0, VOS_CONT_ORDER,
				   &uma, &pool_df->pd_cont_root, &hdl);
	if (rc != 0)
		goto end;

	dbtree_close(hdl);

	uuid_copy(pool_df->pd_id, uuid);
	pool_df->pd_scm_sz	= scm_sz;
	pool_df->pd_nvme_sz	= nvme_sz;
	pool_df->pd_magic	= POOL_DF_MAGIC;
	if (DAOS_FAIL_CHECK(FLC_POOL_DF_VER))
		pool_df->pd_version = 0;
	else
		pool_df->pd_version = version;

	gc_init_pool(&umem, pool_df);
end:
	/**
	 * The transaction can in reality be aborted
	 * only when there is no memory, either due
	 * to loss of power or no more memory in pool
	 */
	if (rc == 0)
		rc = umem_tx_commit(&umem);
	else
		rc = umem_tx_abort(&umem, rc);

	if (rc != 0) {
		D_ERROR("Initialize pool root error: "DF_RC"\n", DP_RC(rc));
		goto close;
	}

	/* SCM only pool or data blob isn't configured */
	if (nvme_sz == 0 || !bio_nvme_configured(SMD_DEV_TYPE_DATA))
		goto open;

	/* Format SPDK blob header */
	blob_hdr.bbh_blk_sz = VOS_BLK_SZ;
	blob_hdr.bbh_hdr_sz = VOS_BLOB_HDR_BLKS;
	uuid_copy(blob_hdr.bbh_pool, uuid);

	/* Determine VEA compatibility bits */
	/* TODO: only enable bitmap for large pool size */
	if (version >= VOS_POOL_DF_2_6)
		vea_compat |= VEA_COMPAT_FEATURE_BITMAP;

	/* Format SPDK blob*/
	rc = vea_format(&umem, vos_txd_get(flags & VOS_POF_SYSDB), &pool_df->pd_vea_df,
			VOS_BLK_SZ, VOS_BLOB_HDR_BLKS, nvme_sz, vos_blob_format_cb,
			&blob_hdr, false, vea_compat);
	if (rc) {
		D_ERROR("Format blob error for pool:"DF_UUID". "DF_RC"\n",
			DP_UUID(uuid), DP_RC(rc));
		goto close;
	}

open:
	/* If the caller does not want a VOS pool handle, we're done. */
	if (poh == NULL)
		goto close;

	/* Create a VOS pool handle using ph. */
	rc = pool_open(ph, pool_df, flags, NULL, poh);
	ph = NULL;

close:
	/* Close this local handle, if it hasn't been consumed nor already
	 * been closed by pool_open upon error.
	 */
	if (ph != NULL)
		vos_pmemobj_close(ph);
	return rc;
}

//Yuanguo:
//  MD-on-SSD情况下，创建pool:
//
//     dmg pool create --scm-size=200G --nvme-size=3000G --properties rd_fac:2 ensblock
//
//     上述200G,3000G都是单个engine的配置；
//     假设单engine的target数是18
//     假设pool的uuid是c090c2fc-8d83-45de-babe-104bad165593 (可以通过dmg pool list -v查看)
//
//     - vos_db pool
//         - path = "/var/daos/config/daos_control/engine0/daos_sys/sys_db"   (NOT on tmpfs)
//
//     - 数据pool
//         - path = "/mnt/daos0/NEWBORNS/c090c2fc-8d83-45de-babe-104bad165593/vos-0"  (on tmpfs)
//                  "/mnt/daos0/NEWBORNS/c090c2fc-8d83-45de-babe-104bad165593/vos-1"  (on tmpfs)
//                  ...
//                  "/mnt/daos0/NEWBORNS/c090c2fc-8d83-45de-babe-104bad165593/vos-17" (on tmpfs)
//
//                  这些文件已经被创建(可能是dao_server创建的)
//                  它们的size都是20G/18
//
//         - uuid = "c090c2fc"
//         - scm_sz = 0     注意不是200G/18 (可以获取path文件的size)
//         - nvme_sz = 3000G/18
int
vos_pool_create(const char *path, uuid_t uuid, daos_size_t scm_sz, daos_size_t nvme_sz,
		unsigned int flags, uint32_t version, daos_handle_t *poh)
{
	/* create vos pool with default WAL size */
	return vos_pool_create_ex(path, uuid, scm_sz, nvme_sz, 0, flags, version, poh);
}

/**
 * kill the pool before destroy:
 * - detach from GC, delete SPDK blob
 */
int
vos_pool_kill(uuid_t uuid, unsigned int flags)
{
	struct d_uuid	ukey;
	int		rc;

	uuid_copy(ukey.uuid, uuid);
	while (1) {
		struct vos_pool	*pool = NULL;

		rc = pool_lookup(&ukey, &pool);
		if (rc) {
			D_ASSERT(rc == -DER_NONEXIST);
			rc = 0;
			break;
		}
		D_ASSERT(pool->vp_sysdb == false);

		D_ASSERT(pool != NULL);
		if (gc_have_pool(pool)) {
			/* still pinned by GC, un-pin it because there is no
			 * need to run GC for this pool anymore.
			 */
			gc_del_pool(pool);
			vos_pool_decref(pool); /* -1 for lookup */
			continue;	/* try again */
		}
		pool->vp_dying = 1;
		vos_pool_decref(pool); /* -1 for lookup */

		ras_notify_eventf(RAS_POOL_DEFER_DESTROY, RAS_TYPE_INFO, RAS_SEV_WARNING,
				  NULL, NULL, NULL, NULL, &ukey.uuid, NULL, NULL, NULL, NULL,
				  "pool:"DF_UUID" destroy is deferred", DP_UUID(uuid));
		/* Blob destroy will be deferred to last vos_pool ref drop */
		return -DER_BUSY;
	}
	D_DEBUG(DB_MGMT, DF_UUID": No open handles, OK to delete: flags=%x\n", DP_UUID(uuid),
		flags);

	vos_delete_blob(uuid, flags);
	return 0;
}

/**
 * Destroy a Versioning Object Storage Pool (VOSP) and revoke all its handles
 */
int
vos_pool_destroy_ex(const char *path, uuid_t uuid, unsigned int flags)
{
	int	rc;

	D_DEBUG(DB_MGMT, "delete path: %s UUID: "DF_UUID"\n",
		path, DP_UUID(uuid));

	rc = vos_pool_kill(uuid, flags);
	if (rc)
		return rc;

	if (daos_file_is_dax(path))
		return -DER_INVAL;

	/**
	 * NB: no need to explicitly destroy container index table because
	 * pool file removal will do this for free.
	 */
	rc = remove(path);
	if (rc) {
		if (errno == ENOENT)
			D_GOTO(exit, rc = 0);
		D_ERROR("Failure deleting file from PMEM: %s\n",
			strerror(errno));
	}
exit:
	return rc;
}

int
vos_pool_destroy(const char *path, uuid_t uuid)
{
	return vos_pool_destroy_ex(path, uuid, 0);
}

enum {
	/** Memory locking flag not initialized */
	LM_FLAG_UNINIT,
	/** Memory locking disabled */
	LM_FLAG_DISABLED,
	/** Memory locking enabled */
	LM_FLAG_ENABLED
};

static void
lock_pool_memory(struct vos_pool *pool)
{
	static int	lock_mem = LM_FLAG_UNINIT;
	struct rlimit	rlim;
	size_t		lock_bytes;
	int		rc;

	if (lock_mem == LM_FLAG_UNINIT) {
		rc = getrlimit(RLIMIT_MEMLOCK, &rlim);
		if (rc != 0) {
			D_WARN("getrlimit() failed; errno=%d (%s)\n", errno, strerror(errno));
			lock_mem = LM_FLAG_DISABLED;
			return;
		}

		if (rlim.rlim_cur != RLIM_INFINITY || rlim.rlim_max != RLIM_INFINITY) {
			D_WARN("Infinite rlimit not detected, not locking VOS pool memory\n");
			lock_mem = LM_FLAG_DISABLED;
			return;
		}

		lock_mem = LM_FLAG_ENABLED;
	}

	if (lock_mem == LM_FLAG_DISABLED)
		return;

	/*
	 * Mlock may take several tens of seconds to complete when memory
	 * is tight, so mlock is skipped in current MD-on-SSD scenario.
	 */
	if (bio_nvme_configured(SMD_DEV_TYPE_META))
		return;

	lock_bytes = pool->vp_pool_df->pd_scm_sz;
	rc = mlock((void *)pool->vp_umm.umm_base, lock_bytes);
	if (rc != 0) {
		D_WARN("Could not lock memory for VOS pool "DF_U64" bytes at "DF_X64
		       "; errno=%d (%s)\n", lock_bytes, pool->vp_umm.umm_base,
		       errno, strerror(errno));
		return;
	}

	/* Only save the size if the locking was successful */
	pool->vp_size = lock_bytes;
	D_DEBUG(DB_MGMT, "Locking VOS pool in memory "DF_U64" bytes at "DF_X64"\n", pool->vp_size,
		pool->vp_umm.umm_base);
}

/*
 * If successful, this function consumes ph, and closes it upon any error.
 * So the caller shall not close ph in any case.
 */
static int
pool_open(void *ph, struct vos_pool_df *pool_df, unsigned int flags, void *metrics,
	  daos_handle_t *poh)
{
	struct vos_pool		*pool = NULL;
	struct umem_attr	*uma;
	struct d_uuid		 ukey;
	int			 rc;

	/* Create a new handle during open */
	rc = pool_alloc(pool_df->pd_id, &pool); /* returned with refcount=1 */
	if (rc != 0) {
		D_ERROR("Error allocating pool handle\n");
		vos_pmemobj_close(ph);
		return rc;
	}

	uma = &pool->vp_uma;
	uma->uma_pool = ph;
	uma->uma_id = umempobj_backend_type2class_id(uma->uma_pool->up_store.store_type);

	/* Initialize dummy data I/O context */
	rc = bio_ioctxt_open(&pool->vp_dummy_ioctxt, vos_xsctxt_get(), pool->vp_id, true);
	if (rc) {
		D_ERROR("Failed to open dummy I/O context. "DF_RC"\n", DP_RC(rc));
		D_GOTO(failed, rc);
	}

	/* initialize a umem instance for later btree operations */
	rc = umem_class_init(uma, &pool->vp_umm);
	if (rc != 0) {
		D_ERROR("Failed to instantiate umem: "DF_RC"\n", DP_RC(rc));
		D_GOTO(failed, rc);
	}

	/* Cache container table btree hdl */
	rc = dbtree_open_inplace_ex(&pool_df->pd_cont_root, &pool->vp_uma,
				    DAOS_HDL_INVAL, pool, &pool->vp_cont_th);
	if (rc) {
		D_ERROR("Container Tree open failed\n");
		D_GOTO(failed, rc);
	}

	pool->vp_metrics = metrics;
	if (!(flags & VOS_POF_FOR_FEATURE_FLAG) && bio_nvme_configured(SMD_DEV_TYPE_DATA) &&
	    pool_df->pd_nvme_sz != 0) {
		struct vea_unmap_context	 unmap_ctxt;
		struct vos_pool_metrics		*vp_metrics = metrics;
		void				*vea_metrics = NULL;

		if (vp_metrics)
			vea_metrics = vp_metrics->vp_vea_metrics;
		/* set unmap callback fp */
		unmap_ctxt.vnc_unmap = vos_blob_unmap_cb;
		unmap_ctxt.vnc_data = vos_data_ioctxt(pool);
		unmap_ctxt.vnc_ext_flush = flags & VOS_POF_EXTERNAL_FLUSH;
		rc = vea_load(&pool->vp_umm, vos_txd_get(flags & VOS_POF_SYSDB),
			      &pool_df->pd_vea_df, &unmap_ctxt, vea_metrics, &pool->vp_vea_info);
		if (rc) {
			D_ERROR("Failed to load block space info: "DF_RC"\n",
				DP_RC(rc));
			goto failed;
		}
	}

	rc = vos_dedup_init(pool);
	if (rc)
		goto failed;

	/* Insert the opened pool to the uuid hash table */
	uuid_copy(ukey.uuid, pool_df->pd_id);
	pool->vp_sysdb = !!(flags & VOS_POF_SYSDB);
	rc = pool_link(pool, &ukey, poh);
	if (rc) {
		D_ERROR("Error inserting into vos DRAM hash\n");
		D_GOTO(failed, rc);
	}

	pool->vp_dtx_committed_count = 0;
	pool->vp_pool_df             = pool_df;

	pool->vp_opened = 1;
	pool->vp_excl = !!(flags & VOS_POF_EXCL);
	pool->vp_small = !!(flags & VOS_POF_SMALL);
	pool->vp_rdb    = !!(flags & VOS_POF_RDB);
	if (pool_df->pd_version >= VOS_POOL_DF_2_4)
		pool->vp_feats |= VOS_POOL_FEAT_2_4;
	if (pool_df->pd_version >= VOS_POOL_DF_2_6)
		pool->vp_feats |= VOS_POOL_FEAT_2_6;

	if (pool->vp_vea_info == NULL)
		/** always store on SCM if no bdev */
		pool->vp_data_thresh = 0;
	else
		pool->vp_data_thresh = DAOS_PROP_PO_DATA_THRESH_DEFAULT;

	vos_space_sys_init(pool);
	/* Ensure GC is triggered after server restart */
	gc_add_pool(pool);
	lock_pool_memory(pool);
	D_DEBUG(DB_MGMT, "Opened pool %p df version %d\n", pool, pool_df->pd_version);
	return 0;
failed:
	vos_pool_decref(pool); /* -1 for myself */
	return rc;
}

int
vos_pool_open_metrics(const char *path, uuid_t uuid, unsigned int flags, void *metrics,
		      daos_handle_t *poh)
{
	struct vos_pool_df	*pool_df;
	struct vos_pool		*pool = NULL;
	struct d_uuid		 ukey;
	struct umem_pool	*ph;
	int			 rc;
	bool			 skip_uuid_check = flags & VOS_POF_SKIP_UUID_CHECK;

	if (path == NULL || poh == NULL) {
		D_ERROR("Invalid parameters.\n");
		return -DER_INVAL;
	}

	D_DEBUG(DB_MGMT, "Pool Path: %s, UUID: "DF_UUID"\n", path,
		DP_UUID(uuid));

	if (flags & VOS_POF_SMALL)
		flags |= VOS_POF_EXCL;

	if (!skip_uuid_check) {
		uuid_copy(ukey.uuid, uuid);
		rc = pool_lookup(&ukey, &pool);
		if (rc == 0) {
			D_ASSERT(pool != NULL);
			D_DEBUG(DB_MGMT, "Found already opened(%d) pool : %p\n",
				pool->vp_opened, pool);
			if (pool->vp_dying) {
				D_ERROR("Found dying pool : %p\n", pool);
				vos_pool_decref(pool);
				return -DER_BUSY;
			}
			if (!(flags & VOS_POF_FOR_CHECK_QUERY) &&
			    ((flags & VOS_POF_EXCL) || pool->vp_excl)) {
				vos_pool_decref(pool);
				return -DER_BUSY;
			}
			pool->vp_opened++;
			*poh = vos_pool2hdl(pool);
			return 0;
		}
	}

	rc = bio_xsctxt_health_check(vos_xsctxt_get(), false, false);
	if (rc) {
		DL_WARN(rc, DF_UUID": Skip pool open due to faulty NVMe.", DP_UUID(uuid));
		return rc;
	}

	rc = vos_pmemobj_open(path, uuid, VOS_POOL_LAYOUT, flags, metrics, &ph);
	if (rc) {
		D_ERROR("Error in opening the pool "DF_UUID". "DF_RC"\n",
			DP_UUID(uuid), DP_RC(rc));
		return rc;
	}

	pool_df = vos_pool_pop2df(ph);
	if (pool_df->pd_magic != POOL_DF_MAGIC) {
		D_CRIT("Unknown DF magic %x\n", pool_df->pd_magic);
		rc = -DER_DF_INVAL;
		goto out;
	}

	if (pool_df->pd_version > POOL_DF_VERSION ||
	    pool_df->pd_version < POOL_DF_VER_1) {
		D_ERROR("Unsupported DF version %x\n", pool_df->pd_version);
		/** Send a RAS notification */
		vos_report_layout_incompat("VOS pool", pool_df->pd_version,
					   POOL_DF_VER_1, POOL_DF_VERSION,
					   &ukey.uuid);
		rc = -DER_DF_INCOMPT;
		goto out;
	}

	if (!skip_uuid_check && uuid_compare(uuid, pool_df->pd_id)) {
		D_ERROR("Mismatch uuid, user="DF_UUIDF", pool="DF_UUIDF"\n",
			DP_UUID(uuid), DP_UUID(pool_df->pd_id));
		rc = -DER_ID_MISMATCH;
		goto out;
	}

	rc = pool_open(ph, pool_df, flags, metrics, poh);
	ph = NULL;

out:
	/* Close this local handle, if it hasn't been consumed nor already
	 * been closed by pool_open upon error.
	 */
	if (ph != NULL)
		vos_pmemobj_close(ph);
	return rc;
}

int
vos_pool_open(const char *path, uuid_t uuid, unsigned int flags, daos_handle_t *poh)
{
	return vos_pool_open_metrics(path, uuid, flags, NULL, poh);
}

int
vos_pool_upgrade(daos_handle_t poh, uint32_t version)
{
	struct vos_pool    *pool;
	struct vos_pool_df *pool_df;
	int                 rc = 0;

	pool = vos_hdl2pool(poh);
	D_ASSERT(pool != NULL);

	pool_df = pool->vp_pool_df;

	if (version <= pool_df->pd_version) {
		D_INFO(DF_UUID ": Ignore pool durable format upgrade from version %u to %u\n",
		       DP_UUID(pool->vp_id), pool_df->pd_version, version);
		return 0;
	}

	D_INFO(DF_UUID ": Attempting pool durable format upgrade from %d to %d\n",
	       DP_UUID(pool->vp_id), pool_df->pd_version, version);
	D_ASSERTF(version > pool_df->pd_version && version <= POOL_DF_VERSION,
		  "Invalid pool upgrade version %d, current version is %d\n", version,
		  pool_df->pd_version);

	if (version >= VOS_POOL_DF_2_6 && pool_df->pd_version < VOS_POOL_DF_2_6 &&
	    pool->vp_vea_info)
		rc = vea_upgrade(pool->vp_vea_info, &pool->vp_umm, &pool_df->pd_vea_df, version);
	if (rc)
		return rc;

	rc = umem_tx_begin(&pool->vp_umm, NULL);
	if (rc != 0)
		return rc;

	rc = umem_tx_add_ptr(&pool->vp_umm, &pool_df->pd_version, sizeof(pool_df->pd_version));
	if (rc != 0)
		goto end;

	pool_df->pd_version = version;

end:
	rc = umem_tx_end(&pool->vp_umm, rc);

	if (rc != 0)
		return rc;

	if (version >= VOS_POOL_DF_2_2)
		pool->vp_feats |= VOS_POOL_FEAT_2_2;
	if (version >= VOS_POOL_DF_2_4)
		pool->vp_feats |= VOS_POOL_FEAT_2_4;
	if (version >= VOS_POOL_DF_2_6)
		pool->vp_feats |= VOS_POOL_FEAT_2_6;

	return 0;
}

/**
 * Close a VOSP, all opened containers sharing this pool handle
 * will be revoked.
 */
int
vos_pool_close(daos_handle_t poh)
{
	struct vos_pool	*pool;

	pool = vos_hdl2pool(poh);
	if (pool == NULL) {
		D_ERROR("Cannot close a NULL handle\n");
		return -DER_NO_HDL;
	}
	D_DEBUG(DB_MGMT, "Close opened(%d) pool "DF_UUID" (%p).\n",
		pool->vp_opened, DP_UUID(pool->vp_id), pool);

	D_ASSERT(pool->vp_opened > 0);
	pool->vp_opened--;

	/* If the last reference is holding by GC */
	if (pool->vp_opened == 1 && gc_have_pool(pool))
		gc_del_pool(pool);
	else if (pool->vp_opened == 0)
		vos_pool_hash_del(pool);

	vos_pool_decref(pool); /* -1 for myself */
	return 0;
}

/**
 * Query attributes and statistics of the current pool
 */
int
vos_pool_query(daos_handle_t poh, vos_pool_info_t *pinfo)
{
	struct vos_pool		*pool;
	struct vos_pool_df	*pool_df;
	int			 rc;

	pool = vos_hdl2pool(poh);
	if (pool == NULL)
		return -DER_NO_HDL;

	pool_df = pool->vp_pool_df;

	D_ASSERT(pinfo != NULL);
	pinfo->pif_cont_nr = pool_df->pd_cont_nr;
	pinfo->pif_gc_stat = pool->vp_gc_stat_global;

	/*
	 * NOTE: The chk_pool_info::cpi_statistics contains the inconsistency statistics during
	 *	 phase range [CSP_DTX_RESYNC, CSP_AGGREGATION] for the pool shard on the target.
	 *	 Related information will be filled in subsequent CR project milestone.
	 */
	memset(&pinfo->pif_chk, 0, sizeof(pinfo->pif_chk));

	rc = vos_space_query(pool, &pinfo->pif_space, true);
	if (rc)
		D_ERROR("Query pool "DF_UUID" failed. "DF_RC"\n",
			DP_UUID(pool->vp_id), DP_RC(rc));
	return rc;
}

int
vos_pool_query_space(uuid_t pool_id, struct vos_pool_space *vps)
{
	struct vos_pool	*pool = NULL;
	struct d_uuid	 ukey;
	int		 rc;

	uuid_copy(ukey.uuid, pool_id);
	rc = pool_lookup(&ukey, &pool);
	if (rc) {
		D_ASSERT(rc == -DER_NONEXIST);
		return rc;
	}

	D_ASSERT(pool != NULL);
	D_ASSERT(pool->vp_sysdb == false);
	rc = vos_space_query(pool, vps, false);
	vos_pool_decref(pool);
	return rc;
}

int
vos_pool_space_sys_set(daos_handle_t poh, daos_size_t *space_sys)
{
	struct vos_pool	*pool = vos_hdl2pool(poh);

	if (pool == NULL)
		return -DER_NO_HDL;
	if (space_sys == NULL)
		return -DER_INVAL;

	return vos_space_sys_set(pool, space_sys);
}

int
vos_pool_ctl(daos_handle_t poh, enum vos_pool_opc opc, void *param)
{
	struct vos_pool		*pool;
	int			i;

	pool = vos_hdl2pool(poh);
	if (pool == NULL)
		return -DER_NO_HDL;

	switch (opc) {
	default:
		return -DER_NOSYS;
	case VOS_PO_CTL_RESET_GC:
		memset(&pool->vp_gc_stat_global, 0, sizeof(pool->vp_gc_stat_global));
		break;
	case VOS_PO_CTL_SET_DATA_THRESH:
		if (param == NULL)
			return -DER_INVAL;

		if (pool->vp_vea_info == NULL)
			/** no bdev, discard request */
			break;

		pool->vp_data_thresh = *((uint32_t *)param);
		break;
	case VOS_PO_CTL_SET_SPACE_RB:
		if (param == NULL)
			return -DER_INVAL;

		i = *((unsigned int *)param);
		if (i >= 100 || i < 0) {
			D_ERROR("Invalid space reserve ratio for rebuild. %d\n", i);
			return -DER_INVAL;
		}
		pool->vp_space_rb = i;
		break;
	}

	return 0;
}

/** Convenience function to return address of a bio_addr in pmem.  If it's a hole or NVMe address,
 *  it returns NULL.
 */
const void *
vos_pool_biov2addr(daos_handle_t poh, struct bio_iov *biov)
{
	struct vos_pool *pool;

	pool = vos_hdl2pool(poh);
	D_ASSERT(pool != NULL);

	if (bio_addr_is_hole(&biov->bi_addr))
		return NULL;

	if (bio_iov2media(biov) == DAOS_MEDIA_NVME)
		return NULL;

	return umem_off2ptr(vos_pool2umm(pool), bio_iov2raw_off(biov));
}

bool
vos_pool_feature_skip_start(daos_handle_t poh)
{
	struct vos_pool *vos_pool;

	vos_pool = vos_hdl2pool(poh);
	D_ASSERT(vos_pool != NULL);

	return vos_pool->vp_pool_df->pd_compat_flags & VOS_POOL_COMPAT_FLAG_SKIP_START;
}

bool
vos_pool_feature_immutable(daos_handle_t poh)
{
	struct vos_pool *vos_pool;

	vos_pool = vos_hdl2pool(poh);
	D_ASSERT(vos_pool != NULL);

	return vos_pool->vp_pool_df->pd_compat_flags & VOS_POOL_COMPAT_FLAG_IMMUTABLE;
}

bool
vos_pool_feature_skip_rebuild(daos_handle_t poh)
{
	struct vos_pool *vos_pool;

	vos_pool = vos_hdl2pool(poh);
	D_ASSERT(vos_pool != NULL);

	return vos_pool->vp_pool_df->pd_compat_flags & VOS_POOL_COMPAT_FLAG_SKIP_REBUILD;
}

bool
vos_pool_feature_skip_dtx_resync(daos_handle_t poh)
{
	struct vos_pool *vos_pool;

	vos_pool = vos_hdl2pool(poh);
	D_ASSERT(vos_pool != NULL);

	return vos_pool->vp_pool_df->pd_compat_flags & VOS_POOL_COMPAT_FLAG_SKIP_DTX_RESYNC;
}
