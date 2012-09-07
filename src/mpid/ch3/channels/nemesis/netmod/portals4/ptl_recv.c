/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *  (C) 2012 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

#include "ptl_impl.h"

#undef FUNCNAME
#define FUNCNAME dequeue_req
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
static void dequeue_req(const ptl_event_t *e)
{
    int found;
    MPID_Request *const rreq = e->user_ptr;
    
    found = MPIDI_CH3U_Recvq_DP(rreq);
    MPIU_Assert(found);

    rreq->status.MPI_SOURCE = NPTL_HEADER_SOURCE(e->hdr_data);
    rreq->status.MPI_TAG = NPTL_MATCH_GET_TAG(NPTL_HEADER_MATCH_BITS(e->hdr_data));
}

#undef FUNCNAME
#define FUNCNAME handler_recv_complete
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
static int handler_recv_complete(const ptl_event_t *e)
{
    int mpi_errno = MPI_SUCCESS;
    MPID_Request *const rreq = e->user_ptr;
    int ret;
    int found;
    MPIDI_STATE_DECL(MPID_STATE_HANDLER_RECV_COMPLETE);

    MPIDI_FUNC_ENTER(MPID_STATE_HANDLER_RECV_COMPLETE);

    req->dev.recv_data_sz += e->mlength;
    req->status.count = req->dev.recv_data_sz;

    if (REQ_PTL(rreq)->md != PTL_INVALID_HANDLE) {
        ret = PtlMDRelease(REQ_PTL(rreq)->md);
        MPIU_ERR_CHKANDJUMP(ret, mpi_errno, MPI_ERR_OTHER, "**ptlmdrelease");
    }

    for (i = 0; i < MPID_NEM_PTL_NUM_CHUNK_BUFFERS; ++i)
        if (REQ_PTL(rreq)->chunk_buffer[i])
            MPIU_Free(REQ_PTL(rreq)->chunk_buffer[i]);
    
    MPIDI_CH3U_Request_complete(rreq);

 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_HANDLER_RECV_COMPLETE);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME handler_recv_dequeue_complete
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
static int handler_recv_dequeue_complete(const ptl_event_t *e)
{
    int mpi_errno = MPI_SUCCESS;
    MPID_Request *const rreq = e->user_ptr;
    int ret;
    int found;
    MPIDI_STATE_DECL(MPID_STATE_HANDLER_RECV_DEQUEUE_COMPLETE);

    MPIDI_FUNC_ENTER(MPID_STATE_HANDLER_RECV_DEQUEUE_COMPLETE);

    dequeue_req(e);
    mpi_errno = handler_recv_complete(e);

 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_HANDLER_RECV_DEQUEUE_COMPLETE);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME handler_recv_unpack_complete
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
static int handler_recv_unpack_complete(const ptl_event_t *e)
{
    int mpi_errno = MPI_SUCCESS;
    MPID_Request *const rreq = e->user_ptr;
    MPI_Aint last;
    MPIDI_STATE_DECL(MPID_STATE_HANDLER_RECV_UNPACK_COMPLETE);

    MPIDI_FUNC_ENTER(MPID_STATE_HANDLER_RECV_UNPACK_COMPLETE);

    unpack_byte(req->dev.segment_ptr, req->dev.segment_first, e->mlength, REQ_PTL(rreq_)->chunk_buffer[0]);

    mpi_errno = handler_recv_complete(e);

 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_HANDLER_RECV_UNPACK_COMPLETE);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME handler_recv_dequeue_unpack_complete
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
static int handler_recv_dequeue_unpack_complete(const ptl_event_t *e)
{
    int mpi_errno = MPI_SUCCESS;
    MPID_Request *const rreq = e->user_ptr;
    MPI_Aint last;
    MPIDI_STATE_DECL(MPID_STATE_HANDLER_RECV_DEQUEUE_UNPACK_COMPLETE);

    MPIDI_FUNC_ENTER(MPID_STATE_HANDLER_RECV_DEQUEUE_UNPACK_COMPLETE);

    dequeue_req(e);
    mpi_errno = handler_recv_unpack_complete(e);

 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_HANDLER_RECV_DEQUEUE_UNPACK_COMPLETE);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME handler_recv_dequeue_large
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
static int handler_recv_dequeue_large(const ptl_event_t *e)
{
    int mpi_errno = MPI_SUCCESS;
    MPID_Request *const rreq = e->user_ptr;
    int ret;
    int dt_contig;
    MPIDI_msg_sz_t data_sz;
    MPID_Datatype *dt_ptr;
    MPI_Aint dt_true_lb;
    MPI_Aint last;
    MPIDI_STATE_DECL(MPID_STATE_HANDLER_RECV_DEQUEUE_LARGE);

    MPIDI_FUNC_ENTER(MPID_STATE_HANDLER_RECV_DEQUEUE_LARGE);

    dequeue_req(e);

    if (!(e->hdr_data & NPTL_LARGE)) {
        /* all data has already been received; we're done */
        mpi_errno = handler_recv_complete(e);
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
        goto fn_exit;
    }

    /* we need to receive more data.  record how much we've received so far */
    req->dev.recv_data_sz = e->mlength;
    
    /* we need to GET the rest of the data from the sender's buffer */
    MPIDI_Datatype_get_info(rreq->dev.user_count, rreq->dev.datatype, dt_contig, data_sz, dt_ptr, dt_true_lb);

    if (dt_contig) {
        /* recv buffer is contig */
        ret = PtlGet(MPIDI_nem_ptl_global_md, (ptl_size_t)rreq->user_buf + PTL_LARGE_THRESHOLD, data_sz - PTL_LARGE_THRESHOLD,
                     vc_ptl->id, vc_ptl->pt, NPTL_HEADER_MATCH_BITS(e->hdr_data), 0, req);
        MPIU_ERR_CHKANDJUMP(ret, mpi_errno, MPI_ERR_OTHER, "**ptlget");
        REQ_PTL(rreq)->put_handler = NULL;
        REQ_PTL(rreq)->reply_handler = handler_recv_complete;
        goto fn_exit;
    }

    /* noncontig recv buffer */
    last = rreq->dev.segment_size;
    rreq->dev.iov_count = MPID_IOV_LIMIT;
    MPID_Segment_pack_vector(rreq->dev.segment_ptr, rreq->dev.segment_first, &last, rreq->dev.iov, &rreq->dev.iov_count);

    if (last == rreq->dev.segment_size) {
        /* Rest of message fits in one IOV */
        ptl_md_t md;

        md.start = rreq->dev.iov;
        md.length = rreq->dev.iov_count;
        md.options = PTL_IOVEC;
        md.eq_handle = MPIDI_nem_ptl_eq;
        md.ct_handle = PTL_CT_NONE;
        ret = PtlMDBind(MPIDI_nem_ptl_ni, &md, &REQ_PTL(rreq)->md);
        MPIU_ERR_CHKANDJUMP(ret, mpi_errno, MPI_ERR_OTHER, "**ptlmdbind");

        REQ_PTL(rreq)->put_handler = NULL;
        REQ_PTL(rreq)->reply_handler = handler_recv_complete;
        ret = PtlGet(REQ_PTL(rreq)->md, 0, rreq->dev.segment_size - rreq->dev.segment_first, vc_ptl->id, vc_ptl->pt,
                     NPTL_HEADER_MATCH_BITS(e->hdr_data), 0, req);
        MPIU_ERR_CHKANDJUMP(ret, mpi_errno, MPI_ERR_OTHER, "**ptlget");
        REQ_PTL(rreq)->put_handler = NULL;
        REQ_PTL(rreq)->reply_handler = handler_recv_complete;
        goto fn_exit;
    }
        
    /* message won't fit in a single IOV, allocate buffer and unpack when received */
    /* FIXME: For now, allocate a single large buffer to hold entire message */
    MPIU_CHKPMEM_MALLOC(REQ_PTL(rreq)->chunk_buffer[0], void *, rreq->dev.segment_size - rreq->dev.segment_first, mpi_errno, "chunk_buffer");

    ret = PtlGet(MPIDI_nem_ptl_global_md, (ptl_size_t)REQ_PTL(rreq)->chunk_buffer[0],
                 rreq->dev.segment_size - rreq->dev.segment_first, vc_ptl->id, vc_ptl->pt,
                 NPTL_HEADER_MATCH_BITS(e->hdr_data), 0, req);
    MPIU_ERR_CHKANDJUMP(ret, mpi_errno, MPI_ERR_OTHER, "**ptlget");
    REQ_PTL(rreq)->put_handler = NULL;
    REQ_PTL(rreq)->reply_handler = handler_recv_unpack_complete;

 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_HANDLER_RECV_DEQUEUE_LARGE);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}


