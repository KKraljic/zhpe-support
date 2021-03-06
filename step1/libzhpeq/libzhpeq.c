/*
 * Copyright (C) 2017-2018 Hewlett Packard Enterprise Development LP.
 * All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <internal.h>

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>

#define PRINT_DEBUG printf("zhpe-support Within function: %s in file %s \n", __func__, __FILE__)

static_assert(sizeof(union zhpe_offloaded_hw_wq_entry) ==  ZHPE_OFFLOADED_ENTRY_LEN,
              "zhpe_offloaded_hw_wq_entry");
static_assert(sizeof(union zhpe_offloaded_hw_cq_entry) ==  ZHPE_OFFLOADED_ENTRY_LEN,
              "zhpe_offloaded_hw_cq_entry");

/* Set to 1 to dump qkdata when registered/exported/imported/freed. */
#define QKDATA_DUMP     (0)

#define LIBNAME         "libzhpeq"
#define BACKNAME        "libzhpeq_backend.so"

static pthread_mutex_t  init_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool             b_zhpe;
static struct backend_ops *b_ops;
static struct zhpeq_attr b_attr;

uuid_t                  zhpeq_uuid;

static void __attribute__((constructor)) lib_init(void)
{
    void                *dlhandle = dlopen(BACKNAME, RTLD_NOW);
    PRINT_DEBUG;

    if (!dlhandle) {
        print_err("Failed to load %s:%s\n", BACKNAME, dlerror());
        abort();
    }
}

void zhpeq_register_backend(enum zhpe_offloaded_backend backend, struct backend_ops *ops)
{
    PRINT_DEBUG;
    /* For the moment, the zhpe backend will only register if the zhpe device
     * can be opened and the libfabric backend will only register if the zhpe
     * device can't be opened.
     */

    switch (backend) {

    case ZHPEQ_BACKEND_LIBFABRIC:
        b_ops = ops;
        break;

    case ZHPEQ_BACKEND_ZHPE:
        b_zhpe = true;
        b_ops = ops;
        break;

    default:
        print_err("Unexpected backed %d\n", backend);
        break;
    }
}

int zhpeq_init(int api_version)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;
    static int          init_status = 1;

    if (init_status > 0) {
        if (!expected_saw("api_version", ZHPEQ_API_VERSION, api_version))
            goto done;

        mutex_lock(&init_mutex);
        if (b_ops->lib_init)
            ret = b_ops->lib_init(&b_attr);
        init_status = (ret <= 0 ? ret : 0);
        mutex_unlock(&init_mutex);
    }
    ret = init_status;
 done:

    return ret;
}

int zhpeq_query_attr(struct zhpeq_attr *attr)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;

    /* Compatibility handling is left for another day. */
    if (!attr)
        goto done;

    *attr = b_attr;
    ret = 0;

 done:

    return ret;
}

int zhpeq_domain_free(struct zhpeq_dom *zdom)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;

    if (!zdom)
        goto done;

    ret = 0;
    if (b_ops->domain_free)
        ret = b_ops->domain_free(zdom);
    free(zdom);

 done:
    return ret;
}

int zhpeq_domain_alloc(struct zhpeq_dom **zdom_out)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;
    struct zhpeq_dom    *zdom = NULL;

    if (!zdom_out)
        goto done;
    *zdom_out = NULL;

    ret = -ENOMEM;
    zdom = calloc_cachealigned(1, sizeof(*zdom));
    if (!zdom)
        goto done;

    ret = 0;
    if (b_ops->domain)
        ret = b_ops->domain(zdom);

 done:
    if (ret >= 0)
        *zdom_out = zdom;
    else
        (void)zhpeq_domain_free(zdom);

    return ret;
}

