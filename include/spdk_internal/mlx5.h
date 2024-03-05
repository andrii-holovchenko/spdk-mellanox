/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#ifndef SPDK_MLX5_H
#define SPDK_MLX5_H

#include "spdk/likely.h"
#include "spdk/tree.h"

#include <infiniband/mlx5dv.h>

#define SPDK_MLX5_VENDOR_ID_MELLANOX 0x2c9

#define SPDK_MLX5_DRIVER_NAME "mlx5"

struct spdk_mlx5_mkey_pool_obj {
	uint32_t mkey;
	/* Determines which pool the mkey belongs to. See \ref spdk_mlx5_mkey_pool_flags */
	uint8_t pool_flag;
	RB_ENTRY(spdk_mlx5_mkey_pool_obj) node;
	struct {
		uint32_t sigerr_count;
		bool sigerr;
	} sig;
};

struct spdk_mlx5_driver_io_context {
	struct ibv_qp *qp;
	struct spdk_mlx5_mkey_pool_obj *mkey;
};

struct spdk_mlx5_crypto_dek;
struct spdk_mlx5_crypto_keytag;

struct spdk_mlx5_crypto_dek_create_attr {
	/* Data Encryption Key in binary form */
	char *dek;
	/* Length of the dek */
	size_t dek_len;
	/* LBA is located in upper part of a tweak */
	bool tweak_upper_lba;
};

enum spdk_mlx5_crypto_key_tweak_mode {
	SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_BE	= 0,
	SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_SIMPLE_LBA_LE	= 1,
	SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_UPPER_LBA_BE	= 2,
	SPDK_MLX5_CRYPTO_KEY_TWEAK_MODE_UPPER_LBA_LE	= 3
};

struct spdk_mlx5_crypto_dek_data {
	/** low level devx obj id which represents the DEK */
	uint32_t dek_obj_id;
	/** Crypto key tweak mode */
	enum spdk_mlx5_crypto_key_tweak_mode tweak_mode;
};

/**
 * Specify which devices are allowed to be used for crypto operation.
 *
 * If the user doesn't call this function then all devices which support crypto will be used.
 * This function copies devices names, in order to free allocated memory, the user must call
 * this function with either NULL \b dev_names or with \b devs_count equal 0. That method can
 * also be used to allow all devices.
 *
 * Subsequent calls with non-NULL \b dev_names and non-zero \b devs_count overwrite previously set
 * values.
 *
 * This function is not thread safe.
 *
 * \param dev_names Array of devices names which are allowed to be used for crypto operations
 * \param devs_count Size of \b devs_count array
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_crypto_devs_allow(const char *const dev_names[], size_t devs_count);

/**
 * Return a NULL terminated array of devices which support crypto operation on Nvidia NICs
 *
 * \param dev_num The size of the array or 0
 * \return Array of contexts. This array must be released with \b spdk_mlx5_crypto_devs_release
 */
struct ibv_context **spdk_mlx5_crypto_devs_get(int *dev_num);

/**
 * Releases array of devices allocated by \b spdk_mlx5_crypto_devs_get
 *
 * \param rdma_devs Array of device to be released
 */
void spdk_mlx5_crypto_devs_release(struct ibv_context **rdma_devs);

/**
 * Create a keytag which contains DEKs per each crypto device in the system
 *
 * \param attr Crypto attributes
 * \param out Keytag
 * \return 0 on success, negated errno of failure
 */
int spdk_mlx5_crypto_keytag_create(struct spdk_mlx5_crypto_dek_create_attr *attr,
				   struct spdk_mlx5_crypto_keytag **out);

/**
 * Destroy a keytag created using \b spdk_mlx5_crypto_keytag_create
 *
 * \param keytag Keytag pointer
 */
void spdk_mlx5_crypto_keytag_destroy(struct spdk_mlx5_crypto_keytag *keytag);

