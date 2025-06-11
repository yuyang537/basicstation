/*
 * TC - Traffic Controller连接管理模块头文件
 * ==========================================
 * 功能：BasicStation与LoRaWAN网络服务器(LNS)的通信桥梁
 * 核心职责：WebSocket连接管理、S2E协议处理、状态机控制、重连机制
 */

#ifndef _tc_h_
#define _tc_h_

#include "ws.h"   // WebSocket通信层
#include "s2e.h"  // S2E协议定义

// TC连接状态枚举
enum {
    TC_INI            = 0,  // 初始状态：未开始连接
    TC_INFOS_REQ_PEND    ,  // 信息请求等待：获取LNS连接信息
    TC_INFOS_GOT_URI     ,  // 获得URI：成功获取MUXS服务地址
    TC_MUXS_REQ_PEND     ,  // MUXS请求等待：正在连接到MUXS服务
    TC_MUXS_CONNECTED    ,  // MUXS已连接：成功建立WebSocket连接
    TC_MUXS_BACKOFF      ,  // MUXS退避：连接失败，等待重试
    TC_INFOS_BACKOFF     ,  // INFOS退避：信息获取失败，等待重试

    TC_ERR_FAILED        = -1,  // 连接失败：一般性连接错误
    TC_ERR_NOURI         = -2,  // 无URI：未能获取有效的服务地址
    TC_ERR_TIMEOUT       = -3,  // 超时错误：连接或响应超时
    TC_ERR_REJECTED      = -4,  // 请求被拒：infos/muxs服务返回错误
    TC_ERR_CLOSED        = -5,  // 连接关闭：服务端主动关闭连接
    TC_ERR_DEAD          = -6,  // 连接死亡：网络连接不可恢复
};

/**
 * struct tc - TC连接管理主结构
 * 功能：维护与LNS的完整连接状态和配置信息
 */
typedef struct tc {
    ws_t     ws;          // WebSocket连接状态：底层通信管理
    tmr_t    timeout;     // 超时定时器：连接和响应超时控制
    s1_t     tstate;      // TC状态：当前连接状态机状态
    u1_t     credset;     // 凭证集：使用的TLS凭证集合
    u1_t     retries;     // 重试计数：当前重连尝试次数
    char     muxsuri[MAX_URI_LEN+3];  // MUXS URI：目标服务器地址
    tmrcb_t  ondone;      // 完成回调：连接状态变化时的回调函数
    s2ctx_t  s2ctx;       // S2上下文：S2E协议处理上下文
} tc_t;

// 指针转换宏定义
#define timeout2tc(p) memberof(tc_t, p, timeout)  // 定时器指针转TC指针
#define conn2tc(p)    memberof(tc_t, p, ws)       // 连接指针转TC指针
#define s2ctx2tc(p)   memberof(tc_t, p, s2ctx)    // S2上下文指针转TC指针

extern tc_t* TC;  // 全局TC实例指针

// TC管理API函数
tc_t*  tc_ini   (tmrcb_t ondone);    // 初始化TC实例
void   tc_free  (tc_t* tc);          // 释放TC资源
void   tc_start (tc_t* tc);          // 启动TC连接
void   tc_ondone_default (tmr_t* timeout);  // 默认完成回调
void   tc_continue (tc_t* tc);       // 继续TC连接过程

#endif // _tc_h_
