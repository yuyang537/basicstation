/*
 * S2CONF - S2配置管理模块头文件
 * ==============================
 * 功能：BasicStation的配置参数管理和默认值定义
 * 核心：系统配置、Flash规格、LoRa参数、运行时常量
 */

#ifndef _s2conf_h_
#define _s2conf_h_

#include "rt.h"  // 运行时基础库

// 默认配置值定义
#define DFLT_LOGFILE_SIZE        "\"10MB\""    // 默认日志文件大小：单个日志文件最大10MB
#define DFLT_LOGFILE_ROTATE             "3"    // 默认日志轮转数：保留3个历史日志文件
#define DFLT_CUPS_BUFSZ           "\"8KB\""    // 默认CUPS缓冲区：8KB分块读取CUPS数据

// TC连接相关默认值
#define DFLT_MAX_RXDATA           (10*1024)    // 最大RX数据：单次接收最大10KB
#define DFLT_MAX_TXDATA           (16*1024)    // 最大TX数据：单次发送最大16KB
#define DFLT_MAX_WSSDATA               2048    // 最大WebSocket数据：单个WSS帧最大2KB
#define DFLT_TC_RECV_BUFSZ        (40*1024)    // TC接收缓冲区：WebSocket接收缓冲区40KB
#define DFLT_TC_SEND_BUFSZ        (80*1024)    // TC发送缓冲区：WebSocket发送缓冲区80KB
#define DFLT_RADIO_INIT_WAIT    "\"200ms\""    // 无线初始化等待：等待无线模块初始化200ms
#define DFLT_MAX_TXUNITS                  4    // 最大发送单元：支持4个并发发送单元
#define DFLT_MAX_130X                     8    // 最大130X芯片：支持最多8个SX130x芯片
#define DFLT_MAX_TXJOBS                 128    // 最大发送作业：发送队列最多128个作业
#define DFLT_MAX_RXJOBS                  64    // 最大接收作业：接收队列最多64个作业
#define DFLT_RADIODEV  "\"/dev/spidev?.0\""    // 默认无线设备：SPI设备路径模板
#define DFLT_TX_MIN_GAP          "\"10ms\""    // 最小发送间隔：帧间最小10ms间隔（ODU最坏情况）
#define DFLT_TX_AIM_GAP          "\"20ms\""    // 目标发送间隔：理想20ms提前量
#define DFLT_TX_MAX_AHEAD        "\"600s\""    // 最大提前调度：消息最多提前600秒调度
#define DFLT_TXCHECK_FUDGE        "\"5ms\""    // 发送检查余量：检查发送状态的时间余量

// TCP保活机制默认值
#define DFLT_TCP_KEEPALIVE              "1"    // TCP保活启用：启用TCP keepalive
#define DFLT_TCP_KEEPIDLE              "60"    // TCP空闲时间：60秒空闲后开始保活探测
#define DFLT_TCP_KEEPINTVL             "15"    // TCP保活间隔：保活探测间隔15秒
#define DFLT_TCP_KEEPCNT                "4"    // TCP保活次数：最多4次探测失败后断开

#define DFLT_MAX_RMTSH                    2    // 最大远程Shell：最多2个并发远程Shell会话
#define DFLT_BEACON_INTVL        "\"128s\""    // 信标间隔：Class B信标间隔128秒

// 平台特定配置覆盖
#if defined(CFG_platform_cisco) || defined(CFG_platform_rpi64)  // Cisco或RPi64平台优化
#undef DFLT_TX_MIN_GAP
#undef DFLT_TX_AIM_GAP
#define DFLT_TX_MIN_GAP          "\"10ms\""    // 平台优化：最小间隔保持10ms
#define DFLT_TX_AIM_GAP          "\"60ms\""    // 平台优化：目标间隔调整为60ms
#endif // defined(CFG_platform_cisco)

