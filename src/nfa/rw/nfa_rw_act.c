/******************************************************************************
 *
 *  Copyright (C) 2010-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains the action functions the NFA_RW state machine.
 *
 ******************************************************************************/
#include <string.h>
#include "nfa_rw_int.h"
#include "nfa_dm_int.h"
#include "nfa_sys_int.h"
#include "nfa_mem_co.h"
#include "ndef_utils.h"
#include "rw_api.h"

/* Local static function prototypes */
static tNFC_STATUS nfa_rw_start_ndef_read(void);
static tNFC_STATUS nfa_rw_start_ndef_write(void);
static tNFC_STATUS nfa_rw_start_ndef_detection(void);
static tNFC_STATUS nfa_rw_config_tag_ro(BOOLEAN b_hard_lock);
static BOOLEAN     nfa_rw_op_req_while_busy(tNFA_RW_MSG *p_data);
static void        nfa_rw_error_cleanup (UINT8 event);

/*******************************************************************************
**
** Function         nfa_rw_free_ndef_rx_buf
**
** Description      Free buffer allocated to hold incoming NDEF message
**
** Returns          Nothing
**
*******************************************************************************/
void nfa_rw_free_ndef_rx_buf(void)
{
    if (nfa_rw_cb.p_ndef_buf)
    {
        nfa_mem_co_free(nfa_rw_cb.p_ndef_buf);
        nfa_rw_cb.p_ndef_buf = NULL;
    }
}

/*******************************************************************************
**
** Function         nfa_rw_store_ndef_rx_buf
**
** Description      Store data into NDEF buffer
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_store_ndef_rx_buf (tRW_DATA *p_rw_data)
{
    UINT8      *p;

    p = (UINT8 *)(p_rw_data->data.p_data + 1) + p_rw_data->data.p_data->offset;

    /* Save data into buffer */
    memcpy(&nfa_rw_cb.p_ndef_buf[nfa_rw_cb.ndef_rd_offset], p, p_rw_data->data.p_data->len);
    nfa_rw_cb.ndef_rd_offset += p_rw_data->data.p_data->len;

    GKI_freebuf(p_rw_data->data.p_data);
    p_rw_data->data.p_data = NULL;
}

/*******************************************************************************
**
** Function         nfa_rw_send_data_to_upper
**
** Description      Send data to upper layer
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_send_data_to_upper (tRW_DATA *p_rw_data)
{
    tNFA_CONN_EVT_DATA conn_evt_data;

    if (  (p_rw_data->status == NFC_STATUS_TIMEOUT)
        ||(p_rw_data->data.p_data == NULL) )
        return;

    /* Notify conn cback of NFA_DATA_EVT */
    conn_evt_data.data.p_data = (UINT8 *)(p_rw_data->data.p_data + 1) + p_rw_data->data.p_data->offset;
    conn_evt_data.data.len    = p_rw_data->data.p_data->len;

    nfa_dm_act_conn_cback_notify(NFA_DATA_EVT, &conn_evt_data);

    GKI_freebuf(p_rw_data->data.p_data);
    p_rw_data->data.p_data = NULL;
}

/*******************************************************************************
**
** Function         nfa_rw_error_cleanup
**
** Description      Handle failure - signal command complete and notify app
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_error_cleanup (UINT8 event)
{
    tNFA_CONN_EVT_DATA conn_evt_data;

    nfa_rw_command_complete();

    conn_evt_data.status = NFA_STATUS_FAILED;

    nfa_dm_act_conn_cback_notify (event, &conn_evt_data);
}

/*******************************************************************************
**
** Function         nfa_rw_check_start_presence_check_timer
**
** Description      Start timer for presence check
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_check_start_presence_check_timer (void)
{
#if (defined (NFA_DM_AUTO_PRESENCE_CHECK) && (NFA_DM_AUTO_PRESENCE_CHECK == TRUE))
    if (nfa_rw_cb.flags & NFA_RW_FL_NOT_EXCL_RF_MODE)
    {
        NFA_TRACE_DEBUG0("Starting presence check timer...");
        nfa_sys_start_timer(&nfa_rw_cb.tle, NFA_RW_PRESENCE_CHECK_TICK_EVT, NFA_RW_PRESENCE_CHECK_INTERVAL);
    }
#endif   /* NFA_DM_AUTO_PRESENCE_CHECK  */
}

/*******************************************************************************
**
** Function         nfa_rw_stop_presence_check_timer
**
** Description      Stop timer for presence check
**
** Returns          Nothing
**
*******************************************************************************/
void nfa_rw_stop_presence_check_timer(void)
{
    nfa_sys_stop_timer(&nfa_rw_cb.tle);
    NFA_TRACE_DEBUG0("Stopped presence check timer (if started)");
}

/*******************************************************************************
**
** Function         nfa_rw_handle_ndef_detect
**
** Description      Handler for NDEF detection reader/writer event
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_handle_ndef_detect(tRW_EVENT event, tRW_DATA *p_rw_data)
{
    tNFA_CONN_EVT_DATA conn_evt_data;

    NFA_TRACE_DEBUG3("NDEF Detection completed: cur_size=%i, max_size=%i, flags=0x%x",
        p_rw_data->ndef.cur_size, p_rw_data->ndef.max_size, p_rw_data->ndef.flags);

    /* Check if NDEF detection succeeded */
    if (p_rw_data->ndef.status == NFC_STATUS_OK)
    {
        /* Set NDEF detection state */
        nfa_rw_cb.ndef_st = NFA_RW_NDEF_ST_TRUE;

        /* Store ndef properties */
        conn_evt_data.ndef_detect.status = NFA_STATUS_OK;
        conn_evt_data.ndef_detect.protocol = p_rw_data->ndef.protocol;
        conn_evt_data.ndef_detect.cur_size = nfa_rw_cb.ndef_cur_size = p_rw_data->ndef.cur_size;
        conn_evt_data.ndef_detect.max_size = nfa_rw_cb.ndef_max_size = p_rw_data->ndef.max_size;
        conn_evt_data.ndef_detect.flags    = p_rw_data->ndef.flags;

        if (p_rw_data->ndef.flags & RW_NDEF_FL_READ_ONLY)
            nfa_rw_cb.flags |= NFA_RW_FL_TAG_IS_READONLY;
        else
            nfa_rw_cb.flags &= ~NFA_RW_FL_TAG_IS_READONLY;

        /* Determine what operation triggered the NDEF detection procedure */
        if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
        {
            /* if ndef detection was done as part of ndef-read operation, then perform ndef read now */
            if ((conn_evt_data.status = nfa_rw_start_ndef_read()) != NFA_STATUS_OK)
            {
                /* Failed to start NDEF Read */

                /* Command complete - perform cleanup, notify app */
                nfa_rw_command_complete();
                nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
            }
        }
        else if (nfa_rw_cb.cur_op == NFA_RW_OP_WRITE_NDEF)
        {
            /* if ndef detection was done as part of ndef-write operation, then perform ndef write now */
            if ((conn_evt_data.status = nfa_rw_start_ndef_write()) != NFA_STATUS_OK)
            {
                /* Failed to start NDEF Write.  */

                /* Command complete - perform cleanup, notify app */
                nfa_rw_command_complete();
                nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);
            }
        }
        else
        {
            /* current op was stand-alone NFA_DetectNDef. Command complete - perform cleanup and notify app */
            nfa_rw_cb.cur_op = NFA_RW_OP_MAX;
            nfa_rw_command_complete();

            nfa_dm_act_conn_cback_notify(NFA_NDEF_DETECT_EVT, &conn_evt_data);
        }
    }
    else
    {
        /* NDEF detection failed... */

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        nfa_rw_cb.ndef_st = NFA_RW_NDEF_ST_FALSE;

        if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
        {
            /* if ndef detection was done as part of ndef-read operation, then notify NDEF handlers of failure */
            nfa_dm_ndef_handle_message(NFA_STATUS_FAILED, NULL, 0);

            /* Notify app of read status */
            conn_evt_data.status = NFC_STATUS_FAILED;
            nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        }
        else if (nfa_rw_cb.cur_op == NFA_RW_OP_WRITE_NDEF)
        {
            /* if ndef detection was done as part of ndef-write operation, then notify app of failure */
            conn_evt_data.status = NFA_STATUS_FAILED;
            nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);
        }
        else if (nfa_rw_cb.cur_op == NFA_RW_OP_DETECT_NDEF)
        {
            conn_evt_data.ndef_detect.protocol = p_rw_data->ndef.protocol;
            /* current op was stand-alone NFA_DetectNDef. Notify app of failure */
            conn_evt_data.ndef_detect.status = NFA_STATUS_FAILED;
            if (p_rw_data->ndef.status == NFC_STATUS_TIMEOUT)
            {
                /* Tag could have moved away */
                conn_evt_data.ndef_detect.cur_size = 0;
                conn_evt_data.ndef_detect.max_size = 0;
                conn_evt_data.ndef_detect.flags    = RW_NDEF_FL_UNKNOWN;
            }
            else
            {
                /* NDEF Detection failed for other reasons */
                conn_evt_data.ndef_detect.cur_size = nfa_rw_cb.ndef_cur_size = p_rw_data->ndef.cur_size;
                conn_evt_data.ndef_detect.max_size = nfa_rw_cb.ndef_max_size = p_rw_data->ndef.max_size;
                conn_evt_data.ndef_detect.flags    = p_rw_data->ndef.flags;
            }
            nfa_dm_act_conn_cback_notify(NFA_NDEF_DETECT_EVT, &conn_evt_data);
        }

        nfa_rw_cb.cur_op = NFA_RW_OP_MAX; /* clear current operation */
    }
}

/*******************************************************************************
**
** Function         nfa_rw_handle_tlv_detect
**
** Description      Handler for TLV detection reader/writer event
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_handle_tlv_detect(tRW_EVENT event, tRW_DATA *p_rw_data)
{
    tNFA_CONN_EVT_DATA conn_evt_data;

    /* Set TLV detection state */
    if (nfa_rw_cb.cur_op == NFA_RW_OP_SET_TAG_RO)
    {
        if(nfa_rw_cb.tlv_st == NFA_RW_TLV_DETECT_ST_OP_NOT_STARTED)
        {
            nfa_rw_cb.tlv_st = NFA_RW_TLV_DETECT_ST_LOCK_TLV_OP_COMPLETE;
        }
        else
        {
            nfa_rw_cb.tlv_st = NFA_RW_TLV_DETECT_ST_COMPLETE;
        }
    }
    else
    {
        if(nfa_rw_cb.cur_op == NFA_RW_OP_DETECT_LOCK_TLV)
        {
            nfa_rw_cb.tlv_st |= NFA_RW_TLV_DETECT_ST_LOCK_TLV_OP_COMPLETE;
        }
        else if(nfa_rw_cb.cur_op == NFA_RW_OP_DETECT_MEM_TLV)
        {
            nfa_rw_cb.tlv_st |= NFA_RW_TLV_DETECT_ST_MEM_TLV_OP_COMPLETE;
        }
    }

    /* Check if TLV detection succeeded */
    if (p_rw_data->tlv.status == NFC_STATUS_OK)
    {
        NFA_TRACE_DEBUG1("TLV Detection succeeded: num_bytes=%i",p_rw_data->tlv.num_bytes);

        /* Store tlv properties */
        conn_evt_data.tlv_detect.status     = NFA_STATUS_OK;
        conn_evt_data.tlv_detect.protocol   = p_rw_data->tlv.protocol;
        conn_evt_data.tlv_detect.num_bytes  = p_rw_data->tlv.num_bytes;


        /* Determine what operation triggered the TLV detection procedure */
        if(nfa_rw_cb.cur_op == NFA_RW_OP_SET_TAG_RO)
        {
            if (nfa_rw_config_tag_ro(nfa_rw_cb.b_hard_lock) != NFC_STATUS_OK)
            {
                /* Failed to set tag read only */
                conn_evt_data.tlv_detect.status = NFA_STATUS_FAILED;
                nfa_dm_act_conn_cback_notify(NFA_SET_TAG_RO_EVT, &conn_evt_data);
            }
        }
        else
        {
            /* current op was stand-alone NFA_DetectTlv. Command complete - perform cleanup and notify app */
            nfa_rw_command_complete();
            nfa_dm_act_conn_cback_notify(NFA_TLV_DETECT_EVT, &conn_evt_data);
        }
    }

    /* Handle failures */
    if (p_rw_data->tlv.status != NFC_STATUS_OK)
    {
        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();

        conn_evt_data.tlv_detect.status = NFA_STATUS_FAILED;
        if(  (nfa_rw_cb.cur_op == NFA_RW_OP_DETECT_LOCK_TLV)
           ||(nfa_rw_cb.cur_op == NFA_RW_OP_DETECT_MEM_TLV) )
        {
            nfa_dm_act_conn_cback_notify(NFA_TLV_DETECT_EVT, &conn_evt_data);
        }
        else if(nfa_rw_cb.cur_op == NFA_RW_OP_SET_TAG_RO)
        {
            if (nfa_rw_config_tag_ro(nfa_rw_cb.b_hard_lock) != NFC_STATUS_OK)
            {
                /* Failed to set tag read only */
                conn_evt_data.tlv_detect.status = NFA_STATUS_FAILED;
                nfa_dm_act_conn_cback_notify(NFA_SET_TAG_RO_EVT, &conn_evt_data);
            }
        }
    }
}


