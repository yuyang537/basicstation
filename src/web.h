/*
 * WEB - Web管理界面模块头文件
 * ===========================
 * 功能：BasicStation的HTTP管理服务和Web界面
 * 核心：RESTful API、静态文件服务、路由管理、认证系统
 */

#ifndef _web_h_
#define _web_h_

#include "httpd.h"  // HTTP服务器核心

#define WEB_PORT "8080"  // 默认Web服务端口

// Web服务状态枚举
enum {
    WEB_INI            = 0,     // 初始状态：服务未启动

    WEB_ERR_FAILED        = -1, // 服务失败：一般性服务错误
    WEB_ERR_TIMEOUT       = -3, // 超时错误：请求处理超时
    WEB_ERR_REJECTED      = -4, // 请求拒绝：客户端请求被拒
    WEB_ERR_CLOSED        = -5, // 连接关闭：客户端连接断开
    WEB_ERR_DEAD          = -6, // 服务死亡：Web服务不可用
};

/**
 * struct web - Web服务管理结构
 * 功能：管理HTTP服务器的运行状态和配置
 */
typedef struct web {
    httpd_t   hd;          // HTTP服务器状态：底层HTTP连接管理
    tmr_t     timeout;     // 超时定时器：请求处理超时控制
    s1_t      wstate;      // Web状态：当前服务状态
} web_t;

/**
 * struct web_handler - Web请求处理器结构
 * 功能：定义URL路径到处理函数的映射关系
 */
typedef struct {
    ujcrc_t pathcrc;  // 路径CRC：URL路径的哈希值，用于快速匹配
    int (*f)(httpd_pstate_t* pstate, httpd_t* hd, dbuf_t* buf);  // 处理函数指针
} web_handler_t;

// 全局处理器表
extern const web_handler_t SYS_HANDLERS[];   // 系统API处理器表
extern web_handler_t AUTH_HANDLERS[];        // 认证API处理器表

// Web服务API
void web_authini();                          // 初始化认证系统
dbuf_t sys_webFile (str_t filename);         // 读取Web静态文件

// 指针转换宏
#define timeout2web(p) memberof(web_t, p, timeout)  // 定时器指针转Web指针
#define conn2web(p)    memberof(web_t, p, hd.c)     // 连接指针转Web指针

#endif // _web_h_
