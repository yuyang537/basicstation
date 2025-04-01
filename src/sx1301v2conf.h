/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的 SX1301v2 配置系统，负责处理升级版 Semtech SX1301 LoRa 集中器的配置。
 * 它提供了对 SX1301AR 芯片的初始化、配置和管理功能，支持多板卡和多芯片配置。
 * 该模块与 sx130xconf 模块类似，但专为新一代 SX1301AR 硬件设计，提供更高的集成度和性能。
 * 
 * 文件功能
 * ========
 * 本头文件定义了 SX1301v2 配置的核心接口和数据结构：
 * - 板级配置结构体定义
 * - 芯片配置结构体定义
 * - 配置解析和设置函数
 * - 通道分配函数
 * - 硬件启动函数
 * 
 * 主要组件
 * ========
 * 1. 板级配置系统
 *    - 板级硬件参数
 *    - LBT（侦听前发送）配置
 *    - 天线类型配置
 *    - 发射功率调整
 *    - FPGA 类型标识
 * 
 * 2. 芯片配置系统
 *    - 芯片核心参数设置
 *    - 通道配置管理
 *    - 接收路径设置
 *    - 发射参数配置
 * 
 * 3. 多板卡支持
 *    - 最多支持 SX1301AR_MAX_BOARD_NB 个板卡
 *    - 板卡间的资源分配
 *    - 设备路径管理
 *    - 统一控制接口
 * 
 * 4. 多芯片管理
 *    - 最多支持 MAX_SX1301_NUM 个 SX1301 芯片
 *    - 芯片间的通道分配
 *    - 配置整合和管理
 *    - 统一初始化流程
 * 
 * 关键流程
 * ========
 * 1. 配置解析流程
 *    - 解析 JSON 配置
 *    - 设置板级和芯片参数
 *    - 配置通道和射频链路
 *    - 应用特定硬件规格
 * 
 * 2. 通道分配流程
 *    - 根据区域设置分配通道
 *    - 配置上行通道参数
 *    - 设置频率计划
 *    - 处理多芯片/多板卡分配
 * 
 * 3. 硬件启动流程
 *    - 初始化所有配置的板卡
 *    - 启动 SX1301AR 芯片
 *    - 配置接收和发射参数
 *    - 准备通信接口
 * 
 * 注意事项
 * ========
 * 1. 硬件兼容性
 *    - 专为 SX1301AR 系列设计
 *    - 与旧版 SX1301 不完全兼容
 *    - 需要特定的驱动库支持
 * 
 * 2. 多设备管理
 *    - 多板卡管理的复杂性
 *    - 资源分配和优先级
 *    - 时间同步要求
 * 
 * 3. 性能和扩展性
 *    - 支持更高容量配置
 *    - 更复杂的通道管理
 *    - 资源利用优化
 */

/*
 * --- Revised 3-Clause BSD License ---
 * Copyright Semtech Corporation 2022. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice,
 *       this list of conditions and the following disclaimer in the documentation
 *       and/or other materials provided with the distribution.
 *     * Neither the name of the Semtech corporation nor the names of its
 *       contributors may be used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION. BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _sx1301v2conf_h_
#define _sx1301v2conf_h_
#if defined(CFG_lgw2)

#include "lgw2/sx1301ar_hal.h"
#include "s2conf.h"
#include "ral.h"


#define SX1301_ANT_NIL    0
#define SX1301_ANT_OMNI   1
#define SX1301_ANT_SECTOR 2
#define SX1301_ANT_UNDEF  3
#define MAX_SX1301_NUM    8

struct board_conf {
    sx1301ar_board_cfg_t boardConf;
    sx1301ar_lbt_cfg_t   lbtConf;
    char  device[MAX_DEVICE_LEN];   // SPI device, FTDI spec etc.
    float txpowAdjusts[SX1301AR_BOARD_RFCHAIN_NB];
    u1_t  antennaTypes[SX1301AR_BOARD_RFCHAIN_NB];
    u1_t  pps;           // enable PPS latch of trigger count
    ujcrc_t fpga_flavor;
};

struct chip_conf {
    sx1301ar_chip_cfg_t chipConf;
    sx1301ar_chan_cfg_t chanConfs[SX1301AR_CHIP_CHAN_NB];
};

struct sx1301v2conf {
    struct board_conf boards[SX1301AR_MAX_BOARD_NB];
    struct chip_conf  sx1301[MAX_SX1301_NUM];
};


int  sx1301v2conf_parse_setup (struct sx1301v2conf* sx1301v2conf, int slaveIdx, str_t hwspec, char* json, int jsonlen);
int  sx1301v2conf_challoc (struct sx1301v2conf* sx1301v2conf, chdefl_t* upchs);
int  sx1301v2conf_start (struct sx1301v2conf* sx1301v2conf, u4_t region);

#endif // defined(CFG_lgw2)
#endif // _sx1301v2conf_h_