/**
 * Get Data Encryption Key data
 *
 * \param keytag Keytag with DEKs
 * \param pd Protection Domain which is going to be used to register UMR.
 * \param data Data to be filled by this function
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_crypto_get_dek_data(struct spdk_mlx5_crypto_keytag *keytag, struct ibv_pd *pd,
				  struct spdk_mlx5_crypto_dek_data *data);

/* low level cq view, suitable for the direct polling, adapted from struct mlx5dv_cq */
struct spdk_mlx5_hw_cq {
	uint64_t cq_addr;
	uint32_t cqe_cnt;
	uint32_t cqe_size;
	uint32_t ci;
	uint32_t cq_num;
};

/* qp_num is 24 bits. 2D lookup table uses upper and lower 12 bits to find a qp by qp_num */
#define SPDK_MLX5_QP_NUM_UPPER_SHIFT (12)
#define SPDK_MLX5_QP_NUM_LOWER_MASK ((1 << SPDK_MLX5_QP_NUM_UPPER_SHIFT) - 1)
#define SPDK_MLX5_QP_NUM_LUT_SIZE (1 << 12)

struct spdk_mlx5_cq {
	struct spdk_mlx5_hw_cq hw;
	STAILQ_HEAD(, spdk_mlx5_qp) ring_db_qps;
	/* TODO: its better to store this table in a global object per core */
	struct {
		struct spdk_mlx5_qp **table;
		uint32_t count;
	} qps [SPDK_MLX5_QP_NUM_LUT_SIZE];
	struct ibv_cq *verbs_cq;
	uint32_t qps_count;
};

struct spdk_mlx5_cq_attr {
	uint32_t cqe_cnt;
	uint32_t cqe_size;
	void *cq_context;
	struct ibv_comp_channel *comp_channel;
	int comp_vector;
};

struct spdk_mlx5_hw_qp {
	uint64_t dbr_addr;
	uint64_t sq_addr;
	uint64_t sq_bf_addr;
	uint32_t sq_wqe_cnt;
	uint16_t sq_pi;
	uint32_t sq_tx_db_nc;
	uint32_t qp_num;
};

struct spdk_mlx5_qp_attr {
	struct ibv_qp_cap cap;
	bool sigall;
	/* If set then CQ_UPDATE will be cleared for every ctrl WQE and only last ctlr WQE before ringing the doorbell
	 * will be updated with CQ_UPDATE flag */
	bool siglast;
};

struct mlx5_qp_completion {
	uint64_t wr_id;
	/* Number of unsignaled completions before this one. Used to track qp overflow */
	uint32_t completions;
};

enum spdk_mlx5_qp_sig_mode {
	/* Default mode, use flags passed by the user */
	SPDK_MLX5_QP_SIG_NONE = 0,
	/* Enable completion for every control WQE segment, regardless of the flags passed by the user */
	SPDK_MLX5_QP_SIG_ALL = 1,
	/* Enable completion only for the last control WQE segment, regardless of the flags passed by the user */
	SPDK_MLX5_QP_SIG_LAST = 2,
};

struct spdk_mlx5_qp {
	struct spdk_mlx5_hw_qp hw;
	struct mlx5_qp_completion *completions;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct spdk_mlx5_cq *cq;
	struct ibv_qp *verbs_qp;
	STAILQ_ENTRY(spdk_mlx5_qp) db_link;
	uint16_t nonsignaled_outstanding;
	uint16_t max_sge;
	uint16_t tx_available;
	uint16_t last_pi;
	uint8_t sigmode;
	bool tx_need_ring_db;
	bool aes_xts_inc_64;
};

/*
 * MLX5_CQE_SYNDROME_SIGERR is a fake syndrome that corresponds to opcode
 * MLX5_CQE_SIG_ERR. It is added to avoid growing spdk_mlx5_cq_completion.
 *
 * The size of the syndrome field in the HW CQE is 8 bits. So, the new syndrome
 * cannot overlap with the HW syndromes.
 */