// Flash存储规格定义
// Flash布局示意图：
//      _ FLASH_ADDR         _ FLASH_BEG_A   _ FLASH_BEG_B
// |___/____________________/.............../..............._____|_BYTES_
// |   \                  _/\________             _________/  /  | PAGES
//      \   FS_PAGE_START             FS_PAGE_CNT            /
//       \________________                __________________/
//                         FLASH_PAGE_CNT
//
//  - FLASH_ADDR, FLASH_PAGE_CNT 定义可寻址的Flash空间
//  - FS_PAGE_START, FS_PAGE_CNT 定义文件系统在Flash中的位置

#define FLASH_PAGE_SIZE  (4*1024)      // Flash页大小：4KB每页
#define FLASH_PAGE_CNT   (1024)        // Flash页数量：总共1024页
#define FLASH_SIZE       (FLASH_PAGE_CNT*FLASH_PAGE_SIZE)  // Flash总大小：4MB
#define FLASH_ADDR       (0*FLASH_PAGE_SIZE)  // Flash起始地址：从第0页开始
#define FLASH_ERASED     ((u4_t)0xFFFFFFFF)   // Flash擦除值：擦除后的数据值
#define FS_PAGE_START    (512)         // 文件系统起始页：从第512页开始
#define FS_PAGE_CNT      (500)         // 文件系统页数：使用500页
#define FS_MAX_FD        8             // 最大文件描述符：同时打开8个文件
#define FS_MAX_FNSIZE    256           // 最大文件名长度：256字符

// 非LoRa运行时参数
enum {  MAX_DEVICE_LEN = 64 };      // 最大设备名长度：SPI/FTDI等无线设备名最大64字符
enum {  MAX_HOSTNAME_LEN = 128 };   // 最大主机名长度：URI中主机名最大128字符
enum {  MAX_PORT_LEN = 16 };        // 最大端口长度：URI中端口部分最大16字符
enum {  MAX_URI_LEN = 128 };        // 最大URI长度：完整URI最大128字符
enum {  MAX_FILEPATH_LEN = 256 };   // 最大文件路径长度：文件路径最大256字符

enum {  TC_RECV_BUFFER_SIZE =   DFLT_TC_RECV_BUFSZ }; // TC接收缓冲区大小：WebSocket连接到TC的接收缓冲区
enum {  TC_SEND_BUFFER_SIZE =   DFLT_TC_SEND_BUFSZ }; // TC发送缓冲区大小：WebSocket连接到TC的发送缓冲区

enum {  MAX_HWSPEC_SIZE = 32 };     // 最大硬件规格长度：硬件描述字符串最大32字符
enum {  MAX_CMDARGS = 64 };         // 最大命令参数：命令行参数最多64个
enum {  MUXS_PROTOCOL_VERSION = 2 };// MUXS协议版本：当前使用版本2
enum {  MAX_RMTSH = DFLT_MAX_RMTSH };// 最大远程Shell数：同时支持的远程Shell会话数

enum {  LOGLINE_LEN = 512 };        // 日志行长度：单行日志最大512字符

// LoRa处理参数
enum {  RTT_SAMPLES     = 100 };    // RTT样本数：往返时间统计样本数量
enum {  MAX_WSSFRAMES   =  32 };    // 最大WebSocket帧：缓存的WSS帧数量
enum {  MIN_UPJSON_SIZE = 384 };    // 最小上行JSON大小：上行消息JSON最小字节数
enum {  MAX_TXUNITS     = DFLT_MAX_TXUNITS };  // 最大发送单元数：使用默认值
enum {  MAX_130X        = DFLT_MAX_130X };     // 最大130X芯片数：使用默认值
enum {  MAX_TXJOBS      = DFLT_MAX_TXJOBS  };  // 最大发送作业数：使用默认值
enum {  MAX_TXFRAME_LEN =  255 };   // 最大发送帧长度：单个TX帧最大255字节
enum {  MAX_RXFRAME_LEN =  255 };   // 最大接收帧长度：单个RX帧最大255字节
enum {  MAX_RXJOBS      = DFLT_MAX_RXJOBS };   // 最大接收作业数：使用默认值
enum {  TXPOW_SCALE     =   10 };   // 发送功率缩放：内部以s2_t存储时的缩放因子
enum {  MAX_RXDATA      = DFLT_MAX_RXDATA };   // 最大接收数据：使用默认值
enum {  MAX_TXDATA      = DFLT_MAX_TXDATA };   // 最大发送数据：使用默认值
enum {  MAX_WSSDATA     = DFLT_MAX_WSSDATA };  // 最大WebSocket数据：使用默认值

