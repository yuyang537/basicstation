/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的 SX1301/SX1302 配置系统，负责处理 Semtech SX130x 系列 LoRa 集中器的配置。
 * 它提供了对 SX1301/SX1302 芯片的初始化、配置和管理功能，作为射频前端控制的关键组件。
 * 该模块是基站硬件抽象层的重要部分，确保基站能够正确配置射频参数并与 LoRa 终端设备进行通信。
 * 
 * 文件功能
 * ========
 * 本头文件定义了 SX130x 配置的核心接口和数据结构：
 * - SX130x 配置结构体定义
 * - 配置解析和设置函数
 * - 通道分配函数
 * - 硬件启动函数
 * - 天线类型定义
 * 
 * 主要组件
 * ========
 * 1. 配置数据结构
 *    - 板级配置参数
 *    - 发射增益查找表
 *    - 射频链路配置
 *    - 中频链路配置
 *    - LBT（侦听前发送）配置
 * 
 * 2. 配置管理函数
 *    - 配置解析函数
 *    - JSON 配置处理
 *    - 硬件规格解析
 *    - 区域设置处理
 * 
 * 3. 通道管理系统
 *    - 通道分配函数
 *    - 上行通道定义
 *    - 频率计划处理
 *    - 区域特定配置
 * 
 * 4. 硬件控制接口
 *    - 设备启动函数
 *    - PPS（每秒脉冲）控制
 *    - 天线类型管理
 *    - 发射功率调整
 * 
 * 关键流程
 * ========
 * 1. 配置初始化流程
 *    - 解析 JSON 配置
 *    - 设置板级参数
 *    - 配置射频链路
 *    - 初始化发射增益表
 * 
 * 2. 通道分配流程
 *    - 根据区域设置分配通道
 *    - 配置上行通道参数
 *    - 设置频率计划
 *    - 应用区域特定规则
 * 
 * 3. 硬件启动流程
 *    - 初始化 SX130x 硬件
 *    - 应用配置参数
 *    - 启动射频接收链路
 *    - 准备发射能力
 * 
 * 注意事项
 * ========
 * 1. 硬件依赖性
 *    - 基于特定 SX130x 系列芯片
 *    - 硬件版本兼容性要求
 *    - 对外部库的依赖
 * 
 * 2. 配置限制
 *    - 区域特定的频率限制
 *    - 发射功率限制
 *    - 天线配置要求
 * 
 * 3. 性能考虑
 *    - 配置解析效率
 *    - 芯片初始化时间
 *    - 资源使用情况
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

#ifndef _sx130xconf_h_
#define _sx130xconf_h_
#if defined(CFG_lgw1)

#include <stdio.h> // loragw_fpga.h refers to FILE
#include "lgw/loragw_hal.h"
#if !defined(CFG_sx1302)
#include "lgw/loragw_lbt.h"
#include "lgw/loragw_fpga.h"
#endif
#include "s2conf.h"
#include "ral.h" //chdefl_t


#define SX130X_ANT_NIL    0
#define SX130X_ANT_OMNI   1
#define SX130X_ANT_SECTOR 2
#define SX130X_ANT_UNDEF  3

struct sx130xconf {
    struct lgw_conf_board_s  boardconf;
    struct lgw_tx_gain_lut_s txlut;
    struct lgw_conf_rxrf_s   rfconf[LGW_RF_CHAIN_NB];
    struct lgw_conf_rxif_s   ifconf[LGW_IF_CHAIN_NB];
#if !defined(CFG_sx1302)
    struct lgw_conf_lbt_s    lbt;
#endif
    s2_t  txpowAdjust;   // assuming there is only one TX path / SX130X (scaled by TXPOW_SCALE)
    u1_t  pps;           // enable PPS latch of trigger count
    u1_t  antennaType;   // type of antenna
    char  device[MAX_DEVICE_LEN];   // SPI device, FTDI spec etc.
};

extern str_t station_conf_USAGE;

int  sx130xconf_parse_setup (struct sx130xconf* sx130xconf, int slaveIdx, str_t hwspec, char* json, int jsonlen);
int  sx130xconf_challoc (struct sx130xconf* sx130xconf, chdefl_t* upchs);
int  sx130xconf_start (struct sx130xconf* sx130xconf, u4_t region);


#endif // defined(CFG_lgw1)
#endif // _sx130xconf_h_
