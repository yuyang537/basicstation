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

/**
 * ============================================================================
 * TC (Traffic Controller) - 传输控制器模块
 * ============================================================================
 * 
 * 功能概述：
 * TC模块是BasicStation的核心网络连接管理器，负责建立和维护与LoRaWAN网络服务器(LNS)
 * 的WebSocket连接。它实现了完整的连接生命周期管理，包括服务发现、连接建立、
 * 重连机制和错误恢复。
 * 
 * 核心功能：
 * 1. INFOS服务发现 - 查询路由器信息获取MUXS连接地址
 * 2. MUXS连接管理 - 建立和维护与数据复用器的WebSocket连接
 * 3. 连接状态机 - 管理从初始化到连接建立的完整状态转换
 * 4. 重连机制 - 实现指数退避重连策略，处理网络异常
 * 5. 协议集成 - 与S2E协议层紧密集成，提供数据传输接口
 * 
 * 连接流程：
 * 初始化 → INFOS查询 → 获取MUXS地址 → MUXS连接 → 数据传输
 * 
 * 设计特点：
 * - 异步状态机设计，支持非阻塞连接管理
 * - 智能重连策略，自动处理网络中断和服务器故障
 * - 与CUPS集成，支持动态配置更新
 * - 支持TLS加密连接，确保通信安全
 * - 完整的错误处理和日志记录
 */

#include "s2conf.h"          // S2配置系统：系统参数和配置管理
#include "sys.h"             // 系统抽象层：平台相关的系统功能接口
#include "uj.h"              // JSON处理器：轻量级JSON编解码库
#include "kwcrc.h"           // 关键字CRC：JSON字段名的快速哈希匹配
#include "s2e.h"             // S2E协议层：Station-to-Server通信协议
#include "tc.h"              // TC传输控制器：头文件定义和接口声明

// ============================================================================
// 全局变量和状态管理
// ============================================================================

tc_t* TC;                    // 全局TC实例：当前活跃的传输控制器对象
static s1_t tstateLast;      // 上次连接状态：用于状态查询和调试

// ============================================================================
// 核心状态管理函数
// ============================================================================

/**
 * 函数功能：TC操作完成处理，清理资源并触发回调
 * 参数说明：
 *   tc - TC对象指针
 *   tstate - 最终状态（成功或各种错误状态）
 * 返回值：无
 * 调用时机：TC操作完成时调用，包括成功连接和各种错误情况
 * 
 * 执行过程：
 * 1. 设置最终状态
 * 2. 释放WebSocket连接资源
 * 3. 通过定时器回调通知上层
 * 4. 更新系统连接状态
 */
static void tc_done (tc_t* tc, s1_t tstate) {
    tc->tstate = tstate;                                    // 设置最终状态
    ws_free(&tc->ws);                                       // 释放WebSocket连接
    rt_yieldTo(&tc->timeout, tc->ondone);                   // 触发完成回调
    sys_inState(SYSIS_TC_DISCONNECTED);                     // 更新系统状态为断开
}

/**
 * 函数功能：TC超时处理，当连接或操作超时时调用
 * 参数说明：tmr - 定时器对象指针
 * 返回值：无
 * 调用时机：设定的超时时间到达时由定时器系统调用
 * 
 * 处理逻辑：
 * - 记录超时错误日志
 * - 设置错误状态并结束当前操作
 */
static void tc_timeout (tmr_t* tmr) {
    tc_t* tc = timeout2tc(tmr);                             // 从定时器获取TC对象
    LOG(MOD_TCE|ERROR, "TC engine timed out");              // 记录超时错误
    tc_done(tc, TC_ERR_TIMEOUT);                            // 结束操作，设置超时错误状态
}

