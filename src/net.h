/*
 * NET - 网络通信基础设施头文件
 * =============================
 * 功能：BasicStation的网络连接和TLS安全通信基础
 * 核心：连接管理、TLS集成、URI解析、事件回调
 */

#ifndef _net_h_
#define _net_h_

#include "mbedtls/net_sockets.h"  // mbedTLS网络套接字
#include "rt.h"                  // 运行时基础库
#include "tls.h"                 // TLS安全层

// 前置声明和类型定义
struct conn;                                 // 连接结构前置声明
typedef void (*evcb_t)(struct conn*, int ev); // 事件回调函数类型
typedef mbedtls_net_context netctx_t;        // 网络上下文类型别名

/**
 * struct conn - 网络连接结构
 * 功能：管理单个网络连接的完整状态，包括缓冲区、TLS、定时器等
 */
typedef struct conn {
    aio_t*   aio;         // 异步IO：AIO事件管理器
    tmr_t    tmr;         // 定时器：连接超时和重试定时器
    
    // 读取端配置
    u1_t*    rbuf;        // 读缓冲区：接收数据的缓冲区指针
    doff_t   rbufsize;    // 读缓冲区大小：接收缓冲区总字节数
    doff_t   rpos;        // 读位置：套接字在此位置填入数据
    doff_t   rbeg;        // 读开始：最旧帧在接收缓冲区中的位置，rbeg[-1]是操作码
    doff_t   rend;        // 读结束：帧结束位置，之后开始WebSocket头部
    
    // 写入端配置
    u1_t*    wbuf;        // 写缓冲区：发送数据的缓冲区指针
    doff_t   wbufsize;    // 写缓冲区大小：发送缓冲区总字节数
    doff_t   wpos;        // 写位置：套接字从此位置读取数据并发送
    doff_t   wend;        // 写结束：WebSocket帧结束位置，之后是2字节帧长度+帧数据
    doff_t   wfill;       // 写填充：本地生产者在此位置填入数据

    u1_t     state;       // 连接状态：当前连接的状态机状态
    s1_t     optemp;      // 操作临时值：与操作上下文相关的临时值
    u2_t     creason;     // 关闭原因：连接关闭的原因代码
    evcb_t   evcb;        // 事件回调：连接事件回调函数

    netctx_t   netctx;    // 网络上下文：mbedTLS网络套接字上下文
    tlsctx_p   tlsctx;    // TLS上下文：TLS/SSL连接上下文指针
    tlsconf_t* tlsconf;   // TLS配置：TLS配置指针，如果共享则存储在其他地方可为NULL
    str_t      authtoken; // 认证令牌：用于身份验证的令牌字符串

    void*      opctx;     // 操作上下文：管理此连接的上下文指针

    char* host;           // 主机名：目标主机名或IP地址
    char* port;           // 端口：目标端口号字符串
    char* uripath;        // URI路径：请求的URI路径部分
} conn_t;

// 连接管理API
void conn_evcb_nil(conn_t* conn, int wsev);              // 空事件回调：conn_t.evcb不能为NULL时使用
int  conn_setup_tls (conn_t* conn, int cred_cat, int cred_set, const char* servername);
     // 设置TLS：为连接配置TLS/SSL参数

/**
 * struct uri_info - URI信息结构
 * 功能：解析URI后各部分的位置信息
 */
struct uri_info {
    doff_t schemeEnd;     // 协议结束：协议部分结束位置
    doff_t hostportBeg;   // 主机端口开始：主机:端口部分开始位置
    doff_t hostportEnd;   // 主机端口结束：主机:端口部分结束位置
    doff_t hostBeg;       // 主机开始：主机名开始位置
    doff_t hostEnd;       // 主机结束：主机名结束位置
    doff_t portBeg;       // 端口开始：端口号开始位置
    doff_t portEnd;       // 端口结束：端口号结束位置
    doff_t pathBeg;       // 路径开始：URI路径开始位置
    doff_t pathEnd;       // 路径结束：URI路径结束位置
};

// URI处理API
int uri_isScheme (const char* uri, const char* scheme);    // 检查协议：判断URI是否使用指定协议
int uri_parse    (dbuf_t* b, struct uri_info* u, int skipSchema); // 解析URI：解析URI各个组成部分

// URI类型枚举
enum { URI_BAD=0, URI_TCP, URI_TLS };  // URI类型：错误、TCP、TLS

int uri_checkHostPortUri (const char* uri,          // 检查主机端口URI：验证并提取主机端口信息
                          const char* scheme,       // 预期协议：期望的URI协议
                          char* host, int hostlen,  // 主机缓冲区：存储主机名的缓冲区
                          char* port, int portlen); // 端口缓冲区：存储端口号的缓冲区

#endif // _net_h_
