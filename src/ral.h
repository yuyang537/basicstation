/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的射频抽象层(Radio Abstraction Layer)，提供统一的射频接口。
 * 它封装了不同射频芯片(如SX1301/SX1302)的具体实现细节，提供标准化的操作接口。
 * 该模块是基站与物理射频硬件之间的桥梁，确保上层应用的硬件无关性。
 * 
 * 文件功能
 * ========
 * 本头文件定义了射频抽象层的核心接口：
 * - 射频通道的配置和管理
 * - 发送和接收控制
 * - 时间同步
 * - 硬件资源分配
 * - 射频参数转换
 * 
 * 主要组件
 * ========
 * 1. 时间戳管理
 *    - 扩展时间戳(xtime)编码
 *    - 会话标识
 *    - 射频单元标识
 *    - 时间同步支持
 * 
 * 2. 通道管理系统
 *    - 通道定义结构(chdef_t)
 *    - 通道参数配置
 *    - 频率和数据速率设置
 *    - 多通道分配
 * 
 * 3. 硬件抽象接口
 *    - 初始化和配置
 *    - 发送状态管理
 *    - 天线选择
 *    - CCA(信道检测)支持
 * 
 * 4. 资源分配
 *    - 芯片资源管理
 *    - 通道分配回调
 *    - 频率范围控制
 *    - RFF(射频前端)配置
 * 
 * 关键流程
 * ========
 * 1. 初始化流程
 *    - 硬件规格解析
 *    - 通道配置
 *    - 时间同步设置
 *    - 资源分配
 * 
 * 2. 发送流程
 *    - 发送参数验证
 *    - 状态检查
 *    - CCA处理
 *    - 发送控制
 * 
 * 3. 资源分配流程
 *    - 芯片选择
 *    - 通道分配
 *    - 频率规划
 *    - 参数配置
 * 
 * 注意事项
 * ========
 * 1. 硬件限制
 *    - 最大支持8个射频芯片
 *    - 每个芯片10个通道
 *    - 每个芯片2个RFF
 *    - 频率范围限制
 * 
 * 2. 时序要求
 *    - 时间戳精度
 *    - 同步要求
 *    - 发送时序控制
 * 
 * 3. 配置约束
 *    - 通道参数有效性
 *    - 频率分配规则
 *    - 资源冲突避免
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

#ifndef _ral_h_
#define _ral_h_

#if defined(CFG_lgw1) && defined(CFG_lgw2)
#error Only one of the two params can be set: CFG_lgw1 CFG_lgw2
#endif

#include "s2e.h"
#include "timesync.h"

// Encoding of xtime:
//  bits:
//   63    sign (always positive)
//   62-56 radio unit where the time stamp originated from (max 128)
//   55-48 random value to disambiguate different SX1301 sessions (never 0, aka valid xtime is never 0)
//   47-0  microseconds since SX1301 start (rollover every 9y >> uptime of sessions)
//
// Encoding of rctx:
//   6-0   radio unit
#define RAL_TXUNIT_SHIFT 56
#define RAL_XTSESS_SHIFT 48
#define RAL_TXUNIT_MASK  0x7F
#define RAL_XTSESS_MASK  0xFF

#define RAL_TX_OK     0  // ok
#define RAL_TX_FAIL  -1  // unspecific error
#define RAL_TX_NOCA  -2  // channel access denied (LBT)

#define ral_xtime2sess(  xtime) ((u1_t)(((xtime)>>RAL_XTSESS_SHIFT)&RAL_XTSESS_MASK))
#define ral_xtime2txunit(xtime) ((u1_t)(((xtime)>>RAL_TXUNIT_SHIFT)&RAL_TXUNIT_MASK))
#define ral_xtime2rctx(  xtime) ((sL_t)ral_xtime2txunit(xtime))
#define ral_rctx2txunit(  rctx) ((u1_t)((rctx)&RAL_TXUNIT_MASK))

struct chrps {
    u1_t minSF :3; // s2w SF* enum
    u1_t maxSF :3;
    u1_t bw    :2; // s2e BW* enum
};

typedef struct {
    u4_t freq[MAX_UPCHNLS];
    struct chrps rps[MAX_UPCHNLS];
} chdefl_t;

typedef struct {
    u4_t freq;
    struct chrps rps;
} chdef_t;

enum {
    CHALLOC_START,
    CHALLOC_CHIP_START,
    CHALLOC_CH,
    CHALLOC_CHIP_DONE,
    CHALLOC_DONE
};

typedef union {
struct {
    u1_t chip  :3 ; // MAX_130X <= 8
    u1_t chan  :4 ; // 10 channels per chip
    u1_t rff   :1 ; // 2 RFF per chip
    u4_t rff_freq;
    chdef_t chdef;
};
// CHALLOC_CHIP_DONE
struct {
    u1_t chipid  :4 ; // MAX_130X <= 8
    u1_t chans   :4 ; // 10 channels per chip
    u4_t minFreq;
    u4_t maxFreq;
};
} challoc_t;

typedef void (*challoc_cb) (void* ctx, challoc_t* ch, int flag);
int   ral_challoc (chdefl_t* upchs, challoc_cb alloc_cb, void* ctx);

int ral_rps2bw (rps_t rps);
int ral_rps2sf (rps_t rps);

void  ral_ini ();
void  ral_stop ();
int   ral_config (str_t hwspec, u4_t cca_region, char* json, int jsonlen, chdefl_t* upchs);
int   ral_txstatus (u1_t txunit);
void  ral_txabort (u1_t txunit);
int   ral_tx  (txjob_t* txjob, s2ctx_t* s2ctx, int nocca);
u1_t  ral_altAntennas (u1_t txunit);


// RAL internal APIs and shared code
int ral_getTimesync (u1_t pps_en, sL_t* last_xtime, timesync_t* timesync);


#endif // _ral_h_
