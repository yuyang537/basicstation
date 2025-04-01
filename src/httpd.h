/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的 HTTP 服务器实现，基于 HTTP 模块提供完整的 Web 服务器功能。
 * 它负责接收和处理来自客户端的 HTTP 请求，并生成适当的响应。
 * 该模块是基站 Web 管理界面的核心组件，提供了 RESTful API 和静态文件服务能力。
 * 
 * 文件功能
 * ========
 * 本头文件定义了 HTTP 服务器的核心接口：
 * - HTTP 服务器初始化和控制
 * - 请求接收和解析
 * - 响应生成和发送
 * - 路径解析和处理
 * - 服务器状态管理
 * 
 * 主要组件
 * ========
 * 1. 服务器核心
 *    - 服务器初始化和配置
 *    - 监听和连接管理
 *    - 状态控制
 *    - 事件处理
 * 
 * 2. 请求处理系统
 *    - 请求行解析
 *    - HTTP 方法识别
 *    - 路径解析
 *    - 查询参数处理
 * 
 * 3. 路径处理系统
 *    - 路径解析状态机
 *    - 路径元素提取
 *    - 路径 CRC 计算
 *    - 后缀识别
 * 
 * 4. 响应生成系统
 *    - 响应缓冲区管理
 *    - 状态码设置
 *    - 头部生成
 *    - 内容发送
 * 
 * 关键流程
 * ========
 * 1. 服务器启动流程
 *    - 初始化服务器
 *    - 绑定端口
 *    - 开始监听连接
 *    - 等待客户端请求
 * 
 * 2. 请求处理流程
 *    - 接收客户端连接
 *    - 读取请求头
 *    - 解析请求行
 *    - 处理请求头字段
 *    - 读取请求体（如有）
 * 
 * 3. 响应生成流程
 *    - 准备响应缓冲区
 *    - 设置状态码和状态文本
 *    - 添加响应头
 *    - 填充响应体
 *    - 发送响应
 * 
 * 注意事项
 * ========
 * 1. 协议兼容性
 *    - HTTP 协议版本支持
 *    - 方法支持限制
 *    - 头部字段处理限制
 * 
 * 2. 资源管理
 *    - 连接数限制
 *    - 缓冲区大小控制
 *    - 超时管理
 * 
 * 3. 安全考虑
 *    - 请求验证
 *    - 路径遍历防护
 *    - 资源访问控制
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

#ifndef _httpd_h_
#define _httpd_h_

#include "net.h"
#include "http.h"
#include "uj.h"

#define conn2httpd(p) memberof(httpd_t, p, c)
#define tmr2httpd(p) memberof(httpd_t, p, c.tmr)

typedef http_t httpd_t;

enum {
    // keep in sync with http - we can share code
    HTTPD_DEAD         = HTTP_DEAD,
    HTTPD_CONNECTED    = HTTP_CONNECTED,   // just connected / or request is in - answer it
    HTTPD_CLOSED       = HTTP_CLOSED,      // no client connected
    HTTPD_SENDING_RESP = HTTP_SENDING_REQ,
    HTTPD_READING_HDR  = HTTP_READING_HDR,
    HTTPD_READING_BODY = HTTP_READING_BODY,
};

enum {
    // keep in sync with http - we can share code
    HTTPDEV_DEAD      = HTTPEV_DEAD,
    HTTPDEV_CLOSED    = HTTPEV_CLOSED,     // HTTP connection is closed
    HTTPDEV_REQUEST   = HTTPEV_RESPONSE,   // received a request - start preparing a response
};

void   httpd_ini        (httpd_t*, int bufsize);
void   httpd_free       (httpd_t*);
int    httpd_listen     (httpd_t*, const char* port);
void   httpd_close      (httpd_t*);
void   httpd_stop       (httpd_t*);
dbuf_t httpd_getRespbuf (httpd_t*);
dbuf_t httpd_getHdr     (httpd_t*);
dbuf_t httpd_getBody    (httpd_t*);
void   httpd_response   (httpd_t*, dbuf_t* resp);

enum {
    HTTPD_PATH_DONE,
    HTTPD_PATH_ROOT,
    HTTPD_PATH_ELEM,
    HTTPD_PATH_LAST,
    HTTPD_PARAM_ELEM,
    HTTPD_QUERY_ELEM,
};

typedef struct httpd_pstate {
    char*  meth;
    char*  path;
    char*  suffix;
    char*  query;
    char*  fragment;
    str_t  contentType;
    str_t  contentEnc;
    int    httpVersion;
    int    method;
    ujcrc_t  pathcrc;
} httpd_pstate_t;

// methods returned by httpd_iniParseReqLine
enum { HTTP_OTHER_METHOD, HTTP_GET, HTTP_POST };
// versions are returned by httpd_iniParseReqLine
enum { HTTP_x_x = 0 , HTTP_1_0 = 1000, HTTT_1_1 = 1001 };

int httpd_parseReqLine (httpd_pstate_t* pstate, dbuf_t* hdr);

#endif // _httpd_h_