/**
 * struct conf_param - 配置参数结构
 * 功能：定义单个配置参数的完整描述
 */
struct conf_param {
    str_t name;     // 参数名：配置参数的名称
    str_t type;     // 参数类型：数据类型描述
    str_t info;     // 参数信息：参数的详细说明
    str_t src;      // 参数来源：配置来源标识
    str_t value;    // 参数值：当前配置值字符串
    void* pvalue;   // 参数指针：指向实际存储位置的指针
    int (*parseFn)(struct conf_param* param);  // 解析函数：将字符串值解析为实际类型的函数
};

extern struct conf_param conf_params[];  // 全局配置参数表：所有配置参数的数组

// S2配置管理API
void  s2conf_ini ();                      // 初始化配置：设置默认配置参数
int   s2conf_set (str_t src, str_t name, str_t value);  // 设置配置：更新指定配置参数值
void* s2conf_get (str_t name);            // 获取配置：检查是否为有效配置参数
void  s2conf_printAll ();                // 打印所有配置：输出当前所有配置参数

#endif // _s2conf_h_

#ifndef _s2conf_x_
#define _s2conf_x_

#ifndef CONF_PARAM
#define CONF_PARAM(name,type,fn,value,info) extern type##_t name;  // 配置参数宏：定义外部变量
#endif

