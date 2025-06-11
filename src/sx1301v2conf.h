/*
 * SX1301V2CONF - SX1301 AR架构LoRa集中器配置头文件
 * ===============================================
 * 功能：SX1301 AR(Advanced RISC)架构的LoRa集中器配置管理
 * 核心：多板卡支持、多芯片配置、信道分配、天线管理
 */

#ifndef _sx1301v2conf_h_
#define _sx1301v2conf_h_
#if defined(CFG_lgw2)  // 仅在LoRa Gateway 2.x配置下编译

#include "lgw2/sx1301ar_hal.h"  // SX1301 AR HAL库
#include "s2conf.h"             // S2配置解析
#include "ral.h"                // RAL层定义

// 天线类型常量定义
#define SX1301_ANT_NIL    0      // 无天线：未连接天线
#define SX1301_ANT_OMNI   1      // 全向天线：360度覆盖天线
#define SX1301_ANT_SECTOR 2      // 扇形天线：定向天线
#define SX1301_ANT_UNDEF  3      // 未定义：天线类型未知
#define MAX_SX1301_NUM    8      // 最大SX1301芯片数量：支持最多8个芯片

/**
 * struct board_conf - 板卡配置结构
 * 功能：定义单个LoRa集中器板卡的硬件配置参数
 */
struct board_conf {
    sx1301ar_board_cfg_t boardConf;     // 板级配置：基础板卡硬件参数
    sx1301ar_lbt_cfg_t   lbtConf;       // LBT配置：Listen Before Talk参数
    char  device[MAX_DEVICE_LEN];       // 设备路径：SPI设备路径、FTDI规格等
    float txpowAdjusts[SX1301AR_BOARD_RFCHAIN_NB];  // 发送功率调整：每个RF链的功率偏移
    u1_t  antennaTypes[SX1301AR_BOARD_RFCHAIN_NB];  // 天线类型：每个RF链连接的天线类型
    u1_t  pps;                          // PPS使能：启用PPS(每秒脉冲)触发计数锁存
    ujcrc_t fpga_flavor;                // FPGA版本：FPGA固件版本标识
};

/**
 * struct chip_conf - 芯片配置结构
 * 功能：定义单个SX1301芯片的配置参数
 */
struct chip_conf {
    sx1301ar_chip_cfg_t chipConf;       // 芯片配置：SX1301芯片基础配置
    sx1301ar_chan_cfg_t chanConfs[SX1301AR_CHIP_CHAN_NB];  // 信道配置：每个信道的参数设置
};

/**
 * struct sx1301v2conf - SX1301 v2配置主结构
 * 功能：包含完整的SX1301 AR架构系统配置
 */
struct sx1301v2conf {
    struct board_conf boards[SX1301AR_MAX_BOARD_NB];  // 板卡配置：支持多个板卡的配置数组
    struct chip_conf  sx1301[MAX_SX1301_NUM];          // 芯片配置：支持多个SX1301芯片的配置数组
};

// SX1301 v2配置管理API
int  sx1301v2conf_parse_setup (struct sx1301v2conf* sx1301v2conf, int slaveIdx, str_t hwspec, char* json, int jsonlen);
     // 解析配置：从JSON配置文件解析SX1301 v2硬件参数
int  sx1301v2conf_challoc (struct sx1301v2conf* sx1301v2conf, chdefl_t* upchs);
     // 信道分配：为上行信道分配射频和芯片资源
int  sx1301v2conf_start (struct sx1301v2conf* sx1301v2conf, u4_t region);
     // 启动配置：应用配置参数并启动SX1301 AR系统

#endif // defined(CFG_lgw2)
#endif // _sx1301v2conf_h_
