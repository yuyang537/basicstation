/*
 * WS - WebSocket通信层头文件
 * ==========================
 * 功能：RFC 6455标准的WebSocket协议实现
 * 核心：握手协议、帧封装、二进制/文本传输、连接管理
 */

#ifndef _ws_h_
#define _ws_h_

#include "net.h"  // 网络基础设施

typedef conn_t ws_t;  // WebSocket连接类型别名

// 指针转换宏
#define conn2ws(p) (p)                            // 连接指针转WebSocket指针
#define tmr2ws(p) memberof(ws_t, p, tmr)         // 定时器指针转WebSocket指针

// WebSocket连接状态枚举
enum {
    WS_DEAD = 0,           // 死亡状态：连接未建立或已完全关闭
    WS_TLS_HANDSHAKE,      // TLS握手：正在进行TLS/SSL握手
    WS_CLIENT_REQ,         // 客户端请求：发送WebSocket升级请求
    WS_SERVER_RESP,        // 服务器响应：等待WebSocket升级响应
    WS_CONNECTED,          // 已连接：WebSocket连接建立成功
    WS_CLOSING_DRAINC,     // 关闭排空(客户端)：客户端发起关闭，等待数据传输完成
    WS_CLOSING_DRAINS,     // 关闭排空(服务端)：服务端发起关闭，等待数据传输完成
    WS_CLOSING_SENDCLOSE,  // 发送关闭：准备发送关闭帧
    WS_CLOSING_ECHOCLOSE,  // 回应关闭：回应对方的关闭帧
    WS_CLOSING_SENTCLOSE,  // 已发关闭：关闭帧已发送，等待对方确认
    WS_CLOSED              // 已关闭：连接完全关闭
};

// WebSocket事件枚举
enum {
    WSEV_DEAD = 0,      // 死亡事件：连接死亡或不可用
    WSEV_CLOSED,        // 关闭事件：连接正常关闭
    WSEV_DATASENT,      // 数据已发送：数据成功发送到对端
    WSEV_BINARYRCVD,    // 二进制接收：收到二进制数据帧
    WSEV_TEXTRCVD,      // 文本接收：收到文本数据帧
    WSEV_CONNECTED,     // 连接事件：WebSocket连接成功建立
};

// WebSocket管理API
void   ws_shutdown   (ws_t*);                   // 立即关闭连接
void   ws_close      (ws_t*, int reason);       // 协议关闭：发起关闭握手
dbuf_t ws_getRecvbuf (ws_t*);                  // 获取接收缓冲区
dbuf_t ws_getSendbuf (ws_t*, int minsize);     // 获取发送缓冲区
void   ws_sendData   (ws_t*, dbuf_t* b, int binaryData); // 发送数据(指定类型)
void   ws_sendText   (ws_t*, dbuf_t* b);       // 发送文本数据
void   ws_sendBinary (ws_t*, dbuf_t* b);       // 发送二进制数据
void   ws_ini        (ws_t*, int rbufsize, int wbufsize);  // 初始化WebSocket
void   ws_free       (ws_t*);                  // 释放WebSocket资源
int    ws_connect    (ws_t*, char* host, char* port, char* uripath);  // 建立连接

// 性能监控API
int    ws_getRtt     (ws_t*, u2_t* q_80_90_95); // 获取RTT分位数统计

#endif // _ws_h_