enum {
	MLX5_CQE_SYNDROME_SIGERR = 1 << 8,
};

struct spdk_mlx5_cq_completion {
	union {
		uint64_t wr_id;
		uint32_t mkey; /* applicable if status == MLX5_CQE_SYNDROME_SIGERR */
	};
	int status;
};

struct spdk_mlx5_indirect_mkey {
	struct mlx5dv_devx_obj *devx_obj;
	uint32_t mkey;
	uint64_t addr;
};

enum {
	MLX5_ENCRYPTION_ORDER_ENCRYPTED_WIRE_SIGNATURE    = 0x0,
	MLX5_ENCRYPTION_ORDER_ENCRYPTED_MEMORY_SIGNATURE  = 0x1,
	MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE          = 0x2,
	MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_MEMORY        = 0x3,
};

struct spdk_mlx5_umr_crypto_attr {
	/* MLX5_ENCRYPTION_ORDER_ENCRYPTED_RAW_WIRE to encrypt
	 * MLX5_ENCRYPTION_ORDER_ENCRYPTED_MEMORY_SIGNATURE to decrypt */
	uint8_t enc_order;
	uint8_t bs_selector;
	/* Uses enum spdk_mlx5_crypto_key_tweak_mode */
	uint8_t tweak_mode;
	uint32_t dek_obj_id;
	uint64_t xts_iv;
	uint64_t keytag;
};

struct spdk_mlx5_umr_attr {
	struct mlx5_wqe_data_seg *klm;
	uint32_t dv_mkey; /* mkey to configure */
	uint32_t umr_len;
	uint16_t klm_count;
};

/**
 * Create Completion Queue
 *
 * \note: CQ and all associated qpairs must be accessed in scope of a single thread
 * \note: CQ size must be enough to hold completions of all connected qpairs
 *
 * \param pd Protection Domain
 * \param cq_attr Attributes to be used to create CQ
 * \param cq_out Pointer created CQ
 * \return 0 on success, negated errno on failure. \b cq_out is set only on success result
 */
int spdk_mlx5_cq_create(struct ibv_pd *pd, struct spdk_mlx5_cq_attr *cq_attr,
			struct spdk_mlx5_cq **cq_out);

/**
 * Destroy Completion Queue
 *
 * \param cq CQ created with \ref spdk_mlx5_cq_create
 */
int spdk_mlx5_cq_destroy(struct spdk_mlx5_cq *cq);

/**
 * Create loopback qpair suitable for RDMA operations
 *
 * \param pd Protection Domain
 * \param cq Completion Queue to bind QP to
 * \param qp_attr Attributes to be used to create QP
 * \param qp_out Pointer created QP
 * \return 0 on success, negated errno on failure. \b qp_out is set only on success result
 */
int spdk_mlx5_qp_create(struct ibv_pd *pd, struct spdk_mlx5_cq *cq,
			struct spdk_mlx5_qp_attr *qp_attr, struct spdk_mlx5_qp **qp_out);

int spdk_mlx5_qp_set_error_state(struct spdk_mlx5_qp *qp);

/**
 * Destroy qpair
 *
 * \param cq QP created with \ref spdk_mlx5_qp_create
 */
void spdk_mlx5_qp_destroy(struct spdk_mlx5_qp *qp);

/**
 * Poll Completion Queue, save up to \b max_completions into \b comp array
 *
 * \param cq Completion Queue
 * \param comp Array of completions to be filled by this function
 * \param max_completions
 * \return
 */
int spdk_mlx5_cq_poll_completions(struct spdk_mlx5_cq *cq,
				  struct spdk_mlx5_cq_completion *comp, int max_completions);

/**
 * Ring doorbells for all qpairs associated with CQ which have outstanding WQEs
 *
 * \param cq Completion Queue
 * \return Number of updated doorbells or negated errno
 */
int spdk_mlx5_cq_flush_doorbells(struct spdk_mlx5_cq *cq);