/**
 * 函数功能：MUXS连接事件处理回调，处理WebSocket连接的各种事件
 * 参数说明：
 *   _conn - WebSocket连接对象指针
 *   ev - 事件类型（连接、数据发送、数据接收、关闭等）
 * 返回值：无
 * 调用时机：MUXS WebSocket连接状态变化时由网络层调用
 * 
 * 事件处理：
 * - WSEV_CONNECTED: 连接建立，发送版本信息
 * - WSEV_DATASENT: 数据发送完成，刷新待发送队列
 * - WSEV_TEXTRCVD: 接收文本消息，转发给S2E协议层
 * - WSEV_BINARYRCVD: 接收二进制消息，转发给S2E协议层
 * - WSEV_CLOSED: 连接关闭，处理重连逻辑
 */
static void tc_muxs_connection (conn_t* _conn, int ev) {
    tc_t* tc = conn2tc(_conn);                              // 从连接对象获取TC对象
    
    if( ev == WSEV_CONNECTED ) {
        // 连接建立成功：清除超时定时器，发送版本信息
        rt_clrTimer(&tc->timeout);                          // 清除连接超时定时器
        tc->tstate = TC_MUXS_CONNECTED;                      // 更新状态为已连接
        LOG(MOD_TCE|VERBOSE, "Connected to MUXS.");          // 记录连接成功
        
        // 构造并发送版本信息消息
        dbuf_t b = ws_getSendbuf(&tc->ws, MIN_UPJSON_SIZE);  // 获取发送缓冲区
        assert(b.buf != NULL);                               // 新连接应该有足够缓冲区
        
        // 构建JSON版本信息
        uj_encOpen(&b, '{');                                 // 开始JSON对象
        uj_encKV(&b, "msgtype",  's', "version");            // 消息类型：版本信息
        uj_encKV(&b, "station",  's', CFG_version);          // 基站软件版本
        uj_encKV(&b, "firmware", 's', sys_version());        // 固件版本
        uj_encKV(&b, "package",  's', sys_version());        // 软件包版本
        // uj_encKV(&b, "os",       's', sys_osversion());   // 操作系统版本（可选）
        uj_encKV(&b, "model",    's', CFG_platform);         // 硬件平台型号
        uj_encKV(&b, "protocol", 'i', MUXS_PROTOCOL_VERSION);// 协议版本号
        uj_encKV(&b, "features", 's', rt_features());        // 支持的功能特性
        uj_encClose(&b, '}');                                // 结束JSON对象
        
        ws_sendText(&tc->ws, &b);                            // 发送版本信息
        
        // 成功连接后的配置管理
        if( tc->credset == SYS_CRED_REG )                    // 如果使用注册凭据
            sys_backupConfig(SYS_CRED_TC);                   // 备份TC配置
        sys_delayCUPS();                                     // 延迟CUPS检查
        return;
    }
    
    if( ev == WSEV_DATASENT ) {
        // 数据发送完成：刷新待发送的接收作业队列
        s2e_flushRxjobs(&tc->s2ctx);                         // 发送更多待处理的接收作业
        return;
    }
    
    if( ev == WSEV_TEXTRCVD ) {
        // 接收到文本消息：转发给S2E协议层处理
        dbuf_t b = ws_getRecvbuf(&tc->ws);                   // 获取接收缓冲区
        if( !s2e_onMsg(&tc->s2ctx, b.buf, b.bufsize) ) {    // 交给S2E处理JSON消息
            LOG(ERROR, "Closing connection to muxs - error in s2e_onMsg");
            tc->tstate = TC_ERR_FAILED;                      // 设置处理失败状态
            ws_close(&tc->ws, 1000);                         // 关闭WebSocket连接
        }
        return;
    }
    
    if( ev == WSEV_BINARYRCVD ) {
        // 接收到二进制消息：转发给S2E协议层处理
        dbuf_t b = ws_getRecvbuf(&tc->ws);                   // 获取接收缓冲区
        if( !s2e_onBinary(&tc->s2ctx, (u1_t*)b.buf, b.bufsize) ) { // 交给S2E处理二进制数据
            LOG(ERROR, "Closing connection to muxs - error in s2e_onBinary");
            tc->tstate = TC_ERR_FAILED;                      // 设置处理失败状态
            ws_close(&tc->ws, 1000);                         // 关闭WebSocket连接
        }
        return;
    }
    
    if( ev == WSEV_CLOSED ) {
        // 连接关闭：处理重连逻辑
        s1_t tstate = tc->tstate;                            // 获取当前状态
        LOG(MOD_TCE|VERBOSE, "Connection to MUXS closed in state %d", tstate);
        
        if( tstate >= 0 ) {                                  // 如果是正常状态下的关闭
            // 根据关闭时的状态决定错误类型
            tstate = tstate == TC_MUXS_CONNECTED ? TC_ERR_CLOSED : TC_ERR_FAILED;
        }
        tc_done(tc, tstate);                                 // 完成当前操作
        return;
    }
    
    LOG(MOD_TCE|INFO, "tc_muxs_connection - Unknown event: %d", ev); // 记录未知事件
}