#undef FUNCNAME
#define FUNCNAME handler_recv_dequeue_unpack_large
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
static int handler_recv_dequeue_unpack_large(const ptl_event_t *e)
{
    int mpi_errno = MPI_SUCCESS;
    MPIDI_STATE_DECL(MPID_STATE_HANDLER_RECV_DEQUEUE_UNPACK_LARGE);

    MPIDI_FUNC_ENTER(MPID_STATE_HANDLER_RECV_DEQUEUE_UNPACK_LARGE);

    dequeue_req(e);

    if (!(e->hdr_data & NPTL_LARGE)) {
        /* all data has already been received; we're done */
        mpi_errno = handler_recv_unpack_complete(e);
        if (mpi_errno) MPIU_ERR_POP(mpi_errno);
        goto fn_exit;
    }

    /* we need to receive more data.  record how much we've received so far */
    req->dev.recv_data_sz = e->mlength;

    MPIU_Assert(e->mlength == PTL_LARGE_THRESHOLD);
    unpack_byte(rreq->dev.segment_ptr, rreq->dev.segment_first, PTL_LARGE_THRESHOLD, REQ_PTL(rreq)->chunk_buffer[0],
                &REQ_PTL(rreq)->overflow[0]);
    rreq->dev.segment_first += PTL_LARGE_THRESHOLD;
    MPIU_Free(REQ_PTL(rreq)->chunk_buffer[0]);

    MPIU_CHKPMEM_MALLOC(REQ_PTL(rreq)->chunk_buffer[0], void *, rreq->dev.segment_size - rreq->dev.segment_first, mpi_errno, "chunk_buffer");
    
    ret = PtlGet(MPIDI_nem_ptl_global_md, (ptl_size_t)REQ_PTL(rreq)->chunk_buffer[0],
                 rreq->dev.segment_size - rreq->dev.segment_first, vc_ptl->id, vc_ptl->pt,
                 NPTL_HEADER_MATCH_BITS(e->hdr_data), 0, req);
    MPIU_ERR_CHKANDJUMP(ret, mpi_errno, MPI_ERR_OTHER, "**ptlget");
    REQ_PTL(rreq)->put_handler = NULL;
    REQ_PTL(rreq)->reply_handler = handler_recv_unpack_complete;

 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_HANDLER_DEQUEUE_RECV_UNPACK_LARGE);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}


