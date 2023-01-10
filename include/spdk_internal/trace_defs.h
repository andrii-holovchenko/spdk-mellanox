/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef SPDK_INTERNAL_TRACE_DEFS
#define SPDK_INTERNAL_TRACE_DEFS

/* Owner definitions */
#define OWNER_ISCSI_CONN	0x1
#define OWNER_BDEV		0x2
#define OWNER_NVME_PCIE_QP	0x3
#define OWNER_NVME_TCP_QP	0x4
#define OWNER_SCSI_DEV		0x10
#define OWNER_FTL		0x20
#define OWNER_NVMF_TCP		0x30
#define OWNER_NVME_NVDA_TCP_QP	0xF0

/* Object definitions */
#define OBJECT_ISCSI_PDU	0x1
#define OBJECT_BDEV_IO		0x2
#define OBJECT_NVME_PCIE_REQ	0x3
#define OBJECT_NVME_TCP_REQ	0x4
#define OBJECT_BDEV_NVME_IO	0x5
#define OBJECT_SCSI_TASK	0x10
#define OBJECT_NVMF_RDMA_IO	0x40
#define OBJECT_NVMF_TCP_IO	0x80
#define OBJECT_NVMF_FC_IO	0xA0
#define OBJECT_NVME_NVDA_TCP_REQ	0xF0

/* Trace group definitions */
#define TRACE_GROUP_ISCSI	0x1
#define TRACE_GROUP_SCSI	0x2
#define TRACE_GROUP_BDEV	0x3
#define TRACE_GROUP_NVMF_RDMA	0x4
#define TRACE_GROUP_NVMF_TCP	0x5
#define TRACE_GROUP_FTL		0x6
#define TRACE_GROUP_BLOBFS	0x7
#define TRACE_GROUP_NVMF_FC	0x8
#define TRACE_GROUP_ACCEL_DSA	0x9
#define TRACE_GROUP_THREAD	0xA
#define TRACE_GROUP_NVME_PCIE	0xB
#define TRACE_GROUP_ACCEL_IAA	0xC
#define TRACE_GROUP_NVME_TCP	0xD
#define TRACE_GROUP_BDEV_NVME	0xE
#define TRACE_GROUP_NVME_NVDA_TCP	0xF

/* Bdev tracepoint definitions */
#define TRACE_BDEV_IO_START		SPDK_TPOINT_ID(TRACE_GROUP_BDEV, 0x0)
#define TRACE_BDEV_IO_DONE		SPDK_TPOINT_ID(TRACE_GROUP_BDEV, 0x1)
#define TRACE_BDEV_IOCH_CREATE		SPDK_TPOINT_ID(TRACE_GROUP_BDEV, 0x2)
#define TRACE_BDEV_IOCH_DESTROY		SPDK_TPOINT_ID(TRACE_GROUP_BDEV, 0x3)

/* NVMe-of TCP tracepoint  definitions */
#define TRACE_TCP_REQUEST_STATE_NEW				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x00)
#define TRACE_TCP_REQUEST_STATE_NEED_BUFFER			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x01)
#define TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_START		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x02)
#define TRACE_TCP_REQUEST_STATE_ZCOPY_START_COMPLETED		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x03)
#define TRACE_TCP_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x04)
#define TRACE_TCP_REQUEST_STATE_READY_TO_EXECUTE		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x05)
#define TRACE_TCP_REQUEST_STATE_EXECUTING			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x06)
#define TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_COMMIT		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x07)
#define TRACE_TCP_REQUEST_STATE_EXECUTED			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x08)
#define TRACE_TCP_REQUEST_STATE_READY_TO_COMPLETE		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x09)
#define TRACE_TCP_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x0a)
#define TRACE_TCP_REQUEST_STATE_AWAIT_ZCOPY_RELEASE		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x0b)
#define TRACE_TCP_REQUEST_STATE_COMPLETED			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x0c)
#define TRACE_TCP_FLUSH_WRITEBUF_START				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x0d)
#define TRACE_TCP_FLUSH_WRITEBUF_DONE				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x0e)
#define TRACE_TCP_READ_FROM_SOCKET_DONE				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x0f)
#define TRACE_TCP_REQUEST_STATE_AWAIT_R2T_ACK			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x10)
#define TRACE_TCP_QP_CREATE					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x11)
#define TRACE_TCP_QP_SOCK_INIT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x12)
#define TRACE_TCP_QP_STATE_CHANGE				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x13)
#define TRACE_TCP_QP_DISCONNECT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x14)
#define TRACE_TCP_QP_DESTROY					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x15)
#define TRACE_TCP_QP_ABORT_REQ					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x16)
#define TRACE_TCP_QP_RCV_STATE_CHANGE				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_TCP, 0x17)