/**
 * Prefetch \b wqe_count building blocks into cache
 *
 * \param qp
 * \param wqe_count
 */
/* TODO: use more "intelligent" interface like - num_umrs, num_writes, etc */
static inline void
spdk_mlx5_qp_prefetch_sq(struct spdk_mlx5_qp *qp, uint32_t wqe_count)
{
	struct spdk_mlx5_hw_qp *hw = &qp->hw;
	uint32_t to_end, pi, i;
	char *sq;

	pi = hw->sq_pi & (hw->sq_wqe_cnt - 1);
	sq = (char *)hw->sq_addr + pi * MLX5_SEND_WQE_BB;
	to_end = (hw->sq_wqe_cnt - pi) * MLX5_SEND_WQE_BB;

	if (spdk_likely(to_end >= wqe_count * MLX5_SEND_WQE_BB)) {
		for (i = 0; i < wqe_count; i++) {
			__builtin_prefetch(sq);
			sq += MLX5_SEND_WQE_BB;
		}
	} else {
		for (i = 0; i < wqe_count; i++) {
			__builtin_prefetch(sq);
			to_end -= MLX5_SEND_WQE_BB;
			if (to_end == 0) {
				sq = (char *)hw->sq_addr;
				to_end = hw->sq_wqe_cnt * MLX5_SEND_WQE_BB;
			} else {
				sq += MLX5_SEND_WQE_BB;
			}
		}
	}
}

enum {
	SPDK_MLX5_WQE_CTRL_CE_CQ_ECE			= 3 << 2,
	SPDK_MLX5_WQE_CTRL_CE_CQ_NO_FLUSH_ERROR		= 1 << 2,
	SPDK_MLX5_WQE_CTRL_CE_CQ_UPDATE			= MLX5_WQE_CTRL_CQ_UPDATE,
	SPDK_MLX5_WQE_CTRL_CE_MASK			= 3 << 2,
	SPDK_MLX5_WQE_CTRL_SOLICITED			= MLX5_WQE_CTRL_SOLICITED,
	SPDK_MLX5_WQE_CTRL_FENCE			= MLX5_WQE_CTRL_FENCE,
	SPDK_MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE	= MLX5_WQE_CTRL_INITIATOR_SMALL_FENCE,
	SPDK_MLX5_WQE_CTRL_STRONG_ORDERING		= 3 << 5,
};

/**
 *
 * @param qp
 * @param klm values in BE format
 * @param klm_count
 * @param dstaddr
 * @param rkey
 * @param wrid
 * param flags MLX5_WQE_CTRL_CQ_UPDATE to have a signaled completion or 0
 * @return
 */
int spdk_mlx5_qp_rdma_write(struct spdk_mlx5_qp *qp, struct mlx5_wqe_data_seg *klm,
			    uint32_t klm_count, uint64_t dstaddr, uint32_t rkey,
			    uint64_t wrid, uint32_t flags);

int spdk_mlx5_qp_rdma_read(struct spdk_mlx5_qp *qp, struct mlx5_wqe_data_seg *klm,
			   uint32_t klm_count, uint64_t dstaddr, uint32_t rkey,
			   uint64_t wrid, uint32_t flags);

int spdk_mlx5_umr_configure_crypto(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
				   struct spdk_mlx5_umr_crypto_attr *crypto_attr, uint64_t wr_id, uint32_t flags);

int spdk_mlx5_umr_configure(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
			    uint64_t wr_id, uint32_t flags);

enum spdk_mlx5_umr_sig_domain {
	SPDK_MLX5_UMR_SIG_DOMAIN_MEMORY,
	SPDK_MLX5_UMR_SIG_DOMAIN_WIRE
};

struct spdk_mlx5_umr_sig_attr {
	uint32_t seed;
	uint32_t psv_index;
	enum spdk_mlx5_umr_sig_domain domain;
	unsigned sigerr_count;
	uint32_t raw_data_size;
	bool init;
	bool check_gen;
};

