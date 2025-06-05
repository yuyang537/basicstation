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

// 包含必要的头文件
#include "s2conf.h"    // S2配置系统
#include "sys.h"       // 系统抽象层
#include "uj.h"        // JSON编解码
#include "kwcrc.h"     // 关键字CRC
#include "s2e.h"       // Station to Engine协议
#include "tc.h"        // TC模块头文件


// 全局变量：TC引擎实例指针
tc_t* TC;
// 上次TC状态，用于状态跟踪
static s1_t tstateLast;


/**
 * TC操作完成处理函数
 * 当TC操作（连接、断开等）完成时调用此函数进行清理
 * 
 * @param tc TC实例指针
 * @param tstate 最终状态码
 */
static void tc_done (tc_t* tc, s1_t tstate) {
    tc->tstate = tstate;          // 设置最终状态
    ws_free(&tc->ws);             // 释放WebSocket资源
    rt_yieldTo(&tc->timeout, tc->ondone);  // 触发完成回调
    sys_inState(SYSIS_TC_DISCONNECTED);    // 通知系统TC已断开
}


/**
 * TC超时处理函数
 * 当TC操作超时时调用此函数
 * 
 * @param tmr 超时定时器指针
 */
static void tc_timeout (tmr_t* tmr) {
    tc_t* tc = timeout2tc(tmr);   // 从定时器获取TC实例
    LOG(MOD_TCE|ERROR, "TC engine timed out");  // 记录超时错误
    tc_done(tc, TC_ERR_TIMEOUT);  // 完成TC操作，状态为超时
}


/**
 * MUXS连接事件处理函数
 * 处理与LNS MUXS服务器的WebSocket连接事件
 * 
 * @param _conn 连接对象指针
 * @param ev 事件类型
 */
static void tc_muxs_connection (conn_t* _conn, int ev) {
    tc_t* tc = conn2tc(_conn);     // 从连接对象获取TC实例
    
    if( ev == WSEV_CONNECTED ) {
        // WebSocket连接建立成功
        rt_clrTimer(&tc->timeout);  // 清除超时定时器
        tc->tstate = TC_MUXS_CONNECTED;  // 设置状态为已连接
        LOG(MOD_TCE|VERBOSE, "Connected to MUXS.");
        
        // 准备发送缓冲区用于版本协商
        dbuf_t b = ws_getSendbuf(&tc->ws, MIN_UPJSON_SIZE);
        assert(b.buf != NULL);   // 新连接应该有可用的发送缓冲区
        
        // 构建版本信息JSON消息
        uj_encOpen(&b, '{');
        uj_encKV(&b, "msgtype",  's', "version");     // 消息类型：版本
        uj_encKV(&b, "station",  's', CFG_version);   // Station版本
        uj_encKV(&b, "firmware", 's', sys_version()); // 固件版本
        uj_encKV(&b, "package",  's', sys_version()); // 包版本
        // uj_encKV(&b, "os",       's', sys_osversion()); // 操作系统版本（可选）
        uj_encKV(&b, "model",    's', CFG_platform);  // 平台型号
        uj_encKV(&b, "protocol", 'i', MUXS_PROTOCOL_VERSION);  // 协议版本
        uj_encKV(&b, "features", 's', rt_features()); // 支持的特性
        uj_encClose(&b, '}');
        
        // 发送版本信息到MUXS
        ws_sendText(&tc->ws, &b);
        
        // 如果使用的是REG凭证集，备份TC配置
        if( tc->credset == SYS_CRED_REG )
            sys_backupConfig(SYS_CRED_TC);
        
        // 延迟CUPS检查，因为TC已连接
        sys_delayCUPS();
        return;
    }
    
    if( ev == WSEV_DATASENT ) {
        // 数据发送完成，刷新待发送的接收作业
        s2e_flushRxjobs(&tc->s2ctx);   // 发送更多待处理的rxjobs
        return;
    }
    
    if( ev == WSEV_TEXTRCVD ) {
        // 接收到文本数据（JSON消息）
        dbuf_t b = ws_getRecvbuf(&tc->ws);
        if( !s2e_onMsg(&tc->s2ctx, b.buf, b.bufsize) ) {
            LOG(ERROR, "Closing connection to muxs - error in s2e_onMsg");
            tc->tstate = TC_ERR_FAILED;
            ws_close(&tc->ws, 1000);   // 关闭WebSocket连接
        }
        return;
    }
    
    if( ev == WSEV_BINARYRCVD ) {
        // 接收到二进制数据
        dbuf_t b = ws_getRecvbuf(&tc->ws);
        if( !s2e_onBinary(&tc->s2ctx, (u1_t*)b.buf, b.bufsize) ) {
            LOG(ERROR, "Closing connection to muxs - error in s2e_onBinary");
            tc->tstate = TC_ERR_FAILED;
            ws_close(&tc->ws, 1000);   // 关闭WebSocket连接
        }
        return;
    }
    
    if( ev == WSEV_CLOSED ) {
        // WebSocket连接关闭
        s1_t tstate = tc->tstate;
        LOG(MOD_TCE|VERBOSE, "Connection to MUXS closed in state %d", tstate);
        if( tstate >= 0 ) {
            // 如果是正常连接状态下关闭，快速重连MUXS；否则通过infos重新获取
            tstate = tstate == TC_MUXS_CONNECTED ? TC_ERR_CLOSED : TC_ERR_FAILED;
        }
        tc_done(tc, tstate);
        return;
    }
    LOG(MOD_TCE|INFO, "tc_muxs_connection - Unknown event: %d", ev);
}