#undef FUNCNAME
#define FUNCNAME MPID_nem_ptl_recv_posted
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
int MPID_nem_ptl_recv_posted(MPIDI_VC_t *vc, MPID_Request_t *rreq)
{
    int mpi_errno = MPI_SUCCESS;
    MPID_nem_ptl_vc_area *const vc_ptl = VC_PTL(vc);
    ptl_me_t me;
    int dt_contig;
    MPIDI_msg_sz_t data_sz;
    MPID_Datatype *dt_ptr;
    MPI_Aint dt_true_lb;
    MPI_Aint last;
    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_PTL_RECV_POSTED);

    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_PTL_RECV_POSTED);

    MPID_nem_ptl_init_req(rreq);
    
    me.ct_handle = PTL_CT_NONE;
    me.uid = PTL_UID_ANY;
    me.options = ( PTL_ME_OP_PUT | PTL_ME_IS_ACCESSIBLE | PTL_ME_EVENT_LINK_DISABLE |
                   PTL_ME_EVENT_UNLINK_DISABLE | PTL_ME_USE_ONCE );
    me.match_id = vc_ptl->id;
    me.match_bits = NPTL_MATCH(rreq->dev.match.parts.tag, rreq->dev.match.parts.context_id);
    me.ignore_bits = 0;
    me.min_free = 0;

    MPIDI_Datatype_get_info(rreq->dev.user_count, rreq->dev.datatype, dt_contig, data_sz, dt_ptr, dt_true_lb);

    if (data_sz < PTL_LARGE_THRESHOLD) {
        if (dt_contig) {
            /* small contig message */
            me.start = rreq->dev.user_buf;
            me.length = data_sz;
            REQ_PTL(rreq)->put_handler = handler_recv_dequeue_complete;
        } else {
            /* small noncontig */
            rreq->dev.segment_ptr = MPID_Segment_alloc();
            MPIU_ERR_CHKANDJUMP1(rreq->dev.segment_ptr == NULL, mpi_errno, MPI_ERR_OTHER, "**nomem", "**nomem %s", "MPID_Segment_alloc");
            MPID_Segment_init(rreq->dev.user_buf, rreq->dev.user_count, rreq->dev.datatype, rreq->dev.segment_ptr, 0);
            rreq->dev.segment_first = 0;
            rreq->dev.segment_size = data_sz;

            last = rreq->dev.segment_size;
            rreq->dev.iov_count = MPID_IOV_LIMIT;
            MPID_Segment_pack_vector(rreq->dev.segment_ptr, 0, &last, rreq->dev.iov, &rreq->dev.iov_count);

            if (last == rreq->dev.segment_size) {
                /* entire message fits in IOV */
                me.start = rreq->dev.iov;
                me.length = rreq->dev.iov_count;
                me.options |= PTL_IOVEC;
                REQ_PTL(rreq)->put_handler = handler_recv_dequeue_complete;
            } else {
                /* IOV is not long enough to describe entire message: recv into
                   buffer and unpack later */
                MPIU_CHKPMEM_MALLOC(REQ_PTL(req)->chunk_buffer[0], void *, data_sz, mpi_errno, "chunk_buffer");
                me.start = REQ_PTL(rreq)->chunk_buffer[0];
                me.length = data_sz;
                REQ_PTL(rreq)->put_handler = handler_recv_dequeue_unpack_complete;
            }
        }
    } else {
        /* Large message: Create an ME for the first chunk of data, then do a GET for the rest */
        if (dt_contig) {
            /* large contig message */
            me.start = rreq->dev.user_buf;
            me.length = PTL_LARGE_THRESHOLD;
            REQ_PTL(req_)->put_handler = handler_recv_dequeue_large;
        } else {
            /* large noncontig */
            rreq->dev.segment_ptr = MPID_Segment_alloc();
            MPIU_ERR_CHKANDJUMP1(rreq->dev.segment_ptr == NULL, mpi_errno, MPI_ERR_OTHER, "**nomem", "**nomem %s", "MPID_Segment_alloc");
            MPID_Segment_init(rreq->dev.user_buf, rreq->dev.user_count, rreq->dev.datatype, rreq->dev.segment_ptr, 0);
            rreq->dev.segment_first = 0;
            rreq->dev.segment_size = data_sz;

            last = PTL_LARGE_THRESHOLD;
            rreq->dev.iov_count = MPID_IOV_LIMIT;
            MPID_Segment_pack_vector(rreq->dev.segment_ptr, 0, &last, rreq->dev.iov, &rreq->dev.iov_count);

            if (last == PTL_LARGE_THRESHOLD) {
                /* first chunk fits in IOV */
                rreq->dev.segment_first = last;
                me.start = rreq->dev.iov;
                me.length = rreq->dev.iov_count;
                me.options |= PTL_IOVEC;
                REQ_PTL(rreq)->put_handler = handler_recv_dequeue_large;
            } else {
                /* IOV is not long enough to describe the first chunk: recv into
                   buffer and unpack later */
                MPIU_CHKPMEM_MALLOC(REQ_PTL(req)->chunk_buffer[0], void *, PTL_LARGE_THRESHOLD, mpi_errno, "chunk_buffer");
                me.start = REQ_PTL(req)->chunk_buffer[0];
                me.length = PTL_LARGE_THRESHOLD;
                REQ_PTL(rreq)->put_handler = handler_recv_dequeue_unpack_large;
            }
        }
        
    }

    ret = PtlMEAppend(MPIDI_nem_ptl_ni, MPIDI_nem_ptl_pt, &me, PTL_PRIORITY_LIST, rreq, &REQ_PTL(rreq)->me);
    MPIU_ERR_CHKANDJUMP(ret, mpi_errno, MPI_ERR_OTHER, "**ptlmeappend");


 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_PTL_RECV_POSTED);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPID_nem_ptl_cancel_recv
#undef FCNAME
#define FCNAME MPIU_QUOTE(FUNCNAME)
int MPID_nem_ptl_cancel_recv(MPIDI_VC_t *vc,  MPID_Request *rreq)
{
    int mpi_errno = MPI_SUCCESS;
    MPIDI_STATE_DECL(MPID_STATE_MPID_NEM_PTL_CANCEL_RECV);

    MPIDI_FUNC_ENTER(MPID_STATE_MPID_NEM_PTL_CANCEL_RECV);

    

 fn_exit:
    MPIDI_FUNC_EXIT(MPID_STATE_MPID_NEM_PTL_CANCEL_RECV);
    return mpi_errno;
 fn_fail:
    goto fn_exit;
}
