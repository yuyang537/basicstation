/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的配置与更新服务（CUPS）客户端实现。
 * 它负责与 CUPS 服务器通信，处理基站的远程配置更新、证书管理和固件升级。
 * 该模块是基站远程维护的关键组件，确保基站能够安全地接收最新配置和软件更新。
 * 
 * 文件功能
 * ========
 * 本头文件定义了 CUPS 客户端的核心接口：
 * - CUPS 客户端初始化和控制
 * - 更新过程状态管理
 * - 数据接收和处理
 * - 更新验证和应用
 * - 超时和错误处理
 * 
 * 主要组件
 * ========
 * 1. CUPS 客户端核心
 *    - 客户端初始化和配置
 *    - 连接管理
 *    - 状态控制
 *    - 超时处理
 * 
 * 2. 更新处理系统
 *    - 更新标志管理
 *    - 数据分段接收
 *    - 临时缓冲区控制
 *    - 更新完整性检查
 * 
 * 3. 证书管理系统
 *    - CUPS 证书处理
 *    - TC（传输控制）证书处理
 *    - 证书验证和存储
 *    - 安全更新机制
 * 
 * 4. 签名验证系统
 *    - 签名数据接收
 *    - 签名验证处理
 *    - 安全检查
 *    - 证书链验证
 * 
 * 关键流程
 * ========
 * 1. 更新检查流程
 *    - 初始化 CUPS 客户端
 *    - 连接到 CUPS 服务器
 *    - 发送更新请求
 *    - 接收更新响应
 * 
 * 2. 更新接收流程
 *    - 接收 URI 更新
 *    - 接收证书更新
 *    - 接收签名数据
 *    - 接收软件更新
 * 
 * 3. 更新应用流程
 *    - 验证签名
 *    - 备份当前配置
 *    - 应用新配置
 *    - 完成更新过程
 * 
 * 注意事项
 * ========
 * 1. 安全考虑
 *    - 签名验证的重要性
 *    - 证书链验证
 *    - 更新完整性检查
 * 
 * 2. 容错机制
 *    - 网络错误处理
 *    - 更新回滚能力
 *    - 部分更新处理
 * 
 * 3. 性能影响
 *    - 更新时系统负载
 *    - 内存使用考虑
 *    - 网络带宽需求
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

#ifndef _cups_h_
#define _cups_h_

#include "http.h"

enum {
    CUPS_INI            = 0,
    CUPS_HTTP_REQ_PEND     ,
    CUPS_FEED_CUPS_URI     ,  // never used - both expected to fit into HTTP buffer
    CUPS_FEED_TC_URI       ,  //  ditto       together with HTTP header
    CUPS_FEED_CUPS_CRED    ,
    CUPS_FEED_TC_CRED      ,
    CUPS_FEED_SIGNATURE    ,
    CUPS_FEED_UPDATE       ,
    CUPS_DONE              ,

    CUPS_ERR_FAILED        = -1,
    CUPS_ERR_NOURI         = -2,
    CUPS_ERR_TIMEOUT       = -3,
    CUPS_ERR_REJECTED      = -4,
    CUPS_ERR_CLOSED        = -5,
    CUPS_ERR_DEAD          = -6,
};

#define UPDATE_FLAG(n) (1 << (CUPS_FEED_##n - CUPS_FEED_CUPS_URI))

typedef struct cups_sig cups_sig_t;

typedef struct cups {
    http_t   hc;          // HTTP connection state
    tmr_t    timeout;
    s1_t     cstate;      // state of CUPS engine
    u1_t     uflags;      // update flags - which parts have been updated
    u1_t     temp_n;
    u1_t     temp[4];     // assemble length fields
    int      segm_off;
    int      segm_len;
    tmrcb_t  ondone;
    cups_sig_t* sig;
} cups_t;

#define timeout2cups(p) memberof(cups_t, p, timeout)
#define conn2cups(p)    memberof(cups_t, p, hc.c)

cups_t* cups_ini ();
void    cups_free (cups_t* cups);
void    cups_start (cups_t* cups);


#endif // _cups_h_
