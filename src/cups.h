/*
 * CUPS - Configuration and Update Protocol Server客户端头文件
 * =========================================================
 * 功能：BasicStation的CUPS配置更新协议实现
 * 核心：配置获取、固件更新、凭证管理、签名验证
 */

#ifndef _cups_h_
#define _cups_h_

#include "http.h"  // HTTP客户端基础

// CUPS状态机枚举
enum {
    CUPS_INI            = 0,  // 初始状态：CUPS客户端未启动
    CUPS_HTTP_REQ_PEND     ,  // HTTP请求等待：正在发送CUPS请求
    CUPS_FEED_CUPS_URI     ,  // 接收CUPS URI：获取新的CUPS服务地址(未使用)
    CUPS_FEED_TC_URI       ,  // 接收TC URI：获取新的LNS连接地址
    CUPS_FEED_CUPS_CRED    ,  // 接收CUPS凭证：获取CUPS认证证书
    CUPS_FEED_TC_CRED      ,  // 接收TC凭证：获取LNS连接证书
    CUPS_FEED_SIGNATURE    ,  // 接收签名：获取配置数字签名
    CUPS_FEED_UPDATE       ,  // 接收更新：获取固件更新包
    CUPS_DONE              ,  // 完成状态：CUPS协议处理完成

    CUPS_ERR_FAILED        = -1,  // 请求失败：一般性CUPS错误
    CUPS_ERR_NOURI         = -2,  // 无URI：未获取到有效的服务地址
    CUPS_ERR_TIMEOUT       = -3,  // 超时错误：CUPS请求或响应超时
    CUPS_ERR_REJECTED      = -4,  // 请求拒绝：CUPS服务器拒绝请求
    CUPS_ERR_CLOSED        = -5,  // 连接关闭：CUPS连接被服务器关闭
    CUPS_ERR_DEAD          = -6,  // 连接死亡：CUPS连接不可恢复
};

// 更新标志位宏定义
#define UPDATE_FLAG(n) (1 << (CUPS_FEED_##n - CUPS_FEED_CUPS_URI))  // 计算更新标志位

typedef struct cups_sig cups_sig_t;  // CUPS签名结构前置声明

/**
 * struct cups - CUPS客户端主结构
 * 功能：管理与CUPS服务器的通信和配置更新过程
 */
typedef struct cups {
    http_t   hc;          // HTTP连接状态：底层HTTP通信管理
    tmr_t    timeout;     // 超时定时器：CUPS请求超时控制
    s1_t     cstate;      // CUPS状态：当前状态机状态
    u1_t     uflags;      // 更新标志：记录哪些配置部分已更新
    u1_t     temp_n;      // 临时计数：长度字段组装计数器
    u1_t     temp[4];     // 临时缓冲：组装多字节长度字段
    int      segm_off;    // 段偏移：当前处理的数据段偏移量
    int      segm_len;    // 段长度：当前处理的数据段长度
    tmrcb_t  ondone;      // 完成回调：CUPS处理完成时的回调函数
    cups_sig_t* sig;      // 签名验证：数字签名验证上下文
} cups_t;

// 指针转换宏定义
#define timeout2cups(p) memberof(cups_t, p, timeout)  // 定时器指针转CUPS指针
#define conn2cups(p)    memberof(cups_t, p, hc.c)     // 连接指针转CUPS指针

// CUPS客户端API
cups_t* cups_ini ();           // 初始化CUPS客户端：创建并配置CUPS实例
void    cups_free (cups_t* cups);  // 释放CUPS资源：清理CUPS客户端内存
void    cups_start (cups_t* cups); // 启动CUPS请求：开始配置更新流程

#endif // _cups_h_