/*******************************************************************************
**
** Function         nfa_rw_handle_presence_check_rsp
**
** Description      Handler RW_T#t_PRESENCE_CHECK_EVT
**
** Returns          Nothing
**
*******************************************************************************/
void nfa_rw_handle_presence_check_rsp (tNFC_STATUS status)
{
    BT_HDR *p_pending_msg;

    if (status == NFA_STATUS_OK)
    {
        /* Clear the BUSY flag and restart the presence-check timer */
        nfa_rw_command_complete();
    }
    else
    {
        /* If presence check failed just clear the BUSY flag */
        nfa_rw_cb.flags &= ~NFA_RW_FL_API_BUSY;
    }

    /* Handle presence check due to auto-presence-check  */
    if (nfa_rw_cb.flags & NFA_RW_FL_AUTO_PRESENCE_CHECK_BUSY)
    {
        nfa_rw_cb.flags &= ~NFA_RW_FL_AUTO_PRESENCE_CHECK_BUSY;

        /* If an API was called during auto-presence-check, then handle it now */
        if (nfa_rw_cb.p_pending_msg)
        {
            /* If NFA_RwPresenceCheck was called during auto-presence-check, notify app of result */
            if (nfa_rw_cb.p_pending_msg->op_req.op == NFA_RW_OP_PRESENCE_CHECK)
            {
                /* Notify app of presence check status */
                nfa_dm_act_conn_cback_notify(NFA_PRESENCE_CHECK_EVT, (tNFA_CONN_EVT_DATA *)&status);
                GKI_freebuf(nfa_rw_cb.p_pending_msg);
                nfa_rw_cb.p_pending_msg = NULL;
            }
            /* For all other APIs called during auto-presence check, perform the command now (if tag is still present) */
            else if (status == NFC_STATUS_OK)
            {
                NFA_TRACE_DEBUG0("Performing deferred operation after presence check...");
                p_pending_msg = (BT_HDR *)nfa_rw_cb.p_pending_msg;
                nfa_rw_cb.p_pending_msg = NULL;
                nfa_rw_handle_event(p_pending_msg);
            }
            else
            {
                /* Tag no longer present. Free command for pending API command */
                GKI_freebuf(nfa_rw_cb.p_pending_msg);
                nfa_rw_cb.p_pending_msg = NULL;
            }
        }

        /* Auto-presence check failed. Deactivate */
        if (status != NFC_STATUS_OK)
        {
            NFA_TRACE_DEBUG0("Auto presence check failed. Deactivating...");
            nfa_dm_rf_deactivate (NFA_DEACTIVATE_TYPE_DISCOVERY);
        }
    }
    /* Handle presence check due to NFA_RwPresenceCheck API call */
    else
    {
        /* Notify app of presence check status */
        nfa_dm_act_conn_cback_notify(NFA_PRESENCE_CHECK_EVT, (tNFA_CONN_EVT_DATA *)&status);

        /* If in normal mode (not-exclusive RF mode) then deactivate the link if presence check failed */
        if ((nfa_rw_cb.flags & NFA_RW_FL_NOT_EXCL_RF_MODE) && (status != NFC_STATUS_OK))
        {
            NFA_TRACE_DEBUG0("Presence check failed. Deactivating...");
            nfa_dm_rf_deactivate (NFA_DEACTIVATE_TYPE_DISCOVERY);
        }
    }
}

/*******************************************************************************
**
** Function         nfa_rw_handle_t1t_evt
**
** Description      Handler for Type-1 tag reader/writer events
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_handle_t1t_evt (tRW_EVENT event, tRW_DATA *p_rw_data)
{
    tNFA_CONN_EVT_DATA conn_evt_data;
#ifdef NXP_EXT
    tNFA_TAG_PARAMS tag_params;
    UINT8 *p_rid_rsp;
#endif
    conn_evt_data.status = p_rw_data->data.status;
    switch (event)
    {
#ifdef NXP_EXT
    case RW_T1T_RID_EVT:
        /* Assume the data is just the response byte sequence */
        p_rid_rsp = (UINT8 *) (p_rw_data->data.p_data + 1) + p_rw_data->data.p_data->offset;
        /* Fetch HR from RID response message */
        STREAM_TO_ARRAY (tag_params.t1t.hr,  p_rid_rsp, T1T_HR_LEN);
        /* Fetch UID0-3 from RID response message */
        STREAM_TO_ARRAY (tag_params.t1t.uid,  p_rid_rsp, T1T_CMD_UID_LEN);

        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();

        nfa_dm_notify_activation_status(NFA_STATUS_OK, &tag_params);
        break;
#endif
    case RW_T1T_RALL_CPLT_EVT:
    case RW_T1T_READ_CPLT_EVT:
    case RW_T1T_RSEG_CPLT_EVT:
    case RW_T1T_READ8_CPLT_EVT:
        nfa_rw_send_data_to_upper (p_rw_data);

        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T1T_WRITE_E_CPLT_EVT:
    case RW_T1T_WRITE_NE_CPLT_EVT:
    case RW_T1T_WRITE_E8_CPLT_EVT:
    case RW_T1T_WRITE_NE8_CPLT_EVT:
        nfa_rw_send_data_to_upper (p_rw_data);

        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T1T_TLV_DETECT_EVT:
        nfa_rw_handle_tlv_detect(event, p_rw_data);
        break;

    case RW_T1T_NDEF_DETECT_EVT:
        nfa_rw_cb.tlv_st = NFA_RW_TLV_DETECT_ST_COMPLETE;
        nfa_rw_handle_ndef_detect(event, p_rw_data);
        break;

    case RW_T1T_NDEF_READ_EVT:
        nfa_rw_cb.tlv_st = NFA_RW_TLV_DETECT_ST_COMPLETE;
        if (p_rw_data->status == NFC_STATUS_OK)
        {
            /* Process the ndef record */
            nfa_dm_ndef_handle_message(NFA_STATUS_OK, nfa_rw_cb.p_ndef_buf, nfa_rw_cb.ndef_cur_size);
        }
        else
        {
            /* Notify app of failure */
            if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
            {
                /* If current operation is READ_NDEF, then notify ndef handlers of failure */
                nfa_dm_ndef_handle_message(NFA_STATUS_FAILED, NULL, 0);
            }
        }

        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);

        /* Free ndef buffer */
        nfa_rw_free_ndef_rx_buf();
        break;

    case RW_T1T_NDEF_WRITE_EVT:
        if (p_rw_data->data.status != NFA_STATUS_OK)
            nfa_rw_cb.ndef_st = NFA_RW_NDEF_ST_UNKNOWN;
        nfa_rw_cb.tlv_st = NFA_RW_TLV_DETECT_ST_COMPLETE;


        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();

        /* Notify app */
        conn_evt_data.status = (p_rw_data->data.status == NFC_STATUS_OK) ? NFA_STATUS_OK : NFA_STATUS_FAILED;
        if (nfa_rw_cb.cur_op == NFA_RW_OP_WRITE_NDEF)
        {
            /* Update local cursize of ndef message */
            nfa_rw_cb.ndef_cur_size = nfa_rw_cb.ndef_wr_len;
        }

        /* Notify app of ndef write complete status */
        nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T1T_SET_TAG_RO_EVT:
        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_dm_act_conn_cback_notify(NFA_SET_TAG_RO_EVT, &conn_evt_data);
        break;

    case RW_T1T_RAW_FRAME_EVT:
        nfa_rw_send_data_to_upper (p_rw_data);

        /* Command complete - perform cleanup */
        nfa_rw_command_complete();
        break;

    case RW_T1T_PRESENCE_CHECK_EVT:             /* Presence check completed */
        nfa_rw_handle_presence_check_rsp(p_rw_data->status);
        break;

    case RW_T1T_FORMAT_CPLT_EVT:
        if (p_rw_data->data.status == NFA_STATUS_OK)
            nfa_rw_cb.ndef_st = NFA_RW_NDEF_ST_UNKNOWN;

        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_dm_act_conn_cback_notify(NFA_FORMAT_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T1T_INTF_ERROR_EVT:
        nfa_dm_act_conn_cback_notify(NFA_RW_INTF_ERROR_EVT, &conn_evt_data);
        break;
    }
}

/*******************************************************************************
**
** Function         nfa_rw_handle_t2t_evt
**
** Description      Handler for Type-2 tag reader/writer events
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_handle_t2t_evt (tRW_EVENT event, tRW_DATA *p_rw_data)
{
    tNFA_CONN_EVT_DATA conn_evt_data;

    conn_evt_data.status = p_rw_data->data.status;
    switch (event)
    {
    case RW_T2T_READ_CPLT_EVT:              /* Read completed          */
        nfa_rw_send_data_to_upper (p_rw_data);
        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T2T_WRITE_CPLT_EVT:             /* Write completed         */
        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T2T_SELECT_CPLT_EVT:            /* Sector select completed */
        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_dm_act_conn_cback_notify(NFA_SELECT_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T2T_NDEF_DETECT_EVT:            /* NDEF detection complete */
        nfa_rw_handle_ndef_detect(event, p_rw_data);
        break;

    case RW_T2T_TLV_DETECT_EVT:             /* Lock control/Mem/Prop tlv detection complete */
        nfa_rw_handle_tlv_detect(event, p_rw_data);
        break;

    case RW_T2T_NDEF_READ_EVT:              /* NDEF read completed     */
        if (p_rw_data->status == NFC_STATUS_OK)
        {
            /* Process the ndef record */
            nfa_dm_ndef_handle_message(NFA_STATUS_OK, nfa_rw_cb.p_ndef_buf, nfa_rw_cb.ndef_cur_size);
        }
        else
        {
            /* Notify app of failure */
            if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
            {
                /* If current operation is READ_NDEF, then notify ndef handlers of failure */
                nfa_dm_ndef_handle_message(NFA_STATUS_FAILED, NULL, 0);
            }
        }

        /* Notify app of read status */
        conn_evt_data.status = p_rw_data->status;
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        /* Free ndef buffer */
        nfa_rw_free_ndef_rx_buf();

        /* Command complete - perform cleanup */
        nfa_rw_command_complete();
        break;

    case RW_T2T_NDEF_WRITE_EVT:             /* NDEF write complete     */

        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();

        /* Notify app */
        conn_evt_data.status = (p_rw_data->data.status == NFC_STATUS_OK) ? NFA_STATUS_OK : NFA_STATUS_FAILED;
        if (nfa_rw_cb.cur_op == NFA_RW_OP_WRITE_NDEF)
        {
            /* Update local cursize of ndef message */
            nfa_rw_cb.ndef_cur_size = nfa_rw_cb.ndef_wr_len;
        }

        /* Notify app of ndef write complete status */
        nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);

        break;

    case RW_T2T_SET_TAG_RO_EVT:
        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_dm_act_conn_cback_notify(NFA_SET_TAG_RO_EVT, &conn_evt_data);
        break;

    case RW_T2T_RAW_FRAME_EVT:
        nfa_rw_send_data_to_upper (p_rw_data);

        /* Command complete - perform cleanup */
        nfa_rw_command_complete();
        break;

    case RW_T2T_PRESENCE_CHECK_EVT:             /* Presence check completed */
        nfa_rw_handle_presence_check_rsp(p_rw_data->status);
        break;

    case RW_T2T_FORMAT_CPLT_EVT:
        if (p_rw_data->data.status == NFA_STATUS_OK)
            nfa_rw_cb.ndef_st = NFA_RW_NDEF_ST_UNKNOWN;

        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_dm_act_conn_cback_notify(NFA_FORMAT_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T2T_INTF_ERROR_EVT:
        nfa_dm_act_conn_cback_notify(NFA_RW_INTF_ERROR_EVT, &conn_evt_data);
        break;
    }
}

