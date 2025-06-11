/*
 * TIMESYNC - 时间同步管理模块头文件
 * =================================
 * 功能：BasicStation的高精度时间同步系统
 * 核心：GPS时间、扩展时间(xtime)、系统时间的统一管理和转换
 */

#ifndef _timesync_h_
#define _timesync_h_

#include "rt.h"  // 运行时基础类型

/**
 * struct timesync - 时间同步状态结构
 * 功能：记录不同时间域之间的同步关系
 */
typedef struct timesync {
    ustime_t ustime;     // 系统微秒时间：单调递增的系统时间
    sL_t     xtime;      // 扩展时间：硬件精确时间，用于LoRa调度
    sL_t     pps_xtime;  // PPS扩展时间：GPS秒脉冲对应的扩展时间
} timesync_t;

// 时间转换API
sL_t     ts_xtime2gpstime (sL_t xtime);                    // 扩展时间转GPS时间
sL_t     ts_gpstime2xtime (u1_t txunit, sL_t gpstime);    // GPS时间转扩展时间
sL_t     ts_xtime2xtime   (sL_t xtime, u1_t txunit);      // 扩展时间单元转换
sL_t     ts_ustime2xtime  (u1_t txunit, ustime_t ustime); // 系统时间转扩展时间
ustime_t ts_xtime2ustime  (sL_t xtime);                   // 扩展时间转系统时间

// 时间同步管理API
ustime_t ts_normalizeTimespanMCU (ustime_t timespan);     // 标准化MCU时间跨度
ustime_t ts_updateTimesync (u1_t txunit, int quality, const timesync_t* curr);  // 更新时间同步
void     ts_setTimesyncLns (ustime_t xtime, sL_t gpstime);     // 设置LNS时间同步
void     ts_processTimesyncLns (ustime_t txtime, ustime_t rxtime, sL_t servertime);  // 处理LNS时间同步
void     ts_iniTimesync ();                                // 初始化时间同步系统

// RAL实现专用API
sL_t ts_xticks2xtime (u4_t xticks, sL_t last_xtime);      // 硬件tick转扩展时间
sL_t ts_newXtimeSession  (u1_t txunit);                   // 创建新的扩展时间会话

#endif // _timesync_h_
