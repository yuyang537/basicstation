/*
 * 模块概述
 * ========
 * 本文件实现了LoRaWAN基站的发送队列(TXQ)和接收队列(RXQ)系统，为无线通信提供
 * 缓冲和队列管理机制。该模块解决了多任务环境下无线数据包的排队、优先级处理
 * 和高效传输问题，是基站数据处理流程的关键组件。
 * 
 * 文件功能
 * ========
 * 本文件主要实现以下功能：
 * - 发送(TX)任务队列管理系统
 * - 发送数据缓冲区的分配与释放
 * - 接收(RX)任务队列管理系统
 * - 内存高效利用的紧凑化策略
 * - 队列指针和索引管理
 * 
 * 主要组件
 * ========
 * 1. 发送队列(TXQ)系统
 *    - 任务分配与释放机制
 *    - 数据空间预留与提交
 *    - 队列操作（添加、删除、遍历）
 *    - 内存紧凑化处理
 * 
 * 2. 接收队列(RXQ)系统
 *    - FIFO队列实现
 *    - 数据区段管理
 *    - 紧凑化和索引调整
 *    - 溢出处理策略
 * 
 * 3. 索引管理
 *    - 任务索引和指针转换
 *    - 链表遍历辅助函数
 *    - 特殊索引处理（NIL, END等）
 *    - 索引有效性验证
 * 
 * 4. 内存管理
 *    - 数据区段分配
 *    - 碎片整理
 *    - 空闲空间追踪
 *    - 资源耗尽处理
 * 
 * 关键流程
 * ========
 * 1. 发送任务处理流程
 *    - 任务与数据空间申请
 *    - 数据填充
 *    - 任务提交到发送队列
 *    - 任务完成与资源释放
 * 
 * 2. 接收数据流程
 *    - 接收任务创建
 *    - 数据段填充
 *    - 任务提交到接收队列
 *    - 数据处理与释放
 * 
 * 3. 内存紧凑化流程
 *    - 释放资源触发紧凑化
 *    - 数据移动和索引调整
 *    - 空闲空间合并
 *    - 内存一致性维护
 * 
 * 注意事项
 * ========
 * 1. 效率考虑
 *    - 尽量减少内存复制操作
 *    - 只在必要时进行紧凑化
 *    - 链表操作的原子性
 * 
 * 2. 内存限制
 *    - 固定大小的缓冲区管理
 *    - 资源耗尽的处理策略
 *    - 峰值负载的考虑
 * 
 * 3. 一致性保证
 *    - 索引和数据的一致性维护
 *    - 并发访问的安全处理
 *    - 边界条件的正确处理
 */