/* NVMe-of RDMA tracepoint definitions */
#define TRACE_RDMA_REQUEST_STATE_NEW					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x0)
#define TRACE_RDMA_REQUEST_STATE_NEED_BUFFER				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x1)
#define TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x2)
#define TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x3)
#define TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x4)
#define TRACE_RDMA_REQUEST_STATE_EXECUTING				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x5)
#define TRACE_RDMA_REQUEST_STATE_EXECUTED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x6)
#define TRACE_RDMA_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x7)
#define TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x8)
#define TRACE_RDMA_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x9)
#define TRACE_RDMA_REQUEST_STATE_COMPLETING				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xA)
#define TRACE_RDMA_REQUEST_STATE_COMPLETED				SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xB)
#define TRACE_RDMA_QP_CREATE						SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xC)
#define TRACE_RDMA_IBV_ASYNC_EVENT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xD)
#define TRACE_RDMA_CM_ASYNC_EVENT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xE)
#define TRACE_RDMA_QP_STATE_CHANGE					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xF)
#define TRACE_RDMA_QP_DISCONNECT					SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x10)
#define TRACE_RDMA_QP_DESTROY						SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x11)
#define TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE_PENDING		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x12)

/* Thread tracepoint definitions */
#define TRACE_THREAD_IOCH_GET		SPDK_TPOINT_ID(TRACE_GROUP_THREAD, 0x0)
#define TRACE_THREAD_IOCH_PUT		SPDK_TPOINT_ID(TRACE_GROUP_THREAD, 0x1)

/* Blobfs tracepoint definitions */
#define TRACE_BLOBFS_XATTR_START	SPDK_TPOINT_ID(TRACE_GROUP_BLOBFS, 0x0)
#define TRACE_BLOBFS_XATTR_END		SPDK_TPOINT_ID(TRACE_GROUP_BLOBFS, 0x1)
#define TRACE_BLOBFS_OPEN		SPDK_TPOINT_ID(TRACE_GROUP_BLOBFS, 0x2)
#define TRACE_BLOBFS_CLOSE		SPDK_TPOINT_ID(TRACE_GROUP_BLOBFS, 0x3)
#define TRACE_BLOBFS_DELETE_START	SPDK_TPOINT_ID(TRACE_GROUP_BLOBFS, 0x4)
#define TRACE_BLOBFS_DELETE_DONE	SPDK_TPOINT_ID(TRACE_GROUP_BLOBFS, 0x5)