int zhpeq_free(struct zhpeq *zq)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;
    int                 rc;
    union xdm_active    active;

    if (!zq)
        goto done;
    /* Stop the queue. */
    iowrite64(1, zq->qcm + ZHPE_OFFLOADED_XDM_QCM_STOP_OFFSET);
    for (;;) {
        active.u64 =
            ioread64(zq->qcm + ZHPE_OFFLOADED_XDM_QCM_ACTIVE_STATUS_ERROR_OFFSET);
        if (!active.bits.active)
            break;
        sched_yield();
    }
    if (b_ops->qfree_pre)
        rc = b_ops->qfree_pre(zq);

    ret = 0;
    /* Unmap qcm, wq, and cq. */
    rc = do_munmap((void *)zq->qcm, zq->xqinfo.qcm.size);
    if (ret >= 0 && rc < 0)
        ret = rc;
    rc = do_munmap(zq->wq, zq->xqinfo.cmdq.size);
    if (ret >= 0 && rc < 0)
        ret = rc;
    rc = do_munmap(zq->cq, zq->xqinfo.cmplq.size);
    if (ret >= 0 && rc < 0)
        ret = rc;
    /* Call the driver to free the queue. */
    rc = b_ops->qfree(zq);
    if (ret >= 0 && rc < 0)
        ret = rc;
    /* Free queue memory. */
    free(zq->context);
    free(zq);

 done:
    return ret;
}

int zhpeq_alloc(struct zhpeq_dom *zdom, int cmd_qlen, int cmp_qlen,
                int traffic_class, int priority, int slice_mask,
                struct zhpeq **zq_out)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;
    struct zhpeq        *zq = NULL;
    union xdm_cmp_tail  tail = {
        .bits.toggle_valid = 1,
    };
    int                 flags;
    size_t              i;

    if (!zq_out)
        goto done;
    *zq_out = NULL;
    if (!zdom ||
        cmd_qlen < 2 || cmd_qlen > b_attr.z.max_tx_qlen ||
        cmp_qlen < 2 || cmp_qlen > b_attr.z.max_tx_qlen ||
        traffic_class < 0 || traffic_class > ZHPEQ_TC_MAX ||
        priority < 0 || priority > ZHPEQ_PRI_MAX ||
        (slice_mask & ~(ALL_SLICES | SLICE_DEMAND)))
        goto done;

    ret = -ENOMEM;
    zq = calloc_cachealigned(1, sizeof(*zq));
    if (!zq)
        goto done;
    zq->zdom = zdom;

    cmd_qlen = roundup_pow_of_2(cmd_qlen);
    cmp_qlen = roundup_pow_of_2(cmp_qlen);

    ret = b_ops->qalloc(zq, cmd_qlen, cmp_qlen, traffic_class,
                        priority, slice_mask);
    if (ret < 0)
        goto done;

    zq->context = calloc_cachealigned(zq->xqinfo.cmplq.ent,
                                      sizeof(*zq->context));
    if (!zq->context)
        goto done;

    /* Initialize context storage free list. */
    for (i = 0; i < zq->xqinfo.cmplq.ent - 1; i++)
        zq->context[i] = TO_PTR(i + 1);
    zq->context[i] = TO_PTR(FREE_END);
    /* context_free is zeroed. */

    /* zq->fd == -1 means we're faking things out. */
    flags = (zq->fd == -1 ? MAP_ANONYMOUS | MAP_PRIVATE : MAP_SHARED);
    /* Map registers, wq, and cq. */
    zq->qcm = do_mmap(NULL, zq->xqinfo.qcm.size, PROT_READ | PROT_WRITE,
                      flags, zq->fd, zq->xqinfo.qcm.off, &ret);
    if (!zq->qcm)
        goto done;
    zq->wq = do_mmap(NULL, zq->xqinfo.cmdq.size, PROT_READ | PROT_WRITE,
                     flags, zq->fd, zq->xqinfo.cmdq.off, &ret);
    if (!zq->wq)
        goto done;
    zq->cq = do_mmap(NULL, zq->xqinfo.cmplq.size, PROT_READ | PROT_WRITE,
                     flags, zq->fd, zq->xqinfo.cmplq.off, &ret);
    if (!zq->cq)
        goto done;
    if (b_ops->qalloc_post) {
        ret = b_ops->qalloc_post(zq);
        if (ret < 0)
            goto done;
    }

    /* Initialize completion tail to zero and set toggle bit. */
    iowrite64(tail.u64, zq->qcm + ZHPE_OFFLOADED_XDM_QCM_CMPL_QUEUE_TAIL_TOGGLE_OFFSET);
    /* Intialize command head and tail to zero. */
    iowrite64(0, zq->qcm + ZHPE_OFFLOADED_XDM_QCM_CMD_QUEUE_HEAD_OFFSET);
    iowrite64(0, zq->qcm + ZHPE_OFFLOADED_XDM_QCM_CMD_QUEUE_TAIL_OFFSET);
    /* Start the queue. */
    iowrite64(0, zq->qcm + ZHPE_OFFLOADED_XDM_QCM_STOP_OFFSET);
    ret = 0;

 done:
    if (ret >= 0)
        *zq_out = zq;
    else
        (void)zhpeq_free(zq);

    return ret;
}