/**
 * 连接到MUXS服务器
 * 使用从INFOS获得的URI信息连接到MUXS
 * 
 * @param tc TC实例指针
 */
static void tc_connect_muxs (tc_t* tc) {
    // 解析MUXS URI信息
    char* u = tc->muxsuri;         // URI字符串
    int   tlsmode  = u[0];         // TLS模式标志
    char* hostname = u+3;          // 主机名
    char* port     = &u[(u1_t)u[1]];  // 端口号
    char* path     = &u[(u1_t)u[2]];  // 路径

    // 初始化WebSocket连接
    ws_ini(&tc->ws, TC_RECV_BUFFER_SIZE, TC_SEND_BUFFER_SIZE);
    
    // 如果是TLS模式，设置TLS配置
    if( tlsmode == URI_TLS && !conn_setup_tls(&tc->ws, SYS_CRED_TC, SYS_CRED_REG, hostname) ) {
        goto errexit;
    }
    
    LOG(MOD_TCE|VERBOSE, "Connecting to MUXS...");
    log_flushIO();
    
    // 建立WebSocket连接
    if( !ws_connect(&tc->ws, hostname, port, path) ) {
        LOG(MOD_TCE|ERROR, "Muxs connect failed - URI: ws%s://%s:%s%s", tlsmode==URI_TLS?"s":"", hostname, port, path);
        goto errexit;
    }
    
    // 设置连接超时
    rt_setTimerCb(&tc->timeout, rt_micros_ahead(TC_TIMEOUT), tc_timeout);
    // 设置事件回调函数
    tc->ws.evcb = (evcb_t)tc_muxs_connection;
    // 设置状态为MUXS请求等待中
    tc->tstate = TC_MUXS_REQ_PEND;
    return;

 errexit:
    tc_done(tc, TC_ERR_FAILED);
    return;
}


