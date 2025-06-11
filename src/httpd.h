/*
 * HTTPD - HTTP服务器模块头文件
 * ============================
 * 功能：嵌入式HTTP/1.1服务器实现，支持静态文件和API服务
 * 核心：多连接管理、状态机处理、请求路由、响应生成
 */

#ifndef _httpd_h_
#define _httpd_h_

#include "net.h"   // 网络基础设施
#include "http.h"  // HTTP客户端基础
#include "uj.h"    // JSON解析器

// 指针转换宏
#define conn2httpd(p) memberof(httpd_t, p, c)    // 连接指针转HTTPD指针
#define tmr2httpd(p) memberof(httpd_t, p, c.tmr) // 定时器指针转HTTPD指针

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

/**
 * enum httpd_pstate_mode - HTTP处理状态模式
 * 功能：定义HTTP请求处理的不同模式
 */
typedef enum {
    HTTPD_PSTATE_UNDEFINED = 0,    // 未定义：初始状态
    HTTPD_PSTATE_PREPARE_RESPONSE, // 准备响应：构建HTTP响应头和正文
    HTTPD_PSTATE_SEND_RESPONSE,    // 发送响应：向客户端发送完整响应
    HTTPD_PSTATE_READ_REQUEST,     // 读取请求：从客户端接收HTTP请求
} httpd_pstate_mode_t;

/**
 * struct httpd_pstate - HTTP处理状态结构
 * 功能：维护单个HTTP连接的完整处理状态
 */
typedef struct httpd_pstate {
    http_t        conn;               // HTTP连接：底层连接管理
    httpd_pstate_mode_t mode;         // 处理模式：当前状态机状态
    int           status;             // 状态码：HTTP响应状态(200/404/500等)
    str_t         path;               // 请求路径：HTTP请求的URI路径
    str_t         hdr;                // 请求头：HTTP请求头字符串
    str_t         body;               // 请求体：POST请求的正文数据
    u1_t          chunked;            // 分块传输：是否使用分块编码
    u1_t          close;              // 连接关闭：是否在响应后关闭连接
    u1_t          keepalive;          // 保持连接：是否启用HTTP持久连接
    s1_t          method;             // 请求方法：GET/POST/PUT/DELETE等
    s1_t          version;            // HTTP版本：1.0或1.1
} httpd_pstate_t;

/**
 * struct httpd - HTTP服务器主结构
 * 功能：管理HTTP服务器的整体状态和配置
 */
typedef struct httpd {
    http_t   c;             // 基础连接：继承HTTP连接能力  
    u1_t     ipver;         // IP版本：IPv4或IPv6支持
    str_t    service;       // 服务配置：端口和绑定信息
    cbfunc_t readycb;       // 就绪回调：服务器启动完成回调
    cbfunc_t discocb;       // 断连回调：客户端断开连接回调
    cbfunc_t requestcb;     // 请求回调：接收到HTTP请求的回调
} httpd_t;

// HTTP方法枚举
enum {
    HTTP_GET    = 0,   // GET方法：获取资源
    HTTP_POST   = 1,   // POST方法：提交数据
    HTTP_PUT    = 2,   // PUT方法：上传/更新资源
    HTTP_DELETE = 3,   // DELETE方法：删除资源
    HTTP_OPTIONS= 4,   // OPTIONS方法：预检请求
    HTTP_HEAD   = 5,   // HEAD方法：仅获取响应头
    HTTP_TRACE  = 6,   // TRACE方法：追踪请求路径
    HTTP_CONNECT= 7,   // CONNECT方法：建立隧道连接
    HTTP_PATCH  = 8,   // PATCH方法：部分更新资源
};

// HTTP版本枚举
enum {
    HTTP_VERSION_1_0 = 0,  // HTTP/1.0协议版本
    HTTP_VERSION_1_1 = 1,  // HTTP/1.1协议版本
};

// HTTP服务器管理API
int  httpd_setup   (httpd_t* hd, str_t service, int ipver);  // 初始化服务器
int  httpd_start   (httpd_t* hd);                           // 启动HTTP服务
void httpd_stop    (httpd_t* hd);                           // 停止HTTP服务
void httpd_free    (httpd_t* hd);                           // 释放服务器资源

// HTTP请求处理API
void httpd_send_response (httpd_pstate_t* pst, int status,  // 发送完整HTTP响应
                         const char* headers, const char* body);
void httpd_send_data     (httpd_pstate_t* pst, dbuf_t* b); // 发送响应数据
void httpd_send_chunk    (httpd_pstate_t* pst, dbuf_t* b); // 发送分块数据
void httpd_send_headers  (httpd_pstate_t* pst, int status, // 发送响应头
                         const char* headers);
void httpd_send_status   (httpd_pstate_t* pst, int status);// 发送状态码
void httpd_end_response  (httpd_pstate_t* pst);           // 结束响应传输

// HTTP工具函数
str_t httpd_get_header   (httpd_pstate_t* pst, const char* name);  // 获取请求头字段
int   httpd_parse_query  (str_t query, char* params[], int maxparams); // 解析查询参数

// methods returned by httpd_iniParseReqLine
enum { HTTP_OTHER_METHOD, HTTP_GET, HTTP_POST };
// versions are returned by httpd_iniParseReqLine
enum { HTTP_x_x = 0 , HTTP_1_0 = 1000, HTTT_1_1 = 1001 };

int httpd_parseReqLine (httpd_pstate_t* pstate, dbuf_t* hdr);

#endif // _httpd_h_