int zhpeq_backend_exchange(struct zhpeq *zq, int sock_fd,
                           void *sa, size_t *sa_len)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;

    if (!zq || !sa || !sa_len)
        goto done;

    ret = b_ops->exchange(zq, sock_fd, sa, sa_len);

 done:
    return ret;
}

int zhpeq_backend_open(struct zhpeq *zq, void *sa)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;

    if (!zq)
        goto done;

    ret = b_ops->open(zq, sa);
 done:

    return ret;
}

int zhpeq_backend_close(struct zhpeq *zq, int open_idx)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;

    if (!zq)
        goto done;

    ret = b_ops->close(zq, open_idx);
 done:

    return ret;
}

int64_t zhpeq_reserve(struct zhpeq *zq, uint32_t n_entries)
{
    PRINT_DEBUG;
    int64_t             ret = -EINVAL;
    uint32_t            qmask;
    uint32_t            avail;
    struct zhpeq_ht     old;
    struct zhpeq_ht     new;

    if (!zq)
        goto done;
    qmask = zq->xqinfo.cmdq.ent - 1;
    if (!zq || n_entries < 1 || n_entries > qmask)
        goto done;

    ret = 0;
    for (old = atm_load_rlx(&zq->head_tail) ;;) {
        avail = qmask - (old.tail - old.head);
        if (avail < n_entries) {
            ret = -EAGAIN;
            break;
        }
        new.head = old.head;
        ret = old.tail;
        new.tail = old.tail + n_entries;
        if (atm_cmpxchg(&zq->head_tail, &old, new))
            break;
    }

 done:
    return ret;
}

int zhpeq_commit(struct zhpeq *zq, uint32_t qindex, uint32_t n_entries)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;
    uint32_t            qmask;
    uint32_t            old;
    uint32_t            new;

    if (!zq)
        goto done;

    qmask = zq->xqinfo.cmdq.ent - 1;

#ifdef HAVE_ZHPE_OFFLOADED_STATS
    zhpe_offloaded_stats_pause_all();
    uint32_t            i;
    union zhpe_offloaded_hw_wq_entry *wqe;

    for (i = 0; i < n_entries; i++) {
        wqe = zq->wq + ((qindex + i) & qmask);
        zhpe_offloaded_stats_stamp(zhpe_offloaded_stats_subid(ZHPQ, 60), (uintptr_t)zq,
                         wqe->hdr.cmp_index,
                         (uintptr_t)zq->context[wqe->hdr.cmp_index]);
    }
    zhpe_offloaded_stats_restart_all();
#endif

    old = atm_load_rlx(&zq->tail_commit);
    if (old != qindex) {
        ret = -EAGAIN;
        goto done;
    }
    new = old + n_entries;
    io_wmb();
    iowrite64(new & qmask,
              zq->qcm + ZHPE_OFFLOADED_XDM_QCM_CMD_QUEUE_TAIL_OFFSET);
    io_wmb();
    atm_store_rlx(&zq->tail_commit, new);
    ret = 0;

 done:
    return ret;
}

int zhpeq_signal(struct zhpeq *zq)
{
    PRINT_DEBUG;
    return b_ops->wq_signal(zq);
}

static inline void set_context(struct zhpeq *zq, union zhpe_offloaded_hw_wq_entry *wqe,
                               void *context)
{
    PRINT_DEBUG;
    struct free_index   old;
    struct free_index   new;

    for (old = atm_load_rlx(&zq->context_free);;) {
        if (unlikely(old.index == FREE_END)) {
            /* Tiny race between head moving and context slot freed. */
            sched_yield();
            old = atm_load_rlx(&zq->context_free);
            continue;
        }
        new.index = (int32_t)(uintptr_t)zq->context[old.index];
        new.seq = old.seq + 1;
        if (atm_cmpxchg(&zq->context_free, &old, new))
            break;
    }
    zq->context[old.index] = context;
    wqe->hdr.cmp_index = old.index;
}

