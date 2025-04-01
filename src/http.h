/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的 HTTP 协议实现层，提供了 HTTP 客户端和服务器功能的核心组件。
 * 它实现了简化版的 HTTP 协议，支持基站与外部系统进行基于 HTTP 的通信。
 * 该模块是 Web 服务器和 CUPS（配置与更新服务）客户端功能的基础。
 * 
 * 文件功能
 * ========
 * 本头文件定义了 HTTP 协议的核心接口：
 * - HTTP 连接管理
 * - 请求和响应处理
 * - 头部解析和生成
 * - 内容处理
 * - 状态码管理
 * 
 * 主要组件
 * ========
 * 1. HTTP 连接管理
 *    - 连接建立和关闭
 *    - 状态控制
 *    - 缓冲区管理
 *    - 事件通知
 * 
 * 2. 请求/响应处理
 *    - 请求生成和发送
 *    - 响应接收和解析
 *    - 状态码处理
 *    - 内容长度管理
 * 
 * 3. 头部处理系统
 *    - 头部字段查找
 *    - 值解析和提取
 *    - 大小写不敏感比较
 *    - 标准头部处理
 * 
 * 4. 内容处理系统
 *    - MIME 类型映射
 *    - 内容缓冲区管理
 *    - 分块传输处理
 *    - 内容长度控制
 * 
 * 关键流程
 * ========
 * 1. 客户端请求流程
 *    - 初始化 HTTP 连接
 *    - 连接到服务器
 *    - 构建和发送请求
 *    - 接收和处理响应
 *    - 关闭连接
 * 
 * 2. 服务器响应流程
 *    - 监听连接请求
 *    - 接收客户端请求
 *    - 解析请求头和内容
 *    - 生成和发送响应
 *    - 维护连接状态
 * 
 * 3. 头部解析流程
 *    - 按行解析头部
 *    - 提取字段和值
 *    - 处理特殊头部
 *    - 验证头部格式
 * 
 * 注意事项
 * ========
 * 1. 协议限制
 *    - 简化的 HTTP 实现
 *    - 有限的 HTTP 方法支持
 *    - 基本的头部处理
 * 
 * 2. 资源管理
 *    - 缓冲区大小限制
 *    - 连接超时控制
 *    - 内存使用效率
 * 
 * 3. 性能考虑
 *    - 解析效率
 *    - 连接复用
 *    - 缓冲区管理策略
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

#ifndef _http_h_
#define _http_h_

#include "net.h"

extern str_t const SUFFIX2CT[];

#define conn2http(p) memberof(http_t, p, c)
#define tmr2http(p) memberof(http_t, p, c.tmr)

typedef struct http {
    conn_t c;
    struct {
        int clen;  // content length
        int coff;  // content offset
    } extra;

    // HTTPD mode only
    struct {
        netctx_t netctx;
        aio_t*   aio;
    } listen;
} http_t;

enum {
    HTTP_DEAD = 0,
    HTTP_CONNECTED,    // just connected or response received
    HTTP_CLOSED,       // not connected to server
    HTTP_SENDING_REQ,
    HTTP_READING_HDR,
    HTTP_READING_BODY,
};

enum {
    HTTPEV_DEAD = 0,
    HTTPEV_CLOSED,        // HTTP connection closed
    HTTPEV_CONNECTED,     // connected to server
    HTTPEV_RESPONSE,      // received a response (hdr + body - maybe partially) - start processing it
    HTTPEV_RESPONSE_MORE, // received more data from body (if large)
};

void   http_ini       (http_t*, int bufsize);
void   http_free      (http_t*);
int    http_connect   (http_t*, char* host, char* port);
void   http_close     (http_t*);
void   http_request   (http_t*, dbuf_t* req);
int    http_getMore   (http_t*);  // read more body data
dbuf_t http_getReqbuf (http_t*);
dbuf_t http_getHdr    (http_t*);
dbuf_t http_getBody   (http_t*);
int    http_getStatus (http_t*);


// Some functions facilitating simple HTTP parsing:
char*  http_skipWsp    (char* p);
int    http_readDec    (char* p);
int    http_statusCode (char* p);
int    http_icaseCmp   (const char* p, const char* what);
char*  http_findHeader (char* p, const char* field);
int    http_findContentLength (char* p);
int    http_setContentLength  (char* p, int clen);
dbuf_t http_statusText (dbuf_t* hdr);
int    http_unquote    (char** p);


#endif // _http_h_
