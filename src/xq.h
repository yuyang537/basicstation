/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的发送和接收队列管理系统，负责协调基站与终端设备之间的数据传输。
 * 它提供了发送队列（TXQ）和接收队列（RXQ）的实现，用于管理无线通信中的数据包处理。
 * 该模块确保了数据包的有序传输和接收，是基站通信能力的核心组件。
 * 
 * 文件功能
 * ========
 * 本头文件定义了发送和接收队列的核心接口：
 * - 发送作业（txjob）的结构定义和管理
 * - 接收作业（rxjob）的结构定义和管理
 * - 队列操作函数
 * - 数据缓冲区管理
 * - 作业优先级控制
 * 
 * 主要组件
 * ========
 * 1. 发送队列系统（TXQ）
 *    - 发送作业池（txjobs）
 *    - 发送数据缓冲区（txdata）
 *    - 作业链接和管理
 *    - 作业预留和提交机制
 * 
 * 2. 接收队列系统（RXQ）
 *    - 接收作业池（rxjobs）
 *    - 接收数据缓冲区（rxdata）
 *    - 接收数据处理
 *    - RSSI和SNR信息管理
 * 
 * 3. 资源管理系统
 *    - 缓冲区分配与回收
 *    - 作业索引管理
 *    - 内存使用优化
 * 
 * 4. 调度系统
 *    - 作业优先级管理
 *    - 时间安排控制
 *    - 重试机制
 * 
 * 关键流程
 * ========
 * 1. 发送处理流程
 *    - 预留发送作业
 *    - 分配数据缓冲区
 *    - 填充作业参数
 *    - 提交作业到队列
 *    - 作业执行和释放
 * 
 * 2. 接收处理流程
 *    - 获取下一个接收作业
 *    - 填充接收数据
 *    - 提交接收作业
 *    - 处理接收到的数据
 *    - 释放接收作业
 * 
 * 3. 资源管理流程
 *    - 初始化队列资源
 *    - 动态分配缓冲区
 *    - 释放已完成作业
 *    - 回收资源
 * 
 * 注意事项
 * ========
 * 1. 资源限制
 *    - 作业池大小限制
 *    - 数据缓冲区容量限制
 *    - 内存使用效率
 * 
 * 2. 调度考虑
 *    - 作业优先级处理
 *    - 时间关键型操作
 *    - 资源争用问题
 * 
 * 3. 性能因素
 *    - 队列处理效率
 *    - 内存分配效率
 *    - 作业链接管理效率
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

#ifndef _xq_h_
#define _xq_h_

#include "rt.h"
#include "s2conf.h"

typedef u2_t txoff_t;
typedef u1_t txidx_t;

enum { TXIDX_NIL = 255 };
enum { TXIDX_END = 254 };
enum { TXOFF_NIL = 0xFFFF };

typedef struct txjob {
    ustime_t txtime;
    uL_t     deveui;
    sL_t     diid;     // device interaction ID (was 'seqno')
    sL_t     rctx;
    sL_t     gpstime;
    sL_t     xtime;
    u4_t     freq;
    u4_t     rx2freq;
    u4_t     airtime;
    txidx_t  next;     // next index in txjobs or TXIDX_END, if not q'd TXIDX_NIL
    txoff_t  off;      // frame start in txdata or TXOFF_NIL if none
    s2_t     txpow;    // (scaled by TXPOW_SCALE)
    u1_t     txunit;   // currently queued for this TX path
    u1_t     altAnts;  // alternate antennas
    u1_t     txflags;  // see TXFLAGS_* in s2e.h
    u1_t     retries;  // class C: TX attempts
    u1_t     dr;
    u1_t     rx2dr;
    u1_t     rxdelay;
    u1_t     len;     // frame length
    u1_t     prio;    // priority
    u1_t     dnchnl;  // channel number (internal use only - for DC tracking)
    u1_t     dnchnl2; //   -ditto- RX2
    u1_t     addcrc;   // add CRC to Lora DN frame
    u2_t     preamble; // preamble length - if zero use default
} txjob_t;

typedef struct txq {
    txjob_t txjobs[MAX_TXJOBS];  // pool of txjobs
    u1_t    txdata[MAX_TXDATA];  // pool for pending txdata
    txidx_t freeJobs;            // linked list of free txjob elements
    txoff_t txdataInUse;         // free buffer space from here to end of txdata
} txq_t;


void     txq_ini      (txq_t* txq);
txidx_t  txq_job2idx  (txq_t* txq, txjob_t* j);
txjob_t* txq_idx2job  (txq_t* txq, txidx_t  i);
txjob_t* txq_nextJob  (txq_t* txq, txjob_t* j);
txidx_t* txq_nextIdx  (txq_t* txq, txidx_t* pi);
txjob_t* txq_unqJob   (txq_t* txq, txidx_t* pi);
void     txq_insJob   (txq_t* txq, txidx_t* pi, txjob_t* j);
void     txq_freeJob  (txq_t* txq, txjob_t* j);
void     txq_freeData (txq_t* txq, txjob_t* j);
txjob_t* txq_reserveJob  (txq_t* txq);
u1_t*    txq_reserveData (txq_t* txq, txoff_t maxlen);
void     txq_commitJob   (txq_t* txq, txjob_t*j);


typedef u2_t rxoff_t;
typedef u1_t rxidx_t;

typedef struct rxjob {
    sL_t     rctx;
    sL_t     xtime;
    s4_t     fts;
    u4_t     freq;
    rxoff_t  off;    // frame start in rxdata
    u1_t     rssi;   // scaled RSSI (*-1)
    s1_t     snr;    // scaled SNR (*4)
    u1_t     dr;
    u1_t     len;    // frame end
} rxjob_t;

typedef struct rxq {
    rxjob_t rxjobs[MAX_RXJOBS];
    u1_t    rxdata[MAX_RXDATA];
    rxidx_t first;   // first filled job
    rxidx_t next;    // next job to fill
} rxq_t;


void     rxq_ini       (rxq_t* rxq);
rxjob_t* rxq_nextJob   (rxq_t* rxq);
void     rxq_commitJob (rxq_t* rxq, rxjob_t* p);
rxjob_t* rxq_dropJob   (rxq_t* rxq, rxjob_t* p);


#endif // _xq_h_