static inline void *get_context(struct zhpeq *zq, struct zhpe_offloaded_cq_entry *cqe)
{
    PRINT_DEBUG;
    void                *ret = zq->context[cqe->index];
    struct free_index   old;
    struct free_index   new;

    for (old = atm_load_rlx(&zq->context_free) ;;) {
        zq->context[cqe->index] = TO_PTR(old.index);
        new.index = cqe->index;
        new.seq = old.seq + 1;
        if (atm_cmpxchg(&zq->context_free, &old, new))
            break;
    }

    return ret;
}

int zhpeq_nop(struct zhpeq *zq, uint32_t qindex, bool fence,
              void *context)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;
    union zhpe_offloaded_hw_wq_entry *wqe;

    if (!zq)
        goto done;
    if (!context)
        goto done;

    qindex = qindex & (zq->xqinfo.cmdq.ent - 1);
    wqe = zq->wq + qindex;

    wqe->hdr.opcode = ZHPE_OFFLOADED_HW_OPCODE_NOP;
    set_context(zq, wqe, context);

    ret = 0;

 done:
    return ret;
}

static inline int zhpeq_rw(struct zhpeq *zq, uint32_t qindex, bool fence,
                           uint64_t rd_addr, size_t len, uint64_t wr_addr,
                           void *context, uint16_t opcode)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;
    union zhpe_offloaded_hw_wq_entry *wqe;

    if (!zq)
        goto done;
    if (len > b_attr.z.max_dma_len)
        goto done;

    qindex = qindex & (zq->xqinfo.cmdq.ent - 1);
    wqe = zq->wq + qindex;

    opcode |= (fence ? ZHPE_OFFLOADED_HW_OPCODE_FENCE : 0);
    wqe->hdr.opcode = opcode;
    set_context(zq, wqe, context);
    wqe->dma.len = len;
    wqe->dma.rd_addr = rd_addr;
    wqe->dma.wr_addr = wr_addr;
    ret = 0;

 done:
    return ret;
}

int zhpeq_put(struct zhpeq *zq, uint32_t qindex, bool fence,
              uint64_t lcl_addr, size_t len, uint64_t rem_addr,
              void *context)
{
    PRINT_DEBUG;
    return zhpeq_rw(zq, qindex, fence, lcl_addr, len, rem_addr, context,
                    ZHPE_OFFLOADED_HW_OPCODE_PUT);
}

int zhpeq_puti(struct zhpeq *zq, uint32_t qindex, bool fence,
               const void *buf, size_t len, uint64_t remote_addr,
               void *context)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;
    union zhpe_offloaded_hw_wq_entry *wqe;

    if (!zq)
        goto done;
    if (!buf || !len || len > sizeof(wqe->imm.data))
        goto done;

    qindex = qindex & (zq->xqinfo.cmdq.ent - 1);
    wqe = zq->wq + qindex;

    wqe->hdr.opcode = ZHPE_OFFLOADED_HW_OPCODE_PUTIMM;
    wqe->hdr.opcode |= (fence ? ZHPE_OFFLOADED_HW_OPCODE_FENCE : 0);
    set_context(zq, wqe, context);
    wqe->imm.len = len;
    wqe->imm.rem_addr = remote_addr;
    memcpy(wqe->imm.data, buf, len);

    ret = 0;

 done:
    return ret;
}

int zhpeq_get(struct zhpeq *zq, uint32_t qindex, bool fence,
              uint64_t lcl_addr, size_t len, uint64_t rem_addr,
              void *context)
{
    PRINT_DEBUG;
    return zhpeq_rw(zq, qindex, fence, rem_addr, len, lcl_addr, context,
                    ZHPE_OFFLOADED_HW_OPCODE_GET);
}

int zhpeq_geti(struct zhpeq *zq, uint32_t qindex, bool fence,
               size_t len, uint64_t remote_addr, void *context)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;
    union zhpe_offloaded_hw_wq_entry *wqe;

    if (!zq)
        goto done;
    if (!len || len > sizeof(wqe->imm.data))
        goto done;

    qindex = qindex & (zq->xqinfo.cmdq.ent - 1);
    wqe = zq->wq + qindex;

    wqe->hdr.opcode = ZHPE_OFFLOADED_HW_OPCODE_GETIMM;
    wqe->hdr.opcode |= (fence ? ZHPE_OFFLOADED_HW_OPCODE_FENCE : 0);
    set_context(zq, wqe, context);
    wqe->imm.len = len;
    wqe->imm.rem_addr = remote_addr;

    ret = 0;
 done:
    return ret;
}