int spdk_mlx5_umr_configure_sig(struct spdk_mlx5_qp *qp,
				struct spdk_mlx5_umr_attr *umr_attr,
				struct spdk_mlx5_umr_sig_attr *sig_attr, uint64_t wr_id, uint32_t flags);

int spdk_mlx5_umr_configure_sig_crypto(struct spdk_mlx5_qp *qp, struct spdk_mlx5_umr_attr *umr_attr,
				       struct spdk_mlx5_umr_sig_attr *sig_attr,
				       struct spdk_mlx5_umr_crypto_attr *crypto_attr,
				       uint64_t wr_id, uint32_t flags);

struct mlx5_devx_mkey_attr {
	uint64_t addr;
	uint64_t size;
	uint32_t log_entity_size;
	uint32_t relaxed_ordering_write: 1;
	uint32_t relaxed_ordering_read: 1;
	struct ibv_sge *sg;
	uint32_t sg_count;
	/* Size of bsf in octowords. If 0 then bsf is disabled */
	uint32_t bsf_octowords;
	bool crypto_en;
};

struct spdk_mlx5_relaxed_ordering_caps {
	bool relaxed_ordering_write_pci_enabled;
	bool relaxed_ordering_write;
	bool relaxed_ordering_read;
	bool relaxed_ordering_write_umr;
	bool relaxed_ordering_read_umr;
};

struct spdk_mlx5_crypto_caps {
	/* crypto supported or not */
	bool crypto;
	bool wrapped_crypto_operational;
	bool wrapped_crypto_going_to_commissioning;
	bool wrapped_import_method_aes_xts;
	bool single_block_le_tweak;
	bool multi_block_be_tweak;
	bool multi_block_le_tweak;
	bool tweak_inc_64;
	bool large_mtu_tweak;
	bool crc32c;
};

int spdk_mlx5_query_crypto_caps(struct ibv_context *context, struct spdk_mlx5_crypto_caps *caps);

/**
 * spdk_mlx5_query_relaxed_ordering_caps() - Query for Relaxed-Ordering
 *				       capabilities.
 * \context: ibv_context to query.
 * \caps: relaxed-ordering capabilities (output)
 *
 * Relaxed Ordering is a feature that improves performance by disabling the
 * strict order imposed on PCIe writes/reads. Applications that can handle
 * this lack of strict ordering can benefit from it and improve performance.
 *
 * The function queries for the below capabilities:
 * - relaxed_ordering_write_pci_enabled: relaxed_ordering_write is supported by
 *     the device and also enabled in PCI.
 * - relaxed_ordering_write: relaxed_ordering_write is supported by the device
 *     and can be set in Mkey Context when creating Mkey.
 * - relaxed_ordering_read: relaxed_ordering_read can be set in Mkey Context
 *     when creating Mkey.
 * - relaxed_ordering_write_umr: relaxed_ordering_write can be modified by UMR.
 * - relaxed_ordering_read_umr: relaxed_ordering_read can be modified by UMR.
 *
 * \return 0 or -errno on error
 */
int spdk_mlx5_query_relaxed_ordering_caps(struct ibv_context *context,
		struct spdk_mlx5_relaxed_ordering_caps *caps);

struct spdk_mlx5_indirect_mkey *spdk_mlx5_create_indirect_mkey(struct ibv_pd *pd,
		struct mlx5_devx_mkey_attr *attr);
int spdk_mlx5_destroy_indirect_mkey(struct spdk_mlx5_indirect_mkey *mkey);

struct spdk_mlx5_psv {
	struct mlx5dv_devx_obj *devx_obj;
	uint32_t index;
};

struct spdk_mlx5_psv *spdk_mlx5_create_psv(struct ibv_pd *pd);
int spdk_mlx5_destroy_psv(struct spdk_mlx5_psv *psv);
int spdk_mlx5_set_psv(struct spdk_mlx5_qp *dma_qp, uint32_t psv_index, uint32_t crc_seed,
		      uint64_t wr_id,
		      uint32_t flags);

