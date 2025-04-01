/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的传输控制层，负责管理与网络服务器的通信连接。
 * 它实现了基于 WebSocket 的双向通信协议，处理基站与服务器之间的数据交换。
 * 该模块是基站与网络服务器交互的核心组件，确保可靠的数据传输和连接管理。
 * 
 * 文件功能
 * ========
 * 本头文件定义了传输控制层的核心接口：
 * - WebSocket 连接管理
 * - 状态机和错误处理
 * - 重试和超时机制
 * - 凭证管理
 * - 数据交换控制
 * 
 * 主要组件
 * ========
 * 1. 连接状态管理
 *    - WebSocket 连接状态
 *    - 超时控制
 *    - 重试计数
 *    - 错误处理
 * 
 * 2. 状态机系统
 *    - 初始化状态
 *    - 信息请求状态
 *    - 多路复用状态
 *    - 错误状态
 * 
 * 3. 凭证管理
 *    - 凭证集选择
 *    - 认证处理
 *    - 安全连接
 * 
 * 4. 上下文管理
 *    - S2E上下文
 *    - 回调函数
 *    - URI管理
 * 
 * 关键流程
 * ========
 * 1. 连接建立流程
 *    - 初始化连接
 *    - 获取服务器URI
 *    - 建立WebSocket连接
 *    - 状态转换
 * 
 * 2. 错误处理流程
 *    - 超时检测
 *    - 重试机制
 *    - 错误恢复
 *    - 状态重置
 * 
 * 3. 数据传输流程
 *    - 数据封装
 *    - 发送控制
 *    - 接收处理
 *    - 确认机制
 * 
 * 注意事项
 * ========
 * 1. 连接管理
 *    - 连接状态监控
 *    - 自动重连机制
 *    - 超时处理
 *    - 资源释放
 * 
 * 2. 错误处理
 *    - 错误状态定义
 *    - 恢复策略
 *    - 日志记录
 *    - 用户通知
 * 
 * 3. 性能考虑
 *    - 连接复用
 *    - 缓冲区管理
 *    - 超时设置
 *    - 重试策略
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

#ifndef _tc_h_
#define _tc_h_

#include "ws.h"
#include "s2e.h"


enum {
    TC_INI            = 0,
    TC_INFOS_REQ_PEND    ,
    TC_INFOS_GOT_URI     ,
    TC_MUXS_REQ_PEND     ,
    TC_MUXS_CONNECTED    ,
    TC_MUXS_BACKOFF      ,
    TC_INFOS_BACKOFF     ,

    TC_ERR_FAILED        = -1,
    TC_ERR_NOURI         = -2,
    TC_ERR_TIMEOUT       = -3,
    TC_ERR_REJECTED      = -4,  // infos/muxs send back an error
    TC_ERR_CLOSED        = -5,
    TC_ERR_DEAD          = -6,
};

typedef struct tc {
    ws_t     ws;          // WS connection state
    tmr_t    timeout;
    s1_t     tstate;      // state of TC engine
    u1_t     credset;     // connect via this credential set
    u1_t     retries;
    char     muxsuri[MAX_URI_LEN+3];
    tmrcb_t  ondone;
    s2ctx_t  s2ctx;
} tc_t;


#define timeout2tc(p) memberof(tc_t, p, timeout)
#define conn2tc(p)    memberof(tc_t, p, ws)
#define s2ctx2tc(p)   memberof(tc_t, p, s2ctx)

extern tc_t* TC;

tc_t*  tc_ini   (tmrcb_t ondone);
void   tc_free  (tc_t* tc);
void   tc_start (tc_t* tc);
void   tc_ondone_default (tmr_t* timeout);
void   tc_continue (tc_t* tc);

#endif // _tc_h_
