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

/*
 * XQ - 扩展队列管理系统头文件
 * ============================
 * 功能描述：高性能的发送(TXQ)和接收(RXQ)队列管理系统
 * 核心职责：
 *   1. TXQ管理：LoRa下行数据包的调度和队列管理
 *   2. RXQ管理：LoRa上行数据包的缓存和处理
 *   3. 内存优化：零拷贝设计，分离元数据和数据载荷
 *   4. 队列算法：TXQ使用优先级队列，RXQ使用FIFO队列
 *   5. 资源管理：动态内存分配和智能垃圾回收
 * 
 * 设计特点：
 *   - 分离式存储：作业描述符与数据载荷独立存储
 *   - 链表结构：高效的插入、删除和遍历操作
 *   - 内存紧凑：支持内存碎片整理和空间优化
 *   - 类型安全：强类型检查，防止队列操作错误
 */

#ifndef _xq_h_
#define _xq_h_

#include "rt.h"      // 运行时基础类型
#include "s2conf.h"  // S2配置常量定义

// ==============================================
// 队列索引和偏移类型定义
// ==============================================

typedef u2_t txoff_t;  // 发送数据偏移量类型：标记数据在缓冲区中的位置
typedef u1_t txidx_t;  // 发送作业索引类型：标识队列中的作业编号

// ==============================================
// 特殊索引值常量
// ==============================================

enum { TXIDX_NIL = 255 };  // 空索引值：表示无效或未分配的索引
enum { TXIDX_END = 254 };  // 结束索引值：表示链表或队列的末尾
enum { TXOFF_NIL = 0xFFFF };  // 空偏移值：表示无数据或无效偏移

// ==============================================
// 发送作业结构定义
// ==============================================

/**
 * struct txjob - 发送作业描述符
 * 功能：描述一个完整的LoRa下行传输任务
 * 包含：时间调度、设备信息、射频参数、数据位置等
 */
typedef struct txjob {
    // -------------------- 时间调度参数 --------------------
    ustime_t txtime;    // 发送时间：绝对时间戳，微秒精度
    sL_t     gpstime;   // GPS时间：用于Class B同步和信标
    sL_t     xtime;     // 扩展时间：高精度硬件时间戳
    
    // -------------------- 设备和会话信息 --------------------
    uL_t     deveui;    // 设备EUI：目标设备的唯一标识符
    sL_t     diid;      // 设备交互ID：用于消息跟踪和确认
    sL_t     rctx;      // 请求上下文：关联上行消息的上下文信息
    
    // -------------------- 射频参数 --------------------
    u4_t     freq;      // 发送频率：Hz单位的载波频率
    u4_t     rx2freq;   // RX2窗口频率：第二接收窗口的频率
    u4_t     airtime;   // 空中时间：数据包的传输时间（微秒）
    s2_t     txpow;     // 发送功率：dBm单位，按TXPOW_SCALE缩放
    u1_t     dr;        // 数据速率：LoRaWAN数据速率索引
    u1_t     rx2dr;     // RX2数据速率：第二接收窗口的数据速率
    
    // -------------------- 硬件和天线配置 --------------------
    u1_t     txunit;    // 发送单元：当前队列所属的TX路径编号
    u1_t     altAnts;   // 备用天线：可选的天线配置选项
    u1_t     rxdelay;   // 接收延迟：RX1窗口的延迟时间
    
    // -------------------- 协议参数 --------------------
    u1_t     txflags;   // 发送标志：见s2e.h中的TXFLAGS_*定义
    u1_t     retries;   // 重试次数：Class C消息的发送尝试次数
    u1_t     addcrc;    // CRC标志：是否为LoRa下行帧添加CRC
    u2_t     preamble;  // 前导码长度：0表示使用默认值
    
    // -------------------- 数据信息 --------------------
    u1_t     len;       // 帧长度：数据载荷的字节数
    u1_t     prio;      // 优先级：调度优先级，数值越小优先级越高
    txoff_t  off;       // 数据偏移：帧在txdata中的起始位置
    
    // -------------------- 内部管理 --------------------
    txidx_t  next;      // 下一个索引：链表中下一个作业的索引
    u1_t     dnchnl;    // 下行信道号：内部使用，用于占空比跟踪
    u1_t     dnchnl2;   // RX2信道号：内部使用，用于占空比跟踪
} txjob_t;