int zhpeq_atomic(struct zhpeq *zq, uint32_t qindex, bool fence, bool retval,
                 enum zhpeq_atomic_size datasize, enum zhpeq_atomic_op op,
                 uint64_t remote_addr, const union zhpeq_atomic *operands,
                 void *context)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;
    union zhpe_offloaded_hw_wq_entry *wqe;
    size_t              n_operands;

    if (!zq)
        goto done;
    if (!operands)
        goto done;

    qindex = qindex & (zq->xqinfo.cmdq.ent - 1);
    wqe = zq->wq + qindex;

    wqe->hdr.opcode = (fence ? ZHPE_OFFLOADED_HW_OPCODE_FENCE : 0);
    set_context(zq, wqe, context);

    switch (op) {

    case ZHPEQ_ATOMIC_ADD:
        wqe->hdr.opcode |= ZHPE_OFFLOADED_HW_OPCODE_ATM_ADD;
        n_operands = 1;
        break;

    case ZHPEQ_ATOMIC_CAS:
        wqe->hdr.opcode |= ZHPE_OFFLOADED_HW_OPCODE_ATM_CAS;
        n_operands = 2;
        break;

    case ZHPEQ_ATOMIC_SWAP:
        wqe->hdr.opcode |= ZHPE_OFFLOADED_HW_OPCODE_ATM_SWAP;
        n_operands = 1;
        break;

    default:
        goto done;
    }

    wqe->atm.size = (retval ? ZHPE_OFFLOADED_HW_ATOMIC_RETURN : 0);

    switch (datasize) {

    case ZHPEQ_ATOMIC_SIZE32:
        wqe->atm.size |= ZHPE_OFFLOADED_HW_ATOMIC_SIZE_32;
        break;

    case ZHPEQ_ATOMIC_SIZE64:
        wqe->atm.size |= ZHPE_OFFLOADED_HW_ATOMIC_SIZE_64;
        break;

    default:
        goto done;
    }

    wqe->hdr.cmp_index = qindex;
    wqe->atm.rem_addr = remote_addr;
    while (n_operands-- > 0)
        wqe->atm.operands[n_operands] = operands[n_operands].z;

    ret = 0;

 done:
    return ret;
}

int zhpeq_mr_reg(struct zhpeq_dom *zdom, const void *buf, size_t len,
                 uint32_t access, struct zhpeq_key_data **qkdata_out)
{
    PRINT_DEBUG;
    zhpe_offloaded_stats_start(zhpe_offloaded_stats_subid(ZHPQ, 0));

    int                 ret = -EINVAL;

    if (!qkdata_out)
        goto done;
    *qkdata_out = NULL;
    if (!zdom)
         goto done;

    ret = b_ops->mr_reg(zdom, buf, len, access, qkdata_out);
#if QKDATA_DUMP
    if (ret >= 0)
        zhpeq_print_qkdata(__func__, __LINE__, zdom, *qkdata_out);
#endif

 done:
    zhpe_offloaded_stats_stop(zhpe_offloaded_stats_subid(ZHPQ, 0));

    return ret;
}

int zhpeq_mr_free(struct zhpeq_dom *zdom, struct zhpeq_key_data *qkdata)
{
    PRINT_DEBUG;
    zhpe_offloaded_stats_start(zhpe_offloaded_stats_subid(ZHPQ, 10));

    int                 ret = 0;

    if (!qkdata)
        goto done;
    ret = -EINVAL;
    if (!zdom)
        goto done;

#if QKDATA_DUMP
    zhpeq_print_qkdata(__func__, __LINE__, zdom, qkdata);
#endif
    ret = b_ops->mr_free(zdom, qkdata);

 done:
    zhpe_offloaded_stats_stop(zhpe_offloaded_stats_subid(ZHPQ, 10));

    return ret;
}

int zhpeq_zmmu_import(struct zhpeq_dom *zdom, int open_idx, const void *blob,
                      size_t blob_len, bool cpu_visible,
                      struct zhpeq_key_data **qkdata_out)
{
    PRINT_DEBUG;
    zhpe_offloaded_stats_start(zhpe_offloaded_stats_subid(ZHPQ, 40));