/*******************************************************************************
**
** Function         nfa_rw_handle_t3t_evt
**
** Description      Handler for Type-3 tag reader/writer events
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_handle_t3t_evt (tRW_EVENT event, tRW_DATA *p_rw_data)
{
    tNFA_CONN_EVT_DATA conn_evt_data;
    tNFA_TAG_PARAMS tag_params;

    switch (event)
    {
    case RW_T3T_NDEF_DETECT_EVT:            /* NDEF detection complete */
        nfa_rw_handle_ndef_detect(event, p_rw_data);
        break;

    case RW_T3T_UPDATE_CPLT_EVT:        /* Write completed */
        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();

        /* Notify app */
        conn_evt_data.status = (p_rw_data->data.status == NFC_STATUS_OK) ? NFA_STATUS_OK : NFA_STATUS_FAILED;
        if (nfa_rw_cb.cur_op == NFA_RW_OP_WRITE_NDEF)
        {
            /* Update local cursize of ndef message */
            nfa_rw_cb.ndef_cur_size = nfa_rw_cb.ndef_wr_len;
        }

        /* Notify app of ndef write complete status */
        nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);

        break;

    case RW_T3T_CHECK_CPLT_EVT:         /* Read completed */
        if (p_rw_data->status == NFC_STATUS_OK)
        {
            /* Process the ndef record */
            nfa_dm_ndef_handle_message(NFA_STATUS_OK, nfa_rw_cb.p_ndef_buf, nfa_rw_cb.ndef_cur_size);
        }
        else
        {
            /* Notify app of failure */
            if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
            {
                /* If current operation is READ_NDEF, then notify ndef handlers of failure */
                nfa_dm_ndef_handle_message(NFA_STATUS_FAILED, NULL, 0);
            }
        }

        /* Free ndef buffer */
        nfa_rw_free_ndef_rx_buf();

        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        conn_evt_data.status = p_rw_data->status;
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T3T_CHECK_EVT:                  /* Segment of data received from type 3 tag */
        if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
        {
            nfa_rw_store_ndef_rx_buf (p_rw_data);
        }
        else
        {
            nfa_rw_send_data_to_upper (p_rw_data);
        }
        break;

    case RW_T3T_RAW_FRAME_EVT:              /* SendRawFrame response */
        nfa_rw_send_data_to_upper (p_rw_data);

        /* Command complete - perform cleanup */
        nfa_rw_command_complete();
        break;

    case RW_T3T_PRESENCE_CHECK_EVT:             /* Presence check completed */
        nfa_rw_handle_presence_check_rsp(p_rw_data->status);
        break;

    case RW_T3T_GET_SYSTEM_CODES_EVT:           /* Presence check completed */
        /* Command complete - perform cleanup */
        nfa_rw_command_complete();

        /* System codes retrieved - notify app of ACTIVATION */
        if (p_rw_data->status == NFC_STATUS_OK)
        {
            tag_params.t3t.num_system_codes = p_rw_data->t3t_sc.num_system_codes;
            tag_params.t3t.p_system_codes = p_rw_data->t3t_sc.p_system_codes;
        }
        else
        {
            tag_params.t3t.num_system_codes = 0;
            tag_params.t3t.p_system_codes = NULL;
        }

        nfa_dm_notify_activation_status(NFA_STATUS_OK, &tag_params);
        break;

    case RW_T3T_FORMAT_CPLT_EVT:        /* Format completed */
        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();

        /* Notify app */
        conn_evt_data.status = (p_rw_data->data.status == NFC_STATUS_OK) ? NFA_STATUS_OK : NFA_STATUS_FAILED;

        /* Notify app of ndef write complete status */
        nfa_dm_act_conn_cback_notify(NFA_FORMAT_CPLT_EVT, &conn_evt_data);
        break;


    case RW_T3T_INTF_ERROR_EVT:
        conn_evt_data.status = p_rw_data->status;
        nfa_dm_act_conn_cback_notify(NFA_RW_INTF_ERROR_EVT, &conn_evt_data);
        break;
    }

}


/*******************************************************************************
**
** Function         nfa_rw_handle_t4t_evt
**
** Description      Handler for Type-4 tag reader/writer events
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_handle_t4t_evt (tRW_EVENT event, tRW_DATA *p_rw_data)
{
    tNFA_CONN_EVT_DATA conn_evt_data;

    switch (event)
    {
    case RW_T4T_NDEF_DETECT_EVT :           /* Result of NDEF detection procedure */
        nfa_rw_handle_ndef_detect(event, p_rw_data);
        break;

#ifdef NXP_EXT
    case RW_T4T_NDEF_FORMAT_CPLT_EVT:
        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_rw_cb.cur_op = NFA_RW_OP_MAX;
        nfa_rw_cb.ndef_cur_size = p_rw_data->ndef.cur_size;
        nfa_rw_cb.ndef_max_size = p_rw_data->ndef.max_size;
        conn_evt_data.status = (p_rw_data->status == NFC_STATUS_OK) ? NFA_STATUS_OK : NFA_STATUS_FAILED;

        nfa_dm_act_conn_cback_notify(NFA_FORMAT_CPLT_EVT, &conn_evt_data);
        break;
#endif

    case RW_T4T_NDEF_READ_EVT:              /* Segment of data received from type 4 tag */
        if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
        {
            nfa_rw_store_ndef_rx_buf (p_rw_data);
        }
        else
        {
            nfa_rw_send_data_to_upper (p_rw_data);
        }
        break;

    case RW_T4T_NDEF_READ_CPLT_EVT:         /* Read operation completed           */
        if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
        {
            nfa_rw_store_ndef_rx_buf (p_rw_data);

            /* Process the ndef record */
            nfa_dm_ndef_handle_message (NFA_STATUS_OK, nfa_rw_cb.p_ndef_buf, nfa_rw_cb.ndef_cur_size);

            /* Free ndef buffer */
            nfa_rw_free_ndef_rx_buf();
        }
        else
        {
            nfa_rw_send_data_to_upper (p_rw_data);
        }

        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_rw_cb.cur_op = NFA_RW_OP_MAX;
        conn_evt_data.status = NFC_STATUS_OK;
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T4T_NDEF_READ_FAIL_EVT:         /* Read operation failed              */
        if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
        {
            /* If current operation is READ_NDEF, then notify ndef handlers of failure */
            nfa_dm_ndef_handle_message(NFA_STATUS_FAILED, NULL, 0);

            /* Free ndef buffer */
            nfa_rw_free_ndef_rx_buf();
        }

        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_rw_cb.cur_op = NFA_RW_OP_MAX;
        conn_evt_data.status = NFA_STATUS_FAILED;
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T4T_NDEF_UPDATE_CPLT_EVT:       /* Update operation completed         */
    case RW_T4T_NDEF_UPDATE_FAIL_EVT:       /* Update operation failed            */

        if (nfa_rw_cb.cur_op == NFA_RW_OP_WRITE_NDEF)
        {
            /* Update local cursize of ndef message */
            nfa_rw_cb.ndef_cur_size = nfa_rw_cb.ndef_wr_len;
        }

        /* Notify app */
        if (event == RW_T4T_NDEF_UPDATE_CPLT_EVT)
            conn_evt_data.status = NFA_STATUS_OK;
        else
            conn_evt_data.status = NFA_STATUS_FAILED;

        /* Command complete - perform cleanup, notify the app */
        nfa_rw_command_complete();
        nfa_rw_cb.cur_op = NFA_RW_OP_MAX;
        nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);
        break;

    case RW_T4T_RAW_FRAME_EVT:              /* Raw Frame data event         */
        nfa_rw_send_data_to_upper (p_rw_data);

        /* Command complete - perform cleanup */
        nfa_rw_command_complete();
        nfa_rw_cb.cur_op = NFA_RW_OP_MAX;
        break;

    case RW_T4T_INTF_ERROR_EVT:             /* RF Interface error event         */
        /* if NDEF operation */
        if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
        {
            /* If current operation is READ_NDEF, then notify ndef handlers of failure */
            nfa_dm_ndef_handle_message(NFA_STATUS_FAILED, NULL, 0);

            /* Free ndef buffer */
            nfa_rw_free_ndef_rx_buf();
        }
        else if (nfa_rw_cb.cur_op == NFA_RW_OP_WRITE_NDEF)
        {
            /* Update local cursize of ndef message */
            nfa_rw_cb.ndef_cur_size = nfa_rw_cb.ndef_wr_len;
        }

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        conn_evt_data.status = p_rw_data->status;

        if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
        {
            nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        }
        else if (nfa_rw_cb.cur_op == NFA_RW_OP_WRITE_NDEF)
        {
            nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);
        }
        else if (nfa_rw_cb.cur_op == NFA_RW_OP_DETECT_NDEF)
        {
            conn_evt_data.ndef_detect.cur_size = 0;
            conn_evt_data.ndef_detect.max_size = 0;
            conn_evt_data.ndef_detect.flags    = RW_NDEF_FL_UNKNOWN;
            nfa_dm_act_conn_cback_notify(NFA_NDEF_DETECT_EVT, &conn_evt_data);
        }
        else
            nfa_dm_act_conn_cback_notify(NFA_RW_INTF_ERROR_EVT, &conn_evt_data);
        nfa_rw_cb.cur_op = NFA_RW_OP_MAX;
        break;


    case RW_T4T_PRESENCE_CHECK_EVT:             /* Presence check completed */
        nfa_rw_handle_presence_check_rsp(p_rw_data->status);
        break;

    default:
        NFA_TRACE_DEBUG1("nfa_rw_handle_t4t_evt(); Unhandled RW event 0x%X", event);
        break;
    }
}