/**
 * 函数功能：建立到MUXS的WebSocket连接
 * 参数说明：tc - TC对象指针
 * 返回值：无
 * 调用时机：从INFOS获取到MUXS地址后调用
 * 
 * 连接过程：
 * 1. 解析存储的MUXS URI信息
 * 2. 初始化WebSocket连接
 * 3. 如果需要TLS，设置TLS连接
 * 4. 建立WebSocket连接
 * 5. 设置连接超时和事件回调
 */
static void tc_connect_muxs (tc_t* tc) {
    // 解析存储的MUXS URI（经过编码的格式）
    char* u = tc->muxsuri;                                   // 获取MUXS URI缓冲区
    int   tlsmode  = u[0];                                   // TLS模式标志
    char* hostname = u+3;                                    // 主机名起始位置
    char* port     = &u[(u1_t)u[1]];                         // 端口号起始位置
    char* path     = &u[(u1_t)u[2]];                         // 路径起始位置

    // 初始化WebSocket连接
    ws_ini(&tc->ws, TC_RECV_BUFFER_SIZE, TC_SEND_BUFFER_SIZE); // 设置收发缓冲区大小
    
    // 如果需要TLS加密连接
    if( tlsmode == URI_TLS && !conn_setup_tls(&tc->ws, SYS_CRED_TC, SYS_CRED_REG, hostname) ) {
        goto errexit;                                        // TLS设置失败
    }
    
    LOG(MOD_TCE|VERBOSE, "Connecting to MUXS...");          // 记录连接尝试
    log_flushIO();                                           // 立即刷新日志
    
    // 建立WebSocket连接
    if( !ws_connect(&tc->ws, hostname, port, path) ) {
        LOG(MOD_TCE|ERROR, "Muxs connect failed - URI: ws%s://%s:%s%s", 
            tlsmode==URI_TLS?"s":"", hostname, port, path);   // 记录连接失败
        goto errexit;
    }
    
    // 设置连接超时和回调
    rt_setTimerCb(&tc->timeout, rt_micros_ahead(TC_TIMEOUT), tc_timeout); // 设置连接超时
    tc->ws.evcb = (evcb_t)tc_muxs_connection;                // 设置事件回调函数
    tc->tstate = TC_MUXS_REQ_PEND;                           // 更新状态为MUXS连接待处理
    return;

 errexit:
    tc_done(tc, TC_ERR_FAILED);                              // 连接失败，结束操作
    return;
}

/**
 * 函数功能：INFOS请求事件处理回调，处理路由器信息查询的WebSocket事件
 * 参数说明：
 *   _conn - WebSocket连接对象指针
 *   ev - 事件类型（连接、数据接收、关闭等）
 * 返回值：无
 * 调用时机：INFOS WebSocket连接状态变化时由网络层调用
 * 
 * 处理流程：
 * 1. WSEV_CONNECTED: 发送路由器EUI查询请求
 * 2. WSEV_TEXTRCVD: 解析INFOS响应，提取MUXS连接信息
 * 3. WSEV_CLOSED: 根据获取结果决定后续动作
 */
