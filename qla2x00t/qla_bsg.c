/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2003-2011 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */
#include "qla_def.h"

#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>

/* BSG support for ELS/CT pass through */
void
qla2x00_bsg_job_done(void *data, void *ptr, int res)
{
	srb_t *sp = (srb_t*)ptr;
	struct scsi_qla_host *vha = (scsi_qla_host_t *)data;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct fc_bsg_job *bsg_job = sp->u.bsg_job;
#else
	struct bsg_job *bsg_job = sp->u.bsg_job;
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->reply->result = res;
	bsg_job->job_done(bsg_job);
#else
	bsg_reply->result = res;
	bsg_job_done(bsg_job, bsg_reply->result,
		       bsg_reply->reply_payload_rcv_len);
#endif
	sp->free(vha, sp);
}

void
qla2x00_bsg_sp_free(void *data, void *ptr)
{
	srb_t *sp = (srb_t*)ptr;
	struct scsi_qla_host *vha = (scsi_qla_host_t *)data;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct fc_bsg_job *bsg_job = sp->u.bsg_job;
#else
	struct bsg_job *bsg_job = sp->u.bsg_job;
#endif
	struct qla_hw_data *ha = vha->hw;

	dma_unmap_sg(&ha->pdev->dev, bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, DMA_TO_DEVICE);

	dma_unmap_sg(&ha->pdev->dev, bsg_job->reply_payload.sg_list,
	    bsg_job->reply_payload.sg_cnt, DMA_FROM_DEVICE);

	if (sp->type == SRB_CT_CMD ||
	    sp->type == SRB_ELS_CMD_HST)
		kfree(sp->fcport);
	mempool_free(sp, vha->hw->srb_mempool);
}

int
qla24xx_fcp_prio_cfg_valid(scsi_qla_host_t *vha,
	struct qla_fcp_prio_cfg *pri_cfg, uint8_t flag)
{
	int i, ret, num_valid;
	uint8_t *bcode;
	struct qla_fcp_prio_entry *pri_entry;
	uint32_t *bcode_val_ptr, bcode_val;

	ret = 1;
	num_valid = 0;
	bcode = (uint8_t *)pri_cfg;
	bcode_val_ptr = (uint32_t *)pri_cfg;
	bcode_val = (uint32_t)(*bcode_val_ptr);

	if (bcode_val == 0xFFFFFFFF) {
		/* No FCP Priority config data in flash */
		ql_dbg(ql_dbg_user, vha, 0x7051,
		    "No FCP Priority config data.\n");
		return 0;
	}

	if (bcode[0] != 'H' || bcode[1] != 'Q' || bcode[2] != 'O' ||
			bcode[3] != 'S') {
		/* Invalid FCP priority data header*/
		ql_dbg(ql_dbg_user, vha, 0x7052,
		    "Invalid FCP Priority data header. bcode=0x%x.\n",
		    bcode_val);
		return 0;
	}
	if (flag != 1)
		return ret;

	pri_entry = &pri_cfg->entry[0];
	for (i = 0; i < pri_cfg->num_entries; i++) {
		if (pri_entry->flags & FCP_PRIO_ENTRY_TAG_VALID)
			num_valid++;
		pri_entry++;
	}

	if (num_valid == 0) {
		/* No valid FCP priority data entries */
		ql_dbg(ql_dbg_user, vha, 0x7053,
		    "No valid FCP Priority data entries.\n");
		ret = 0;
	} else {
		/* FCP priority data is valid */
		ql_dbg(ql_dbg_user, vha, 0x7054,
		    "Valid FCP priority data. num entries = %d.\n",
		    num_valid);
	}