// 系统配置参数定义（使用CONF_PARAM宏批量定义）
CONF_PARAM(RADIODEV            , str   , str     ,        DFLT_RADIODEV, "default radio device")  // 默认无线设备
CONF_PARAM(LOGFILE_SIZE        , u4    , size_mb ,    DFLT_LOGFILE_SIZE, "default size of a logfile")  // 默认日志文件大小
CONF_PARAM(LOGFILE_ROTATE      , u4    , u4      ,  DFLT_LOGFILE_ROTATE, "besides current log file keep *.1..N (none if 0)")  // 日志轮转数量
CONF_PARAM(TCP_KEEPALIVE_EN    , u4    , u4      ,   DFLT_TCP_KEEPALIVE, "TCP keepalive enabled")  // TCP保活启用
CONF_PARAM(TCP_KEEPALIVE_IDLE  , u4    , u4      ,    DFLT_TCP_KEEPIDLE, "TCP keepalive TCP_KEEPIDLE [s]")  // TCP保活空闲时间
CONF_PARAM(TCP_KEEPALIVE_INTVL , u4    , u4      ,   DFLT_TCP_KEEPINTVL, "TCP keepalive TCP_KEEPINTVL [s]")  // TCP保活间隔
CONF_PARAM(TCP_KEEPALIVE_CNT   , u4    , u4      ,     DFLT_TCP_KEEPCNT, "TCP keepalive TCP_KEEPCNT")  // TCP保活计数
CONF_PARAM(MAX_JOINEUI_RANGES  , u4    , u4      ,                 "10", "max ranges to suppress unwanted join requests")  // 最大JoinEUI范围
CONF_PARAM(CUPS_CONN_TIMEOUT   , ustime, tspan_s ,            "\"60s\"", "connection timeout")  // CUPS连接超时
CONF_PARAM(CUPS_OKSYNC_INTV    , ustime, tspan_h ,            "\"24h\"", "regular check-in with CUPS for updates")  // CUPS定期同步间隔
CONF_PARAM(CUPS_RESYNC_INTV    , ustime, tspan_m ,             "\"1m\"", "check-in with CUPS for updates after a failure")  // CUPS失败重同步间隔
CONF_PARAM(CUPS_BUFSZ          , u4    , size_kb ,      DFLT_CUPS_BUFSZ, "read from CUPS in chunks of this size")  // CUPS缓冲区大小
CONF_PARAM(GPS_REPORT_DELAY    , ustime, tspan_s ,           "\"120s\"", "delay GPS reports and consolidate")  // GPS报告延迟
CONF_PARAM(GPS_REOPEN_TTY_INTV , ustime, tspan_ms,             "\"1s\"", "recheck TTY open if it failed")  // GPS TTY重开间隔
CONF_PARAM(GPS_REOPEN_FIFO_INTV, ustime, tspan_ms,             "\"1s\"", "recheck if FIFO writer fake GPS")  // GPS FIFO重开间隔
CONF_PARAM(CMD_REOPEN_FIFO_INTV, ustime, tspan_ms,             "\"1s\"", "recheck if FIFO writer")  // 命令FIFO重开间隔
CONF_PARAM(RX_POLL_INTV        , ustime, tspan_ms,           "\"20ms\"", "interval to poll SX1301 RX FIFO")  // RX轮询间隔
CONF_PARAM(TC_TIMEOUT          , ustime, tspan_s ,            "\"60s\"", "reconnected to muxs")  // TC重连超时
CONF_PARAM(CLASS_C_BACKOFF_BY  , ustime, tspan_s ,          "\"100ms\"", "retry interval for class C TX attempts")  // Class C退避间隔
CONF_PARAM(CLASS_C_BACKOFF_MAX , u4    , u4      ,                 "10", "max number of class C TX attempts")  // Class C最大重试次数
CONF_PARAM(RADIO_INIT_WAIT     , ustime, tspan_s , DFLT_RADIO_INIT_WAIT, "max wait for radio init command to finish")  // 无线初始化等待
CONF_PARAM(PPS_VALID_INTV      , ustime, tspan_ms,            "\"10m\"", "max age of last PPS sync for GPS time conversions")  // PPS有效间隔
CONF_PARAM(TIMESYNC_RADIO_INTV , ustime, tspan_ms,         "\"2100ms\"", "interval to resync MCU/SX1301")  // 无线时间同步间隔
CONF_PARAM(TIMESYNC_LNS_RETRY  , ustime, tspan_s ,           "\"71ms\"", "resend timesync message to server")  // LNS时间同步重试
CONF_PARAM(TIMESYNC_LNS_PAUSE  , ustime, tspan_s ,             "\"5s\"", "pause after unsuccessful volley of timesync messages")  // LNS时间同步暂停
CONF_PARAM(TIMESYNC_LNS_BURST  , u4    , u4      ,                 "10", "volley of timesync messages before pausing")  // LNS时间同步突发
CONF_PARAM(TIMESYNC_REPORTS    , ustime, tspan_s ,             "\"5m\"", "report interval for current timesync status")  // 时间同步报告间隔
CONF_PARAM(TX_MIN_GAP          , ustime, tspan_s ,      DFLT_TX_MIN_GAP, "min distance between two frames being TXed")  // 最小发送间隔
CONF_PARAM(TX_AIM_GAP          , ustime, tspan_s ,      DFLT_TX_AIM_GAP, "aim for this TX lead time, if delayed should not fall under min")  // 目标发送间隔
CONF_PARAM(TX_MAX_AHEAD        , ustime, tspan_s ,    DFLT_TX_MAX_AHEAD, "maximum time message can be scheduled into the future")  // 最大提前调度
CONF_PARAM(TXCHECK_FUDGE       , ustime, tspan_s ,   DFLT_TXCHECK_FUDGE, "check radio state this time into ongoing TX")  // 发送检查余量
CONF_PARAM(BEACON_INTVL        , ustime, tspan_s ,    DFLT_BEACON_INTVL, "beaconing interval")  // 信标间隔
CONF_PARAM(TLS_SNI             ,     u4,    bool ,               "true", "Set and verify server name of TLS connections")  // TLS SNI启用

#endif // _s2conf_x_