static void tc_info_request (conn_t* _conn, int ev) {
    tc_t* tc = conn2tc(_conn);                              // 从连接对象获取TC对象
    
    if( ev == WSEV_CONNECTED ) {
        // 连接建立：发送路由器EUI查询请求
        dbuf_t b = ws_getSendbuf(&tc->ws, MIN_UPJSON_SIZE);  // 获取发送缓冲区
        assert(b.buf != NULL);                               // 新连接应该有足够缓冲区
        
        // 构建JSON查询请求
        uj_encOpen(&b, '{');                                 // 开始JSON对象
        uj_encKV(&b, "router", '6', sys_eui());              // 发送基站EUI进行查询
        uj_encClose(&b, '}');                                // 结束JSON对象
        
        ws_sendText(&tc->ws, &b);                            // 发送查询请求
        return;
    }
    
    if( ev == WSEV_DATASENT ) {
        return;                                              // 数据发送完成事件，无需处理
    }
    
    if( ev == WSEV_BINARYRCVD ) {
        LOG(MOD_TCE|ERROR, "Binary data from 'infos' - ignored"); // INFOS不应发送二进制数据
        return;
    }
    
    if( ev == WSEV_TEXTRCVD ) {
        // 接收到INFOS响应：解析JSON获取MUXS连接信息
        s1_t err = TC_ERR_FAILED;                            // 默认错误状态
        dbuf_t b = ws_getRecvbuf(&tc->ws);                   // 获取接收缓冲区
        
        // 初始化JSON解码器
        ujdec_t D;
        uj_iniDecoder(&D, b.buf, b.bufsize);                 // 准备解析JSON响应
        
        if( uj_decode(&D) ) {
            LOG(MOD_TCE|ERROR, "Parsing of INFOS response failed"); // JSON解析失败
            err = TC_ERR_FAILED;
            goto failed;
        }
        
        // 解析JSON对象字段
        uj_nextValue(&D);                                    // 定位到JSON值
        uj_enterObject(&D);                                  // 进入JSON对象
        
        ujcrc_t field;
        char* router  = NULL;                                // 路由器ID
        char* muxsid  = NULL;                                // MUXS标识符
        char* muxsuri = NULL;                                // MUXS连接URI
        char* error   = NULL;                                // 错误信息
        
        // 遍历JSON对象字段
        while( (field = uj_nextField(&D)) ) {
            switch(field) {
            case J_router: { router  = uj_str(&D); break; }  // 路由器ID
            case J_muxs  : { muxsid  = uj_str(&D); break; }  // MUXS ID
            case J_error : { error   = uj_str(&D); break; }  // 错误信息
            case J_uri   : { 
                // 解析和验证MUXS URI
                muxsuri = uj_str(&D);
                
                // 验证URI协议（必须是ws或wss）
                if( !uri_isScheme(muxsuri,"ws") && !uri_isScheme(muxsuri,"wss") ) {
                    LOG(MOD_TCE|ERROR, "Muxs URI must be ws://.. or wss://..: %s", muxsuri);
                    goto failed;
                }
                
                // 验证URI长度
                if( D.str.len+1 > MAX_URI_LEN ) {
                    LOG(MOD_TCE|ERROR, "Muxs URI too long (max %d): %s", MAX_URI_LEN, muxsuri);
                    goto failed;
                }
                
                // 解析URI组件
                struct uri_info ui;
                dbuf_t uri = { .buf=D.str.beg, .bufsize=D.str.len, .pos=0 };
                if( !uri_parse(&uri, &ui, 0) || ui.portBeg==ui.portEnd || ui.pathBeg==ui.pathEnd ) {
                    LOG(MOD_TCE|ERROR, "Illegal muxs URI (no port/path etc.): %s", muxsuri);
                    goto failed;
                }
                
                // 编码并存储MUXS URI信息（压缩格式）
                memset(tc->muxsuri, 0, sizeof(tc->muxsuri));  // 清空URI缓冲区
                u1_t portoff = ui.hostEnd - ui.hostBeg + 4;   // 端口偏移量
                u1_t pathoff = portoff + ui.portEnd - ui.portBeg + 1; // 路径偏移量
                
                // 存储编码的URI信息
                tc->muxsuri[0] = muxsuri[2]=='s' ? URI_TLS : URI_TCP; // TLS标志
                tc->muxsuri[1] = portoff;                     // 端口偏移
                tc->muxsuri[2] = pathoff;                     // 路径偏移
                
                // 复制URI组件
                memcpy(&tc->muxsuri[3],       &muxsuri[ui.hostBeg], ui.hostEnd - ui.hostBeg); // 主机名
                memcpy(&tc->muxsuri[portoff], &muxsuri[ui.portBeg], ui.portEnd - ui.portBeg); // 端口
                memcpy(&tc->muxsuri[pathoff], &muxsuri[ui.pathBeg], ui.pathEnd - ui.pathBeg); // 路径
                break;
            }
            default: {
                LOG(MOD_TCE|WARNING, "Unknown field in infos response - ignored: %s", D.field.name);
                uj_skipValue(&D);                             // 跳过未知字段
                break;
            }
            }
        }
        
        uj_exitObject(&D);                                   // 退出JSON对象
        uj_assertEOF(&D);                                    // 确认JSON解析完成
        
        // 检查响应结果
        if( error || muxsuri == NULL ) {
            LOG(MOD_TCE|ERROR, "Infos error: %s %s", router, error); // 服务器返回错误
            err = TC_ERR_REJECTED;
            goto failed;
        }
        
        LOG(MOD_TCE|INFO, "Infos: %s %s %s", router, muxsid, muxsuri); // 记录成功获取的信息
        err = TC_INFOS_GOT_URI;                              // 设置成功状态
        
      failed:
        tc->tstate = err;                                    // 更新状态
        ws_close(&tc->ws, 1000);                             // 关闭INFOS连接
        return;
    }
    
    if( ev == WSEV_CLOSED ) {
        // INFOS连接关闭：根据状态决定后续动作
        s1_t tstate = tc->tstate;                            // 获取当前状态
        
        if( tstate >= 0 && tstate != TC_INFOS_GOT_URI )      // 如果是非预期关闭
            tstate = TC_ERR_CLOSED;
            
        if( tstate != TC_INFOS_GOT_URI ) {                   // 如果未成功获取URI
            tc_done(tc, tstate);                             // 结束操作
            return;
        }
        
        // 成功获取MUXS URI，开始连接MUXS
        ws_free(&tc->ws);                                    // 释放INFOS连接资源
        tc_connect_muxs(tc);                                 // 建立MUXS连接
        return;
    }
    
    LOG(MOD_TCE|INFO, "tc_info_request - Unknown event: %d", ev); // 记录未知事件
}

