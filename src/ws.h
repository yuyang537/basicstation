/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的 WebSocket 协议实现层，负责管理基于 WebSocket 的通信连接。
 * 它提供了 WebSocket 协议的客户端实现，使基站能够与网络服务器建立双向通信通道。
 * 该模块是基站网络通信的基础组件，确保可靠、高效的数据传输。
 * 
 * 文件功能
 * ========
 * 本头文件定义了 WebSocket 协议的核心接口：
 * - WebSocket 连接建立和管理
 * - 数据收发控制
 * - 连接状态管理
 * - 事件通知机制
 * - 缓冲区管理
 * 
 * 主要组件
 * ========
 * 1. 连接状态管理
 *    - 连接状态定义（WS_DEAD 到 WS_CLOSED）
 *    - 状态转换控制
 *    - 握手过程管理
 *    - 关闭过程管理
 * 
 * 2. 数据传输系统
 *    - 发送缓冲区管理
 *    - 接收缓冲区管理
 *    - 二进制数据传输
 *    - 文本数据传输
 * 
 * 3. 事件通知系统
 *    - 连接事件（已连接、已关闭等）
 *    - 数据事件（数据已发送、数据已接收等）
 *    - 错误事件处理
 * 
 * 4. 性能监控组件
 *    - RTT（往返时间）监控
 *    - 连接状态统计
 * 
 * 关键流程
 * ========
 * 1. 连接建立流程
 *    - 初始化 WebSocket
 *    - 建立 TLS 握手
 *    - 发送客户端请求
 *    - 接收服务器响应
 *    - 建立连接
 * 
 * 2. 数据传输流程
 *    - 获取发送缓冲区
 *    - 数据帧打包
 *    - 发送数据帧
 *    - 接收数据帧
 *    - 解析接收到的数据
 * 
 * 3. 连接关闭流程
 *    - 初始化关闭过程
 *    - 发送关闭帧
 *    - 等待确认
 *    - 资源释放
 * 
 * 注意事项
 * ========
 * 1. 连接管理
 *    - 连接状态变化监控
 *    - 异常连接处理
 *    - 资源释放确保
 * 
 * 2. 缓冲区使用
 *    - 缓冲区大小设置
 *    - 内存使用优化
 *    - 缓冲区溢出防护
 * 
 * 3. 性能优化
 *    - RTT 监控和优化
 *    - 大数据量处理策略
 *    - 连接稳定性保障
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

#ifndef _ws_h_
#define _ws_h_

#include "net.h"

typedef conn_t ws_t;

#define conn2ws(p) (p)
#define tmr2ws(p) memberof(ws_t, p, tmr)

// Websocket states
enum {
    WS_DEAD = 0,
    WS_TLS_HANDSHAKE,
    WS_CLIENT_REQ,
    WS_SERVER_RESP,
    WS_CONNECTED,
    WS_CLOSING_DRAINC,     // client initiated
    WS_CLOSING_DRAINS,     // server initiated
    WS_CLOSING_SENDCLOSE,
    WS_CLOSING_ECHOCLOSE,
    WS_CLOSING_SENTCLOSE,
    WS_CLOSED
};


// Events reported via evcb
enum {
    WSEV_DEAD = 0,
    WSEV_CLOSED,
    WSEV_DATASENT,
    WSEV_BINARYRCVD,
    WSEV_TEXTRCVD,
    WSEV_CONNECTED,
};


void   ws_shutdown   (ws_t*);                   // immediately close (=> ws_connect)
void   ws_close      (ws_t*, int reason);       // initialte close protocol => WSEV_CLOSED
dbuf_t ws_getRecvbuf (ws_t*);
dbuf_t ws_getSendbuf (ws_t*, int minsize);
void   ws_sendData   (ws_t*, dbuf_t* b, int binaryData); // send b obtained from get_sendbuf + b.pos indicates size
void   ws_sendText   (ws_t*, dbuf_t* b);
void   ws_sendBinary (ws_t*, dbuf_t* b);
void   ws_ini        (ws_t*, int rbufsize, int wbufsize);
void   ws_free       (ws_t*);                   // free all resources (=> ws_ini)
int    ws_connect    (ws_t*, char* host, char* port, char* uripath);

int    ws_getRtt     (ws_t*, u2_t* q_80_90_95); // round trip quantiles 80/90/95% in millis

#endif // _ws_h_
