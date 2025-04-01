/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的 Web 服务器系统，提供了基于 HTTP 的管理接口。
 * 它实现了轻量级的 Web 服务器功能，支持基站的远程配置、监控和管理。
 * 该模块为基站提供了用户友好的界面，方便操作人员进行设备管理和状态监控。
 * 
 * 文件功能
 * ========
 * 本头文件定义了 Web 服务器的核心接口：
 * - Web 服务器状态管理
 * - HTTP 请求处理
 * - 路径处理器注册
 * - 认证和安全控制
 * - 静态文件服务
 * 
 * 主要组件
 * ========
 * 1. Web 服务器核心
 *    - 服务器状态管理
 *    - 连接处理
 *    - 超时控制
 *    - 错误处理
 * 
 * 2. 请求处理系统
 *    - 路径处理器（Handler）框架
 *    - URL路由机制
 *    - 请求解析和响应生成
 *    - MIME类型管理
 * 
 * 3. 身份验证系统
 *    - 认证初始化
 *    - 访问控制
 *    - 安全处理器
 *    - 会话管理
 * 
 * 4. 文件服务系统
 *    - 静态文件访问
 *    - 资源缓存
 *    - 文件类型识别
 * 
 * 关键流程
 * ========
 * 1. 服务器启动流程
 *    - 初始化Web服务器
 *    - 注册处理器
 *    - 初始化认证系统
 *    - 开始监听连接
 * 
 * 2. 请求处理流程
 *    - 接收HTTP请求
 *    - 解析请求头和路径
 *    - 定位合适的处理器
 *    - 执行请求处理
 *    - 返回HTTP响应
 * 
 * 3. 认证流程
 *    - 检查认证信息
 *    - 验证凭证
 *    - 授权访问
 *    - 会话管理
 * 
 * 注意事项
 * ========
 * 1. 安全考虑
 *    - 认证机制的安全性
 *    - 跨站点请求伪造防护
 *    - 敏感信息保护
 * 
 * 2. 资源限制
 *    - 并发连接数限制
 *    - 请求超时设置
 *    - 内存使用控制
 * 
 * 3. 性能优化
 *    - 静态文件缓存
 *    - 请求处理效率
 *    - 连接复用
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

#ifndef _web_h_
#define _web_h_

#include "httpd.h"

#define WEB_PORT "8080"

enum {
    WEB_INI            = 0,

    WEB_ERR_FAILED        = -1,
    WEB_ERR_TIMEOUT       = -3,
    WEB_ERR_REJECTED      = -4,
    WEB_ERR_CLOSED        = -5,
    WEB_ERR_DEAD          = -6,
};


typedef struct web {
    httpd_t   hd;          // HTTPD connection state
    tmr_t     timeout;
    s1_t      wstate;      // state of web
} web_t;

typedef struct {
    ujcrc_t pathcrc;
    int (*f)(httpd_pstate_t* pstate, httpd_t* hd, dbuf_t* buf);
} web_handler_t;

extern const web_handler_t SYS_HANDLERS[];
extern web_handler_t AUTH_HANDLERS[];

void web_authini();

dbuf_t sys_webFile (str_t filename);

#define timeout2web(p) memberof(web_t, p, timeout)
#define conn2web(p)    memberof(web_t, p, hd.c)

#endif // _web_h_