static void tc_info_request (conn_t* _conn, int ev) {
    tc_t* tc = conn2tc(_conn);
    if( ev == WSEV_CONNECTED ) {
        dbuf_t b = ws_getSendbuf(&tc->ws, MIN_UPJSON_SIZE);
        assert(b.buf != NULL);   // this should not fail on a fresh connection
        uj_encOpen(&b, '{');
        uj_encKV(&b, "router", '6', sys_eui());
        uj_encClose(&b, '}');
        ws_sendText(&tc->ws, &b);
        return;
    }
    if( ev == WSEV_DATASENT ) {
        return; // we're not interested in this event
    }
    if( ev == WSEV_BINARYRCVD ) {
        LOG(MOD_TCE|ERROR, "Binary data from 'infos' - ignored");
        return;
    }
    if( ev == WSEV_TEXTRCVD ) {
        s1_t err = TC_ERR_FAILED;
        dbuf_t b = ws_getRecvbuf(&tc->ws);
        ujdec_t D;
        uj_iniDecoder(&D, b.buf, b.bufsize);
        if( uj_decode(&D) ) {
            LOG(MOD_TCE|ERROR, "Parsing of INFOS response failed");
            err = TC_ERR_FAILED;
            goto failed;
        }
        uj_nextValue(&D);
        uj_enterObject(&D);
        ujcrc_t field;
        char* router  = NULL;
        char* muxsid  = NULL;
        char* muxsuri = NULL;
        char* error   = NULL;
        while( (field = uj_nextField(&D)) ) {
            switch(field) {
            case J_router: { router  = uj_str(&D); break; }
            case J_muxs  : { muxsid  = uj_str(&D); break; }
            case J_error : { error   = uj_str(&D); break; }
            case J_uri   : { muxsuri = uj_str(&D);
                if( !uri_isScheme(muxsuri,"ws") && !uri_isScheme(muxsuri,"wss") ) {
                    LOG(MOD_TCE|ERROR, "Muxs URI must be ws://.. or wss://..: %s", muxsuri);
                    goto failed;
                }
                if( D.str.len+1 > MAX_URI_LEN ) {
                    LOG(MOD_TCE|ERROR, "Muxs URI too long (max %d): %s", MAX_URI_LEN, muxsuri);
                    goto failed;
                }
                struct uri_info ui;
                dbuf_t uri = { .buf=D.str.beg, .bufsize=D.str.len, .pos=0 };
                if( !uri_parse(&uri, &ui, 0) || ui.portBeg==ui.portEnd || ui.pathBeg==ui.pathEnd ) {
                    LOG(MOD_TCE|ERROR, "Illegal muxs URI (no port/path etc.): %s", muxsuri);
                    goto failed;
                }
                memset(tc->muxsuri, 0, sizeof(tc->muxsuri));
                u1_t portoff = ui.hostEnd - ui.hostBeg + 4;
                u1_t pathoff = portoff + ui.portEnd - ui.portBeg + 1;
                tc->muxsuri[0] = muxsuri[2]=='s' ? URI_TLS : URI_TCP;
                tc->muxsuri[1] = portoff;
                tc->muxsuri[2] = pathoff;
                memcpy(&tc->muxsuri[3],       &muxsuri[ui.hostBeg], ui.hostEnd - ui.hostBeg);
                memcpy(&tc->muxsuri[portoff], &muxsuri[ui.portBeg], ui.portEnd - ui.portBeg);
                memcpy(&tc->muxsuri[pathoff], &muxsuri[ui.pathBeg], ui.pathEnd - ui.pathBeg);
                break;
            }
            default: {
                LOG(MOD_TCE|WARNING, "Unknown field in infos response - ignored: %s", D.field.name);
                uj_skipValue(&D);
                break;
            }
            }
        }
        uj_exitObject(&D);
        uj_assertEOF(&D);
        if( error || muxsuri == NULL ) {
            LOG(MOD_TCE|ERROR, "Infos error: %s %s", router, error);
            err = TC_ERR_REJECTED;
            goto failed;
        }
        LOG(MOD_TCE|INFO, "Infos: %s %s %s", router, muxsid, muxsuri);
        err = TC_INFOS_GOT_URI;
      failed:
        tc->tstate = err;
        ws_close(&tc->ws, 1000);
        return;
    }
    if( ev == WSEV_CLOSED ) {
        s1_t tstate = tc->tstate;
        if( tstate >= 0 && tstate != TC_INFOS_GOT_URI )
            tstate = TC_ERR_CLOSED; // unexpected close
        if( tstate != TC_INFOS_GOT_URI ) {
            tc_done(tc, tstate);
            return;
        }
        ws_free(&tc->ws);
        tc_connect_muxs(tc);
        return;
    }
    LOG(MOD_TCE|INFO, "tc_info_request - Unknown event: %d", ev);
}


static dbuf_t tc_getSendbuf (s2ctx_t* s2ctx, int minsize) {
    tc_t* tc = s2ctx2tc(s2ctx);
    if( tc->tstate != TC_MUXS_CONNECTED ) {
        // LOG(MOD_TCE|WARNING, "Dropping WS frame - not connected to MUXS");
        dbuf_t b = { .buf=NULL, .bufsize=0, .pos=0 };
        return b;
    }
    return ws_getSendbuf(&tc->ws, minsize);
}


