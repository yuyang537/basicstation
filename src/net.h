/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的网络通信层，负责管理与网络服务器的连接和数据传输。
 * 它实现了基于 TLS 的安全通信，支持 WebSocket 协议，提供可靠的双向通信通道。
 * 该模块是基站与网络服务器之间的通信桥梁，确保数据传输的安全性和可靠性。
 * 
 * 文件功能
 * ========
 * 本头文件定义了网络通信层的核心接口：
 * - 连接管理和状态控制
 * - 数据缓冲区管理
 * - TLS 安全通信
 * - URI 解析和处理
 * - WebSocket 帧处理
 * 
 * 主要组件
 * ========
 * 1. 连接管理
 *    - 连接状态控制
 *    - 事件回调机制
 *    - 超时处理
 *    - 认证管理
 * 
 * 2. 缓冲区系统
 *    - 读缓冲区管理
 *    - 写缓冲区管理
 *    - WebSocket 帧处理
 *    - 数据流控制
 * 
 * 3. 安全通信
 *    - TLS 上下文管理
 *    - 证书处理
 *    - 安全配置
 *    - 认证令牌
 * 
 * 4. URI处理
 *    - URI 解析
 *    - 协议识别
 *    - 主机和端口提取
 *    - 路径处理
 * 
 * 关键流程
 * ========
 * 1. 连接建立流程
 *    - 网络连接创建
 *    - TLS 握手
 *    - WebSocket 升级
 *    - 认证处理
 * 
 * 2. 数据传输流程
 *    - 数据帧封装
 *    - 缓冲区管理
 *    - 流量控制
 *    - 错误处理
 * 
 * 3. 连接维护流程
 *    - 心跳检测
 *    - 重连机制
 *    - 资源清理
 *    - 状态同步
 * 
 * 注意事项
 * ========
 * 1. 安全考虑
 *    - TLS 配置安全
 *    - 证书验证
 *    - 认证令牌保护
 *    - 数据加密
 * 
 * 2. 性能优化
 *    - 缓冲区大小设置
 *    - 内存使用控制
 *    - 数据拷贝最小化
 *    - 异步IO处理
 * 
 * 3. 可靠性保证
 *    - 连接状态监控
 *    - 错误恢复机制
 *    - 资源泄漏防护
 *    - 超时处理
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

#ifndef _net_h_
#define _net_h_

#include "mbedtls/net_sockets.h"
#include "rt.h"
#include "tls.h"

struct conn;
typedef void (*evcb_t)(struct conn*, int ev);
typedef mbedtls_net_context netctx_t;

typedef struct conn {
    aio_t*   aio;
    tmr_t    tmr;
    // Read side
    u1_t*    rbuf;
    doff_t   rbufsize;
    doff_t   rpos;     // socket fills in data here
    doff_t   rbeg;     // oldest frame in recv buffer, rbeg[-1] is OPCODE
    doff_t   rend;     // end of frame, after that starts a WS header
    // Write side
    u1_t*    wbuf;
    doff_t   wbufsize;
    doff_t   wpos;     // socket reads data from here and sends it
    doff_t   wend;     // end of WS frame, after that 2 bytes frame length + frame data
    doff_t   wfill;    // local producers fill in data here

    u1_t     state;
    s1_t     optemp;   // some temp value related to opctx
    u2_t     creason;  // close reason
    evcb_t   evcb;

    netctx_t   netctx;
    tlsctx_p   tlsctx;
    tlsconf_t* tlsconf;   // or NULL if shared and stored someplace else
    str_t      authtoken;

    void*      opctx;     // context managing this connection

    char* host;
    char* port;
    char* uripath;
} conn_t;


// conn_t.evcb must not be NULL - use this
void conn_evcb_nil(conn_t* conn, int wsev);
int  conn_setup_tls (conn_t* conn, int cred_cat, int cred_set, const char* servername);


struct uri_info {
    doff_t schemeEnd;
    doff_t hostportBeg;
    doff_t hostportEnd;
    doff_t hostBeg;
    doff_t hostEnd;
    doff_t portBeg;
    doff_t portEnd;
    doff_t pathBeg;
    doff_t pathEnd;
};

int uri_isScheme (const char* uri, const char* scheme);
int uri_parse    (dbuf_t* b, struct uri_info* u, int skipSchema);
enum { URI_BAD=0, URI_TCP, URI_TLS };
int uri_checkHostPortUri (const char* uri,
                          const char* scheme,
                          char* host, int hostlen,
                          char* port, int portlen);

#endif // _net_h_