/*******************************************************************************
**
** Function         nfa_rw_handle_i93_evt
**
** Description      Handler for ISO 15693 tag reader/writer events
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_handle_i93_evt (tRW_EVENT event, tRW_DATA *p_rw_data)
{
    tNFA_CONN_EVT_DATA conn_evt_data;
    tNFA_TAG_PARAMS    i93_params;

    switch (event)
    {
    case RW_I93_NDEF_DETECT_EVT :           /* Result of NDEF detection procedure */
        nfa_rw_handle_ndef_detect(event, p_rw_data);
        break;

    case RW_I93_NDEF_READ_EVT:              /* Segment of data received from type 4 tag */
        if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
        {
            nfa_rw_store_ndef_rx_buf (p_rw_data);
        }
        else
        {
            nfa_rw_send_data_to_upper (p_rw_data);
        }
        break;

    case RW_I93_NDEF_READ_CPLT_EVT:         /* Read operation completed           */
        if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
        {
            nfa_rw_store_ndef_rx_buf (p_rw_data);

            /* Process the ndef record */
            nfa_dm_ndef_handle_message (NFA_STATUS_OK, nfa_rw_cb.p_ndef_buf, nfa_rw_cb.ndef_cur_size);

            /* Free ndef buffer */
            nfa_rw_free_ndef_rx_buf();
        }
        else
        {
            nfa_rw_send_data_to_upper (p_rw_data);
        }

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        nfa_rw_cb.cur_op = NFA_RW_OP_MAX; /* clear current operation */
        conn_evt_data.status = NFC_STATUS_OK;
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        break;

    case RW_I93_NDEF_READ_FAIL_EVT:         /* Read operation failed              */
        if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
        {
            /* If current operation is READ_NDEF, then notify ndef handlers of failure */
            nfa_dm_ndef_handle_message(NFA_STATUS_FAILED, NULL, 0);

            /* Free ndef buffer */
            nfa_rw_free_ndef_rx_buf();
        }

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        nfa_rw_cb.cur_op = NFA_RW_OP_MAX; /* clear current operation */
        conn_evt_data.status = NFA_STATUS_FAILED;
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        break;

    case RW_I93_NDEF_UPDATE_CPLT_EVT:       /* Update operation completed         */
    case RW_I93_NDEF_UPDATE_FAIL_EVT:       /* Update operation failed            */

        if (nfa_rw_cb.cur_op == NFA_RW_OP_WRITE_NDEF)
        {
            /* Update local cursize of ndef message */
            nfa_rw_cb.ndef_cur_size = nfa_rw_cb.ndef_wr_len;
        }

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        nfa_rw_cb.cur_op = NFA_RW_OP_MAX; /* clear current operation */

        if (event == RW_I93_NDEF_UPDATE_CPLT_EVT)
            conn_evt_data.status = NFA_STATUS_OK;
        else
            conn_evt_data.status = NFA_STATUS_FAILED;

        /* Notify app of ndef write complete status */
        nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);
        break;

    case RW_I93_RAW_FRAME_EVT:              /* Raw Frame data event         */
        nfa_rw_send_data_to_upper (p_rw_data);

        /* Command complete - perform cleanup */
        nfa_rw_command_complete();
        break;

    case RW_I93_INTF_ERROR_EVT:             /* RF Interface error event         */
        if (nfa_rw_cb.flags & NFA_RW_FL_ACTIVATION_NTF_PENDING)
        {
            nfa_rw_cb.flags &= ~NFA_RW_FL_ACTIVATION_NTF_PENDING;

            memset (&i93_params, 0x00, sizeof (tNFA_TAG_PARAMS));
            memcpy (i93_params.i93.uid, nfa_rw_cb.i93_uid, I93_UID_BYTE_LEN);

            /* Command complete - perform cleanup, notify app */
            nfa_rw_command_complete();

            nfa_dm_notify_activation_status (NFA_STATUS_OK, &i93_params);
        }
        else
        {
            /* if NDEF operation */
            if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
            {
                /* If current operation is READ_NDEF, then notify ndef handlers of failure */
                nfa_dm_ndef_handle_message(NFA_STATUS_FAILED, NULL, 0);

                /* Free ndef buffer */
                nfa_rw_free_ndef_rx_buf();
            }
            else if (nfa_rw_cb.cur_op == NFA_RW_OP_WRITE_NDEF)
            {
                /* Update local cursize of ndef message */
                nfa_rw_cb.ndef_cur_size = nfa_rw_cb.ndef_wr_len;
            }

            /* Command complete - perform cleanup, notify app */
            nfa_rw_command_complete();

            conn_evt_data.status = p_rw_data->status;

            if (nfa_rw_cb.cur_op == NFA_RW_OP_READ_NDEF)
            {
                nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
            }
            else if (nfa_rw_cb.cur_op == NFA_RW_OP_WRITE_NDEF)
            {
                nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);
            }
            else if (nfa_rw_cb.cur_op == NFA_RW_OP_DETECT_NDEF)
            {
                conn_evt_data.ndef_detect.cur_size = 0;
                conn_evt_data.ndef_detect.max_size = 0;
                conn_evt_data.ndef_detect.flags    = RW_NDEF_FL_UNKNOWN;
                nfa_dm_act_conn_cback_notify(NFA_NDEF_DETECT_EVT, &conn_evt_data);
            }
            else
            {
                /* this event doesn't have command */
                nfa_dm_act_conn_cback_notify(NFA_RW_INTF_ERROR_EVT, &conn_evt_data);
            }
        }

        nfa_rw_cb.cur_op = NFA_RW_OP_MAX; /* clear current operation */
        break;


    case RW_I93_PRESENCE_CHECK_EVT:             /* Presence check completed */
        nfa_rw_handle_presence_check_rsp(p_rw_data->status);
        break;

    case RW_I93_FORMAT_CPLT_EVT:                /* Format procedure complete          */
        if (p_rw_data->data.status == NFA_STATUS_OK)
            nfa_rw_cb.ndef_st = NFA_RW_NDEF_ST_UNKNOWN;

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        nfa_rw_cb.cur_op = NFA_RW_OP_MAX; /* clear current operation */
        conn_evt_data.status = p_rw_data->status;
        nfa_dm_act_conn_cback_notify(NFA_FORMAT_CPLT_EVT, &conn_evt_data);
        break;

    case RW_I93_SET_TAG_RO_EVT:                 /* Set read-only procedure complete   */
        nfa_rw_cb.flags |= NFA_RW_FL_TAG_IS_READONLY;

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        nfa_rw_cb.cur_op = NFA_RW_OP_MAX; /* clear current operation */
        conn_evt_data.status = p_rw_data->status;
        nfa_dm_act_conn_cback_notify(NFA_SET_TAG_RO_EVT, &conn_evt_data);
        break;

    case RW_I93_INVENTORY_EVT:                  /* Response of Inventory              */

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();

        if (nfa_rw_cb.flags & NFA_RW_FL_ACTIVATION_NTF_PENDING)
        {
            /* read AFI */
            if (RW_I93ReadSingleBlock ((UINT8)(nfa_rw_cb.i93_afi_location / nfa_rw_cb.i93_block_size)) != NFC_STATUS_OK)
            {
                nfa_rw_cb.flags &= ~NFA_RW_FL_ACTIVATION_NTF_PENDING;

                i93_params.i93.info_flags = (I93_INFO_FLAG_DSFID|I93_INFO_FLAG_MEM_SIZE);
                i93_params.i93.dsfid      = p_rw_data->i93_inventory.dsfid;
                i93_params.i93.block_size = nfa_rw_cb.i93_block_size;
                i93_params.i93.num_block  = nfa_rw_cb.i93_num_block;
                memcpy (i93_params.i93.uid, nfa_rw_cb.i93_uid, I93_UID_BYTE_LEN);

                nfa_dm_notify_activation_status (NFA_STATUS_OK, &i93_params);
            }
            else
            {
                nfa_rw_cb.i93_dsfid = p_rw_data->i93_inventory.dsfid;
                break;
            }
        }
        else
        {
            conn_evt_data.i93_cmd_cplt.status       = p_rw_data->i93_inventory.status;
            conn_evt_data.i93_cmd_cplt.sent_command = I93_CMD_INVENTORY;

            conn_evt_data.i93_cmd_cplt.params.inventory.dsfid = p_rw_data->i93_inventory.dsfid;
            memcpy (conn_evt_data.i93_cmd_cplt.params.inventory.uid,
                    p_rw_data->i93_inventory.uid,
                    I93_UID_BYTE_LEN);

            nfa_dm_act_conn_cback_notify(NFA_I93_CMD_CPLT_EVT, &conn_evt_data);
        }

        nfa_rw_cb.cur_op = NFA_RW_OP_MAX; /* clear current operation */
        break;

    case RW_I93_DATA_EVT:                       /* Response of Read, Get Multi Security */

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();

        conn_evt_data.data.p_data = (UINT8 *)(p_rw_data->i93_data.p_data + 1) + p_rw_data->i93_data.p_data->offset;

        if (nfa_rw_cb.flags & NFA_RW_FL_ACTIVATION_NTF_PENDING)
        {
            nfa_rw_cb.flags &= ~NFA_RW_FL_ACTIVATION_NTF_PENDING;

            i93_params.i93.info_flags = (I93_INFO_FLAG_DSFID|I93_INFO_FLAG_MEM_SIZE|I93_INFO_FLAG_AFI);
            i93_params.i93.afi        = *(conn_evt_data.data.p_data + nfa_rw_cb.i93_afi_location % nfa_rw_cb.i93_block_size);
            i93_params.i93.dsfid      = nfa_rw_cb.i93_dsfid;
            i93_params.i93.block_size = nfa_rw_cb.i93_block_size;
            i93_params.i93.num_block  = nfa_rw_cb.i93_num_block;
            memcpy (i93_params.i93.uid, nfa_rw_cb.i93_uid, I93_UID_BYTE_LEN);

            nfa_dm_notify_activation_status (NFA_STATUS_OK, &i93_params);
        }
        else
        {
            conn_evt_data.data.len    = p_rw_data->i93_data.p_data->len;

            nfa_dm_act_conn_cback_notify(NFA_DATA_EVT, &conn_evt_data);
        }

        GKI_freebuf(p_rw_data->i93_data.p_data);
        p_rw_data->i93_data.p_data = NULL;

        nfa_rw_cb.cur_op = NFA_RW_OP_MAX; /* clear current operation */
        break;

    case RW_I93_SYS_INFO_EVT:                   /* Response of System Information     */

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();

        if (nfa_rw_cb.flags & NFA_RW_FL_ACTIVATION_NTF_PENDING)
        {
            nfa_rw_cb.flags &= ~NFA_RW_FL_ACTIVATION_NTF_PENDING;

            nfa_rw_cb.i93_block_size = p_rw_data->i93_sys_info.block_size;
            nfa_rw_cb.i93_num_block  = p_rw_data->i93_sys_info.num_block;

            i93_params.i93.info_flags   = p_rw_data->i93_sys_info.info_flags;
            i93_params.i93.dsfid        = p_rw_data->i93_sys_info.dsfid;
            i93_params.i93.afi          = p_rw_data->i93_sys_info.afi;
            i93_params.i93.num_block    = p_rw_data->i93_sys_info.num_block;
            i93_params.i93.block_size   = p_rw_data->i93_sys_info.block_size;
            i93_params.i93.IC_reference = p_rw_data->i93_sys_info.IC_reference;
            memcpy (i93_params.i93.uid, p_rw_data->i93_sys_info.uid, I93_UID_BYTE_LEN);

            nfa_dm_notify_activation_status (NFA_STATUS_OK, &i93_params);
        }
        else
        {
            conn_evt_data.i93_cmd_cplt.status       = p_rw_data->i93_inventory.status;
            conn_evt_data.i93_cmd_cplt.sent_command = I93_CMD_GET_SYS_INFO;

            conn_evt_data.i93_cmd_cplt.params.sys_info.info_flags = p_rw_data->i93_sys_info.info_flags;
            memcpy (conn_evt_data.i93_cmd_cplt.params.sys_info.uid,
                    p_rw_data->i93_sys_info.uid,
                    I93_UID_BYTE_LEN);
            conn_evt_data.i93_cmd_cplt.params.sys_info.dsfid        = p_rw_data->i93_sys_info.dsfid;
            conn_evt_data.i93_cmd_cplt.params.sys_info.afi          = p_rw_data->i93_sys_info.afi;
            conn_evt_data.i93_cmd_cplt.params.sys_info.num_block    = p_rw_data->i93_sys_info.num_block;
            conn_evt_data.i93_cmd_cplt.params.sys_info.block_size   = p_rw_data->i93_sys_info.block_size;
            conn_evt_data.i93_cmd_cplt.params.sys_info.IC_reference = p_rw_data->i93_sys_info.IC_reference;

            /* store tag memory information for writing blocks */
            nfa_rw_cb.i93_block_size = p_rw_data->i93_sys_info.block_size;
            nfa_rw_cb.i93_num_block  = p_rw_data->i93_sys_info.num_block;

            nfa_dm_act_conn_cback_notify(NFA_I93_CMD_CPLT_EVT, &conn_evt_data);
        }

        nfa_rw_cb.cur_op = NFA_RW_OP_MAX; /* clear current operation */
        break;

    case RW_I93_CMD_CMPL_EVT:                   /* Command complete                   */
        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();

        conn_evt_data.i93_cmd_cplt.status       = p_rw_data->i93_cmd_cmpl.status;
        conn_evt_data.i93_cmd_cplt.sent_command = p_rw_data->i93_cmd_cmpl.command;

        if (conn_evt_data.i93_cmd_cplt.status != NFC_STATUS_OK)
            conn_evt_data.i93_cmd_cplt.params.error_code = p_rw_data->i93_cmd_cmpl.error_code;

        nfa_dm_act_conn_cback_notify(NFA_I93_CMD_CPLT_EVT, &conn_evt_data);

        nfa_rw_cb.cur_op = NFA_RW_OP_MAX; /* clear current operation */
        break;

    default:
        NFA_TRACE_DEBUG1("nfa_rw_handle_i93_evt(); Unhandled RW event 0x%X", event);
        break;
    }
}

/*******************************************************************************
**
** Function         nfa_rw_cback
**
** Description      Callback for reader/writer event notification
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_cback (tRW_EVENT event, tRW_DATA *p_rw_data)
{
    NFA_TRACE_DEBUG1("nfa_rw_cback: event=0x%02x", event);

    /* Call appropriate event handler for tag type */
    if (event < RW_T1T_MAX_EVT)
    {
        /* Handle Type-1 tag events */
        nfa_rw_handle_t1t_evt(event, p_rw_data);
    }
    else if (event < RW_T2T_MAX_EVT)
    {
        /* Handle Type-2 tag events */
        nfa_rw_handle_t2t_evt(event, p_rw_data);
    }
    else if (event < RW_T3T_MAX_EVT)
    {
        /* Handle Type-3 tag events */
        nfa_rw_handle_t3t_evt(event, p_rw_data);
    }
    else if (event < RW_T4T_MAX_EVT)
    {
        /* Handle Type-4 tag events */
        nfa_rw_handle_t4t_evt(event, p_rw_data);
    }
    else if (event < RW_I93_MAX_EVT)
    {
        /* Handle ISO 15693 tag events */
        nfa_rw_handle_i93_evt(event, p_rw_data);
    }
    else
    {
        NFA_TRACE_ERROR1("nfa_rw_cback: unhandled event=0x%02x", event);
    }
}