// ============================================================================
// S2E协议接口函数
// ============================================================================

/**
 * 函数功能：获取发送缓冲区，供S2E协议层使用
 * 参数说明：
 *   s2ctx - S2E上下文指针
 *   minsize - 所需的最小缓冲区大小
 * 返回值：发送缓冲区对象，如果未连接则返回空缓冲区
 * 调用时机：S2E协议层需要发送数据时调用
 */
static dbuf_t tc_getSendbuf (s2ctx_t* s2ctx, int minsize) {
    tc_t* tc = s2ctx2tc(s2ctx);                              // 从S2E上下文获取TC对象
    
    if( tc->tstate != TC_MUXS_CONNECTED ) {                  // 检查连接状态
        // 未连接到MUXS，返回空缓冲区（丢弃数据）
        dbuf_t b = { .buf=NULL, .bufsize=0, .pos=0 };
        return b;
    }
    
    return ws_getSendbuf(&tc->ws, minsize);                  // 返回WebSocket发送缓冲区
}

/**
 * 函数功能：发送文本数据，供S2E协议层使用
 * 参数说明：
 *   s2ctx - S2E上下文指针
 *   buf - 包含要发送数据的缓冲区
 * 返回值：无
 * 调用时机：S2E协议层需要发送JSON文本消息时调用
 */
static void tc_sendText (s2ctx_t* s2ctx, dbuf_t* buf) {
    tc_t* tc = s2ctx2tc(s2ctx);                              // 从S2E上下文获取TC对象
    ws_sendText(&tc->ws, buf);                               // 通过WebSocket发送文本数据
}

