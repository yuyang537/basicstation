/*
 * HTTP - HTTP客户端通信模块头文件
 * ===============================
 * 功能：轻量级HTTP/1.1客户端实现，支持CUPS和LNS通信
 * 核心：请求构建、响应解析、分块传输、连接复用
 */

#ifndef _http_h_
#define _http_h_

#include "net.h"  // 网络基础设施

extern str_t const SUFFIX2CT[];  // 文件扩展名到Content-Type映射表

// 指针转换宏
#define conn2http(p) memberof(http_t, p, c)     // 连接指针转HTTP指针
#define tmr2http(p) memberof(http_t, p, c.tmr)  // 定时器指针转HTTP指针

/**
 * struct http - HTTP连接管理结构
 * 功能：管理HTTP客户端连接的完整状态
 */
typedef struct http {
    conn_t c;           // 基础连接：网络连接和缓冲区管理
    struct {
        int clen;       // 内容长度：HTTP响应体的字节数
        int coff;       // 内容偏移：当前已接收的响应体字节数
    } extra;

    // HTTPD模式专用字段
    struct {
        netctx_t netctx;  // 网络上下文：服务端网络状态
        aio_t*   aio;     // 异步IO：事件循环管理器
    } listen;
} http_t;

// HTTP连接状态枚举
enum {
    HTTP_DEAD = 0,      // 死亡状态：连接未建立
    HTTP_CONNECTED,     // 已连接：刚连接或响应已接收
    HTTP_CLOSED,        // 已关闭：未连接到服务器
    HTTP_SENDING_REQ,   // 发送请求：正在发送HTTP请求
    HTTP_READING_HDR,   // 读取头部：正在接收HTTP响应头
    HTTP_READING_BODY,  // 读取正文：正在接收HTTP响应体
};

// HTTP事件枚举
enum {
    HTTPEV_DEAD = 0,        // 死亡事件：连接不可用
    HTTPEV_CLOSED,          // 关闭事件：HTTP连接关闭
    HTTPEV_CONNECTED,       // 连接事件：连接到服务器成功
    HTTPEV_RESPONSE,        // 响应事件：接收到响应(头部+正文)
    HTTPEV_RESPONSE_MORE,   // 更多响应：接收到更多正文数据
};

// HTTP客户端API
void   http_ini       (http_t*, int bufsize);           // 初始化HTTP连接
void   http_free      (http_t*);                        // 释放HTTP资源
int    http_connect   (http_t*, char* host, char* port);// 连接到服务器
void   http_close     (http_t*);                        // 关闭HTTP连接
void   http_request   (http_t*, dbuf_t* req);          // 发送HTTP请求
int    http_getMore   (http_t*);                       // 读取更多响应体数据
dbuf_t http_getReqbuf (http_t*);                       // 获取请求缓冲区
dbuf_t http_getHdr    (http_t*);                       // 获取响应头缓冲区
dbuf_t http_getBody   (http_t*);                       // 获取响应体缓冲区
int    http_getStatus (http_t*);                       // 获取HTTP状态码

// HTTP解析工具函数
char*  http_skipWsp    (char* p);                      // 跳过空白字符
int    http_readDec    (char* p);                      // 读取十进制数字
int    http_statusCode (char* p);                      // 解析状态码
int    http_icaseCmp   (const char* p, const char* what); // 忽略大小写比较
char*  http_findHeader (char* p, const char* field);   // 查找HTTP头字段
int    http_findContentLength (char* p);               // 查找Content-Length
int    http_setContentLength  (char* p, int clen);     // 设置Content-Length
dbuf_t http_statusText (dbuf_t* hdr);                  // 获取状态文本
int    http_unquote    (char** p);                     // 移除引号

#endif // _http_h_