/*******************************************************************************
**
** Function         nfa_rw_start_ndef_detection
**
** Description      Start NDEF detection on activated tag
**
** Returns          Nothing
**
*******************************************************************************/
static tNFC_STATUS nfa_rw_start_ndef_detection(void)
{
    tNFC_PROTOCOL protocol = nfa_rw_cb.protocol;
    tNFC_STATUS status = NFC_STATUS_FAILED;

    switch (protocol)
    {
    case NFC_PROTOCOL_T1T:    /* Type1Tag    - NFC-A */
        status = RW_T1tDetectNDef();
        break;

    case NFC_PROTOCOL_T2T:   /* Type2Tag    - NFC-A */
        if (nfa_rw_cb.pa_sel_res == NFC_SEL_RES_NFC_FORUM_T2T)
            status = RW_T2tDetectNDef();
        break;

    case NFC_PROTOCOL_T3T:   /* Type3Tag    - NFC-F */
        status = RW_T3tDetectNDef();
        break;

    case NFC_PROTOCOL_ISO_DEP:     /* ISODEP/4A,4B- NFC-A or NFC-B */
        status = RW_T4tDetectNDef();
        break;

    case NFC_PROTOCOL_15693:       /* ISO 15693 */
        status = RW_I93DetectNDef();
        break;

    default:
        break;
    }

    return(status);
}

/*******************************************************************************
**
** Function         nfa_rw_start_ndef_read
**
** Description      Start NDEF read on activated tag
**
** Returns          Nothing
**
*******************************************************************************/
static tNFC_STATUS nfa_rw_start_ndef_read(void)
{
    tNFC_PROTOCOL protocol = nfa_rw_cb.protocol;
    tNFC_STATUS status = NFC_STATUS_FAILED;
    tNFA_CONN_EVT_DATA conn_evt_data;

    /* Handle zero length NDEF message */
    if (nfa_rw_cb.ndef_cur_size == 0)
    {
        NFA_TRACE_DEBUG0("NDEF message is zero-length");

        /* Send zero-lengh NDEF message to ndef callback */
        nfa_dm_ndef_handle_message(NFA_STATUS_OK, NULL, 0);

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        conn_evt_data.status = NFA_STATUS_OK;
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        return NFC_STATUS_OK;
    }

    /* Allocate buffer for incoming NDEF message (free previous NDEF rx buffer, if needed) */
    nfa_rw_free_ndef_rx_buf ();
    if ((nfa_rw_cb.p_ndef_buf = (UINT8 *)nfa_mem_co_alloc(nfa_rw_cb.ndef_cur_size)) == NULL)
    {
        NFA_TRACE_ERROR1("Unable to allocate a buffer for reading NDEF (size=%i)", nfa_rw_cb.ndef_cur_size);

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        conn_evt_data.status = NFA_STATUS_FAILED;
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
        return NFC_STATUS_FAILED;
    }
    nfa_rw_cb.ndef_rd_offset = 0;

    switch (protocol)
    {
    case NFC_PROTOCOL_T1T:    /* Type1Tag    - NFC-A */
        status = RW_T1tReadNDef(nfa_rw_cb.p_ndef_buf,(UINT16)nfa_rw_cb.ndef_cur_size);
        break;

    case NFC_PROTOCOL_T2T:   /* Type2Tag    - NFC-A */
        if (nfa_rw_cb.pa_sel_res == NFC_SEL_RES_NFC_FORUM_T2T)
            status = RW_T2tReadNDef(nfa_rw_cb.p_ndef_buf,(UINT16)nfa_rw_cb.ndef_cur_size);

        break;

    case NFC_PROTOCOL_T3T:   /* Type3Tag    - NFC-F */
        status = RW_T3tCheckNDef();
        break;

    case NFC_PROTOCOL_ISO_DEP:     /* ISODEP/4A,4B- NFC-A or NFC-B */
        status = RW_T4tReadNDef();
        break;

    case NFC_PROTOCOL_15693:       /* ISO 15693 */
        status = RW_I93ReadNDef();
        break;

    default:
        break;
    }
    return(status);
}

/*******************************************************************************
**
** Function         nfa_rw_detect_ndef
**
** Description      Handler for NFA_RW_API_DETECT_NDEF_EVT
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_detect_ndef(tNFA_RW_MSG *p_data)
{
    tNFA_CONN_EVT_DATA conn_evt_data;
    NFA_TRACE_DEBUG0("nfa_rw_detect_ndef");

    if ((conn_evt_data.ndef_detect.status = nfa_rw_start_ndef_detection()) != NFC_STATUS_OK)
    {
        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        conn_evt_data.ndef_detect.cur_size = 0;
        conn_evt_data.ndef_detect.max_size = 0;
        conn_evt_data.ndef_detect.flags    = RW_NDEF_FL_UNKNOWN;
        nfa_dm_act_conn_cback_notify(NFA_NDEF_DETECT_EVT, &conn_evt_data);
    }

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_start_ndef_write
**
** Description      Start NDEF write on activated tag
**
** Returns          Nothing
**
*******************************************************************************/
static tNFC_STATUS nfa_rw_start_ndef_write(void)
{
    tNFC_PROTOCOL protocol = nfa_rw_cb.protocol;
    tNFC_STATUS status = NFC_STATUS_FAILED;

    if (nfa_rw_cb.flags & NFA_RW_FL_TAG_IS_READONLY)
    {
        /* error: ndef tag is read-only */
        status = NFC_STATUS_FAILED;
        NFA_TRACE_ERROR0("Unable to write NDEF. Tag is read-only")
    }
    else if (nfa_rw_cb.ndef_max_size < nfa_rw_cb.ndef_wr_len)
    {
        /* error: ndef tag size is too small */
        status = NFC_STATUS_BUFFER_FULL;
        NFA_TRACE_ERROR2("Unable to write NDEF. Tag maxsize=%i, request write size=%i", nfa_rw_cb.ndef_max_size, nfa_rw_cb.ndef_wr_len)
    }
    else
    {
        switch (protocol)
        {
        case NFC_PROTOCOL_T1T:    /* Type1Tag    - NFC-A */
            status = RW_T1tWriteNDef((UINT16)nfa_rw_cb.ndef_wr_len, nfa_rw_cb.p_ndef_wr_buf);
            break;

        case NFC_PROTOCOL_T2T:   /* Type2Tag    - NFC-A */

            if (nfa_rw_cb.pa_sel_res == NFC_SEL_RES_NFC_FORUM_T2T)
                status = RW_T2tWriteNDef((UINT16)nfa_rw_cb.ndef_wr_len, nfa_rw_cb.p_ndef_wr_buf);
            break;

        case NFC_PROTOCOL_T3T:   /* Type3Tag    - NFC-F */
            status = RW_T3tUpdateNDef(nfa_rw_cb.ndef_wr_len, nfa_rw_cb.p_ndef_wr_buf);
            break;

        case NFC_PROTOCOL_ISO_DEP:     /* ISODEP/4A,4B- NFC-A or NFC-B */
            status = RW_T4tUpdateNDef((UINT16)nfa_rw_cb.ndef_wr_len, nfa_rw_cb.p_ndef_wr_buf);
            break;

        case NFC_PROTOCOL_15693:       /* ISO 15693 */
            status = RW_I93UpdateNDef((UINT16)nfa_rw_cb.ndef_wr_len, nfa_rw_cb.p_ndef_wr_buf);
            break;

        default:
            break;
        }
    }

    return(status);
}

/*******************************************************************************
**
** Function         nfa_rw_read_ndef
**
** Description      Handler for NFA_RW_API_READ_NDEF_EVT
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_read_ndef(tNFA_RW_MSG *p_data)
{
    tNFA_STATUS status = NFA_STATUS_OK;
    tNFA_CONN_EVT_DATA conn_evt_data;

    NFA_TRACE_DEBUG0("nfa_rw_read_ndef");

    /* Check if ndef detection has been performed yet */
    if (nfa_rw_cb.ndef_st == NFA_RW_NDEF_ST_UNKNOWN)
    {
        /* Perform ndef detection first */
        status = nfa_rw_start_ndef_detection();
    }
    else if (nfa_rw_cb.ndef_st == NFA_RW_NDEF_ST_FALSE)
    {
        /* Tag is not NDEF */
        status = NFA_STATUS_FAILED;
    }
    else
    {
        /* Perform the NDEF read operation */
        status = nfa_rw_start_ndef_read();
    }

    /* Handle failure */
    if (status != NFA_STATUS_OK)
    {
        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        conn_evt_data.status = status;
        nfa_dm_act_conn_cback_notify(NFA_READ_CPLT_EVT, &conn_evt_data);
    }


    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_write_ndef
**
** Description      Handler for NFA_RW_API_WRITE_NDEF_EVT
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_write_ndef(tNFA_RW_MSG *p_data)
{
    tNDEF_STATUS ndef_status;
    tNFA_STATUS write_status = NFA_STATUS_OK;
    tNFA_CONN_EVT_DATA conn_evt_data;
    NFA_TRACE_DEBUG0("nfa_rw_write_ndef");

    /* Validate NDEF message */
    if ((ndef_status = NDEF_MsgValidate(p_data->op_req.params.write_ndef.p_data, p_data->op_req.params.write_ndef.len, FALSE)) != NDEF_OK)
    {
        NFA_TRACE_ERROR1("Invalid NDEF message. NDEF_MsgValidate returned %i", ndef_status);

        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        conn_evt_data.status = NFA_STATUS_FAILED;
        nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);
        return TRUE;
    }

    /* Store pointer to source NDEF */
    nfa_rw_cb.p_ndef_wr_buf = p_data->op_req.params.write_ndef.p_data;
    nfa_rw_cb.ndef_wr_len = p_data->op_req.params.write_ndef.len;

    /* Check if ndef detection has been performed yet */
    if (nfa_rw_cb.ndef_st == NFA_RW_NDEF_ST_UNKNOWN)
    {
        /* Perform ndef detection first */
        write_status = nfa_rw_start_ndef_detection();
    }
    else if (nfa_rw_cb.ndef_st == NFA_RW_NDEF_ST_FALSE)
    {
        /* Tag is not NDEF */
        write_status = NFA_STATUS_FAILED;
    }
    else
    {
        /* Perform the NDEF read operation */
        write_status = nfa_rw_start_ndef_write();
    }

    /* Handle failure */
    if (write_status != NFA_STATUS_OK)
    {
        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        conn_evt_data.status = write_status;
        nfa_dm_act_conn_cback_notify(NFA_WRITE_CPLT_EVT, &conn_evt_data);
    }

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_presence_check
**
** Description      Handler for NFA_RW_API_PRESENCE_CHECK
**
** Returns          Nothing
**
*******************************************************************************/
void nfa_rw_presence_check (tNFA_RW_MSG *p_data)
{
    tNFC_PROTOCOL       protocol = nfa_rw_cb.protocol;
    UINT8               sel_res  = nfa_rw_cb.pa_sel_res;
    tNFC_STATUS         status   = NFC_STATUS_FAILED;

    switch (protocol)
    {
    case NFC_PROTOCOL_T1T:    /* Type1Tag    - NFC-A */
        status = RW_T1tPresenceCheck();
        break;

    case NFC_PROTOCOL_T3T:   /* Type3Tag    - NFC-F */
        status = RW_T3tPresenceCheck();
        break;

    case NFC_PROTOCOL_ISO_DEP:     /* ISODEP/4A,4B- NFC-A or NFC-B */
        status = RW_T4tPresenceCheck();
        break;

    case NFC_PROTOCOL_15693:       /* ISO 15693 */
        status = RW_I93PresenceCheck();
        break;

    case NFC_PROTOCOL_T2T:   /* Type2Tag    - NFC-A */
        /* If T2T NFC-Forum, then let RW handle presence check; otherwise fall through */
        if (sel_res == NFC_SEL_RES_NFC_FORUM_T2T)
        {
            status = RW_T2tPresenceCheck();
            break;
        }

    default:
        /* Protocol unsupported by RW module... */
        /* Let DM perform presence check (by putting tag to sleep and then waking it up) */
        status = nfa_dm_disc_presence_check();
        break;
    }

    /* Handle presence check failure */
    if (status != NFC_STATUS_OK)
        nfa_rw_handle_presence_check_rsp(NFC_STATUS_FAILED);
}