/**
 * 函数功能：发送二进制数据，供S2E协议层使用
 * 参数说明：
 *   s2ctx - S2E上下文指针
 *   buf - 包含要发送数据的缓冲区
 * 返回值：无
 * 调用时机：S2E协议层需要发送二进制消息时调用
 */
static void tc_sendBinary (s2ctx_t* s2ctx, dbuf_t* buf) {
    tc_t* tc = s2ctx2tc(s2ctx);                              // 从S2E上下文获取TC对象
    ws_sendBinary(&tc->ws, buf);                             // 通过WebSocket发送二进制数据
}

// ============================================================================
// TC对象生命周期管理
// ============================================================================

/**
 * 函数功能：默认完成回调函数，自动重连处理
 * 参数说明：timeout - 定时器对象指针
 * 返回值：无
 * 调用时机：TC操作完成后，如果没有指定自定义回调则调用此函数
 */
void tc_ondone_default (tmr_t* timeout) {
    tc_continue(timeout2tc(timeout));                        // 继续TC连接尝试
}

/**
 * 函数功能：初始化TC对象，分配资源并设置初始状态
 * 参数说明：ondone - 操作完成回调函数，NULL使用默认回调
 * 返回值：初始化完成的TC对象指针
 * 调用时机：需要创建新的TC实例时调用
 * 
 * 初始化内容：
 * 1. 分配TC对象内存
 * 2. 初始化WebSocket和定时器
 * 3. 设置S2E协议接口
 * 4. 配置初始状态和参数
 */
tc_t* tc_ini (tmrcb_t ondone) {
    assert(TC_RECV_BUFFER_SIZE > MAX_HOSTNAME_LEN + MAX_PORT_LEN + 2); // 验证缓冲区大小
    
    tc_t* tc = rt_malloc(tc_t);                              // 分配TC对象内存
    
    // 初始化WebSocket连接
    ws_ini(&tc->ws, TC_RECV_BUFFER_SIZE, TC_SEND_BUFFER_SIZE);
    
    // 初始化定时器
    rt_iniTimer(&tc->timeout, tc_timeout);                   // 设置超时回调
    
    // 设置初始状态和参数
    tc->tstate = TC_INI;                                     // 设置初始状态
    tc->credset = SYS_CRED_REG;                              // 使用注册凭据集
    tc->ondone = ondone==NULL ? tc_ondone_default : ondone;   // 设置完成回调
    tc->muxsuri[0] = URI_BAD;                                // 标记MUXS URI未设置
    
    // 初始化S2E协议上下文
    s2e_ini(&tc->s2ctx);                                    // 初始化S2E上下文
    tc->s2ctx.getSendbuf = tc_getSendbuf;                    // 设置获取发送缓冲区接口
    tc->s2ctx.sendText   = tc_sendText;                      // 设置发送文本接口
    tc->s2ctx.sendBinary = tc_sendBinary;                    // 设置发送二进制接口
    
    return tc;                                               // 返回初始化完成的TC对象
}

/**
 * 函数功能：释放TC对象，清理所有资源
 * 参数说明：tc - 要释放的TC对象指针，可以为NULL
 * 返回值：无
 * 调用时机：TC对象不再需要时调用，包括系统关闭和重启时
 * 
 * 清理内容：
 * 1. 释放WebSocket连接资源
 * 2. 清除定时器
 * 3. 保存最后状态
 * 4. 释放S2E上下文
 * 5. 释放TC对象内存
 */
void tc_free (tc_t* tc) {
    if( tc == NULL )                                         // 空指针检查
        return;
        
    ws_free(&tc->ws);                                        // 释放WebSocket连接资源
    rt_clrTimer(&tc->timeout);                               // 清除超时定时器
    tstateLast = tc->tstate;                                 // 保存最后状态供查询
    tc->tstate = TC_ERR_DEAD;                                // 标记对象已死亡
    s2e_free(&tc->s2ctx);                                    // 释放S2E协议上下文
    rt_free(tc);                                             // 释放TC对象内存
}