static void tc_sendText (s2ctx_t* s2ctx, dbuf_t* buf) {
    tc_t* tc = s2ctx2tc(s2ctx);
    ws_sendText(&tc->ws, buf);
}


static void tc_sendBinary (s2ctx_t* s2ctx, dbuf_t* buf) {
    tc_t* tc = s2ctx2tc(s2ctx);
    ws_sendBinary(&tc->ws, buf);
}


/**
 * TC引擎默认完成回调函数
 * 当TC操作完成时继续TC引擎的运行
 * 
 * @param timeout 超时定时器指针
 */
void tc_ondone_default (tmr_t* timeout) {
    tc_continue(timeout2tc(timeout));
}

/**
 * TC引擎初始化函数
 * 创建并初始化TC引擎实例
 * 
 * @param ondone 完成回调函数，如果为NULL则使用默认回调
 * @return 初始化完成的TC实例指针
 */
tc_t* tc_ini (tmrcb_t ondone) {
    // 确保缓冲区大小足够容纳主机名和端口号
    assert(TC_RECV_BUFFER_SIZE > MAX_HOSTNAME_LEN + MAX_PORT_LEN + 2);
    
    // 分配TC结构体内存
    tc_t* tc = rt_malloc(tc_t);
    
    // 初始化WebSocket连接
    ws_ini(&tc->ws, TC_RECV_BUFFER_SIZE, TC_SEND_BUFFER_SIZE);
    
    // 初始化超时定时器
    rt_iniTimer(&tc->timeout, tc_timeout);
    
    // 设置初始状态
    tc->tstate = TC_INI;
    
    // 设置凭证集为REG（常规凭证）
    tc->credset = SYS_CRED_REG;
    
    // 设置完成回调函数
    tc->ondone = ondone==NULL ? tc_ondone_default : ondone;
    
    // 初始化MUXS URI为无效状态
    tc->muxsuri[0] = URI_BAD;
    
    // 初始化S2E上下文
    s2e_ini(&tc->s2ctx);
    
    // 设置S2E回调函数
    tc->s2ctx.getSendbuf = tc_getSendbuf;    // 获取发送缓冲区
    tc->s2ctx.sendText   = tc_sendText;      // 发送文本数据
    tc->s2ctx.sendBinary = tc_sendBinary;    // 发送二进制数据
    
    return tc;
}


/**
 * 释放TC引擎资源
 * 清理TC实例并释放相关资源
 * 
 * @param tc TC实例指针
 */
void tc_free (tc_t* tc) {
    if( tc == NULL )
        return;
        
    // 释放WebSocket资源
    ws_free(&tc->ws);
    
    // 清除超时定时器
    rt_clrTimer(&tc->timeout);
    
    // 保存最后状态
    tstateLast = tc->tstate;
    tc->tstate = TC_ERR_DEAD;
    
    // 释放S2E上下文
    s2e_free(&tc->s2ctx);
    
    // 释放TC结构体内存
    rt_free(tc);
}


/**
 * 启动TC引擎
 * 开始TC引擎的连接流程，首先连接到INFOS服务获取MUXS信息
 * 
 * @param tc TC实例指针
 */
void tc_start (tc_t* tc) {
    assert(tc->tstate == TC_INI);
    int tstate_err = TC_ERR_NOURI;

    // 获取TC URI配置
    str_t tcuri = sys_uri(SYS_CRED_TC, tc->credset);
    if( tcuri == NULL ) {
        LOG(MOD_TCE|ERROR, "No TC URI configured");
        goto errexit;
    }
    
    // 使用WebSocket缓冲区作为主机名和端口的临时存储
    // 在建立连接过程中会被销毁
    char hostname[MAX_HOSTNAME_LEN];
    char port[MAX_PORT_LEN];
    int   ok;
    
    // 检查URI格式和提取主机、端口信息
    if( (ok = uri_checkHostPortUri(tcuri, "ws", hostname, MAX_HOSTNAME_LEN, port, MAX_PORT_LEN)) == URI_BAD ) {
        LOG(MOD_TCE|ERROR,"Bad TC URI: %s", tc);
        goto errexit;
    }
    
    // 如果是TLS URI，设置TLS配置
    if( ok == URI_TLS && !conn_setup_tls(&tc->ws, SYS_CRED_TC, tc->credset, hostname) ) {
        goto errexit;
    }
    
    tstate_err = TC_ERR_FAILED;
    LOG(MOD_TCE|INFO, "Connecting to INFOS: %s", tcuri);
    log_flushIO();
    
    // 连接到INFOS服务（路径固定为/router-info）
    if( !ws_connect(&tc->ws, hostname, port, "/router-info") ) {
        LOG(MOD_TCE|ERROR, "TC connect failed - URI: %s", tcuri);
        goto errexit;
    }
    
    // 设置连接超时
    rt_setTimerCb(&tc->timeout, rt_seconds_ahead(TC_TIMEOUT), tc_timeout);
    
    // 设置INFOS请求事件回调
    tc->ws.evcb = (evcb_t)tc_info_request;
    
    // 设置状态为INFOS请求等待中
    tc->tstate = TC_INFOS_REQ_PEND;
    return;
    
 errexit:
    tc_done(tc, tstate_err);
    return;
}