	return ret;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla24xx_proc_fcp_prio_cfg_cmd(struct fc_bsg_job *bsg_job)
#else
qla24xx_proc_fcp_prio_cfg_cmd(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
	struct fc_bsg_request *bsg_request = bsg_job->request;
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	struct qla_hw_data *ha = vha->hw;
	int ret = 0;
	uint32_t len;
	uint32_t oper;

	if (!(IS_QLA24XX_TYPE(ha) || IS_QLA25XX(ha) || IS_QLA82XX(ha))) {
		ret = -EINVAL;
		goto exit_fcp_prio_cfg;
	}

	/* Get the sub command */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	oper = bsg_job->request->rqst_data.h_vendor.vendor_cmd[1];
#else
	oper = bsg_request->rqst_data.h_vendor.vendor_cmd[1];
#endif

	/* Only set config is allowed if config memory is not allocated */
	if (!ha->fcp_prio_cfg && (oper != QLFC_FCP_PRIO_SET_CONFIG)) {
		ret = -EINVAL;
		goto exit_fcp_prio_cfg;
	}
	switch (oper) {
	case QLFC_FCP_PRIO_DISABLE:
		if (ha->flags.fcp_prio_enabled) {
			ha->flags.fcp_prio_enabled = 0;
			ha->fcp_prio_cfg->attributes &=
				~FCP_PRIO_ATTR_ENABLE;
			qla24xx_update_all_fcp_prio(vha);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
			bsg_job->reply->result = DID_OK;
#else
			bsg_reply->result = DID_OK;
#endif
		} else {
			ret = -EINVAL;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
			bsg_job->reply->result = (DID_ERROR << 16);
#else
			bsg_reply->result = (DID_ERROR << 16);
#endif
			goto exit_fcp_prio_cfg;
		}
		break;

	case QLFC_FCP_PRIO_ENABLE:
		if (!ha->flags.fcp_prio_enabled) {
			if (ha->fcp_prio_cfg) {
				ha->flags.fcp_prio_enabled = 1;
				ha->fcp_prio_cfg->attributes |=
				    FCP_PRIO_ATTR_ENABLE;
				qla24xx_update_all_fcp_prio(vha);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
				bsg_job->reply->result = DID_OK;
#else
				bsg_reply->result = DID_OK;
#endif
			} else {
				ret = -EINVAL;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
				bsg_job->reply->result = (DID_ERROR << 16);
#else
				bsg_reply->result = (DID_ERROR << 16);
#endif
				goto exit_fcp_prio_cfg;
			}
		}
		break;

	case QLFC_FCP_PRIO_GET_CONFIG:
		len = bsg_job->reply_payload.payload_len;
		if (!len || len > FCP_PRIO_CFG_SIZE) {
			ret = -EINVAL;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
			bsg_job->reply->result = (DID_ERROR << 16);
#else
			bsg_reply->result = (DID_ERROR << 16);
#endif
			goto exit_fcp_prio_cfg;
		}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = DID_OK;
		bsg_job->reply->reply_payload_rcv_len =
#else
		bsg_reply->result = DID_OK;
		bsg_reply->reply_payload_rcv_len =
#endif
			sg_copy_from_buffer(
			bsg_job->reply_payload.sg_list,
			bsg_job->reply_payload.sg_cnt, ha->fcp_prio_cfg,
			len);

		break;

	case QLFC_FCP_PRIO_SET_CONFIG:
		len = bsg_job->request_payload.payload_len;
		if (!len || len > FCP_PRIO_CFG_SIZE) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
			bsg_job->reply->result = (DID_ERROR << 16);
#else
			bsg_reply->result = (DID_ERROR << 16);
#endif
			ret = -EINVAL;
			goto exit_fcp_prio_cfg;
		}

		if (!ha->fcp_prio_cfg) {
			ha->fcp_prio_cfg = vmalloc(FCP_PRIO_CFG_SIZE);
			if (!ha->fcp_prio_cfg) {
				ql_log(ql_log_warn, vha, 0x7050,
				    "Unable to allocate memory for fcp prio "
				    "config data (%x).\n", FCP_PRIO_CFG_SIZE);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
				bsg_job->reply->result = (DID_ERROR << 16);
#else
				bsg_reply->result = (DID_ERROR << 16);
#endif
				ret = -ENOMEM;
				goto exit_fcp_prio_cfg;
			}
		}

		memset(ha->fcp_prio_cfg, 0, FCP_PRIO_CFG_SIZE);
		sg_copy_to_buffer(bsg_job->request_payload.sg_list,
		bsg_job->request_payload.sg_cnt, ha->fcp_prio_cfg,
			FCP_PRIO_CFG_SIZE);

		/* validate fcp priority data */

		if (!qla24xx_fcp_prio_cfg_valid(vha,
		    (struct qla_fcp_prio_cfg *) ha->fcp_prio_cfg, 1)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
			bsg_job->reply->result = (DID_ERROR << 16);
#else
			bsg_reply->result = (DID_ERROR << 16);
#endif
			ret = -EINVAL;
			/* If buffer was invalidatic int
			 * fcp_prio_cfg is of no use
			 */
			vfree(ha->fcp_prio_cfg);
			ha->fcp_prio_cfg = NULL;
			goto exit_fcp_prio_cfg;
		}

		ha->flags.fcp_prio_enabled = 0;
		if (ha->fcp_prio_cfg->attributes & FCP_PRIO_ATTR_ENABLE)
			ha->flags.fcp_prio_enabled = 1;
		qla24xx_update_all_fcp_prio(vha);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = DID_OK;
#else
		bsg_reply->result = DID_OK;
#endif
		break;
	default:
		ret = -EINVAL;
		break;
	}
exit_fcp_prio_cfg:
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->job_done(bsg_job);
#else
	if (!ret)
		bsg_job_done(bsg_job, bsg_reply->result,
			bsg_reply->reply_payload_rcv_len);
#endif
	return ret;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla2x00_process_els(struct fc_bsg_job *bsg_job)
#else
qla2x00_process_els(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	struct fc_bsg_request *bsg_request = bsg_job->request;
#endif
	struct fc_rport *rport;
	fc_port_t *fcport = NULL;
	struct Scsi_Host *host;
	scsi_qla_host_t *vha;
	struct qla_hw_data *ha;
	srb_t *sp;
	const char *type;
	int req_sg_cnt, rsp_sg_cnt;
	int rval =  (DRIVER_ERROR << 16);
	uint16_t nextlid = 0;

#ifdef __COVERITY__
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	BUG_ON(bsg_job->request->msgcode != FC_BSG_RPT_ELS &&
	       bsg_job->request->msgcode != FC_BSG_HST_ELS_NOLOGIN);
#else
	BUG_ON(bsg_request->msgcode != FC_BSG_RPT_ELS &&
	       bsg_request->msgcode != FC_BSG_HST_ELS_NOLOGIN);
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	if (bsg_job->request->msgcode == FC_BSG_RPT_ELS) {
		rport = bsg_job->rport;
#else
	if (bsg_request->msgcode == FC_BSG_RPT_ELS) {
		rport = fc_bsg_to_rport(bsg_job);;
#endif
		fcport = *(fc_port_t **) rport->dd_data;
		host = rport_to_shost(rport);
		vha = shost_priv(host);
		ha = vha->hw;
		type = "FC_BSG_RPT_ELS";
	} else {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		host = bsg_job->shost;
#else
		host = fc_bsg_to_shost(bsg_job);
#endif
		vha = shost_priv(host);
		ha = vha->hw;
		type = "FC_BSG_HST_ELS_NOLOGIN";
	}

	/* pass through is supported only for ISP 4Gb or higher */
	if (!IS_FWI2_CAPABLE(ha)) {
		ql_dbg(ql_dbg_user, vha, 0x7001,
		    "ELS passthru not supported for ISP23xx based adapters.\n");
		rval = -EPERM;
		goto done;
	}

	/*  Multiple SG's are not supported for ELS requests */
	if (bsg_job->request_payload.sg_cnt > 1 ||
		bsg_job->reply_payload.sg_cnt > 1) {
		ql_dbg(ql_dbg_user, vha, 0x7002,
		    "Multiple SG's are not suppored for ELS requests, "
		    "request_sg_cnt=%x reply_sg_cnt=%x.\n",
		    bsg_job->request_payload.sg_cnt,
		    bsg_job->reply_payload.sg_cnt);
		rval = -EPERM;
		goto done;
	}

	/* ELS request for rport */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	if (bsg_job->request->msgcode == FC_BSG_RPT_ELS) {
#else
	if (bsg_request->msgcode == FC_BSG_RPT_ELS) {
#endif
		/* make sure the rport is logged in,
		 * if not perform fabric login
		 */
		if (qla2x00_fabric_login(vha, fcport, &nextlid)) {
			ql_dbg(ql_dbg_user, vha, 0x7003,
			    "Failed to login port %06X for ELS passthru.\n",
			    fcport->d_id.b24);
			rval = -EIO;
			goto done;
		}
	} else {
		/* Allocate a dummy fcport structure, since functions
		 * preparing the IOCB and mailbox command retrieves port
		 * specific information from fcport structure. For Host based
		 * ELS commands there will be no fcport structure allocated
		 */
		fcport = qla2x00_alloc_fcport(vha, GFP_KERNEL);
		if (!fcport) {
			rval = -ENOMEM;
			goto done;
		}

		/* Initialize all required  fields of fcport */
		fcport->vha = vha;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		fcport->d_id.b.al_pa =
			bsg_job->request->rqst_data.h_els.port_id[0];
		fcport->d_id.b.area =
			bsg_job->request->rqst_data.h_els.port_id[1];
		fcport->d_id.b.domain =
			bsg_job->request->rqst_data.h_els.port_id[2];
#else
		fcport->d_id.b.al_pa =
			bsg_request->rqst_data.h_els.port_id[0];
		fcport->d_id.b.area =
			bsg_request->rqst_data.h_els.port_id[1];
		fcport->d_id.b.domain =
			bsg_request->rqst_data.h_els.port_id[2];
#endif
		fcport->loop_id =
			(fcport->d_id.b.al_pa == 0xFD) ?
			NPH_FABRIC_CONTROLLER : NPH_F_PORT;
	}

	if (!vha->flags.online) {
		ql_log(ql_log_warn, vha, 0x7005, "Host not online.\n");
		rval = -EIO;
		goto done_free_fcport;
	}

	req_sg_cnt =
		dma_map_sg(&ha->pdev->dev, bsg_job->request_payload.sg_list,
		bsg_job->request_payload.sg_cnt, DMA_TO_DEVICE);
	if (!req_sg_cnt) {
		rval = -ENOMEM;
		goto done_free_fcport;
	}

	rsp_sg_cnt = dma_map_sg(&ha->pdev->dev, bsg_job->reply_payload.sg_list,
	    bsg_job->reply_payload.sg_cnt, DMA_FROM_DEVICE);
	if (!rsp_sg_cnt) {
		rval = -ENOMEM;
		goto done_free_fcport;
	}

	if ((req_sg_cnt !=  bsg_job->request_payload.sg_cnt) ||
	    (rsp_sg_cnt != bsg_job->reply_payload.sg_cnt)) {
		ql_log(ql_log_warn, vha, 0x7008,
		    "dma mapping resulted in different sg counts, "
		    "request_sg_cnt: %x dma_request_sg_cnt:%x reply_sg_cnt:%x "
		    "dma_reply_sg_cnt:%x.\n", bsg_job->request_payload.sg_cnt,
		    req_sg_cnt, bsg_job->reply_payload.sg_cnt, rsp_sg_cnt);
		rval = -EAGAIN;
		goto done_unmap_sg;
	}

	/* Alloc SRB structure */
	sp = qla2x00_get_sp(vha, fcport, GFP_KERNEL);
	if (!sp) {
		rval = -ENOMEM;
		goto done_unmap_sg;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	sp->type =
		(bsg_job->request->msgcode == FC_BSG_RPT_ELS ?
		SRB_ELS_CMD_RPT : SRB_ELS_CMD_HST);
	sp->name =
		(bsg_job->request->msgcode == FC_BSG_RPT_ELS ?
		"bsg_els_rpt" : "bsg_els_hst");
#else
	sp->type =
		(bsg_request->msgcode == FC_BSG_RPT_ELS ?
		SRB_ELS_CMD_RPT : SRB_ELS_CMD_HST);
	sp->name =
		(bsg_request->msgcode == FC_BSG_RPT_ELS ?
		"bsg_els_rpt" : "bsg_els_hst");
#endif
	sp->u.bsg_job = bsg_job;
	sp->free = qla2x00_bsg_sp_free;
	sp->done = qla2x00_bsg_job_done;

	ql_dbg(ql_dbg_user, vha, 0x700a,
	    "bsg rqst type: %s els type: %x - loop-id=%x "
	    "portid=%-2x%02x%02x.\n", type,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	    bsg_job->request->rqst_data.h_els.command_code, fcport->loop_id,
#else
	    bsg_request->rqst_data.h_els.command_code, fcport->loop_id,
#endif
	    fcport->d_id.b.domain, fcport->d_id.b.area, fcport->d_id.b.al_pa);

	rval = qla2x00_start_sp(sp);
	if (rval != QLA_SUCCESS) {
		ql_log(ql_log_warn, vha, 0x700e,
		    "qla2x00_start_sp failed = %d\n", rval);
		mempool_free(sp, ha->srb_mempool);
		rval = -EIO;
		goto done_unmap_sg;
	}
	return rval;

done_unmap_sg:
	dma_unmap_sg(&ha->pdev->dev, bsg_job->request_payload.sg_list,
		bsg_job->request_payload.sg_cnt, DMA_TO_DEVICE);
	dma_unmap_sg(&ha->pdev->dev, bsg_job->reply_payload.sg_list,
		bsg_job->reply_payload.sg_cnt, DMA_FROM_DEVICE);
	goto done_free_fcport;

done_free_fcport:
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	if (bsg_job->request->msgcode == FC_BSG_HST_ELS_NOLOGIN)
#else
	if (bsg_request->msgcode == FC_BSG_HST_ELS_NOLOGIN)
#endif
		kfree(fcport);
done:
	return rval;
}

inline uint16_t
qla24xx_calc_ct_iocbs(uint16_t dsds)
{
	uint16_t iocbs;

	iocbs = 1;
	if (dsds > 2) {
		iocbs += (dsds - 2) / 5;
		if ((dsds - 2) % 5)
			iocbs++;
	}
	return iocbs;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla2x00_process_ct(struct fc_bsg_job *bsg_job)
#else
qla2x00_process_ct(struct bsg_job *bsg_job)
#endif
{
	srb_t *sp;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct fc_bsg_request *bsg_request = bsg_job->request;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	struct qla_hw_data *ha = vha->hw;
	int rval = (DRIVER_ERROR << 16);
	int req_sg_cnt, rsp_sg_cnt;
	uint16_t loop_id;
	struct fc_port *fcport;
	char  *type = "FC_BSG_HST_CT";

	req_sg_cnt =
		dma_map_sg(&ha->pdev->dev, bsg_job->request_payload.sg_list,
		bsg_job->request_payload.sg_cnt, DMA_TO_DEVICE);
	if (!req_sg_cnt) {
		ql_log(ql_log_warn, vha, 0x700f,
		    "dma_map_sg return %d for request\n", req_sg_cnt);
		rval = -ENOMEM;
		goto done;
	}

	rsp_sg_cnt = dma_map_sg(&ha->pdev->dev, bsg_job->reply_payload.sg_list,
		bsg_job->reply_payload.sg_cnt, DMA_FROM_DEVICE);
	if (!rsp_sg_cnt) {
		ql_log(ql_log_warn, vha, 0x7010,
		    "dma_map_sg return %d for reply\n", rsp_sg_cnt);
		rval = -ENOMEM;
		goto done;
	}

	if ((req_sg_cnt !=  bsg_job->request_payload.sg_cnt) ||
	    (rsp_sg_cnt != bsg_job->reply_payload.sg_cnt)) {
		ql_log(ql_log_warn, vha, 0x7011,
		    "request_sg_cnt: %x dma_request_sg_cnt: %x reply_sg_cnt:%x "
		    "dma_reply_sg_cnt: %x\n", bsg_job->request_payload.sg_cnt,
		    req_sg_cnt, bsg_job->reply_payload.sg_cnt, rsp_sg_cnt);
		rval = -EAGAIN;
		goto done_unmap_sg;
	}

	if (!vha->flags.online) {
		ql_log(ql_log_warn, vha, 0x7012,
		    "Host is not online.\n");
		rval = -EIO;
		goto done_unmap_sg;
	}

	loop_id =
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		(bsg_job->request->rqst_data.h_ct.preamble_word1 & 0xFF000000)
#else
		(bsg_request->rqst_data.h_ct.preamble_word1 & 0xFF000000)
#endif
			>> 24;
	switch (loop_id) {
	case 0xFC:
		loop_id = cpu_to_le16(NPH_SNS);
		break;
	case 0xFA:
		loop_id = vha->mgmt_svr_loop_id;
		break;
	default:
		ql_dbg(ql_dbg_user, vha, 0x7013,
		    "Unknown loop id: %x.\n", loop_id);
		rval = -EINVAL;
		goto done_unmap_sg;
	}

	/* Allocate a dummy fcport structure, since functions preparing the
	 * IOCB and mailbox command retrieves port specific information
	 * from fcport structure. For Host based ELS commands there will be
	 * no fcport structure allocated
	 */
	fcport = qla2x00_alloc_fcport(vha, GFP_KERNEL);
	if (!fcport) {
		ql_log(ql_log_warn, vha, 0x7014,
		    "Failed to allocate fcport.\n");
		rval = -ENOMEM;
		goto done_unmap_sg;
	}

	/* Initialize all required  fields of fcport */
	fcport->vha = vha;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	fcport->d_id.b.al_pa = bsg_job->request->rqst_data.h_ct.port_id[0];
	fcport->d_id.b.area = bsg_job->request->rqst_data.h_ct.port_id[1];
	fcport->d_id.b.domain = bsg_job->request->rqst_data.h_ct.port_id[2];
#else
	fcport->d_id.b.al_pa = bsg_request->rqst_data.h_ct.port_id[0];
	fcport->d_id.b.area = bsg_request->rqst_data.h_ct.port_id[1];
	fcport->d_id.b.domain = bsg_request->rqst_data.h_ct.port_id[2];
#endif
	fcport->loop_id = loop_id;

	/* Alloc SRB structure */
	sp = qla2x00_get_sp(vha, fcport, GFP_KERNEL);
	if (!sp) {
		ql_log(ql_log_warn, vha, 0x7015,
		    "qla2x00_get_sp failed.\n");
		rval = -ENOMEM;
		goto done_free_fcport;
	}

	sp->type = SRB_CT_CMD;
	sp->name = "bsg_ct";
	sp->iocbs = qla24xx_calc_ct_iocbs(req_sg_cnt + rsp_sg_cnt);
	sp->u.bsg_job = bsg_job;
	sp->free = qla2x00_bsg_sp_free;
	sp->done = qla2x00_bsg_job_done;

	ql_dbg(ql_dbg_user, vha, 0x7016,
	    "bsg rqst type: %s else type: %x - "
	    "loop-id=%x portid=%02x%02x%02x.\n", type,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	    (bsg_job->request->rqst_data.h_ct.preamble_word2 >> 16),
#else
	    (bsg_request->rqst_data.h_ct.preamble_word2 >> 16),
#endif
	    fcport->loop_id, fcport->d_id.b.domain, fcport->d_id.b.area,
	    fcport->d_id.b.al_pa);

	rval = qla2x00_start_sp(sp);
	if (rval != QLA_SUCCESS) {
		ql_log(ql_log_warn, vha, 0x7017,
		    "qla2x00_start_sp failed=%d.\n", rval);
		mempool_free(sp, ha->srb_mempool);
		rval = -EIO;
		goto done_free_fcport;
	}
	return rval;

done_free_fcport:
	kfree(fcport);
done_unmap_sg:
	dma_unmap_sg(&ha->pdev->dev, bsg_job->request_payload.sg_list,
		bsg_job->request_payload.sg_cnt, DMA_TO_DEVICE);
	dma_unmap_sg(&ha->pdev->dev, bsg_job->reply_payload.sg_list,
		bsg_job->reply_payload.sg_cnt, DMA_FROM_DEVICE);
done:
	return rval;
}

/* Set the port configuration to enable the
 * internal loopback on ISP81XX
 */
static inline int
qla81xx_set_internal_loopback(scsi_qla_host_t *vha, uint16_t *config,
    uint16_t *new_config)
{
	int ret = 0;
	int rval = 0;
	struct qla_hw_data *ha = vha->hw;

	if (!IS_QLA81XX(ha) && !IS_QLA8031(ha))
		goto done_set_internal;

	new_config[0] = config[0] | (ENABLE_INTERNAL_LOOPBACK << 1);
	memcpy(&new_config[1], &config[1], sizeof(uint16_t) * 3) ;

	ha->notify_dcbx_comp = 1;
	ret = qla81xx_set_port_config(vha, new_config);
	if (ret != QLA_SUCCESS) {
		ql_log(ql_log_warn, vha, 0x7021,
		    "set port config failed.\n");
		ha->notify_dcbx_comp = 0;
		rval = -EINVAL;
		goto done_set_internal;
	}

	/* Wait for DCBX complete event */
	if (!wait_for_completion_timeout(&ha->dcbx_comp, (20 * HZ))) {
		ql_dbg(ql_dbg_user, vha, 0x7022,
		    "State change notification not received.\n");
	} else
		ql_dbg(ql_dbg_user, vha, 0x7023,
		    "State change received.\n");

	ha->notify_dcbx_comp = 0;

done_set_internal:
	return rval;
}

/* Set the port configuration to disable the
 * internal loopback on ISP81XX
 */
static inline int
qla81xx_reset_internal_loopback(scsi_qla_host_t *vha, uint16_t *config,
    int wait)
{
	int ret = 0;
	int rval = 0;
	uint16_t new_config[4];
	struct qla_hw_data *ha = vha->hw;

	if (!IS_QLA81XX(ha) && !IS_QLA8031(ha))
		goto done_reset_internal;

	memset(new_config, 0 , sizeof(new_config));
	if ((config[0] & INTERNAL_LOOPBACK_MASK) >> 1 ==
			ENABLE_INTERNAL_LOOPBACK) {
		new_config[0] = config[0] & ~INTERNAL_LOOPBACK_MASK;
		memcpy(&new_config[1], &config[1], sizeof(uint16_t) * 3) ;

		ha->notify_dcbx_comp = wait;
		ret = qla81xx_set_port_config(vha, new_config);
		if (ret != QLA_SUCCESS) {
			ql_log(ql_log_warn, vha, 0x7025,
			    "Set port config failed.\n");
			ha->notify_dcbx_comp = 0;
			rval = -EINVAL;
			goto done_reset_internal;
		}

		/* Wait for DCBX complete event */
		if (wait && !wait_for_completion_timeout(&ha->dcbx_comp,
			(20 * HZ))) {
			ql_dbg(ql_dbg_user, vha, 0x7026,
			    "State change notification not received.\n");
			ha->notify_dcbx_comp = 0;
			rval = -EINVAL;
			goto done_reset_internal;
		} else
			ql_dbg(ql_dbg_user, vha, 0x7027,
			    "State change received.\n");

		ha->notify_dcbx_comp = 0;
	}
done_reset_internal:
	return rval;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla2x00_process_loopback(struct fc_bsg_job *bsg_job)
#else
qla2x00_process_loopback(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct fc_bsg_request *bsg_request = bsg_job->request;
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	struct qla_hw_data *ha = vha->hw;
	int rval;
	uint8_t command_sent;
	char *type;
	struct msg_echo_lb elreq;
	uint16_t response[MAILBOX_REGISTER_COUNT];
	uint16_t config[4], new_config[4];
	uint8_t *fw_sts_ptr;
	uint8_t *req_data = NULL;
	dma_addr_t req_data_dma;
	uint32_t req_data_len;
	uint8_t *rsp_data = NULL;
	dma_addr_t rsp_data_dma;
	uint32_t rsp_data_len;

	if (!vha->flags.online) {
		ql_log(ql_log_warn, vha, 0x7019, "Host is not online.\n");
		return -EIO;
	}

	elreq.req_sg_cnt = dma_map_sg(&ha->pdev->dev,
		bsg_job->request_payload.sg_list,
		bsg_job->request_payload.sg_cnt, DMA_TO_DEVICE);

	if (!elreq.req_sg_cnt) {
		ql_log(ql_log_warn, vha, 0x701a,
		    "dma_map_sg returned %d for request.\n", elreq.req_sg_cnt);
		return -ENOMEM;
	}

	elreq.rsp_sg_cnt = dma_map_sg(&ha->pdev->dev,
		bsg_job->reply_payload.sg_list, bsg_job->reply_payload.sg_cnt,
		DMA_FROM_DEVICE);

	if (!elreq.rsp_sg_cnt) {
		ql_log(ql_log_warn, vha, 0x701b,
		    "dma_map_sg returned %d for reply.\n", elreq.rsp_sg_cnt);
		rval = -ENOMEM;
		goto done_unmap_req_sg;
	}

	if ((elreq.req_sg_cnt != bsg_job->request_payload.sg_cnt) ||
		(elreq.rsp_sg_cnt != bsg_job->reply_payload.sg_cnt)) {
		ql_log(ql_log_warn, vha, 0x701c,
		    "dma mapping resulted in different sg counts, "
		    "request_sg_cnt: %x dma_request_sg_cnt: %x "
		    "reply_sg_cnt: %x dma_reply_sg_cnt: %x.\n",
		    bsg_job->request_payload.sg_cnt, elreq.req_sg_cnt,
		    bsg_job->reply_payload.sg_cnt, elreq.rsp_sg_cnt);

		rval = -EAGAIN;
		goto done_unmap_sg;
	}
	req_data_len = rsp_data_len = bsg_job->request_payload.payload_len;
	req_data = dma_alloc_coherent(&ha->pdev->dev, req_data_len,
		&req_data_dma, GFP_KERNEL);
	if (!req_data) {
		ql_log(ql_log_warn, vha, 0x701d,
		    "dma alloc failed for req_data.\n");
		rval = -ENOMEM;
		goto done_unmap_sg;
	}

	rsp_data = dma_alloc_coherent(&ha->pdev->dev, rsp_data_len,
		&rsp_data_dma, GFP_KERNEL);
	if (!rsp_data) {
		ql_log(ql_log_warn, vha, 0x7004,
		    "dma alloc failed for rsp_data.\n");
		rval = -ENOMEM;
		goto done_free_dma_req;
	}

	/* Copy the request buffer in req_data now */
	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
		bsg_job->request_payload.sg_cnt, req_data, req_data_len);

	elreq.send_dma = req_data_dma;
	elreq.rcv_dma = rsp_data_dma;
	elreq.transfer_size = req_data_len;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	elreq.options = bsg_job->request->rqst_data.h_vendor.vendor_cmd[1];
#else
	elreq.options = bsg_request->rqst_data.h_vendor.vendor_cmd[1];
#endif

	if ((ha->current_topology == ISP_CFG_F ||
	    ((IS_QLA81XX(ha) || IS_QLA8031(ha)) &&
	    le32_to_cpu(*(uint32_t *)req_data) == ELS_OPCODE_BYTE
	    && req_data_len == MAX_ELS_FRAME_PAYLOAD)) &&
	    elreq.options == EXTERNAL_LOOPBACK) {
		type = "FC_BSG_HST_VENDOR_ECHO_DIAG";
		ql_dbg(ql_dbg_user, vha, 0x701e,
		    "BSG request type: %s.\n", type);
		command_sent = INT_DEF_LB_ECHO_CMD;
		rval = qla2x00_echo_test(vha, &elreq, response);
       } else {
		if (IS_QLA81XX(ha) || IS_QLA8031(ha)) {
			memset(config, 0, sizeof(config));
			memset(new_config, 0, sizeof(new_config));
			if (qla81xx_get_port_config(vha, config)) {
				ql_log(ql_log_warn, vha, 0x701f,
				    "Get port config failed.\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
				bsg_job->reply->result = (DID_ERROR << 16);
#else
				bsg_reply->result = (DID_ERROR << 16);
#endif
				rval = -EPERM;
				goto done_free_dma_req;
			}

			if (elreq.options != EXTERNAL_LOOPBACK) {
				ql_dbg(ql_dbg_user, vha, 0x7020,
				    "Internal: current port config = %x\n",
				    config[0]);
				if (qla81xx_set_internal_loopback(vha, config,
					new_config)) {
					ql_log(ql_log_warn, vha, 0x7024,
					    "Internal loopback failed.\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
					bsg_job->reply->result =
#else
					bsg_reply->result =
#endif
						(DID_ERROR << 16);
					rval = -EPERM;
					goto done_free_dma_req;
				}
			} else {
				/* For external loopback to work
				 * ensure internal loopback is disabled
				 */
				if (qla81xx_reset_internal_loopback(vha,
					config, 1)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
					bsg_job->reply->result =
#else
					bsg_reply->result =
#endif
						(DID_ERROR << 16);
					rval = -EPERM;
					goto done_free_dma_req;
				}
			}

			type = "FC_BSG_HST_VENDOR_LOOPBACK";
			ql_dbg(ql_dbg_user, vha, 0x7028,
			    "BSG request type: %s.\n", type);

			command_sent = INT_DEF_LB_LOOPBACK_CMD;
			rval = qla2x00_loopback_test(vha, &elreq, response);

			if (new_config[0]) {
				/* Revert back to original port config
				 * Also clear internal loopback
				 */
				qla81xx_reset_internal_loopback(vha,
				    new_config, 0);
			}

			if (response[0] == MBS_COMMAND_ERROR &&
				response[1] == MBS_LB_RESET) {
				ql_log(ql_log_warn, vha, 0x7029,
				    "MBX command error, Aborting ISP.\n");
				set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
				qla2xxx_wake_dpc(vha);
				qla2x00_wait_for_chip_reset(vha);
				/* Also reset the MPI */
				if (qla81xx_restart_mpi_firmware(vha) !=
				    QLA_SUCCESS) {
					ql_log(ql_log_warn, vha, 0x702a,
					    "MPI reset failed.\n");
				}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
				bsg_job->reply->result = (DID_ERROR << 16);
#else
				bsg_reply->result = (DID_ERROR << 16);
#endif
				rval = -EIO;
				goto done_free_dma_req;
			}
		} else {
			type = "FC_BSG_HST_VENDOR_LOOPBACK";
			ql_dbg(ql_dbg_user, vha, 0x702b,
			    "BSG request type: %s.\n", type);
			command_sent = INT_DEF_LB_LOOPBACK_CMD;
			rval = qla2x00_loopback_test(vha, &elreq, response);
		}
	}

	if (rval) {
		ql_log(ql_log_warn, vha, 0x702c,
		    "Vendor request %s failed.\n", type);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
		fw_sts_ptr = ((uint8_t *)scsi_req(bsg_job->req)->sense) +
			 sizeof(struct fc_bsg_reply);
#else
		fw_sts_ptr = ((uint8_t *)bsg_job->req->sense) +
			sizeof(struct fc_bsg_reply);
#endif

		memcpy(fw_sts_ptr, response, sizeof(response));
		fw_sts_ptr += sizeof(response);
		*fw_sts_ptr = command_sent;
		rval = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = (DID_ERROR << 16);
#else
		bsg_reply->result = (DID_ERROR << 16);
#endif
	} else {
		ql_dbg(ql_dbg_user, vha, 0x702d,
		    "Vendor request %s completed.\n", type);

		bsg_job->reply_len = sizeof(struct fc_bsg_reply) +
			sizeof(response) + sizeof(uint8_t);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->reply_payload_rcv_len =
#else
		bsg_reply->reply_payload_rcv_len =
#endif
			bsg_job->reply_payload.payload_len;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
		fw_sts_ptr = ((uint8_t *)scsi_req(bsg_job->req)->sense) +
			 sizeof(struct fc_bsg_reply);
#else
		fw_sts_ptr = ((uint8_t *)bsg_job->req->sense) +
			sizeof(struct fc_bsg_reply);
#endif
		memcpy(fw_sts_ptr, response, sizeof(response));
		fw_sts_ptr += sizeof(response);
		*fw_sts_ptr = command_sent;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = DID_OK;
#else
		bsg_reply->result = DID_OK;
#endif
		sg_copy_from_buffer(bsg_job->reply_payload.sg_list,
			bsg_job->reply_payload.sg_cnt, rsp_data,
			rsp_data_len);
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->job_done(bsg_job);
#else
	if (!rval)
		bsg_job_done(bsg_job, bsg_reply->result,
			bsg_reply->reply_payload_rcv_len);
#endif

	dma_free_coherent(&ha->pdev->dev, rsp_data_len,
		rsp_data, rsp_data_dma);
done_free_dma_req:
	dma_free_coherent(&ha->pdev->dev, req_data_len,
		req_data, req_data_dma);
done_unmap_sg:
	dma_unmap_sg(&ha->pdev->dev,
	    bsg_job->reply_payload.sg_list,
	    bsg_job->reply_payload.sg_cnt, DMA_FROM_DEVICE);
done_unmap_req_sg:
	dma_unmap_sg(&ha->pdev->dev,
		bsg_job->request_payload.sg_list,
		bsg_job->request_payload.sg_cnt, DMA_TO_DEVICE);
	return rval;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla84xx_reset(struct fc_bsg_job *bsg_job)
#else
qla84xx_reset(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct fc_bsg_request *bsg_request = bsg_job->request;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	struct qla_hw_data *ha = vha->hw;
	int rval = 0;
	uint32_t flag;

	if (!IS_QLA84XX(ha)) {
		ql_dbg(ql_dbg_user, vha, 0x702f, "Not 84xx, exiting.\n");
		return -EINVAL;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	flag = bsg_job->request->rqst_data.h_vendor.vendor_cmd[1];
#else
	flag = bsg_request->rqst_data.h_vendor.vendor_cmd[1];
#endif

	rval = qla84xx_reset_chip(vha, flag == A84_ISSUE_RESET_DIAG_FW);

	if (rval) {
		ql_log(ql_log_warn, vha, 0x7030,
		    "Vendor request 84xx reset failed.\n");
		rval = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = (DID_ERROR << 16);
#else
		bsg_reply->result = (DID_ERROR << 16);
#endif
	} else {
		ql_dbg(ql_dbg_user, vha, 0x7031,
		    "Vendor request 84xx reset completed.\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = DID_OK;
#else
		bsg_reply->result = DID_OK;
#endif
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->job_done(bsg_job);
#else
	if (!rval)
		bsg_job_done(bsg_job, bsg_reply->result,
			bsg_reply->reply_payload_rcv_len);
#endif
	return rval;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla84xx_updatefw(struct fc_bsg_job *bsg_job)
#else
qla84xx_updatefw(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct fc_bsg_request *bsg_request = bsg_job->request;
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	struct qla_hw_data *ha = vha->hw;
	struct verify_chip_entry_84xx *mn = NULL;
	dma_addr_t mn_dma, fw_dma;
	void *fw_buf = NULL;
	int rval = 0;
	uint32_t sg_cnt;
	uint32_t data_len;
	uint16_t options;
	uint32_t flag;
	uint32_t fw_ver;

	if (!IS_QLA84XX(ha)) {
		ql_dbg(ql_dbg_user, vha, 0x7032,
		    "Not 84xx, exiting.\n");
		return -EINVAL;
	}

	sg_cnt = dma_map_sg(&ha->pdev->dev, bsg_job->request_payload.sg_list,
		bsg_job->request_payload.sg_cnt, DMA_TO_DEVICE);
	if (!sg_cnt) {
		ql_log(ql_log_warn, vha, 0x7033,
		    "dma_map_sg returned %d for request.\n", sg_cnt);
		return -ENOMEM;
	}

	if (sg_cnt != bsg_job->request_payload.sg_cnt) {
		ql_log(ql_log_warn, vha, 0x7034,
		    "DMA mapping resulted in different sg counts, "
		    "request_sg_cnt: %x dma_request_sg_cnt: %x.\n",
		    bsg_job->request_payload.sg_cnt, sg_cnt);
		rval = -EAGAIN;
		goto done_unmap_sg;
	}

	data_len = bsg_job->request_payload.payload_len;
	fw_buf = dma_alloc_coherent(&ha->pdev->dev, data_len,
		&fw_dma, GFP_KERNEL);
	if (!fw_buf) {
		ql_log(ql_log_warn, vha, 0x7035,
		    "DMA alloc failed for fw_buf.\n");
		rval = -ENOMEM;
		goto done_unmap_sg;
	}

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
		bsg_job->request_payload.sg_cnt, fw_buf, data_len);

	mn = dma_pool_alloc(ha->s_dma_pool, GFP_KERNEL, &mn_dma);
	if (!mn) {
		ql_log(ql_log_warn, vha, 0x7036,
		    "DMA alloc failed for fw buffer.\n");
		rval = -ENOMEM;
		goto done_free_fw_buf;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	flag = bsg_job->request->rqst_data.h_vendor.vendor_cmd[1];
#else
	flag = bsg_request->rqst_data.h_vendor.vendor_cmd[1];
#endif
	fw_ver = le32_to_cpu(*((uint32_t *)((uint32_t *)fw_buf + 2)));

	memset(mn, 0, sizeof(struct access_chip_84xx));
	mn->entry_type = VERIFY_CHIP_IOCB_TYPE;
	mn->entry_count = 1;

	options = VCO_FORCE_UPDATE | VCO_END_OF_DATA;
	if (flag == A84_ISSUE_UPDATE_DIAGFW_CMD)
		options |= VCO_DIAG_FW;

	mn->options = cpu_to_le16(options);
	mn->fw_ver =  cpu_to_le32(fw_ver);
	mn->fw_size =  cpu_to_le32(data_len);
	mn->fw_seq_size =  cpu_to_le32(data_len);
	mn->dseg_address[0] = cpu_to_le32(LSD(fw_dma));
	mn->dseg_address[1] = cpu_to_le32(MSD(fw_dma));
	mn->dseg_length = cpu_to_le32(data_len);
	mn->data_seg_cnt = cpu_to_le16(1);

	rval = qla2x00_issue_iocb_timeout(vha, mn, mn_dma, 0, 120);

	if (rval) {
		ql_log(ql_log_warn, vha, 0x7037,
		    "Vendor request 84xx updatefw failed.\n");

		rval = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = (DID_ERROR << 16);
#else
		bsg_reply->result = (DID_ERROR << 16);
#endif
	} else {
		ql_dbg(ql_dbg_user, vha, 0x7038,
		    "Vendor request 84xx updatefw completed.\n");

		bsg_job->reply_len = sizeof(struct fc_bsg_reply);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = DID_OK;
#else
		bsg_reply->result = DID_OK;
#endif
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->job_done(bsg_job);
#else
	if (!rval)
		bsg_job_done(bsg_job, bsg_reply->result,
			bsg_reply->reply_payload_rcv_len);
#endif
	dma_pool_free(ha->s_dma_pool, mn, mn_dma);

done_free_fw_buf:
	dma_free_coherent(&ha->pdev->dev, data_len, fw_buf, fw_dma);

done_unmap_sg:
	dma_unmap_sg(&ha->pdev->dev, bsg_job->request_payload.sg_list,
		bsg_job->request_payload.sg_cnt, DMA_TO_DEVICE);

	return rval;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla84xx_mgmt_cmd(struct fc_bsg_job *bsg_job)
#else
qla84xx_mgmt_cmd(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct fc_bsg_request *bsg_request = bsg_job->request;
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	struct qla_hw_data *ha = vha->hw;
	struct access_chip_84xx *mn = NULL;
	dma_addr_t mn_dma, mgmt_dma;
	void *mgmt_b = NULL;
	int rval = 0;
	struct qla_bsg_a84_mgmt *ql84_mgmt;
	uint32_t sg_cnt;
	uint32_t data_len = 0;
	uint32_t dma_direction = DMA_NONE;

	if (!IS_QLA84XX(ha)) {
		ql_log(ql_log_warn, vha, 0x703a,
		    "Not 84xx, exiting.\n");
		return -EINVAL;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	ql84_mgmt = (struct qla_bsg_a84_mgmt *)((char *)bsg_job->request +
		sizeof(struct fc_bsg_request));
#else
	ql84_mgmt = (void *)bsg_request + sizeof(struct fc_bsg_request);
#endif

	mn = dma_pool_alloc(ha->s_dma_pool, GFP_KERNEL, &mn_dma);
	if (!mn) {
		ql_log(ql_log_warn, vha, 0x703c,
		    "DMA alloc failed for fw buffer.\n");
		return -ENOMEM;
	}

	memset(mn, 0, sizeof(struct access_chip_84xx));
	mn->entry_type = ACCESS_CHIP_IOCB_TYPE;
	mn->entry_count = 1;

	switch (ql84_mgmt->mgmt.cmd) {
	case QLA84_MGMT_READ_MEM:
	case QLA84_MGMT_GET_INFO:
		sg_cnt = dma_map_sg(&ha->pdev->dev,
			bsg_job->reply_payload.sg_list,
			bsg_job->reply_payload.sg_cnt, DMA_FROM_DEVICE);
		if (!sg_cnt) {
			ql_log(ql_log_warn, vha, 0x703d,
			    "dma_map_sg returned %d for reply.\n", sg_cnt);
			rval = -ENOMEM;
			goto exit_mgmt;
		}

		dma_direction = DMA_FROM_DEVICE;

		if (sg_cnt != bsg_job->reply_payload.sg_cnt) {
			ql_log(ql_log_warn, vha, 0x703e,
			    "DMA mapping resulted in different sg counts, "
			    "reply_sg_cnt: %x dma_reply_sg_cnt: %x.\n",
			    bsg_job->reply_payload.sg_cnt, sg_cnt);
			rval = -EAGAIN;
			goto done_unmap_sg;
		}

		data_len = bsg_job->reply_payload.payload_len;

		mgmt_b = dma_alloc_coherent(&ha->pdev->dev, data_len,
			&mgmt_dma, GFP_KERNEL);
		if (!mgmt_b) {
			ql_log(ql_log_warn, vha, 0x703f,
			    "DMA alloc failed for mgmt_b.\n");
			rval = -ENOMEM;
			goto done_unmap_sg;
		}

		if (ql84_mgmt->mgmt.cmd == QLA84_MGMT_READ_MEM) {
			mn->options = cpu_to_le16(ACO_DUMP_MEMORY);
			mn->parameter1 =
				cpu_to_le32(
				ql84_mgmt->mgmt.mgmtp.u.mem.start_addr);

		} else if (ql84_mgmt->mgmt.cmd == QLA84_MGMT_GET_INFO) {
			mn->options = cpu_to_le16(ACO_REQUEST_INFO);
			mn->parameter1 =
				cpu_to_le32(ql84_mgmt->mgmt.mgmtp.u.info.type);

			mn->parameter2 =
				cpu_to_le32(
				ql84_mgmt->mgmt.mgmtp.u.info.context);
		}
		break;

	case QLA84_MGMT_WRITE_MEM:
		sg_cnt = dma_map_sg(&ha->pdev->dev,
			bsg_job->request_payload.sg_list,
			bsg_job->request_payload.sg_cnt, DMA_TO_DEVICE);

		if (!sg_cnt) {
			ql_log(ql_log_warn, vha, 0x7040,
			    "dma_map_sg returned %d.\n", sg_cnt);
			rval = -ENOMEM;
			goto exit_mgmt;
		}

		dma_direction = DMA_TO_DEVICE;

		if (sg_cnt != bsg_job->request_payload.sg_cnt) {
			ql_log(ql_log_warn, vha, 0x7041,
			    "DMA mapping resulted in different sg counts, "
			    "request_sg_cnt: %x dma_request_sg_cnt: %x.\n",
			    bsg_job->request_payload.sg_cnt, sg_cnt);
			rval = -EAGAIN;
			goto done_unmap_sg;
		}

		data_len = bsg_job->request_payload.payload_len;
		mgmt_b = dma_alloc_coherent(&ha->pdev->dev, data_len,
			&mgmt_dma, GFP_KERNEL);
		if (!mgmt_b) {
			ql_log(ql_log_warn, vha, 0x7042,
			    "DMA alloc failed for mgmt_b.\n");
			rval = -ENOMEM;
			goto done_unmap_sg;
		}

		sg_copy_to_buffer(bsg_job->request_payload.sg_list,
			bsg_job->request_payload.sg_cnt, mgmt_b, data_len);

		mn->options = cpu_to_le16(ACO_LOAD_MEMORY);
		mn->parameter1 =
			cpu_to_le32(ql84_mgmt->mgmt.mgmtp.u.mem.start_addr);
		break;

	case QLA84_MGMT_CHNG_CONFIG:
		mn->options = cpu_to_le16(ACO_CHANGE_CONFIG_PARAM);
		mn->parameter1 =
			cpu_to_le32(ql84_mgmt->mgmt.mgmtp.u.config.id);

		mn->parameter2 =
			cpu_to_le32(ql84_mgmt->mgmt.mgmtp.u.config.param0);

		mn->parameter3 =
			cpu_to_le32(ql84_mgmt->mgmt.mgmtp.u.config.param1);
		break;

	default:
		rval = -EIO;
		goto exit_mgmt;
	}

	if (ql84_mgmt->mgmt.cmd != QLA84_MGMT_CHNG_CONFIG) {
		mn->total_byte_cnt = cpu_to_le32(ql84_mgmt->mgmt.len);
		mn->dseg_count = cpu_to_le16(1);
		mn->dseg_address[0] = cpu_to_le32(LSD(mgmt_dma));
		mn->dseg_address[1] = cpu_to_le32(MSD(mgmt_dma));
		mn->dseg_length = cpu_to_le32(ql84_mgmt->mgmt.len);
	}

	rval = qla2x00_issue_iocb(vha, mn, mn_dma, 0);

	if (rval) {
		ql_log(ql_log_warn, vha, 0x7043,
		    "Vendor request 84xx mgmt failed.\n");

		rval = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = (DID_ERROR << 16);
#else
		bsg_reply->result = (DID_ERROR << 16);
#endif
	} else {
		ql_dbg(ql_dbg_user, vha, 0x7044,
		    "Vendor request 84xx mgmt completed.\n");

		bsg_job->reply_len = sizeof(struct fc_bsg_reply);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = DID_OK;
#else
		bsg_reply->result = DID_OK;
#endif

		if ((ql84_mgmt->mgmt.cmd == QLA84_MGMT_READ_MEM) ||
			(ql84_mgmt->mgmt.cmd == QLA84_MGMT_GET_INFO)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
			bsg_job->reply->reply_payload_rcv_len =
#else
			bsg_reply->reply_payload_rcv_len =
#endif
				bsg_job->reply_payload.payload_len;

			sg_copy_from_buffer(bsg_job->reply_payload.sg_list,
				bsg_job->reply_payload.sg_cnt, mgmt_b,
				data_len);
		}
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->job_done(bsg_job);
#else
	if (!rval)
		bsg_job_done(bsg_job, bsg_reply->result,
			bsg_reply->reply_payload_rcv_len);
#endif

done_unmap_sg:
	if (mgmt_b)
		dma_free_coherent(&ha->pdev->dev, data_len, mgmt_b, mgmt_dma);

	if (dma_direction == DMA_TO_DEVICE)
		dma_unmap_sg(&ha->pdev->dev, bsg_job->request_payload.sg_list,
			bsg_job->request_payload.sg_cnt, DMA_TO_DEVICE);
	else if (dma_direction == DMA_FROM_DEVICE)
		dma_unmap_sg(&ha->pdev->dev, bsg_job->reply_payload.sg_list,
			bsg_job->reply_payload.sg_cnt, DMA_FROM_DEVICE);

exit_mgmt:
	dma_pool_free(ha->s_dma_pool, mn, mn_dma);

	return rval;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla24xx_iidma(struct fc_bsg_job *bsg_job)
#else
qla24xx_iidma(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct fc_bsg_request *bsg_request = bsg_job->request;
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	int rval = 0;
	struct qla_port_param *port_param = NULL;
	fc_port_t *fcport = NULL;
	uint16_t mb[MAILBOX_REGISTER_COUNT];
	uint8_t *rsp_ptr = NULL;

	if (!IS_IIDMA_CAPABLE(vha->hw)) {
		ql_log(ql_log_info, vha, 0x7046, "iiDMA not supported.\n");
		return -EINVAL;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	port_param = (struct qla_port_param *)((char *)bsg_job->request +
		sizeof(struct fc_bsg_request));
#else
	port_param = (void *)bsg_request + sizeof(struct fc_bsg_request);
#endif
	if (port_param->fc_scsi_addr.dest_type != EXT_DEF_TYPE_WWPN) {
		ql_log(ql_log_warn, vha, 0x7048,
		    "Invalid destination type.\n");
		return -EINVAL;
	}

	list_for_each_entry_rcu(fcport, &vha->vp_fcports, list) {
		if (fcport->port_type != FCT_TARGET)
			continue;

		if (memcmp(port_param->fc_scsi_addr.dest_addr.wwpn,
			fcport->port_name, sizeof(fcport->port_name)))
			continue;
		break;
	}

	if (!fcport) {
		ql_log(ql_log_warn, vha, 0x7049,
		    "Failed to find port.\n");
		return -EINVAL;
	}

	if (atomic_read(&fcport->state) != FCS_ONLINE) {
		ql_log(ql_log_warn, vha, 0x704a,
		    "Port is not online.\n");
		return -EINVAL;
	}

	if (fcport->flags & FCF_LOGIN_NEEDED) {
		ql_log(ql_log_warn, vha, 0x704b,
		    "Remote port not logged in flags = 0x%x.\n", fcport->flags);
		return -EINVAL;
	}

	if (port_param->mode)
		rval = qla2x00_set_idma_speed(vha, fcport->loop_id,
			port_param->speed, mb);
	else
		rval = qla2x00_get_idma_speed(vha, fcport->loop_id,
			&port_param->speed, mb);

	if (rval) {
		ql_log(ql_log_warn, vha, 0x704c,
		    "iIDMA cmd failed for %02x%02x%02x%02x%02x%02x%02x%02x -- "
		    "%04x %x %04x %04x.\n", fcport->port_name[0],
		    fcport->port_name[1], fcport->port_name[2],
		    fcport->port_name[3], fcport->port_name[4],
		    fcport->port_name[5], fcport->port_name[6],
		    fcport->port_name[7], rval, fcport->fp_speed, mb[0], mb[1]);
		rval = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = (DID_ERROR << 16);
#else
		bsg_reply->result = (DID_ERROR << 16);
#endif
	} else {
		if (!port_param->mode) {
			bsg_job->reply_len = sizeof(struct fc_bsg_reply) +
				sizeof(struct qla_port_param);

			rsp_ptr = ((uint8_t *)bsg_job->reply) +
				sizeof(struct fc_bsg_reply);

			memcpy(rsp_ptr, port_param,
				sizeof(struct qla_port_param));
		}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = DID_OK;
#else
		bsg_reply->result = DID_OK;
#endif
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->job_done(bsg_job);
#else
	if (!rval)
		bsg_job_done(bsg_job, bsg_reply->result,
			bsg_reply->reply_payload_rcv_len);
#endif
	return rval;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla2x00_optrom_setup(struct fc_bsg_job *bsg_job, scsi_qla_host_t *vha,
#else
qla2x00_optrom_setup(struct bsg_job *bsg_job, scsi_qla_host_t *vha,
#endif
    uint8_t is_update)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	struct fc_bsg_request *bsg_request = bsg_job->request;
#endif
	uint32_t start = 0;
	int valid = 0;
	struct qla_hw_data *ha = vha->hw;

	if (unlikely(pci_channel_offline(ha->pdev)))
		return -EINVAL;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	start = bsg_job->request->rqst_data.h_vendor.vendor_cmd[1];
#else
	start = bsg_request->rqst_data.h_vendor.vendor_cmd[1];
#endif
	if (start > ha->optrom_size) {
		ql_log(ql_log_warn, vha, 0x7055,
		    "start %d > optrom_size %d.\n", start, ha->optrom_size);
		return -EINVAL;
	}

	if (ha->optrom_state != QLA_SWAITING) {
		ql_log(ql_log_info, vha, 0x7056,
		    "optrom_state %d.\n", ha->optrom_state);
		return -EBUSY;
	}

	ha->optrom_region_start = start;
	ql_dbg(ql_dbg_user, vha, 0x7057, "is_update=%d.\n", is_update);
	if (is_update) {
		if (ha->optrom_size == OPTROM_SIZE_2300 && start == 0)
			valid = 1;
		else if (start == (ha->flt_region_boot * 4) ||
		    start == (ha->flt_region_fw * 4))
			valid = 1;
		else if (IS_QLA24XX_TYPE(ha) || IS_QLA25XX(ha) ||
		    IS_CNA_CAPABLE(ha) || IS_QLA2031(ha))
			valid = 1;
		if (!valid) {
			ql_log(ql_log_warn, vha, 0x7058,
			    "Invalid start region 0x%x/0x%x.\n", start,
			    bsg_job->request_payload.payload_len);
			return -EINVAL;
		}

		ha->optrom_region_size = start +
		    bsg_job->request_payload.payload_len > ha->optrom_size ?
		    ha->optrom_size - start :
		    bsg_job->request_payload.payload_len;
		ha->optrom_state = QLA_SWRITING;
	} else {
		ha->optrom_region_size = start +
		    bsg_job->reply_payload.payload_len > ha->optrom_size ?
		    ha->optrom_size - start :
		    bsg_job->reply_payload.payload_len;
		ha->optrom_state = QLA_SREADING;
	}

	ha->optrom_buffer = vmalloc(ha->optrom_region_size);
	if (!ha->optrom_buffer) {
		ql_log(ql_log_warn, vha, 0x7059,
		    "Read: Unable to allocate memory for optrom retrieval "
		    "(%x)\n", ha->optrom_region_size);

		ha->optrom_state = QLA_SWAITING;
		return -ENOMEM;
	}

	memset(ha->optrom_buffer, 0, ha->optrom_region_size);
	return 0;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla2x00_read_optrom(struct fc_bsg_job *bsg_job)
#else
qla2x00_read_optrom(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	struct qla_hw_data *ha = vha->hw;
	int rval = 0;

	if (ha->flags.isp82xx_reset_hdlr_active)
		return -EBUSY;

	rval = qla2x00_optrom_setup(bsg_job, vha, 0);
	if (rval)
		return rval;

	ha->isp_ops->read_optrom(vha, ha->optrom_buffer,
	    ha->optrom_region_start, ha->optrom_region_size);

	sg_copy_from_buffer(bsg_job->reply_payload.sg_list,
	    bsg_job->reply_payload.sg_cnt, ha->optrom_buffer,
	    ha->optrom_region_size);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->reply->reply_payload_rcv_len = ha->optrom_region_size;
	bsg_job->reply->result = DID_OK;
#else
	bsg_reply->reply_payload_rcv_len = ha->optrom_region_size;
	bsg_reply->result = DID_OK;
#endif
	vfree(ha->optrom_buffer);
	ha->optrom_buffer = NULL;
	ha->optrom_state = QLA_SWAITING;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->job_done(bsg_job);
#else
	bsg_job_done(bsg_job, bsg_reply->result,
		bsg_reply->reply_payload_rcv_len);
#endif
	return rval;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla2x00_update_optrom(struct fc_bsg_job *bsg_job)
#else
qla2x00_update_optrom(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	struct qla_hw_data *ha = vha->hw;
	int rval = 0;

	rval = qla2x00_optrom_setup(bsg_job, vha, 1);
	if (rval)
		return rval;

	/* Set the isp82xx_no_md_cap not to capture minidump */
	ha->flags.isp82xx_no_md_cap = 1;

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, ha->optrom_buffer,
	    ha->optrom_region_size);

	ha->isp_ops->write_optrom(vha, ha->optrom_buffer,
	    ha->optrom_region_start, ha->optrom_region_size);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->reply->result = DID_OK;
#else
	bsg_reply->result = DID_OK;
#endif
	vfree(ha->optrom_buffer);
	ha->optrom_buffer = NULL;
	ha->optrom_state = QLA_SWAITING;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->job_done(bsg_job);
#else
	bsg_job_done(bsg_job, bsg_reply->result,
		bsg_reply->reply_payload_rcv_len);
#endif
	return rval;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla2x00_update_fru_versions(struct fc_bsg_job *bsg_job)
#else
qla2x00_update_fru_versions(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	struct qla_hw_data *ha = vha->hw;
	int rval = 0;
	uint8_t bsg[DMA_POOL_SIZE];
	struct qla_image_version_list *list = (void *)bsg;
	struct qla_image_version *image;
	uint32_t count;
	dma_addr_t sfp_dma;
	void *sfp = dma_pool_alloc(ha->s_dma_pool, GFP_KERNEL, &sfp_dma);
	if (!sfp) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->reply_data.vendor_reply.vendor_rsp[0] =
#else
		bsg_reply->reply_data.vendor_reply.vendor_rsp[0] =
#endif
		    EXT_STATUS_NO_MEMORY;
		goto done;
	}

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, list, sizeof(bsg));

	image = list->version;
	count = list->count;
	while (count--) {
		memcpy(sfp, &image->field_info, sizeof(image->field_info));
		rval = qla2x00_write_sfp(vha, sfp_dma, sfp,
		    image->field_address.device, image->field_address.offset,
		    sizeof(image->field_info), image->field_address.option);
		if (rval) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
			bsg_job->reply->reply_data.vendor_reply.vendor_rsp[0] =
#else
			bsg_reply->reply_data.vendor_reply.vendor_rsp[0] =
#endif
			    EXT_STATUS_MAILBOX;
			goto dealloc;
		}
		image++;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->reply->reply_data.vendor_reply.vendor_rsp[0] = 0;
#else
	bsg_reply->reply_data.vendor_reply.vendor_rsp[0] = 0;
#endif

dealloc:
	dma_pool_free(ha->s_dma_pool, sfp, sfp_dma);

done:
	bsg_job->reply_len = sizeof(struct fc_bsg_reply);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->reply->result = DID_OK << 16;
	bsg_job->job_done(bsg_job);
#else
	bsg_reply->result = DID_OK << 16;
	bsg_job_done(bsg_job, bsg_reply->result,
		bsg_reply->reply_payload_rcv_len);
#endif

	return 0;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla2x00_read_fru_status(struct fc_bsg_job *bsg_job)
#else
qla2x00_read_fru_status(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	struct qla_hw_data *ha = vha->hw;
	int rval = 0;
	uint8_t bsg[DMA_POOL_SIZE];
	struct qla_status_reg *sr = (void *)bsg;
	dma_addr_t sfp_dma;
	uint8_t *sfp = dma_pool_alloc(ha->s_dma_pool, GFP_KERNEL, &sfp_dma);
	if (!sfp) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->reply_data.vendor_reply.vendor_rsp[0] =
#else
		bsg_reply->reply_data.vendor_reply.vendor_rsp[0] =
#endif
		    EXT_STATUS_NO_MEMORY;
		goto done;
	}

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, sr, sizeof(*sr));

	rval = qla2x00_read_sfp(vha, sfp_dma, sfp,
	    sr->field_address.device, sr->field_address.offset,
	    sizeof(sr->status_reg), sr->field_address.option);
	sr->status_reg = *sfp;

	if (rval) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->reply_data.vendor_reply.vendor_rsp[0] =
#else
		bsg_reply->reply_data.vendor_reply.vendor_rsp[0] =
#endif
		    EXT_STATUS_MAILBOX;
		goto dealloc;
	}

	sg_copy_from_buffer(bsg_job->reply_payload.sg_list,
	    bsg_job->reply_payload.sg_cnt, sr, sizeof(*sr));

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->reply->reply_data.vendor_reply.vendor_rsp[0] = 0;
#else
	bsg_reply->reply_data.vendor_reply.vendor_rsp[0] = 0;
#endif

dealloc:
	dma_pool_free(ha->s_dma_pool, sfp, sfp_dma);

done:
	bsg_job->reply_len = sizeof(struct fc_bsg_reply);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->reply->reply_payload_rcv_len = sizeof(*sr);
	bsg_job->reply->result = DID_OK << 16;
	bsg_job->job_done(bsg_job);
#else
	bsg_reply->reply_payload_rcv_len = sizeof(*sr);
	bsg_reply->result = DID_OK << 16;
	bsg_job_done(bsg_job, bsg_reply->result,
		bsg_reply->reply_payload_rcv_len);
#endif

	return 0;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla2x00_write_fru_status(struct fc_bsg_job *bsg_job)
#else
qla2x00_write_fru_status(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	struct Scsi_Host *host = bsg_job->shost;
#else
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
#endif
	scsi_qla_host_t *vha = shost_priv(host);
	struct qla_hw_data *ha = vha->hw;
	int rval = 0;
	uint8_t bsg[DMA_POOL_SIZE];
	struct qla_status_reg *sr = (void *)bsg;
	dma_addr_t sfp_dma;
	uint8_t *sfp = dma_pool_alloc(ha->s_dma_pool, GFP_KERNEL, &sfp_dma);
	if (!sfp) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->reply_data.vendor_reply.vendor_rsp[0] =
#else
		bsg_reply->reply_data.vendor_reply.vendor_rsp[0] =
#endif
		    EXT_STATUS_NO_MEMORY;
		goto done;
	}

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, sr, sizeof(*sr));

	*sfp = sr->status_reg;
	rval = qla2x00_write_sfp(vha, sfp_dma, sfp,
	    sr->field_address.device, sr->field_address.offset,
	    sizeof(sr->status_reg), sr->field_address.option);

	if (rval) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->reply_data.vendor_reply.vendor_rsp[0] =
#else
		bsg_reply->reply_data.vendor_reply.vendor_rsp[0] =
#endif
		    EXT_STATUS_MAILBOX;
		goto dealloc;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->reply->reply_data.vendor_reply.vendor_rsp[0] = 0;
#else
	bsg_reply->reply_data.vendor_reply.vendor_rsp[0] = 0;
#endif

dealloc:
	dma_pool_free(ha->s_dma_pool, sfp, sfp_dma);

done:
	bsg_job->reply_len = sizeof(struct fc_bsg_reply);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->reply->result = DID_OK << 16;
	bsg_job->job_done(bsg_job);
#else
	bsg_reply->result = DID_OK << 16;
	bsg_job_done(bsg_job, bsg_reply->result,
		bsg_reply->reply_payload_rcv_len);
#endif

	return 0;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla2x00_process_vendor_specific(struct fc_bsg_job *bsg_job)
#else
qla2x00_process_vendor_specific(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	struct fc_bsg_request *bsg_request = bsg_job->request;
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	switch (bsg_job->request->rqst_data.h_vendor.vendor_cmd[0]) {
#else
	switch (bsg_request->rqst_data.h_vendor.vendor_cmd[0]) {
#endif
	case QL_VND_LOOPBACK:
		return qla2x00_process_loopback(bsg_job);

	case QL_VND_A84_RESET:
		return qla84xx_reset(bsg_job);

	case QL_VND_A84_UPDATE_FW:
		return qla84xx_updatefw(bsg_job);

	case QL_VND_A84_MGMT_CMD:
		return qla84xx_mgmt_cmd(bsg_job);

	case QL_VND_IIDMA:
		return qla24xx_iidma(bsg_job);

	case QL_VND_FCP_PRIO_CFG_CMD:
		return qla24xx_proc_fcp_prio_cfg_cmd(bsg_job);

	case QL_VND_READ_FLASH:
		return qla2x00_read_optrom(bsg_job);

	case QL_VND_UPDATE_FLASH:
		return qla2x00_update_optrom(bsg_job);

	case QL_VND_SET_FRU_VERSION:
		return qla2x00_update_fru_versions(bsg_job);

	case QL_VND_READ_FRU_STATUS:
		return qla2x00_read_fru_status(bsg_job);

	case QL_VND_WRITE_FRU_STATUS:
		return qla2x00_write_fru_status(bsg_job);

	default:
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = (DID_ERROR << 16);
		bsg_job->job_done(bsg_job);
#else
		bsg_reply->result = (DID_ERROR << 16);
#endif
		return -ENOTSUPP;
	}
}

int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla24xx_bsg_request(struct fc_bsg_job *bsg_job)
#else
qla24xx_bsg_request(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	struct fc_bsg_request *bsg_request = bsg_job->request;
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
#endif
	int ret = -EINVAL;
	struct fc_rport *rport;
	fc_port_t *fcport = NULL;
	struct Scsi_Host *host;
	scsi_qla_host_t *vha;

	/* In case no data transferred. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->reply->reply_payload_rcv_len = 0;

	if (bsg_job->request->msgcode == FC_BSG_RPT_ELS) {
		rport = bsg_job->rport;
#else
	bsg_reply->reply_payload_rcv_len = 0;

	if (bsg_request->msgcode == FC_BSG_RPT_ELS) {
		rport = fc_bsg_to_rport(bsg_job);
#endif
		fcport = *(fc_port_t **) rport->dd_data;
		host = rport_to_shost(rport);
		vha = shost_priv(host);
	} else {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		host = bsg_job->shost;
#else
		host = fc_bsg_to_shost(bsg_job);
#endif
		vha = shost_priv(host);
	}

	if (qla2x00_reset_active(vha)) {
		ql_dbg(ql_dbg_user, vha, 0x709f,
		    "BSG: ISP abort active/needed -- cmd=%d.\n",
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		    bsg_job->request->msgcode);
		bsg_job->reply->result = (DID_ERROR << 16);
		bsg_job->job_done(bsg_job);
#else
		    bsg_request->msgcode);
		bsg_reply->result = (DID_ERROR << 16);
#endif
		return -EBUSY;
	}

	ql_dbg(ql_dbg_user, vha, 0x7000,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	    "Entered %s msgcode=0x%x.\n", __func__, bsg_job->request->msgcode);
#else
	    "Entered %s msgcode=0x%x.\n", __func__, bsg_request->msgcode);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	switch (bsg_job->request->msgcode) {
#else
	switch (bsg_request->msgcode) {
#endif
	case FC_BSG_RPT_ELS:
	case FC_BSG_HST_ELS_NOLOGIN:
		ret = qla2x00_process_els(bsg_job);
		break;
	case FC_BSG_HST_CT:
		ret = qla2x00_process_ct(bsg_job);
		break;
	case FC_BSG_HST_VENDOR:
		ret = qla2x00_process_vendor_specific(bsg_job);
		break;
	case FC_BSG_HST_ADD_RPORT:
	case FC_BSG_HST_DEL_RPORT:
	case FC_BSG_RPT_CT:
	default:
		ql_log(ql_log_warn, vha, 0x705a, "Unsupported BSG request.\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
		bsg_job->reply->result = ret;
#else
		bsg_reply->result = ret;
#endif
		break;
	}
	return ret;
}

int
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
qla24xx_bsg_timeout(struct fc_bsg_job *bsg_job)
#else
qla24xx_bsg_timeout(struct bsg_job *bsg_job)
#endif
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	scsi_qla_host_t *vha = shost_priv(bsg_job->shost);
#else
	struct fc_bsg_request *bsg_request = bsg_job->request;
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
	scsi_qla_host_t *vha = shost_priv(fc_bsg_to_shost(bsg_job));
#endif
	struct qla_hw_data *ha = vha->hw;
	srb_t *sp;
	int cnt, que;
	unsigned long flags;
	struct req_que *req;

	/* find the bsg job from the active list of commands */
	spin_lock_irqsave(&ha->hardware_lock, flags);
	for (que = 0; que < ha->max_req_queues; que++) {
		req = ha->req_q_map[que];
		if (!req)
			continue;

		for (cnt = 1; cnt < MAX_OUTSTANDING_COMMANDS; cnt++) {
			sp = req->outstanding_cmds[cnt];
			if (sp) {
				if (((sp->type == SRB_CT_CMD) ||
					(sp->type == SRB_ELS_CMD_HST))
					&& (sp->u.bsg_job == bsg_job)) {
					spin_unlock_irqrestore(&ha->hardware_lock, flags);
					if (ha->isp_ops->abort_command(sp)) {
						ql_log(ql_log_warn, vha, 0x7089,
						    "mbx abort_command "
						    "failed.\n");
						bsg_job->req->errors =
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
						bsg_job->reply->result = -EIO;
#else
						bsg_reply->result = -EIO;
#endif
					} else {
						ql_dbg(ql_dbg_user, vha, 0x708a,
						    "mbx abort_command "
						    "success.\n");
						bsg_job->req->errors =
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
						bsg_job->reply->result = 0;
#else
						bsg_reply->result = 0;
#endif
					}
					spin_lock_irqsave(&ha->hardware_lock, flags);
					goto done;
				}
			}
		}
	}
	spin_unlock_irqrestore(&ha->hardware_lock, flags);
	ql_log(ql_log_info, vha, 0x708b, "SRB not found to abort.\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	bsg_job->req->errors = bsg_job->reply->result = -ENXIO;
#else
	bsg_job->req->errors = bsg_reply->result = -ENXIO;
#endif
	return 0;

done:
	spin_unlock_irqrestore(&ha->hardware_lock, flags);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	if (bsg_job->request->msgcode == FC_BSG_HST_CT)
#else
	if (bsg_request->msgcode == FC_BSG_HST_CT)
#endif
		kfree(sp->fcport);
	mempool_free(sp, ha->srb_mempool);
	return 0;
}