/**
 * 函数功能：启动TC连接过程，开始INFOS查询
 * 参数说明：tc - 已初始化的TC对象指针
 * 返回值：无
 * 调用时机：TC初始化完成后调用，开始连接流程
 * 
 * 启动流程：
 * 1. 验证TC配置URI
 * 2. 解析主机名和端口
 * 3. 如果需要，设置TLS连接
 * 4. 建立到INFOS的WebSocket连接
 * 5. 设置超时和事件回调
 */
void tc_start (tc_t* tc) {
    assert(tc->tstate == TC_INI);                            // 确保TC处于初始状态
    int tstate_err = TC_ERR_NOURI;                           // 默认错误：无URI配置

    // 获取TC配置URI
    str_t tcuri = sys_uri(SYS_CRED_TC, tc->credset);         // 从系统获取TC URI配置
    if( tcuri == NULL ) {
        LOG(MOD_TCE|ERROR, "No TC URI configured");          // TC URI未配置
        goto errexit;
    }
    
    // 解析URI为主机名和端口
    char hostname[MAX_HOSTNAME_LEN];                         // 主机名缓冲区
    char port[MAX_PORT_LEN];                                 // 端口号缓冲区
    int ok;
    
    // 检查并解析URI格式
    if( (ok = uri_checkHostPortUri(tcuri, "ws", hostname, MAX_HOSTNAME_LEN, port, MAX_PORT_LEN)) == URI_BAD ) {
        LOG(MOD_TCE|ERROR,"Bad TC URI: %s", tcuri);          // URI格式错误
        goto errexit;
    }
    
    // 如果需要TLS连接，设置TLS
    if( ok == URI_TLS && !conn_setup_tls(&tc->ws, SYS_CRED_TC, tc->credset, hostname) ) {
        goto errexit;                                        // TLS设置失败
    }
    
    tstate_err = TC_ERR_FAILED;                              // 更新错误类型为连接失败
    
    LOG(MOD_TCE|INFO, "Connecting to INFOS: %s", tcuri);    // 记录连接尝试
    log_flushIO();                                           // 立即刷新日志
    
    // 建立到INFOS的WebSocket连接
    if( !ws_connect(&tc->ws, hostname, port, "/router-info") ) {
        LOG(MOD_TCE|ERROR, "TC connect failed - URI: %s", tcuri); // 连接失败
        goto errexit;
    }
    
    // 设置连接超时和回调
    rt_setTimerCb(&tc->timeout, rt_seconds_ahead(TC_TIMEOUT), tc_timeout); // 设置超时
    tc->ws.evcb = (evcb_t)tc_info_request;                   // 设置INFOS事件处理回调
    tc->tstate = TC_INFOS_REQ_PEND;                          // 更新状态为INFOS请求待处理
    return;
    
 errexit:
    tc_done(tc, tstate_err);                                 // 启动失败，结束操作
    return;
}

/**
 * 函数功能：继续TC连接尝试，实现重连和退避逻辑
 * 参数说明：tc - TC对象指针
 * 返回值：无
 * 调用时机：TC操作完成后由回调函数调用，实现自动重连
 * 
 * 重连策略：
 * 1. 检查是否需要触发CUPS更新
 * 2. 根据当前状态决定重连类型
 * 3. 实现指数退避算法
 * 4. 区分INFOS和MUXS重连逻辑
 */