void tc_continue (tc_t* tc) {
    s1_t tstate = tc->tstate;

    if( (tstate == TC_ERR_REJECTED || tstate == TC_ERR_NOURI || tc->retries >= 10) && !sys_noCUPS ) {
        LOG(MOD_TCE|INFO, "Router rejected or retry limit reached. Invoking CUPS.");
        sys_stopTC();
        sys_triggerCUPS(-1);
        return;
    }

    if( tstate == TC_INFOS_BACKOFF ) {
        int retries_old = tc->retries;
        tmrcb_t ondone = tc->ondone;
        assert(TC == tc);
        tc_free(tc);
        TC = tc = tc_ini(ondone);
        tc_start(tc);
        tc->retries = retries_old + 1;
        return;
    }
    if( tstate == TC_MUXS_BACKOFF ) {
        tc->retries += 1;
        tc_connect_muxs(tc);
        return;
    }

    if( tc->muxsuri[0] != URI_BAD ) {
        // We have a muxs uri
        if( tc->retries <= 4 && tstate == TC_ERR_CLOSED ) {
            // Try to reconnect with increasing backoff
            int backoff = 1 << tc->retries;
            tc->tstate = TC_MUXS_BACKOFF;
            rt_setTimerCb(&tc->timeout, rt_seconds_ahead(backoff), tc->ondone);
            LOG(MOD_TCE|INFO, "MUXS reconnect backoff %ds (retry %d)", backoff, tc->retries);
            return;
        }
        tc->muxsuri[0] = URI_BAD;
        tc->retries = 1;
    }

    int backoff = min(tc->retries, 6);
    tc->tstate = TC_INFOS_BACKOFF;
    rt_setTimerCb(&tc->timeout, rt_seconds_ahead(backoff * 10), tc->ondone);
    LOG(MOD_TCE|INFO, "INFOS reconnect backoff %ds (retry %d)", backoff*10, tc->retries);
}


/**
 * 停止TC引擎
 * 终止当前运行的TC引擎并清理资源
 */
void sys_stopTC () {
    if( TC != NULL ) {
        LOG(MOD_TCE|INFO, "Terminating TC engine");
        tc_free(TC);              // 释放TC资源
        TC = NULL;                // 清空全局TC指针
        sys_inState(SYSIS_TC_DISCONNECTED);  // 通知系统TC已断开
    }
}


/**
 * 启动TC引擎
 * 如果TC引擎未运行且未禁用TC，则启动新的TC引擎实例
 */
void sys_startTC () {
    if( TC != NULL || sys_noTC )
        return;  // TC已在运行或被禁用
        
    LOG(MOD_TCE|INFO, "Starting TC engine");
    TC = tc_ini(NULL);            // 创建新的TC实例
    tc_start(TC);                 // 启动TC连接流程
    sys_inState(SYSIS_TC_DISCONNECTED);  // 初始状态为断开
}


/**
 * TC引擎初始化
 * 执行TC引擎的全局初始化（目前为空实现）
 */
void sys_iniTC () {
    // 目前TC引擎的初始化在sys_startTC中进行
    // 此函数保留用于将来可能的全局初始化需求
}

/**
 * 获取TC引擎状态
 * 返回当前TC引擎的状态
 * 
 * @return TC状态码，如果TC未运行则返回最后状态
 */
s1_t sys_statusTC () {
    return TC ? TC->tstate : tstateLast;
}