/* NVMe-oF FC tracepoint definitions */
#define TRACE_FC_REQ_INIT		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x01)
#define TRACE_FC_REQ_READ_BDEV		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x02)
#define TRACE_FC_REQ_READ_XFER		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x03)
#define TRACE_FC_REQ_READ_RSP		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x04)
#define TRACE_FC_REQ_WRITE_BUFFS	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x05)
#define TRACE_FC_REQ_WRITE_XFER		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x06)
#define TRACE_FC_REQ_WRITE_BDEV		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x07)
#define TRACE_FC_REQ_WRITE_RSP		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x08)
#define TRACE_FC_REQ_NONE_BDEV		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x09)
#define TRACE_FC_REQ_NONE_RSP		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x0A)
#define TRACE_FC_REQ_SUCCESS		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x0B)
#define TRACE_FC_REQ_FAILED		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x0C)
#define TRACE_FC_REQ_ABORTED		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x0D)
#define TRACE_FC_REQ_BDEV_ABORTED	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x0E)
#define TRACE_FC_REQ_PENDING		SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x0F)
#define TRACE_FC_REQ_FUSED_WAITING	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_FC, 0x10)

/* Iscsi conn tracepoint definitions */
#define TRACE_ISCSI_READ_FROM_SOCKET_DONE	SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x0)
#define TRACE_ISCSI_FLUSH_WRITEBUF_START	SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x1)
#define TRACE_ISCSI_FLUSH_WRITEBUF_DONE		SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x2)
#define TRACE_ISCSI_READ_PDU			SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x3)
#define TRACE_ISCSI_TASK_DONE			SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x4)
#define TRACE_ISCSI_TASK_QUEUE			SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x5)
#define TRACE_ISCSI_TASK_EXECUTED		SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x6)
#define TRACE_ISCSI_PDU_COMPLETED		SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x7)

/* Scsi tracepoint definitions */
#define TRACE_SCSI_TASK_DONE	SPDK_TPOINT_ID(TRACE_GROUP_SCSI, 0x0)
#define TRACE_SCSI_TASK_START	SPDK_TPOINT_ID(TRACE_GROUP_SCSI, 0x1)

/* NVMe PCIe tracepoint definitions */
#define TRACE_NVME_PCIE_SUBMIT		SPDK_TPOINT_ID(TRACE_GROUP_NVME_PCIE, 0x0)
#define TRACE_NVME_PCIE_COMPLETE	SPDK_TPOINT_ID(TRACE_GROUP_NVME_PCIE, 0x1)

/* idxd trace definitions */
#define TRACE_ACCEL_DSA_OP_SUBMIT	SPDK_TPOINT_ID(TRACE_GROUP_ACCEL_DSA, 0x0)
#define TRACE_ACCEL_DSA_OP_COMPLETE	SPDK_TPOINT_ID(TRACE_GROUP_ACCEL_DSA, 0x1)
#define TRACE_ACCEL_IAA_OP_SUBMIT	SPDK_TPOINT_ID(TRACE_GROUP_ACCEL_IAA, 0x0)
#define TRACE_ACCEL_IAA_OP_COMPLETE	SPDK_TPOINT_ID(TRACE_GROUP_ACCEL_IAA, 0x1)

/* NVMe TCP tracepoint definitions */
#define TRACE_NVME_TCP_SUBMIT		SPDK_TPOINT_ID(TRACE_GROUP_NVME_TCP, 0x0)
#define TRACE_NVME_TCP_COMPLETE		SPDK_TPOINT_ID(TRACE_GROUP_NVME_TCP, 0x1)

/* Bdev nvme tracepoint definitions */
#define TRACE_BDEV_NVME_IO_START	SPDK_TPOINT_ID(TRACE_GROUP_BDEV_NVME, 0x0)
#define TRACE_BDEV_NVME_IO_DONE		SPDK_TPOINT_ID(TRACE_GROUP_BDEV_NVME, 0x1)

/* NVMe NVDA_TCP tracepoint definitions */
#define TRACE_NVME_NVDA_TCP_SUBMIT		SPDK_TPOINT_ID(TRACE_GROUP_NVME_NVDA_TCP, 0x0)
#define TRACE_NVME_NVDA_TCP_COMPLETE		SPDK_TPOINT_ID(TRACE_GROUP_NVME_NVDA_TCP, 0x1)

#endif /* SPDK_INTERNAL_TRACE_DEFS */