enum spdk_mlx5_mkey_pool_flags {
	SPDK_MLX5_MKEY_POOL_FLAG_CRYPTO = 1 << 0,
	SPDK_MLX5_MKEY_POOL_FLAG_SIGNATURE = 1 << 1,
	/* Max number of pools of different types */
	SPDK_MLX5_MKEY_POOL_FLAG_COUNT = 3,
};

struct spdk_mlx5_mkey_pool_param {
	uint32_t mkey_count;
	uint32_t cache_per_thread;
	/* enum spdk_mlx5_mkey_pool_flags */
	uint32_t flags;
};

/**
 * Creates a pool of memory keys for each given \b PD. If crypto_en is set then a device associated with PD must support
 * crypto operations. Refer to \ref spdk_mlx5_query_crypto_caps.
 *
 * Can be called several times for different PDs. Has no effect if a pool for \b PD already with the same \b flags
 * already exists
 *
 * \param params Parameter of the memory pool, common for every PD-specific pool
 * \param pds Array of PDs. If NULL then all devices in the system will be used and PD will be obtained with \ref spdk_rdma_utils_get_pd
 * \param num_pds Size of the PDs array
 * \return 0 on success, errno on failure
 */
int spdk_mlx5_mkey_pools_init(struct spdk_mlx5_mkey_pool_param *params, struct ibv_pd **pds,
			      uint32_t num_pds);

/**
 * Destroy mkey pools with the given \b flags and \b pds which were created by \ref spdk_mlx5_mkey_pools_init.
 * All pool channels must be released
 *
 * \param pds Array of PDs. If NULL then all devices in the system will be used and PD will be obtained with \ref spdk_rdma_utils_get_pd
 * \param num_pds Size of the PDs array
 * \param flags Specifies type of the pool to delete. Has effect only when \b pds are not NULL
 * \return 0 on success, negated errno on failure
 */
int spdk_mlx5_mkey_pools_destroy(struct ibv_pd **pds, uint32_t num_pds, uint32_t flags);

/**
 * Get a channel to access mkey pool specified by PD
 *
 * \param pd PD to get a mkey pool channel for
 * \param flags Required mkey pool flags, see \ref enum spdk_mlx5_mkey_pool_flags
 * \return Opaque pointer to a channel on success or NULL on error
 */
void *spdk_mlx5_mkey_pool_get_channel(struct ibv_pd *pd, uint32_t flags);

/**
 * Release mkey channel
 * \param ch
 */
void spdk_mlx5_mkey_pool_put_channel(void *ch);

/**
 * Get several mekys from the pool
 * \param ch mkey pool channel
 * \param mkeys array of mkey pointers to be filled by this function
 * \param mkeys_count number of mkeys to get from the pool
 * \return 0 on success, errno on failure
 */
int spdk_mlx5_mkey_pool_get_bulk(void *ch, struct spdk_mlx5_mkey_pool_obj **mkeys,
				 uint32_t mkeys_count);

/**
 * Return mkeys to the pool
 *
 * \param ch mkey pool channel
 * \param mkeys array of mkey pointers to be returned to the pool
 * \param mkeys_count number of mkeys to get from the pool
 */
void spdk_mlx5_mkey_pool_put_bulk(void *ch, struct spdk_mlx5_mkey_pool_obj **mkeys,
				  uint32_t mkeys_count);

/**
 * Find mkey object by mkey ID
 *
 * \param ch mkey pool channel
 * \param mkey_id mkey ID
 * \return Pointer to mkey object or NULL
 */
struct spdk_mlx5_mkey_pool_obj *spdk_mlx5_mkey_pool_find_mkey_by_id(void *ch, uint32_t mkey_id);

#endif /* SPDK_MLX5_H */