/*******************************************************************************
**
** Function         nfa_rw_presence_check_tick
**
** Description      Called on expiration of NFA_RW_PRESENCE_CHECK_INTERVAL
**                  Initiate presence check
**
** Returns          TRUE (caller frees message buffer)
**
*******************************************************************************/
BOOLEAN nfa_rw_presence_check_tick(tNFA_RW_MSG *p_data)
{
    /* Store the current operation */
    nfa_rw_cb.cur_op = NFA_RW_OP_PRESENCE_CHECK;
    nfa_rw_cb.flags |= NFA_RW_FL_AUTO_PRESENCE_CHECK_BUSY;
    NFA_TRACE_DEBUG0("Auto-presence check starting...");

    /* Perform presence check */
    nfa_rw_presence_check(NULL);

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_format_tag
**
** Description      Handler for NFA_RW_API_FORMAT_TAG
**
** Returns          Nothing
**
*******************************************************************************/
static void nfa_rw_format_tag (tNFA_RW_MSG *p_data)
{
    tNFC_PROTOCOL   protocol = nfa_rw_cb.protocol;
    tNFC_STATUS     status   = NFC_STATUS_FAILED;

    if (protocol == NFC_PROTOCOL_T1T)
    {
        status = RW_T1tFormatNDef();
    }
    else if (  (protocol  == NFC_PROTOCOL_T2T)
             &&(nfa_rw_cb.pa_sel_res == NFC_SEL_RES_NFC_FORUM_T2T) )
    {
        status = RW_T2tFormatNDef();
    }
    else if (protocol == NFC_PROTOCOL_T3T)
    {
        status = RW_T3tFormatNDef();
    }
    else if (protocol == NFC_PROTOCOL_15693)
    {
        status = RW_I93FormatNDef();
    }
#ifdef NXP_EXT
    else if (protocol == NFC_PROTOCOL_ISO_DEP)
    {
        status = RW_T4tFormatNDef();
    }
#endif
    /* If unable to format NDEF, notify the app */
    if (status != NFC_STATUS_OK)
        nfa_rw_error_cleanup (NFA_FORMAT_CPLT_EVT);
}

/*******************************************************************************
**
** Function         nfa_rw_detect_tlv
**
** Description      Handler for NFA_RW_API_DETECT_NDEF_EVT
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_detect_tlv (tNFA_RW_MSG *p_data, UINT8 tlv)
{
    NFA_TRACE_DEBUG0("nfa_rw_detect_tlv");

    switch (nfa_rw_cb.protocol)
    {
    case NFC_PROTOCOL_T1T:
        if (RW_T1tLocateTlv(tlv) != NFC_STATUS_OK)
            nfa_rw_error_cleanup (NFA_TLV_DETECT_EVT);
        break;

    case NFC_PROTOCOL_T2T:
        if (nfa_rw_cb.pa_sel_res == NFC_SEL_RES_NFC_FORUM_T2T)
        {
            if (RW_T2tLocateTlv(tlv) != NFC_STATUS_OK)
                nfa_rw_error_cleanup (NFA_TLV_DETECT_EVT);
        }
        break;

    default:
        break;
    }

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_config_tag_ro
**
** Description      Handler for NFA_RW_OP_SET_TAG_RO
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static tNFC_STATUS nfa_rw_config_tag_ro (BOOLEAN b_hard_lock)
{
    tNFC_PROTOCOL                       protocol        = nfa_rw_cb.protocol;
    tNFC_STATUS                         status          = NFC_STATUS_FAILED;

    NFA_TRACE_DEBUG0("nfa_rw_config_tag_ro");

    switch (protocol)
    {
    case NFC_PROTOCOL_T1T:
        if(  (nfa_rw_cb.tlv_st == NFA_RW_TLV_DETECT_ST_OP_NOT_STARTED)
           ||(nfa_rw_cb.tlv_st == NFA_RW_TLV_DETECT_ST_MEM_TLV_OP_COMPLETE) )
        {
            status = RW_T1tLocateTlv(TAG_LOCK_CTRL_TLV);
        }
        else if ( (status = RW_T1tSetTagReadOnly(b_hard_lock)) != NFC_STATUS_OK)
        {
            nfa_rw_error_cleanup (NFA_SET_TAG_RO_EVT);
        }
        else
        {
            nfa_rw_cb.ndef_st = NFA_RW_NDEF_ST_UNKNOWN;
        }
        break;

    case NFC_PROTOCOL_T2T:
        if (nfa_rw_cb.pa_sel_res == NFC_SEL_RES_NFC_FORUM_T2T)
        {
            if ( (status = RW_T2tSetTagReadOnly(b_hard_lock)) != NFC_STATUS_OK)
            {
                nfa_rw_error_cleanup (NFA_SET_TAG_RO_EVT);
            }
            else
            {
                nfa_rw_cb.ndef_st = NFA_RW_NDEF_ST_UNKNOWN;
            }
        }
        break;

    case NFC_PROTOCOL_15693:
        if ( (status = RW_I93SetTagReadOnly()) != NFC_STATUS_OK)
            nfa_rw_error_cleanup (NFA_SET_TAG_RO_EVT);
        break;

    default:
        /* NOTE: type type-3 and type-4 protocol does not define transition to read-only */
        break;

    }
    return (status);
}

/*******************************************************************************
**
** Function         nfa_rw_t1t_rid
**
** Description      Handler for T1T_RID API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t1t_rid(tNFA_RW_MSG *p_data)
{
    if (RW_T1tRid () != NFC_STATUS_OK)
        nfa_rw_error_cleanup (NFA_READ_CPLT_EVT);

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t1t_rall
**
** Description      Handler for T1T_ReadAll API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t1t_rall(tNFA_RW_MSG *p_data)
{
    if (RW_T1tReadAll() != NFC_STATUS_OK)
        nfa_rw_error_cleanup (NFA_READ_CPLT_EVT);

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t1t_read
**
** Description      Handler for T1T_Read API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t1t_read (tNFA_RW_MSG *p_data)
{
    tNFA_RW_OP_PARAMS_T1T_READ *p_t1t_read = (tNFA_RW_OP_PARAMS_T1T_READ *)&(p_data->op_req.params.t1t_read);

    if (RW_T1tRead (p_t1t_read->block_number, p_t1t_read->index) != NFC_STATUS_OK)
        nfa_rw_error_cleanup (NFA_READ_CPLT_EVT);

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t1t_write
**
** Description      Handler for T1T_WriteErase/T1T_WriteNoErase API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t1t_write (tNFA_RW_MSG *p_data)
{
    tNFA_RW_OP_PARAMS_T1T_WRITE *p_t1t_write = (tNFA_RW_OP_PARAMS_T1T_WRITE *)&(p_data->op_req.params.t1t_write);
    tNFC_STATUS                 status;

    if (p_t1t_write->b_erase)
    {
        status = RW_T1tWriteErase (p_t1t_write->block_number,p_t1t_write->index,p_t1t_write->p_block_data[0]);
    }
    else
    {
        status = RW_T1tWriteNoErase (p_t1t_write->block_number,p_t1t_write->index,p_t1t_write->p_block_data[0]);
    }

    if (status != NFC_STATUS_OK)
    {
        nfa_rw_error_cleanup (NFA_WRITE_CPLT_EVT);
    }
    else
    {
        if (p_t1t_write->block_number == 0x01)
            nfa_rw_cb.ndef_st = NFA_RW_NDEF_ST_UNKNOWN;
    }

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t1t_rseg
**
** Description      Handler for T1t_ReadSeg API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t1t_rseg (tNFA_RW_MSG *p_data)
{
    tNFA_RW_OP_PARAMS_T1T_READ *p_t1t_read = (tNFA_RW_OP_PARAMS_T1T_READ *)&(p_data->op_req.params.t1t_read);

    if (RW_T1tReadSeg (p_t1t_read->segment_number) != NFC_STATUS_OK)
        nfa_rw_error_cleanup (NFA_READ_CPLT_EVT);

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t1t_read8
**
** Description      Handler for T1T_Read8 API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t1t_read8 (tNFA_RW_MSG *p_data)
{
    tNFA_RW_OP_PARAMS_T1T_READ *p_t1t_read = (tNFA_RW_OP_PARAMS_T1T_READ *)&(p_data->op_req.params.t1t_read);

    if (RW_T1tRead8 (p_t1t_read->block_number) != NFC_STATUS_OK)
        nfa_rw_error_cleanup (NFA_READ_CPLT_EVT);

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t1t_write8
**
** Description      Handler for T1T_WriteErase8/T1T_WriteNoErase8 API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t1t_write8 (tNFA_RW_MSG *p_data)
{
    tNFA_RW_OP_PARAMS_T1T_WRITE *p_t1t_write = (tNFA_RW_OP_PARAMS_T1T_WRITE *)&(p_data->op_req.params.t1t_write);
    tNFC_STATUS                 status;

    if (p_t1t_write->b_erase)
    {
        status = RW_T1tWriteErase8 (p_t1t_write->block_number,p_t1t_write->p_block_data);
    }
    else
    {
        status = RW_T1tWriteNoErase8 (p_t1t_write->block_number,p_t1t_write->p_block_data);
    }

    if (status != NFC_STATUS_OK)
    {
        nfa_rw_error_cleanup (NFA_WRITE_CPLT_EVT);
    }
    else
    {
        if (p_t1t_write->block_number == 0x01)
            nfa_rw_cb.ndef_st = NFA_RW_NDEF_ST_UNKNOWN;
    }

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t2t_read
**
** Description      Handler for T2T_Read API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t2t_read (tNFA_RW_MSG *p_data)
{
    tNFA_RW_OP_PARAMS_T2T_READ *p_t2t_read = (tNFA_RW_OP_PARAMS_T2T_READ *)&(p_data->op_req.params.t2t_read);
    tNFC_STATUS                status = NFC_STATUS_FAILED;

    if (nfa_rw_cb.pa_sel_res == NFC_SEL_RES_NFC_FORUM_T2T)
        status = RW_T2tRead (p_t2t_read->block_number);

    if (status != NFC_STATUS_OK)
        nfa_rw_error_cleanup (NFA_READ_CPLT_EVT);

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t2t_write
**
** Description      Handler for T2T_Write API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t2t_write (tNFA_RW_MSG *p_data)
{
    tNFA_RW_OP_PARAMS_T2T_WRITE *p_t2t_write = (tNFA_RW_OP_PARAMS_T2T_WRITE *)&(p_data->op_req.params.t2t_write);

    if (RW_T2tWrite (p_t2t_write->block_number,p_t2t_write->p_block_data) != NFC_STATUS_OK)
    {
        nfa_rw_error_cleanup (NFA_WRITE_CPLT_EVT);
    }
    else
    {
        if (p_t2t_write->block_number == 0x03)
            nfa_rw_cb.ndef_st = NFA_RW_NDEF_ST_UNKNOWN;
    }

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t2t_sector_select
**
** Description      Handler for T2T_Sector_Select API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t2t_sector_select(tNFA_RW_MSG *p_data)
{
    tNFA_RW_OP_PARAMS_T2T_SECTOR_SELECT *p_t2t_sector_select = (tNFA_RW_OP_PARAMS_T2T_SECTOR_SELECT *)&(p_data->op_req.params.t2t_sector_select);

    if (RW_T2tSectorSelect (p_t2t_sector_select->sector_number) != NFC_STATUS_OK)
        nfa_rw_error_cleanup (NFA_SELECT_CPLT_EVT);

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t3t_read
**
** Description      Handler for T3T_Read API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t3t_read (tNFA_RW_MSG *p_data)
{
    tNFA_RW_OP_PARAMS_T3T_READ *p_t3t_read = (tNFA_RW_OP_PARAMS_T3T_READ *)&(p_data->op_req.params.t3t_read);

    if (RW_T3tCheck (p_t3t_read->num_blocks, (tT3T_BLOCK_DESC *)p_t3t_read->p_block_desc) != NFC_STATUS_OK)
        nfa_rw_error_cleanup (NFA_READ_CPLT_EVT);

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t3t_write
**
** Description      Handler for T3T_Write API
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t3t_write (tNFA_RW_MSG *p_data)
{
    tNFA_RW_OP_PARAMS_T3T_WRITE *p_t3t_write = (tNFA_RW_OP_PARAMS_T3T_WRITE *)&(p_data->op_req.params.t3t_write);

    if (RW_T3tUpdate (p_t3t_write->num_blocks, (tT3T_BLOCK_DESC *)p_t3t_write->p_block_desc, p_t3t_write->p_block_data) != NFC_STATUS_OK)
        nfa_rw_error_cleanup (NFA_WRITE_CPLT_EVT);

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_t3t_get_system_codes
**
** Description      Get system codes (initiated by NFA after activation)
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_t3t_get_system_codes (tNFA_RW_MSG *p_data)
{
    tNFC_STATUS     status;
    tNFA_TAG_PARAMS tag_params;

    status = RW_T3tGetSystemCodes();

    if (status != NFC_STATUS_OK)
    {
        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();
        tag_params.t3t.num_system_codes = 0;
        tag_params.t3t.p_system_codes   = NULL;

        nfa_dm_notify_activation_status (NFA_STATUS_OK, &tag_params);
    }

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_i93_command
**
** Description      Handler for ISO 15693 command
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
static BOOLEAN nfa_rw_i93_command (tNFA_RW_MSG *p_data)
{
    tNFA_CONN_EVT_DATA conn_evt_data;
    tNFC_STATUS        status = NFC_STATUS_OK;
    UINT8              i93_command = I93_CMD_STAY_QUIET;

    switch (p_data->op_req.op)
    {
    case NFA_RW_OP_I93_INVENTORY:
        i93_command = I93_CMD_INVENTORY;
        if (p_data->op_req.params.i93_cmd.uid_present)
        {
            status = RW_I93Inventory (p_data->op_req.params.i93_cmd.afi,
                                      p_data->op_req.params.i93_cmd.uid);
        }
        else
        {
            status = RW_I93Inventory (p_data->op_req.params.i93_cmd.afi, NULL);
        }
        break;

    case NFA_RW_OP_I93_STAY_QUIET:
        i93_command = I93_CMD_STAY_QUIET;
        status = RW_I93StayQuiet ();
        break;

    case NFA_RW_OP_I93_READ_SINGLE_BLOCK:
        i93_command = I93_CMD_READ_SINGLE_BLOCK;
        status = RW_I93ReadSingleBlock (p_data->op_req.params.i93_cmd.first_block_number);
        break;

    case NFA_RW_OP_I93_WRITE_SINGLE_BLOCK:
        i93_command = I93_CMD_WRITE_SINGLE_BLOCK;
        status = RW_I93WriteSingleBlock (p_data->op_req.params.i93_cmd.first_block_number,
                                         p_data->op_req.params.i93_cmd.p_data);
        break;

    case NFA_RW_OP_I93_LOCK_BLOCK:
        i93_command = I93_CMD_LOCK_BLOCK;
        status = RW_I93LockBlock (p_data->op_req.params.i93_cmd.first_block_number);
        break;

    case NFA_RW_OP_I93_READ_MULTI_BLOCK:
        i93_command = I93_CMD_READ_MULTI_BLOCK;
        status = RW_I93ReadMultipleBlocks (p_data->op_req.params.i93_cmd.first_block_number,
                                           p_data->op_req.params.i93_cmd.number_blocks);
        break;

    case NFA_RW_OP_I93_WRITE_MULTI_BLOCK:
        i93_command = I93_CMD_WRITE_MULTI_BLOCK;
        status = RW_I93WriteMultipleBlocks (p_data->op_req.params.i93_cmd.first_block_number,
                                            p_data->op_req.params.i93_cmd.number_blocks,
                                            p_data->op_req.params.i93_cmd.p_data);
        break;

    case NFA_RW_OP_I93_SELECT:
        i93_command = I93_CMD_SELECT;
        status = RW_I93Select (p_data->op_req.params.i93_cmd.p_data);
        break;

    case NFA_RW_OP_I93_RESET_TO_READY:
        i93_command = I93_CMD_RESET_TO_READY;
        status = RW_I93ResetToReady ();
        break;

    case NFA_RW_OP_I93_WRITE_AFI:
        i93_command = I93_CMD_WRITE_AFI;
        status = RW_I93WriteAFI (p_data->op_req.params.i93_cmd.afi);
        break;

    case NFA_RW_OP_I93_LOCK_AFI:
        i93_command = I93_CMD_LOCK_AFI;
        status = RW_I93LockAFI ();
        break;

    case NFA_RW_OP_I93_WRITE_DSFID:
        i93_command = I93_CMD_WRITE_DSFID;
        status = RW_I93WriteDSFID (p_data->op_req.params.i93_cmd.dsfid);
        break;

    case NFA_RW_OP_I93_LOCK_DSFID:
        i93_command = I93_CMD_LOCK_DSFID;
        status = RW_I93LockDSFID ();
        break;

    case NFA_RW_OP_I93_GET_SYS_INFO:
        i93_command = I93_CMD_GET_SYS_INFO;
        if (p_data->op_req.params.i93_cmd.uid_present)
        {
            status = RW_I93GetSysInfo (p_data->op_req.params.i93_cmd.uid);
        }
        else
        {
            status = RW_I93GetSysInfo (NULL);
        }
        break;

    case NFA_RW_OP_I93_GET_MULTI_BLOCK_STATUS:
        i93_command = I93_CMD_GET_MULTI_BLK_SEC;
        status = RW_I93GetMultiBlockSecurityStatus (p_data->op_req.params.i93_cmd.first_block_number,
                                                    p_data->op_req.params.i93_cmd.number_blocks);
        break;

    default:
        break;
    }

    if (status != NFC_STATUS_OK)
    {
        /* Command complete - perform cleanup, notify app */
        nfa_rw_command_complete();

        conn_evt_data.i93_cmd_cplt.status       = NFA_STATUS_FAILED;
        conn_evt_data.i93_cmd_cplt.sent_command = i93_command;

        nfa_dm_act_conn_cback_notify(NFA_I93_CMD_CPLT_EVT, &conn_evt_data);
    }

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_raw_mode_data_cback
**
** Description      Handler for incoming tag data for unsupported tag protocols
**                  (forward data to upper layer)
**
** Returns          nothing
**
*******************************************************************************/
static void nfa_rw_raw_mode_data_cback (UINT8 conn_id, tNFC_CONN_EVT event, tNFC_CONN *p_data)
{
    BT_HDR             *p_msg = (BT_HDR *)p_data->data.p_data;
    tNFA_CONN_EVT_DATA evt_data;

    NFA_TRACE_DEBUG1 ("nfa_rw_raw_mode_data_cback(): event = 0x%X", event);

    if ((event == NFC_DATA_CEVT) && (p_data->data.status == NFC_STATUS_OK))
    {
        if (p_msg)
        {
            evt_data.data.p_data = (UINT8 *)(p_msg + 1) + p_msg->offset;
            evt_data.data.len    = p_msg->len;

            nfa_dm_conn_cback_event_notify (NFA_DATA_EVT, &evt_data);

            GKI_freebuf (p_msg);
        }
        else
        {
            NFA_TRACE_ERROR0 ("nfa_rw_raw_mode_data_cback (): received NFC_DATA_CEVT with NULL data pointer");
        }
    }
    else if (event == NFC_DEACTIVATE_CEVT)
    {
        NFC_SetStaticRfCback (NULL);
    }
}