    int                 ret = -EINVAL;

    if (!qkdata_out)
        goto done;
    *qkdata_out = NULL;
    if (!zdom || !blob)
        goto done;

    ret = b_ops->zmmu_import(zdom, open_idx, blob, blob_len, cpu_visible,
                             qkdata_out);
#if QKDATA_DUMP
    if (ret >= 0)
        zhpeq_print_qkdata(__func__, __LINE__, zdom, *qkdata_out);
#endif

 done:
    zhpe_offloaded_stats_stop(zhpe_offloaded_stats_subid(ZHPQ, 40));

    return ret;
}

int zhpeq_zmmu_fam_import(struct zhpeq_dom *zdom, int open_idx,
                          bool cpu_visible, struct zhpeq_key_data **qkdata_out)
{
    PRINT_DEBUG;
    int                 ret = -EINVAL;

    zhpe_offloaded_stats_start(zhpe_offloaded_stats_subid(ZHPQ, 20));

    if (!qkdata_out)
        goto done;
    *qkdata_out = NULL;
    if (!zdom)
        goto done;

    if (b_ops->zmmu_fam_import)
        ret = b_ops->zmmu_fam_import(zdom, open_idx, cpu_visible,  qkdata_out);
    else
        ret = -ENOSYS;

#if QKDATA_DUMP
    if (ret >= 0)
        zhpeq_print_qkdata(__func__, __LINE__, zdom, *qkdata_out);
#endif

 done:
    zhpe_offloaded_stats_stop(zhpe_offloaded_stats_subid(ZHPQ, 20));

    return ret;
}

int zhpeq_zmmu_export(struct zhpeq_dom *zdom,
                      const struct zhpeq_key_data *qkdata,
                      void *blob, size_t *blob_len)
{
    PRINT_DEBUG;
    zhpe_offloaded_stats_start(zhpe_offloaded_stats_subid(ZHPQ, 30));

    int                 ret = -EINVAL;
    struct zhpeq_mr_desc_v1 *desc = container_of(qkdata,
                                                 struct zhpeq_mr_desc_v1,
                                                 qkdata);

    if (!zdom || !qkdata || !blob || !blob_len ||
        desc->hdr.magic != ZHPE_OFFLOADED_MAGIC || desc->hdr.version != ZHPEQ_MR_V1)
        goto done;

#if QKDATA_DUMP
    zhpeq_print_qkdata(__func__, __LINE__, zdom, qkdata);
#endif
    ret = b_ops->zmmu_export(zdom, qkdata, blob, blob_len);

 done:
    zhpe_offloaded_stats_stop(zhpe_offloaded_stats_subid(ZHPQ, 30));

    return ret;
}

int zhpeq_zmmu_free(struct zhpeq_dom *zdom, struct zhpeq_key_data *qkdata)
{
    PRINT_DEBUG;
    int                 ret = 0;

    zhpe_offloaded_stats_start(zhpe_offloaded_stats_subid(ZHPQ, 50));

    if (!qkdata)
        goto done;
    ret = -EINVAL;
    if (!zdom)
        goto done;

#if 0
    zhpeq_print_qkdata(__func__, __LINE__, zdom, qkdata);
#endif
    ret = b_ops->zmmu_free(zdom, qkdata);

 done:
    zhpe_offloaded_stats_stop(zhpe_offloaded_stats_subid(ZHPQ, 50));

    return ret;
}