void tc_continue (tc_t* tc) {
    s1_t tstate = tc->tstate;                                // 获取当前状态

    // 检查是否需要触发CUPS配置更新
    if( (tstate == TC_ERR_REJECTED || tstate == TC_ERR_NOURI || tc->retries >= 10) && !sys_noCUPS ) {
        LOG(MOD_TCE|INFO, "Router rejected or retry limit reached. Invoking CUPS.");
        sys_stopTC();                                        // 停止TC引擎
        sys_triggerCUPS(-1);                                 // 触发CUPS配置更新
        return;
    }

    // INFOS重连退避处理
    if( tstate == TC_INFOS_BACKOFF ) {
        int retries_old = tc->retries;                       // 保存重试次数
        tmrcb_t ondone = tc->ondone;                         // 保存完成回调
        
        // 重新创建TC对象（清理状态）
        assert(TC == tc);                                    // 确保是全局TC实例
        tc_free(tc);                                         // 释放旧TC对象
        TC = tc = tc_ini(ondone);                            // 创建新TC对象
        tc_start(tc);                                        // 重新启动连接
        tc->retries = retries_old + 1;                       // 更新重试次数
        return;
    }
    
    // MUXS重连退避处理
    if( tstate == TC_MUXS_BACKOFF ) {
        tc->retries += 1;                                    // 增加重试次数
        tc_connect_muxs(tc);                                 // 直接重连MUXS
        return;
    }

    // 处理MUXS连接的快速重连和退避
    if( tc->muxsuri[0] != URI_BAD ) {
        // 已有MUXS URI，尝试快速重连
        if( tc->retries <= 4 && tstate == TC_ERR_CLOSED ) {
            // 实现指数退避重连（1, 2, 4, 8, 16秒）
            int backoff = 1 << tc->retries;                  // 计算退避时间
            tc->tstate = TC_MUXS_BACKOFF;                    // 设置MUXS退避状态
            rt_setTimerCb(&tc->timeout, rt_seconds_ahead(backoff), tc->ondone); // 设置退避定时器
            LOG(MOD_TCE|INFO, "MUXS reconnect backoff %ds (retry %d)", backoff, tc->retries);
            return;
        }
        
        // 超过快速重连次数，清除MUXS URI重新从INFOS开始
        tc->muxsuri[0] = URI_BAD;                            // 清除MUXS URI
        tc->retries = 1;                                     // 重置重试次数
    }

    // INFOS重连退避逻辑
    int backoff = min(tc->retries, 6);                       // 限制最大退避倍数
    tc->tstate = TC_INFOS_BACKOFF;                           // 设置INFOS退避状态
    rt_setTimerCb(&tc->timeout, rt_seconds_ahead(backoff * 10), tc->ondone); // 设置退避定时器
    LOG(MOD_TCE|INFO, "INFOS reconnect backoff %ds (retry %d)", backoff*10, tc->retries);
}

// ============================================================================
// 系统级TC管理接口
// ============================================================================

/**
 * 函数功能：停止TC引擎，清理全局TC实例
 * 参数说明：无
 * 返回值：无
 * 调用时机：系统关闭或需要重启TC时调用
 */
void sys_stopTC () {
    if( TC != NULL ) {                                       // 检查全局TC实例
        LOG(MOD_TCE|INFO, "Terminating TC engine");          // 记录停止日志
        tc_free(TC);                                         // 释放TC对象
        TC = NULL;                                           // 清空全局指针
        sys_inState(SYSIS_TC_DISCONNECTED);                  // 更新系统状态
    }
}

/**
 * 函数功能：启动TC引擎，创建并启动全局TC实例
 * 参数说明：无
 * 返回值：无
 * 调用时机：系统启动或需要重启TC时调用
 */
void sys_startTC () {
    if( TC != NULL || sys_noTC )                             // 检查是否已运行或被禁用
        return;                                              // 已运行或被禁用，直接返回
        
    LOG(MOD_TCE|INFO, "Starting TC engine");                // 记录启动日志
    TC = tc_ini(NULL);                                       // 创建TC实例，使用默认回调
    tc_start(TC);                                            // 启动连接流程
    sys_inState(SYSIS_TC_DISCONNECTED);                      // 设置初始系统状态
}

/**
 * 函数功能：初始化TC系统，预留的初始化接口
 * 参数说明：无
 * 返回值：无
 * 调用时机：系统初始化时调用
 */
void sys_iniTC () {
    // 当前为空实现，预留给未来的初始化需求
}

/**
 * 函数功能：获取TC当前状态
 * 参数说明：无
 * 返回值：当前TC状态码，如果TC未运行则返回最后记录的状态
 * 调用时机：需要查询TC连接状态时调用
 */
s1_t sys_statusTC () {
    return TC ? TC->tstate : tstateLast;                     // 返回当前或最后状态
}