/*******************************************************************************
**
** Function         nfa_rw_activate_ntf
**
** Description      Handler for NFA_RW_ACTIVATE_NTF
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
BOOLEAN nfa_rw_activate_ntf(tNFA_RW_MSG *p_data)
{
    tNFC_ACTIVATE_DEVT *p_activate_params = p_data->activate_ntf.p_activate_params;
    tNFA_TAG_PARAMS    tag_params;
    tNFA_RW_OPERATION  msg;
    BOOLEAN            activate_notify = TRUE;
    UINT8              *p;

    NFA_TRACE_DEBUG0("nfa_rw_activate_ntf");

    /* Initialize control block */
    nfa_rw_cb.protocol   = p_activate_params->protocol;
    nfa_rw_cb.pa_sel_res = p_activate_params->rf_tech_param.param.pa.sel_rsp;
    nfa_rw_cb.flags      = NFA_RW_FL_ACTIVATED;
    nfa_rw_cb.cur_op     = NFA_RW_OP_MAX;
    nfa_rw_cb.ndef_st    = NFA_RW_NDEF_ST_UNKNOWN;
    nfa_rw_cb.tlv_st     = NFA_RW_TLV_DETECT_ST_OP_NOT_STARTED;

    memset (&tag_params, 0, sizeof(tNFA_TAG_PARAMS));

    /* Check if we are in exclusive RF mode */
    if (p_data->activate_ntf.excl_rf_not_active)
    {
        /* Not in exclusive RF mode */
        nfa_rw_cb.flags |= NFA_RW_FL_NOT_EXCL_RF_MODE;
    }

    /* If protocol not supported by RW module, notify app of NFA_ACTIVATED_EVT and start presence check if needed */
    if (!nfa_dm_is_protocol_supported(p_activate_params->protocol, p_activate_params->rf_tech_param.param.pa.sel_rsp))
    {
        /* Set data callback (pass all incoming data to upper layer using NFA_DATA_EVT) */
        NFC_SetStaticRfCback(nfa_rw_raw_mode_data_cback);

        /* Notify app of NFA_ACTIVATED_EVT and start presence check timer */
        nfa_dm_notify_activation_status (NFA_STATUS_OK, NULL);
        nfa_rw_check_start_presence_check_timer ();
        return TRUE;
    }

    /* Initialize RW module */
    if ((RW_SetActivatedTagType (p_activate_params, nfa_rw_cback)) != NFC_STATUS_OK)
    {
        /* Log error (stay in NFA_RW_ST_ACTIVATED state until deactivation) */
        NFA_TRACE_ERROR0("RW_SetActivatedTagType failed.");
        return TRUE;
    }

    /* Perform protocol-specific actions */
    switch (nfa_rw_cb.protocol)
    {
    case NFC_PROTOCOL_T1T:
        /* Retrieve HR and UID fields from activation notification */
#ifdef NXP_EXT
        memcpy (tag_params.t1t.uid, p_activate_params->rf_tech_param.param.pa.nfcid1, p_activate_params->rf_tech_param.param.pa.nfcid1_len);
        msg.op = NFA_RW_OP_T1T_RID;
        nfa_rw_handle_op_req((tNFA_RW_MSG *)&msg);
        activate_notify = FALSE;                    /* Delay notifying upper layer of NFA_ACTIVATED_EVT until HR0/HR1 is received */
#else
        memcpy (tag_params.t1t.hr, p_activate_params->intf_param.intf_param.frame.param, NFA_T1T_HR_LEN);
        memcpy (tag_params.t1t.uid, p_activate_params->rf_tech_param.param.pa.nfcid1, p_activate_params->rf_tech_param.param.pa.nfcid1_len);
#endif
        break;
    case NFC_PROTOCOL_T2T:
        /* Retrieve UID fields from activation notification */
        memcpy (tag_params.t2t.uid, p_activate_params->rf_tech_param.param.pa.nfcid1, p_activate_params->rf_tech_param.param.pa.nfcid1_len);
        break;

    case NFC_PROTOCOL_T3T:
        /* Issue command to get Felica system codes */
        activate_notify = FALSE;                    /* Delay notifying upper layer of NFA_ACTIVATED_EVT until system codes are retrieved */
        msg.op = NFA_RW_OP_T3T_GET_SYSTEM_CODES;
        nfa_rw_handle_op_req((tNFA_RW_MSG *)&msg);
        break;

    case NFC_PROTOCOL_15693:
        /* Issue INVENTORY command to retrieve additional tag infomation */
        nfa_rw_cb.flags |= NFA_RW_FL_ACTIVATION_NTF_PENDING;
        activate_notify = FALSE;                    /* Delay notifying upper layer of NFA_ACTIVATED_EVT until INVENTORY response is received */

        p = nfa_rw_cb.i93_uid;
        ARRAY8_TO_STREAM (p, p_data->activate_ntf.p_activate_params->rf_tech_param.param.pi93.uid);

        msg.params.i93_cmd.uid_present = TRUE;
        p = msg.params.i93_cmd.uid;
        ARRAY8_TO_STREAM (p, p_data->activate_ntf.p_activate_params->rf_tech_param.param.pi93.uid);

        if ((nfa_rw_cb.i93_uid[1] == I93_UID_IC_MFG_CODE_TI)
          &&(((nfa_rw_cb.i93_uid[2] & I93_UID_TAG_IT_HF_I_PRODUCT_ID_MASK) == I93_UID_TAG_IT_HF_I_STD_CHIP_INLAY)
           ||((nfa_rw_cb.i93_uid[2] & I93_UID_TAG_IT_HF_I_PRODUCT_ID_MASK) == I93_UID_TAG_IT_HF_I_PRO_CHIP_INLAY)))
        {
            /* these don't support Get System Information Command */
            nfa_rw_cb.i93_block_size    = I93_TAG_IT_HF_I_STD_PRO_CHIP_INLAY_BLK_SIZE;
            nfa_rw_cb.i93_afi_location  = I93_TAG_IT_HF_I_STD_PRO_CHIP_INLAY_AFI_LOCATION;

            if ((nfa_rw_cb.i93_uid[2] & I93_UID_TAG_IT_HF_I_PRODUCT_ID_MASK) == I93_UID_TAG_IT_HF_I_STD_CHIP_INLAY)
            {
                nfa_rw_cb.i93_num_block     = I93_TAG_IT_HF_I_STD_CHIP_INLAY_NUM_TOTAL_BLK;
            }
            else
            {
                nfa_rw_cb.i93_num_block     = I93_TAG_IT_HF_I_PRO_CHIP_INLAY_NUM_TOTAL_BLK;
            }

            msg.op = NFA_RW_OP_I93_INVENTORY; /* Let stack know UID of activated tag */
            msg.params.i93_cmd.afi = 0x00;
        }
        else
        {
            /* All of ICODE supports Get System Information Command */
            /* Tag-it HF-I Plus Chip/Inlay supports Get System Information Command */
            /* just try for others */

            nfa_rw_cb.i93_block_size = 0;
            nfa_rw_cb.i93_num_block  = 0;

            msg.op = NFA_RW_OP_I93_GET_SYS_INFO;
        }

        /* Send the command */
        nfa_rw_handle_op_req((tNFA_RW_MSG *)&msg);
        break;


    default:
        /* No action needed for other protocols */
        break;
    }

    /* Notify upper layer of NFA_ACTIVATED_EVT if needed, and start presence check timer */
    if (activate_notify)
    {
        nfa_dm_notify_activation_status (NFA_STATUS_OK, &tag_params);
        nfa_rw_check_start_presence_check_timer ();
    }


    return TRUE;
}