ssize_t zhpeq_cq_read(struct zhpeq *zq, struct zhpeq_cq_entry *entries,
                      size_t n_entries)
{
    PRINT_DEBUG;
    ssize_t             ret = -EINVAL;
    bool                polled = false;
    union zhpe_offloaded_hw_cq_entry *cqe;
    ssize_t             i;
    uint32_t            qmask;
    uint32_t            old;
    uint32_t            new;

    if (!zq || !entries || n_entries > SSIZE_MAX)
        goto done;

    qmask = zq->xqinfo.cmplq.ent - 1;

    for (i = 0, old = atm_load_rlx(&zq->head_tail.head) ; i < n_entries ;) {
        cqe = zq->cq + (old & qmask);
        if ((atm_load_rlx((uint8_t *)cqe) & ZHPE_OFFLOADED_HW_CQ_VALID) !=
             cq_valid(old, qmask)) {
            if (i > 0 || !b_ops->cq_poll || polled) {
                if (i == 0)
                    zhpe_offloaded_stats_stamp(zhpe_offloaded_stats_subid(ZHPQ, 70), (uintptr_t)zq);
                break;
            }
            ret = b_ops->cq_poll(zq, n_entries);
            if (ret < 0)
                goto done;
            polled = true;
            continue;
        }
        entries[i].z = cqe->entry;
        new = old + 1;
        if (!atm_cmpxchg(&zq->head_tail.head, &old, new))
            continue;
        entries[i].z.context = get_context(zq, &entries[i].z);
        zhpe_offloaded_stats_stamp(zhpe_offloaded_stats_subid(ZHPQ, 80), (uintptr_t)zq,
                         entries[i].z.index, (uintptr_t)entries[i].z.context);
        old = new;
        i++;
    }
    ret = i;

 done:
    return ret;
}

void zhpeq_print_info(struct zhpeq *zq)
{
    PRINT_DEBUG;
    const char          *b_str = "unknown";
    struct zhpe_offloaded_attr    *attr = &b_attr.z;

    switch (b_attr.backend) {

    case ZHPEQ_BACKEND_ZHPE:
        b_str = "zhpe_offloaded";
        break;

    case ZHPEQ_BACKEND_LIBFABRIC:
        b_str = "libfabric";
        break;

    default:
        break;
    }

    printf("%s:attributes\n", LIBNAME);
    printf("backend       : %s\n", b_str);
    printf("max_tx_queues : %u\n", attr->max_tx_queues);
    printf("max_rx_queues : %u\n", attr->max_rx_queues);
    printf("max_tx_qlen   : %u\n", attr->max_tx_qlen);
    printf("max_rx_qlen   : %u\n", attr->max_rx_qlen);
    printf("max_dma_len   : %Lu\n", (ullong)attr->max_dma_len);

    if (b_ops->print_info) {
        printf("\n");
        b_ops->print_info(zq);
    }
}

struct zhpeq_dom *zhpeq_dom(struct zhpeq *zq)
{
    PRINT_DEBUG;
    return zq->zdom;
}

int zhpeq_getaddr(struct zhpeq *zq, void *sa, size_t *sa_len)
{
    PRINT_DEBUG;
    ssize_t             ret = -EINVAL;

    if (!zq || !sa || !sa_len)
        goto done;

    ret = b_ops->getaddr(zq, sa, sa_len);
 done:

    return ret;
}

void zhpeq_print_qkdata(const char *func, uint line, struct zhpeq_dom *zdom,
                        const struct zhpeq_key_data *qkdata)
{
    PRINT_DEBUG;
    char                *id_str = NULL;

    if (b_ops->qkdata_id_str)
        id_str = b_ops->qkdata_id_str(zdom, qkdata);
    printf("%s,%u:%p %s\n", func, line, qkdata, (id_str ?: ""));
    printf("%s,%u:v/z/l 0x%Lx 0x%Lx 0x%Lx\n", func, line,
           (ullong)qkdata->z.vaddr, (ullong)qkdata->z.zaddr,
           (ullong)qkdata->z.len);
    printf("%s,%u:a/l 0x%Lx 0x%Lx\n", func, line,
           (ullong)qkdata->z.access, (ullong)qkdata->laddr);
}

static void print_qcm1(const char *func, uint line, const volatile void *qcm,
                      uint offset)
{
    PRINT_DEBUG;
    printf("%s,%u:qcm[0x%03x] = 0x%lx\n",
           func, line, offset, ioread64(qcm + offset));
}

void zhpeq_print_qcm(const char *func, uint line, const struct zhpeq *zq)
{
    PRINT_DEBUG;
    uint                i;

    printf("%s,%u:%s %p\n", func, line, __func__, zq->qcm);
    for (i = 0x00; i < 0x30; i += 0x08)
        print_qcm1(func, line, zq->qcm, i);
    for (i = 0x40; i < 0x108; i += 0x40)
        print_qcm1(func, line, zq->qcm, i);
}

bool zhpeq_is_asic(void)
{
    PRINT_DEBUG;
    return b_zhpe;
}