// ==============================================
// 发送队列结构定义
// ==============================================

/**
 * struct txq - 发送队列管理结构
 * 功能：管理所有待发送的LoRa下行数据包
 * 特点：分离式存储，优先级调度，内存紧凑化
 */
typedef struct txq {
    txjob_t txjobs[MAX_TXJOBS];  // 作业池：所有发送作业的存储空间
    u1_t    txdata[MAX_TXDATA];  // 数据池：所有待发送数据的存储空间
    txidx_t freeJobs;            // 空闲作业链表：可分配作业的链表头
    txoff_t txdataInUse;         // 数据使用量：从此位置到缓冲区末尾为空闲空间
} txq_t;

// ==============================================
// TXQ管理API函数声明
// ==============================================

void     txq_ini      (txq_t* txq);                    // 初始化发送队列
txidx_t  txq_job2idx  (txq_t* txq, txjob_t* j);        // 作业指针转索引
txjob_t* txq_idx2job  (txq_t* txq, txidx_t  i);        // 索引转作业指针
txjob_t* txq_nextJob  (txq_t* txq, txjob_t* j);        // 获取下一个作业
txidx_t* txq_nextIdx  (txq_t* txq, txidx_t* pi);       // 获取下一个索引指针
txjob_t* txq_unqJob   (txq_t* txq, txidx_t* pi);       // 从队列移除作业
void     txq_insJob   (txq_t* txq, txidx_t* pi, txjob_t* j);  // 插入作业到队列
void     txq_freeJob  (txq_t* txq, txjob_t* j);        // 释放作业资源
void     txq_freeData (txq_t* txq, txjob_t* j);        // 释放数据资源
txjob_t* txq_reserveJob  (txq_t* txq);                 // 预留作业空间
u1_t*    txq_reserveData (txq_t* txq, txoff_t maxlen); // 预留数据空间
void     txq_commitJob   (txq_t* txq, txjob_t*j);      // 提交作业到队列

// ==============================================
// 接收队列类型定义
// ==============================================

typedef u2_t rxoff_t;  // 接收数据偏移量类型
typedef u1_t rxidx_t;  // 接收作业索引类型

/**
 * struct rxjob - 接收作业描述符
 * 功能：描述一个接收到的LoRa上行数据包
 * 包含：时间信息、射频参数、信号质量、数据位置
 */
typedef struct rxjob {
    // -------------------- 时间和上下文 --------------------
    sL_t     rctx;      // 请求上下文：接收上下文标识符
    sL_t     xtime;     // 接收时间：硬件时间戳
    s4_t     fts;       // 细粒度时间戳：高精度时间信息
    
    // -------------------- 射频参数 --------------------
    u4_t     freq;      // 接收频率：数据包的载波频率
    u1_t     dr;        // 数据速率：LoRaWAN数据速率索引
    
    // -------------------- 信号质量 --------------------
    u1_t     rssi;      // RSSI值：接收信号强度，按-1缩放
    s1_t     snr;       // SNR值：信噪比，按4倍缩放
    
    // -------------------- 数据信息 --------------------
    rxoff_t  off;       // 数据偏移：帧在rxdata中的起始位置
    u1_t     len;       // 帧长度：数据载荷的字节数
} rxjob_t;

// ==============================================
// 接收队列结构定义
// ==============================================

/**
 * struct rxq - 接收队列管理结构
 * 功能：管理所有接收到的LoRa上行数据包
 * 特点：FIFO队列，保序处理，循环缓冲区
 */
typedef struct rxq {
    rxjob_t rxjobs[MAX_RXJOBS];  // 作业数组：所有接收作业的存储空间
    u1_t    rxdata[MAX_RXDATA];  // 数据数组：所有接收数据的存储空间
    rxidx_t first;               // 队列头：第一个有效作业的索引
    rxidx_t next;                // 队列尾：下一个可填充位置的索引
} rxq_t;

// ==============================================
// RXQ管理API函数声明
// ==============================================

void     rxq_ini       (rxq_t* rxq);           // 初始化接收队列
rxjob_t* rxq_nextJob   (rxq_t* rxq);           // 获取下一个接收作业
void     rxq_commitJob (rxq_t* rxq, rxjob_t* p);  // 提交接收作业
rxjob_t* rxq_dropJob   (rxq_t* rxq, rxjob_t* p);  // 丢弃接收作业

#endif // _xq_h_