/*******************************************************************************
**
** Function         nfa_rw_deactivate_ntf
**
** Description      Handler for NFA_RW_DEACTIVATE_NTF
**
** Returns          TRUE (message buffer to be freed by caller)
**
*******************************************************************************/
BOOLEAN nfa_rw_deactivate_ntf(tNFA_RW_MSG *p_data)
{
    /* Clear the activated flag */
    nfa_rw_cb.flags &= ~NFA_RW_FL_ACTIVATED;

    /* Free buffer for incoming NDEF message, in case we were in the middle of a read operation */
    nfa_rw_free_ndef_rx_buf();

    /* If there is a pending command message, then free it */
    if (nfa_rw_cb.p_pending_msg)
    {
        GKI_freebuf(nfa_rw_cb.p_pending_msg);
        nfa_rw_cb.p_pending_msg = NULL;
    }

    /* Stop presence check timer (if started) */
    nfa_rw_stop_presence_check_timer();

    return TRUE;
}

/*******************************************************************************
**
** Function         nfa_rw_handle_op_req
**
** Description      Handler for NFA_RW_OP_REQUEST_EVT, operation request
**
** Returns          TRUE if caller should free p_data
**                  FALSE if caller does not need to free p_data
**
*******************************************************************************/
BOOLEAN nfa_rw_handle_op_req (tNFA_RW_MSG *p_data)
{
    BOOLEAN freebuf = TRUE;

    /* Check if activated */
    if (!(nfa_rw_cb.flags & NFA_RW_FL_ACTIVATED))
    {
        NFA_TRACE_ERROR0("nfa_rw_handle_op_req: not activated");
        return TRUE;
    }
    /* Check if currently busy with another API call */
    else if (nfa_rw_cb.flags & NFA_RW_FL_API_BUSY)
    {
        return (nfa_rw_op_req_while_busy(p_data));
    }
    /* Check if currently busy with auto-presence check */
    else if (nfa_rw_cb.flags & NFA_RW_FL_AUTO_PRESENCE_CHECK_BUSY)
    {
        /* Cache the command (will be handled once auto-presence check is completed) */
        NFA_TRACE_DEBUG1("Deferring operation %i until after auto-presence check is completed", p_data->op_req.op);
        nfa_rw_cb.p_pending_msg = p_data;
        nfa_rw_cb.flags |= NFA_RW_FL_API_BUSY;
        return (freebuf);
    }

    NFA_TRACE_DEBUG1("nfa_rw_handle_op_req: op=0x%02x", p_data->op_req.op);

    nfa_rw_cb.flags |= NFA_RW_FL_API_BUSY;

    /* Stop the presence check timer */
    nfa_rw_stop_presence_check_timer();

    /* Store the current operation */
    nfa_rw_cb.cur_op = p_data->op_req.op;

    /* Call appropriate handler for requested operation */
    switch (p_data->op_req.op)
    {
    case NFA_RW_OP_DETECT_NDEF:
        nfa_rw_detect_ndef(p_data);
        break;

    case NFA_RW_OP_READ_NDEF:
        nfa_rw_read_ndef(p_data);
        break;

    case NFA_RW_OP_WRITE_NDEF:
        nfa_rw_write_ndef(p_data);
        break;

    case NFA_RW_OP_SEND_RAW_FRAME:
        NFC_SendData (NFC_RF_CONN_ID, p_data->op_req.params.send_raw_frame.p_data);

        /* Command complete - perform cleanup */
        nfa_rw_command_complete();
        break;

    case NFA_RW_OP_PRESENCE_CHECK:
        nfa_rw_presence_check(p_data);
        break;

    case NFA_RW_OP_FORMAT_TAG:
        nfa_rw_format_tag(p_data);
        break;

    case NFA_RW_OP_DETECT_LOCK_TLV:
        nfa_rw_detect_tlv(p_data, TAG_LOCK_CTRL_TLV);
        break;

    case NFA_RW_OP_DETECT_MEM_TLV:
        nfa_rw_detect_tlv(p_data, TAG_MEM_CTRL_TLV);
        break;

    case NFA_RW_OP_SET_TAG_RO:
        nfa_rw_cb.b_hard_lock = p_data->op_req.params.set_readonly.b_hard_lock;
        nfa_rw_config_tag_ro(nfa_rw_cb.b_hard_lock);
        break;

    case NFA_RW_OP_T1T_RID:
        nfa_rw_t1t_rid(p_data);
        break;

    case NFA_RW_OP_T1T_RALL:
        nfa_rw_t1t_rall(p_data);
        break;

    case NFA_RW_OP_T1T_READ:
        nfa_rw_t1t_read(p_data);
        break;

    case NFA_RW_OP_T1T_WRITE:
        nfa_rw_t1t_write(p_data);
        break;

    case NFA_RW_OP_T1T_RSEG:
        nfa_rw_t1t_rseg(p_data);
        break;

    case NFA_RW_OP_T1T_READ8:
        nfa_rw_t1t_read8(p_data);
        break;

    case NFA_RW_OP_T1T_WRITE8:
        nfa_rw_t1t_write8(p_data);
        break;

        /* Type-2 tag commands */
    case NFA_RW_OP_T2T_READ:
        nfa_rw_t2t_read(p_data);
        break;

    case NFA_RW_OP_T2T_WRITE:
        nfa_rw_t2t_write(p_data);
        break;

    case NFA_RW_OP_T2T_SECTOR_SELECT:
        nfa_rw_t2t_sector_select(p_data);
        break;

        /* Type-3 tag commands */
    case NFA_RW_OP_T3T_READ:
        nfa_rw_t3t_read(p_data);
        break;

    case NFA_RW_OP_T3T_WRITE:
        nfa_rw_t3t_write(p_data);
        break;

    case NFA_RW_OP_T3T_GET_SYSTEM_CODES:
        nfa_rw_t3t_get_system_codes(p_data);
        break;

        /* ISO 15693 tag commands */
    case NFA_RW_OP_I93_INVENTORY:
    case NFA_RW_OP_I93_STAY_QUIET:
    case NFA_RW_OP_I93_READ_SINGLE_BLOCK:
    case NFA_RW_OP_I93_WRITE_SINGLE_BLOCK:
    case NFA_RW_OP_I93_LOCK_BLOCK:
    case NFA_RW_OP_I93_READ_MULTI_BLOCK:
    case NFA_RW_OP_I93_WRITE_MULTI_BLOCK:
    case NFA_RW_OP_I93_SELECT:
    case NFA_RW_OP_I93_RESET_TO_READY:
    case NFA_RW_OP_I93_WRITE_AFI:
    case NFA_RW_OP_I93_LOCK_AFI:
    case NFA_RW_OP_I93_WRITE_DSFID:
    case NFA_RW_OP_I93_LOCK_DSFID:
    case NFA_RW_OP_I93_GET_SYS_INFO:
    case NFA_RW_OP_I93_GET_MULTI_BLOCK_STATUS:
        nfa_rw_i93_command (p_data);
        break;

    default:
        NFA_TRACE_ERROR1("nfa_rw_handle_api: unhandled operation: %i", p_data->op_req.op);
        break;
    }

    return (freebuf);
}


/*******************************************************************************
**
** Function         nfa_rw_op_req_while_busy
**
** Description      Handle operation request while busy
**
** Returns          TRUE if caller should free p_data
**                  FALSE if caller does not need to free p_data
**
*******************************************************************************/
static BOOLEAN nfa_rw_op_req_while_busy(tNFA_RW_MSG *p_data)
{
    BOOLEAN             freebuf = TRUE;
    tNFA_CONN_EVT_DATA  conn_evt_data;
    UINT8               event;

    NFA_TRACE_ERROR0("nfa_rw_op_req_while_busy: unable to handle API");

    /* Return appropriate event for requested API, with status=BUSY */
    conn_evt_data.status = NFA_STATUS_BUSY;

    switch (p_data->op_req.op)
    {
    case NFA_RW_OP_DETECT_NDEF:
        conn_evt_data.ndef_detect.cur_size = 0;
        conn_evt_data.ndef_detect.max_size = 0;
        conn_evt_data.ndef_detect.flags    = RW_NDEF_FL_UNKNOWN;
        event = NFA_NDEF_DETECT_EVT;
        break;
    case NFA_RW_OP_READ_NDEF:
    case NFA_RW_OP_T1T_RID:
    case NFA_RW_OP_T1T_RALL:
    case NFA_RW_OP_T1T_READ:
    case NFA_RW_OP_T1T_RSEG:
    case NFA_RW_OP_T1T_READ8:
    case NFA_RW_OP_T2T_READ:
    case NFA_RW_OP_T3T_READ:
        event = NFA_READ_CPLT_EVT;
        break;
    case NFA_RW_OP_WRITE_NDEF:
    case NFA_RW_OP_T1T_WRITE:
    case NFA_RW_OP_T1T_WRITE8:
    case NFA_RW_OP_T2T_WRITE:
    case NFA_RW_OP_T3T_WRITE:
        event = NFA_WRITE_CPLT_EVT;
        break;
    case NFA_RW_OP_FORMAT_TAG:
        event = NFA_FORMAT_CPLT_EVT;
        break;
        case NFA_RW_OP_DETECT_LOCK_TLV:
    case NFA_RW_OP_DETECT_MEM_TLV:
        event = NFA_TLV_DETECT_EVT;
        break;
    case NFA_RW_OP_SET_TAG_RO:
        event = NFA_SET_TAG_RO_EVT;
        break;
    case NFA_RW_OP_T2T_SECTOR_SELECT:
        event = NFA_SELECT_CPLT_EVT;
        break;
    case NFA_RW_OP_I93_INVENTORY:
    case NFA_RW_OP_I93_STAY_QUIET:
    case NFA_RW_OP_I93_READ_SINGLE_BLOCK:
    case NFA_RW_OP_I93_WRITE_SINGLE_BLOCK:
    case NFA_RW_OP_I93_LOCK_BLOCK:
    case NFA_RW_OP_I93_READ_MULTI_BLOCK:
    case NFA_RW_OP_I93_WRITE_MULTI_BLOCK:
    case NFA_RW_OP_I93_SELECT:
    case NFA_RW_OP_I93_RESET_TO_READY:
    case NFA_RW_OP_I93_WRITE_AFI:
    case NFA_RW_OP_I93_LOCK_AFI:
    case NFA_RW_OP_I93_WRITE_DSFID:
    case NFA_RW_OP_I93_LOCK_DSFID:
    case NFA_RW_OP_I93_GET_SYS_INFO:
    case NFA_RW_OP_I93_GET_MULTI_BLOCK_STATUS:
        event = NFA_I93_CMD_CPLT_EVT;
        break;
    default:
        return (freebuf);
    }
    nfa_dm_act_conn_cback_notify(event, &conn_evt_data);

    return (freebuf);
}

/*******************************************************************************
**
** Function         nfa_rw_command_complete
**
** Description      Handle command complete: clear the busy flag,
**                  and start the presence check timer if applicable.
**
** Returns          None
**
*******************************************************************************/
void nfa_rw_command_complete(void)
{
    /* Clear the busy flag */
    nfa_rw_cb.flags &= ~NFA_RW_FL_API_BUSY;

    /* Restart presence_check timer */
    nfa_rw_check_start_presence_check_timer ();
}
