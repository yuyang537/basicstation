/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的 TLS 安全通信层，负责提供加密通信功能。
 * 它基于 mbedTLS 库实现了安全连接管理、证书处理和加密数据传输。
 * 该模块确保基站与网络服务器之间通信的机密性、完整性和认证性。
 * 
 * 文件功能
 * ========
 * 本头文件定义了 TLS 安全通信的核心接口：
 * - TLS 配置创建和管理
 * - 证书处理和验证
 * - 安全会话建立
 * - 加密数据传输
 * - 错误处理和日志记录
 * 
 * 主要组件
 * ========
 * 1. TLS 配置管理
 *    - 配置创建和释放
 *    - 证书设置
 *    - 信任链管理
 *    - 加密选项设置
 * 
 * 2. 会话管理系统
 *    - 会话创建和初始化
 *    - 握手过程控制
 *    - 会话释放和清理
 *    - 服务器名称指示（SNI）
 * 
 * 3. 数据传输接口
 *    - 加密读取操作
 *    - 加密写入操作
 *    - 缓冲区管理
 *    - 错误检测
 * 
 * 4. 错误处理系统
 *    - 错误码定义
 *    - 错误日志记录
 *    - 调试级别控制
 * 
 * 关键流程
 * ========
 * 1. 安全连接建立流程
 *    - 创建 TLS 配置
 *    - 加载证书和密钥
 *    - 设置信任的 CA
 *    - 创建 TLS 会话
 *    - 执行 TLS 握手
 * 
 * 2. 安全数据传输流程
 *    - 准备数据缓冲区
 *    - 加密数据写入
 *    - 接收加密数据
 *    - 解密数据处理
 * 
 * 3. 连接关闭流程
 *    - 关闭 TLS 会话
 *    - 释放 TLS 上下文
 *    - 清理配置资源
 * 
 * 注意事项
 * ========
 * 1. 安全考虑
 *    - 证书验证重要性
 *    - 密钥保护措施
 *    - 密码套件选择
 * 
 * 2. 资源管理
 *    - 内存使用控制
 *    - 会话资源及时释放
 *    - 证书内存管理
 * 
 * 3. 性能影响
 *    - 加密操作的 CPU 消耗
 *    - 握手过程的延迟
 *    - 数据传输的开销
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

#ifndef _tls_h_
#define _tls_h_

#include "mbedtls/ssl.h"
#include "mbedtls/net.h"

typedef struct tlsconf tlsconf_t;
typedef struct mbedtls_ssl_context* tlsctx_p;

extern u1_t tls_dbgLevel;
void log_mbedError (u1_t mod_level, int ret, const char* fmt, ...);

tlsconf_t* tls_makeConf      ();
void       tls_freeConf      (tlsconf_t* conf);
int        tls_setMyCert     (tlsconf_t* conf, const char* cert, int certlen, const char* key, int keylen, const char* pwd);
int        tls_setTrustedCAs (tlsconf_t* conf, const char* file_or_data, int len);
tlsctx_p   tls_makeSession   (tlsconf_t* conf, const char* servername);
void       tls_freeSession   (tlsctx_p tlsctx);

int tls_read  (mbedtls_net_context* netctx, tlsctx_p tlsctx,       u1_t* p, size_t sz);
int tls_write (mbedtls_net_context* netctx, tlsctx_p tlsctx, const u1_t* p, size_t sz);

#endif // _tls_h_
