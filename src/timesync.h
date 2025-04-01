/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的时间同步系统，负责维护基站与网络服务器之间的时间同步。
 * 它实现了多种时间域之间的转换，包括GPS时间、系统时间、扩展时间戳等。
 * 该模块是基站精确定时和Class B设备支持的核心组件。
 * 
 * 文件功能
 * ========
 * 本头文件定义了时间同步系统的核心接口：
 * - 时间域转换函数
 * - 同步状态管理
 * - PPS(脉冲每秒)处理
 * - 时间戳会话管理
 * - 网络时间同步处理
 * 
 * 主要组件
 * ========
 * 1. 时间同步结构
 *    - 系统微秒时间(ustime)
 *    - 扩展时间戳(xtime)
 *    - PPS参考时间
 *    - 同步状态信息
 * 
 * 2. 时间转换系统
 *    - GPS时间转换
 *    - 系统时间映射
 *    - 扩展时间戳处理
 *    - 时间跨度标准化
 * 
 * 3. 同步管理
 *    - 同步质量评估
 *    - 时间偏差修正
 *    - 会话管理
 *    - PPS对齐
 * 
 * 4. 网络时间处理
 *    - LNS时间同步
 *    - 往返延迟计算
 *    - 时间偏移估计
 *    - 同步状态更新
 * 
 * 关键流程
 * ========
 * 1. 初始化流程
 *    - 时间系统初始化
 *    - 同步状态重置
 *    - 会话创建
 *    - PPS配置
 * 
 * 2. 同步维护流程
 *    - 定期同步检查
 *    - 时间偏差计算
 *    - 质量评估
 *    - 状态更新
 * 
 * 3. 时间转换流程
 *    - 时间域映射
 *    - 误差补偿
 *    - 边界处理
 *    - 有效性检查
 * 
 * 注意事项
 * ========
 * 1. 精度要求
 *    - 微秒级时间精度
 *    - PPS同步要求
 *    - 时钟漂移补偿
 *    - 转换误差控制
 * 
 * 2. 同步限制
 *    - GPS信号依赖
 *    - 网络延迟影响
 *    - 时钟稳定性要求
 *    - 会话切换影响
 * 
 * 3. 使用约束
 *    - 初始化顺序
 *    - 有效性检查
 *    - 错误处理
 *    - 资源管理
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

#ifndef _timesync_h_
#define _timesync_h_

#include "rt.h"

typedef struct timesync {
    ustime_t ustime;
    sL_t     xtime;
    sL_t     pps_xtime;
} timesync_t;


sL_t     ts_xtime2gpstime (sL_t xtime);
sL_t     ts_gpstime2xtime (u1_t txunit, sL_t gpstime);
sL_t     ts_xtime2xtime   (sL_t xtime, u1_t txunit);
sL_t     ts_ustime2xtime  (u1_t txunit, ustime_t ustime);
ustime_t ts_xtime2ustime  (sL_t xtime);

ustime_t ts_normalizeTimespanMCU (ustime_t timespan);
ustime_t ts_updateTimesync (u1_t txunit, int quality, const timesync_t* curr);
void     ts_setTimesyncLns (ustime_t xtime, sL_t gpstime);
void     ts_processTimesyncLns (ustime_t txtime, ustime_t rxtime, sL_t servertime);
void     ts_iniTimesync ();

// ------------------------------
// Used by RAL impl only
sL_t ts_xticks2xtime (u4_t xticks, sL_t last_xtime);
sL_t ts_newXtimeSession  (u1_t txunit);


#endif // _timesync_h_
