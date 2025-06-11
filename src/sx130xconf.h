/*
 * SX130XCONF - SX130x LoRa集中器配置头文件
 * =========================================
 * 功能：SX1302/SX1308 LoRa集中器芯片的配置管理
 * 核心：射频配置、信道分配、功率管理、天线控制
 */

#ifndef _sx130xconf_h_
#define _sx130xconf_h_
#if defined(CFG_lgw1)  // 仅在LoRa Gateway 1.x配置下编译

#include <stdio.h> // loragw_fpga.h refers to FILE
#include "lgw/loragw_hal.h"      // LoRa Gateway HAL库
#if !defined(CFG_sx1302)         // SX1301专用配置
#include "lgw/loragw_lbt.h"      // Listen Before Talk功能
#include "lgw/loragw_fpga.h"     // FPGA相关功能
#endif
#include "s2conf.h"              // S2配置解析
#include "ral.h"                 // RAL层定义，包含chdefl_t

// 天线类型常量定义
#define SX130X_ANT_NIL    0      // 无天线：未连接天线
#define SX130X_ANT_OMNI   1      // 全向天线：360度覆盖天线
#define SX130X_ANT_SECTOR 2      // 扇形天线：定向天线
#define SX130X_ANT_UNDEF  3      // 未定义：天线类型未知

/**
 * struct sx130xconf - SX130x配置主结构
 * 功能：包含LoRa集中器的完整硬件配置参数
 */
struct sx130xconf {
    struct lgw_conf_board_s  boardconf; // 板级配置：时钟源、复位管脚等板级参数
    struct lgw_tx_gain_lut_s txlut;     // 发送功率表：TX功率与数字增益的查找表
    struct lgw_conf_rxrf_s   rfconf[LGW_RF_CHAIN_NB];   // 射频链配置：每个RF链的频率和启用状态
    struct lgw_conf_rxif_s   ifconf[LGW_IF_CHAIN_NB];   // 中频链配置：每个IF链的解调参数
#if !defined(CFG_sx1302)             // SX1301专用配置
    struct lgw_conf_lbt_s    lbt;       // LBT配置：Listen Before Talk参数
#endif
    s2_t  txpowAdjust;   // 发送功率调整：全局TX功率偏移(按TXPOW_SCALE缩放)
    u1_t  pps;           // PPS使能：启用PPS(每秒脉冲)触发计数锁存
    u1_t  antennaType;   // 天线类型：连接的天线类型标识
    char  device[MAX_DEVICE_LEN];   // 设备路径：SPI设备路径、FTDI规格等
};

extern str_t station_conf_USAGE;  // 配置使用说明：station.conf文件格式说明

// SX130x配置管理API
int  sx130xconf_parse_setup (struct sx130xconf* sx130xconf, int slaveIdx, str_t hwspec, char* json, int jsonlen);
     // 解析配置：从JSON配置文件解析SX130x硬件参数
int  sx130xconf_challoc (struct sx130xconf* sx130xconf, chdefl_t* upchs);
     // 信道分配：为上行信道分配射频和中频资源
int  sx130xconf_start (struct sx130xconf* sx130xconf, u4_t region);
     // 启动配置：应用配置参数并启动LoRa集中器

#endif // defined(CFG_lgw1)
#endif // _sx130xconf_h_
