/*
 * RALSUB - RAL子系统通信模块头文件
 * =================================
 * 功能：BasicStation的RAL(Radio Abstraction Layer)主从架构通信
 * 核心：命令协议、消息结构、进程间通信、硬件抽象
 */

#ifndef _ralsub_h_
#define _ralsub_h_

#if defined(CFG_lgw1) && defined(CFG_ral_master_slave)  // 仅在RAL主从模式下编译

#include "timesync.h"  // 时间同步机制

// RAL命令类型枚举
enum {
    RAL_CMD_CONFIG = 1,    // 配置命令：配置LoRa硬件参数
    RAL_CMD_TXSTATUS,      // 发送状态：查询TX状态
    RAL_CMD_TXABORT,       // 中止发送：取消正在进行的TX操作
    RAL_CMD_TX,            // 发送命令：执行TX操作(含CCA检测)
    RAL_CMD_TX_NOCCA,      // 无CCA发送：执行TX操作(跳过CCA检测)
    RAL_CMD_RX,            // 接收命令：上报RX数据包
    RAL_CMD_TIMESYNC,      // 时间同步：同步主从进程时间
    RAL_CMD_STOP,          // 停止命令：停止RAL子系统
};

/**
 * struct ral_header - RAL消息头结构
 * 功能：所有RAL消息的通用头部
 */
struct ral_header {
    sL_t rctx;          // 请求上下文：唯一标识请求的上下文ID
    u1_t cmd;           // 命令类型：RAL_CMD_*枚举值
};

/**
 * struct ral_timesync_req - 时间同步请求结构
 * 功能：请求主从进程之间的时间同步
 */
struct ral_timesync_req {
    sL_t rctx;          // 请求上下文：请求标识
    u1_t cmd;           // 命令类型：RAL_CMD_TIMESYNC
};

/**
 * struct ral_stop_req - 停止请求结构
 * 功能：请求停止RAL子系统操作
 */
struct ral_stop_req {
    sL_t rctx;          // 请求上下文：请求标识
    u1_t cmd;           // 命令类型：RAL_CMD_STOP
};

/**
 * struct ral_txstatus_req - 发送状态请求结构
 * 功能：查询当前发送操作的状态
 */
struct ral_txstatus_req {
    sL_t rctx;          // 请求上下文：请求标识
    u1_t cmd;           // 命令类型：RAL_CMD_TXSTATUS
    u1_t status;        // 状态字段：结构体长度需要与响应对齐
};

/**
 * struct ral_txabort_req - 发送中止请求结构
 * 功能：中止正在进行的发送操作
 */
struct ral_txabort_req {
    sL_t rctx;          // 请求上下文：请求标识
    u1_t cmd;           // 命令类型：RAL_CMD_TXABORT
};

/**
 * struct ral_config_req - 配置请求结构
 * 功能：配置LoRa硬件和信道参数
 */
struct ral_config_req {
    sL_t rctx;          // 请求上下文：请求标识
    u1_t cmd;           // 命令类型：RAL_CMD_CONFIG
    u2_t jsonlen;       // JSON长度：配置JSON字符串的字节数
    u4_t region;        // 区域设置：0=无LBT，!=0=指定区域的LBT
    chdefl_t upchs;     // 信道定义：上行信道配置
    char hwspec[MAX_HWSPEC_SIZE];           // 硬件规格：硬件标识字符串
    char json[PIPE_BUF-16-MAX_HWSPEC_SIZE-sizeof(chdefl_t)];  // JSON配置：硬件配置JSON数据
};

/**
 * struct ral_tx_req - 发送请求结构
 * 功能：执行LoRa数据包发送操作
 */
struct ral_tx_req {
    sL_t  rctx;         // 请求上下文：请求标识
    u1_t  cmd;          // 命令类型：RAL_CMD_TX或RAL_CMD_TX_NOCCA
    u1_t  txlen;        // 发送长度：数据包字节数
    s2_t  txpow;        // 发送功率：TX功率设置
    rps_t rps;          // 无线参数：调制、编码率、带宽等
    u1_t  addcrc;       // CRC标志：是否添加CRC校验
    u4_t  freq;         // 发送频率：射频频率(Hz)
    sL_t  xtime;        // 发送时间：精确的发送时间戳
    u1_t  txdata[MAX_TXFRAME_LEN];  // 发送数据：待发送的数据载荷
};

/**
 * struct ral_response - 通用响应结构
 * 功能：大部分RAL命令的标准响应格式
 * 状态含义：
 * - tx操作: RAL_TX_{OK,FAIL,NOCA}
 * - cca检测: 0=信道忙，1=信道空闲
 * - txstatus: TX状态码
 * - config: 0=失败，1=成功
 */
struct ral_response {
    sL_t rctx;          // 请求上下文：对应请求的上下文ID
    u1_t cmd;           // 命令类型：对应的命令类型
    u1_t status;        // 状态码：操作结果状态
};

/**
 * struct ral_timesync_resp - 时间同步响应结构
 * 功能：返回时间同步结果和质量信息
 */
struct ral_timesync_resp {
    sL_t rctx;          // 请求上下文：对应请求的上下文ID
    u1_t cmd;           // 命令类型：RAL_CMD_TIMESYNC
    int  quality;       // 同步质量：时间同步的质量指标
    timesync_t timesync; // 同步数据：时间同步参数
};

/**
 * struct ral_rx_resp - 接收响应结构
 * 功能：上报接收到的LoRa数据包
 */
struct ral_rx_resp {
    sL_t  rctx;         // 请求上下文：接收上下文ID
    u1_t  cmd;          // 命令类型：RAL_CMD_RX
    u1_t  rxlen;        // 接收长度：数据包字节数
    rps_t rps;          // 无线参数：接收时的调制参数
    u4_t  freq;         // 接收频率：射频频率(Hz)
    sL_t  xtime;        // 接收时间：精确的接收时间戳
    u1_t  rssi;         // 信号强度：接收信号强度指示(*-1缩放)
    s1_t  snr;          // 信噪比：信号与噪声比(*8缩放)
    u1_t  rxdata[MAX_RXFRAME_LEN];  // 接收数据：接收到的数据载荷
};

// 前置声明：LoRa Gateway库的数据包结构
struct lgw_pkt_tx_s;  // TX数据包结构
struct lgw_pkt_rx_s;  // RX数据包结构

// RAL参数转换API
rps_t ral_lgw2rps (struct lgw_pkt_rx_s* p);  // LGW RX参数转RAL参数
void  ral_rps2lgw (rps_t rps, struct lgw_pkt_tx_s* p);  // RAL参数转LGW TX参数

#endif // defined(CFG_lgw1) && defined(CFG_ral_master_slave)

#endif // _ralsub_h_