/*
 * --- Revised 3-Clause BSD License ---
 * Copyright Semtech Corporation 2022. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice,
 *       this list of conditions and the following disclaimer in the documentation
 *       and/or other materials provided with the distribution.
 *     * Neither the name of the Semtech corporation nor the names of its
 *       contributors may be used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION. BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "xq.h"


// --------------------------------------------------------------------------------
//
// TXQ
//
// --------------------------------------------------------------------------------
//
// TX jobs are not strictly FIFO and may trade places arbitrarily.
// Txjobs are managed in single linked lists. One for free jobs and one for each
// TX unit. Txjobs optionally have txdata attached. If a txjob is freed an
// associated txdata section is removed and txdata is compacted immediately.
// The remainder of txdata is always the available free data space.
//


void txq_ini (txq_t* txq) {
    memset(txq, 0, sizeof(*txq));
    for( txidx_t i=0; i<MAX_TXJOBS; i++ ) {
        txq->txjobs[i].next = i+1;
        txq->txjobs[i].off = TXOFF_NIL;
    }
    txq->txjobs[MAX_TXJOBS-1].next = TXIDX_END;
}


txjob_t* txq_idx2job (txq_t* txq, txidx_t idx) {
    if( idx == TXIDX_NIL || idx == TXIDX_END )
        return NULL;
    return &txq->txjobs[idx];
}


txidx_t txq_job2idx (txq_t* txq, txjob_t* job) {
    if( job==NULL )
        return TXIDX_NIL;
    return job - txq->txjobs;
}

txjob_t* txq_nextJob (txq_t* txq, txjob_t* j) {
    if( j == NULL )
        return NULL;
    assert(j->next != TXIDX_NIL);
    if( j->next == TXIDX_END )
        return NULL;
    return &txq->txjobs[j->next];
}


txidx_t* txq_nextIdx (txq_t* txq, txidx_t* pidx) {
    assert(*pidx != TXIDX_NIL);
    if( *pidx == TXIDX_END )
        return pidx;
    return &(txq->txjobs[*pidx].next);
}


txjob_t* txq_unqJob (txq_t* txq, txidx_t* pidx) {
    assert(*pidx != TXIDX_NIL);
    if( *pidx == TXIDX_END )
        return NULL;
    txjob_t* j = &txq->txjobs[*pidx];
    *pidx = j->next;
    j->next = TXIDX_NIL;
    return j;
}


void txq_insJob (txq_t* txq, txidx_t* pidx, txjob_t* j) {
    assert(*pidx != TXIDX_NIL && j->next == TXIDX_NIL);
    j->next = *pidx;
    *pidx = j - txq->txjobs;
}


void txq_freeJob (txq_t* txq, txjob_t* j) {
    txq_freeData(txq, j);
    txq_insJob(txq, &txq->freeJobs, j);
}


// Caller starts filling data but can walk away without having
// to free anything. Data is preserver only if commitJob() is called later.
txjob_t* txq_reserveJob (txq_t* txq) {
    txidx_t idx = txq->freeJobs;
    assert(idx != TXIDX_NIL);
    if( idx == TXIDX_END )
        return NULL;  // no more job available
    txjob_t* j = &txq->txjobs[idx];
    // Fields may have been filled but these should be like
    // that since job was not commited.
    assert(j->next != TXIDX_NIL);
    assert(j->off  == TXOFF_NIL);
    // Reset all fields to zero - previous reserveJob called might
    // have partially filled it and walked away.
    idx = j->next;
    memset(j, 0, sizeof(*j));
    j->off = TXOFF_NIL;
    j->next = idx;
    return j;
}


u1_t* txq_reserveData (txq_t* txq, txoff_t maxlen) {
    if( maxlen > MAX_TXDATA - txq->txdataInUse )
        return NULL;  // no enough data space
    return &txq->txdata[txq->txdataInUse];
}


void txq_commitJob (txq_t* txq, txjob_t*j) {
    assert(j == &txq->txjobs[txq->freeJobs]);
    assert(j->len <= MAX_TXDATA - txq->txdataInUse);
    assert(j->off == TXOFF_NIL);
    // Unqueue free head
    txq->freeJobs = j->next;
    j->next = TXIDX_NIL;
    j->off = txq->txdataInUse;
    txq->txdataInUse += j->len;
}




void txq_freeData (txq_t* txq, txjob_t* j) {
    
    // If job had data compactify and fix offset of jobs in any tx queues
    txoff_t freeOff = j->off;
    if( freeOff == TXOFF_NIL )
        return;
    u1_t freeLen = j->len;
    for( txidx_t idx=0; idx<MAX_TXJOBS; idx++ ) {
        txjob_t* fixjob = &txq->txjobs[idx];
        if( fixjob->off != TXOFF_NIL && fixjob->off >= freeOff )
            fixjob->off -= freeLen;
    }
    txoff_t freeEnd = freeOff + freeLen;
    if( freeEnd < txq->txdataInUse )
        memcpy(&txq->txdata[freeOff], &txq->txdata[freeEnd], txq->txdataInUse - freeEnd);
    txq->txdataInUse -= freeLen;
    j->off = TXOFF_NIL;
    j->len = 0;
}


// --------------------------------------------------------------------------------
//
// RXQ
//
// --------------------------------------------------------------------------------

// RX state maintains to FIFO queues for rxjobs and frame data (rxdata).
// FIFO is emptied by serializing an rxjob/rxdata into JSON and passing it along
// to a websocket.
// FIFO is filled by getting frames from the radio layer and filling a rxjob and
// appending rxdata.
// The used area in rxjobs/rxdata wanders to the back the available arrays.
// If the rest is exhausted the used area is shifted to the beginning and indices
// are adjusted.
//
//      off     last
//      first   next
//       |      |                      |      |  compaction
//  |----|xxxxxx|----|         |-------|xxxxxx|    ==>  |xxxxxx|-------|
//
//

void rxq_ini (rxq_t* rxq) {
    rxq->first = rxq->next = 0;
}

// Allocate next job and optionally compact if we need space.
// Rxjob is only earmarked
//  - in case of error caller never comes back
//  - if data is filled in caller must invoke rxq_commitJob
// Return NULL if no more space
rxjob_t* rxq_nextJob (rxq_t* rxq) {
    rxjob_t* jobs = rxq->rxjobs;
    rxidx_t first = rxq->first;
    rxidx_t next = rxq->next;
    if( first==next ) {
        rxq->first = rxq->next = 0;
        jobs[0].off = jobs[0].len = 0;
        jobs[0].fts = -1;
        return &jobs[0];
    }
    if( next >= MAX_RXJOBS ) {
        // No enough jobs or buffer space - compact?
        if( first == 0 ) {
            LOG(MOD_S2E|WARNING, "RX out of jobs");
            return NULL;
        }
        memmove(&jobs[0], &jobs[first], sizeof(jobs[0])*(next-first));
        rxq->next = next -= first;
        rxq->first = first = 0;
    }

    rxjob_t* last = &jobs[next-1];
    rxoff_t end = last->off + last->len;
 check_rxdata:
    if( end + MAX_RXFRAME_LEN > MAX_RXDATA ) {
        // No enough data or buffer space - compact?
        rxoff_t off = jobs[first].off;
        if( off == 0 ) {
            LOG(MOD_S2E|WARNING, "RX out of data space");
            return NULL;
        }
        memmove(&rxq->rxdata[0], &rxq->rxdata[off], end-off);
        for( int i=first; i < next; i++ )
            jobs[i].off -= off;
        end -= off;
        goto check_rxdata;
    }
    last += 1;
    last->off = end;
    last->len = 0;
    last->fts = -1;
    return last;
}

void rxq_commitJob (rxq_t* rxq, rxjob_t* p) {
    assert(p == &rxq->rxjobs[rxq->next]);
    rxq->next += 1;
}

// Drop job j from list and return new pointer to last job
// Used to delete shadow frames.
rxjob_t* rxq_dropJob (rxq_t* rxq, rxjob_t* p) {
    rxjob_t* jobs = rxq->rxjobs;
    u1_t* rxdata = rxq->rxdata;
    rxjob_t* last = &jobs[--rxq->next];
    rxoff_t poff = p->off;
    rxoff_t pend = poff + p->len;
    assert(p >= jobs && p < last);
    memmove(&rxdata[poff], &rxdata[pend], last->off + last->len - pend);
    memmove(&p[0], &p[1], sizeof(jobs[0])*(last-p+1));
    poff = pend-poff;
    while( p <= last ) {
        p->off -= poff;
        p++;
    }
    return last-1;
}
