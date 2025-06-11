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

//=============================================================================
// 文件功能：无线电抽象层(Radio Abstraction Layer, RAL)接口定义
// 作用说明：为不同类型的LoRa无线电硬件提供统一的抽象层接口，
//          支持SX1301/SX1302等不同芯片组，以及单/多无线电配置
//=============================================================================

#ifndef _ral_h_
#define _ral_h_

#if defined(CFG_lgw1) && defined(CFG_lgw2)
#error Only one of the two params can be set: CFG_lgw1 CFG_lgw2
#endif

#include "s2e.h"
#include "timesync.h"

//=============================================================================
// xtime时间戳编码格式定义
// xtime是RAL层使用的64位时间戳格式，编码了时间信息和无线电单元信息
//=============================================================================
// Encoding of xtime:
//  bits:
//   63    符号位 (总是正数)
//   62-56 时间戳来源的无线电单元编号 (最大128个单元)
//   55-48 随机值，用于区分不同SX1301会话 (从不为0，有效xtime从不为0)
//   47-0  SX1301启动后的微秒数 (每9年溢出一次，远大于会话运行时间)
//
// Encoding of rctx:
//   6-0   无线电单元编号
#define RAL_TXUNIT_SHIFT 56    // 发送单元编号在xtime中的位移
#define RAL_XTSESS_SHIFT 48    // 会话编号在xtime中的位移  
#define RAL_TXUNIT_MASK  0x7F  // 发送单元编号掩码(7位)
#define RAL_XTSESS_MASK  0xFF  // 会话编号掩码(8位)

//=============================================================================
// 发送状态返回码定义
//=============================================================================
#define RAL_TX_OK     0  // 发送成功
#define RAL_TX_FAIL  -1  // 发送失败(未指定错误)
#define RAL_TX_NOCA  -2  // 信道接入被拒绝(LBT信道繁忙检测失败)

//=============================================================================
// xtime和rctx格式解析宏函数
// 功能：从xtime时间戳中提取不同的信息字段
//=============================================================================
#define ral_xtime2sess(  xtime) ((u1_t)(((xtime)>>RAL_XTSESS_SHIFT)&RAL_XTSESS_MASK))  // 提取会话编号
#define ral_xtime2txunit(xtime) ((u1_t)(((xtime)>>RAL_TXUNIT_SHIFT)&RAL_TXUNIT_MASK))  // 提取发送单元编号
#define ral_xtime2rctx(  xtime) ((sL_t)ral_xtime2txunit(xtime))                       // 提取接收上下文
#define ral_rctx2txunit(  rctx) ((u1_t)((rctx)&RAL_TXUNIT_MASK))                     // 从接收上下文提取发送单元编号

//=============================================================================
// 信道无线参数结构体
// 功能：定义单个信道的LoRa无线参数范围
//=============================================================================
struct chrps {
    u1_t minSF :3; // 最小扩频因子 (使用s2w SF* 枚举值)
    u1_t maxSF :3; // 最大扩频因子 (使用s2w SF* 枚举值)
    u1_t bw    :2; // 带宽设置 (使用s2e BW* 枚举值)
};

//=============================================================================
// 信道定义列表结构体
// 功能：定义所有上行信道的频率和无线参数配置
//=============================================================================
typedef struct {
    u4_t freq[MAX_UPCHNLS];          // 各信道的频率列表 (Hz)
    struct chrps rps[MAX_UPCHNLS];   // 各信道的无线参数列表
} chdefl_t;

//=============================================================================
// 单个信道定义结构体
// 功能：定义单个信道的完整配置信息
//=============================================================================
typedef struct {
    u4_t freq;        // 信道频率 (Hz)
    struct chrps rps; // 信道无线参数
} chdef_t;

//=============================================================================
// 信道分配状态枚举
// 功能：标识信道分配过程中的不同阶段
//=============================================================================
enum {
    CHALLOC_START,      // 开始信道分配
    CHALLOC_CHIP_START, // 开始芯片信道分配
    CHALLOC_CH,         // 分配单个信道
    CHALLOC_CHIP_DONE,  // 芯片信道分配完成
    CHALLOC_DONE        // 所有信道分配完成
};

//=============================================================================
// 信道分配联合体
// 功能：在信道分配过程中传递不同类型的分配信息
//=============================================================================
typedef union {
// 单个信道分配信息 (CHALLOC_CH阶段使用)
struct {
    u1_t chip  :3 ; // 芯片编号 (MAX_130X <= 8)
    u1_t chan  :4 ; // 芯片内信道编号 (每个芯片最多10个信道)
    u1_t rff   :1 ; // 射频前端编号 (每个芯片2个RFF)
    u4_t rff_freq;  // 射频前端中心频率
    chdef_t chdef;  // 信道定义信息
};
// 芯片分配完成信息 (CHALLOC_CHIP_DONE阶段使用)
struct {
    u1_t chipid  :4 ; // 芯片ID (MAX_130X <= 8)
    u1_t chans   :4 ; // 芯片上分配的信道数量 (每个芯片最多10个信道)
    u4_t minFreq;     // 芯片覆盖的最小频率
    u4_t maxFreq;     // 芯片覆盖的最大频率
};
} challoc_t;

//=============================================================================
// 信道分配回调函数类型定义
// 功能：在信道分配过程中通知调用者分配进度和结果
// 参数：ctx - 用户上下文指针，ch - 分配信息，flag - 分配阶段标识
//=============================================================================
typedef void (*challoc_cb) (void* ctx, challoc_t* ch, int flag);

//=============================================================================
// RAL层核心接口函数声明
//=============================================================================

// 函数功能：执行信道分配算法，将上行信道映射到硬件芯片
// 参数说明：upchs - 上行信道定义列表，alloc_cb - 分配过程回调函数，ctx - 用户上下文
// 返回值：成功返回1，失败返回0
// 调用时机：RAL配置阶段，需要将逻辑信道映射到物理硬件时调用
int   ral_challoc (chdefl_t* upchs, challoc_cb alloc_cb, void* ctx);

// 函数功能：从RPS参数中提取带宽信息
// 参数说明：rps - 无线参数集合
// 返回值：带宽值(BW125/BW250/BW500等)
// 调用时机：需要获取数据速率对应的带宽时调用
int ral_rps2bw (rps_t rps);

// 函数功能：从RPS参数中提取扩频因子信息
// 参数说明：rps - 无线参数集合
// 返回值：扩频因子值(SF7-SF12或FSK)
// 调用时机：需要获取数据速率对应的扩频因子时调用
int ral_rps2sf (rps_t rps);

// 函数功能：初始化无线电抽象层
// 参数说明：无
// 返回值：无
// 调用时机：系统启动时，在配置无线电硬件之前调用
void  ral_ini ();

// 函数功能：停止无线电抽象层，关闭所有无线电硬件
// 参数说明：无
// 返回值：无
// 调用时机：系统关闭时或需要重新配置无线电硬件时调用
void  ral_stop ();

// 函数功能：配置无线电硬件，启动接收和时间同步
// 参数说明：hwspec - 硬件规格字符串，cca_region - CCA区域代码，
//          json - 硬件配置JSON数据，jsonlen - JSON数据长度，upchs - 上行信道配置
// 返回值：成功返回1，失败返回0
// 调用时机：RAL初始化后，需要配置具体硬件参数时调用
int   ral_config (str_t hwspec, u4_t cca_region, char* json, int jsonlen, chdefl_t* upchs);

// 函数功能：查询指定发送单元的发送状态
// 参数说明：txunit - 发送单元编号
// 返回值：发送状态(TXSTATUS_IDLE/TXSTATUS_SCHEDULED/TXSTATUS_EMITTING)
// 调用时机：检查发送任务执行状态时调用
int   ral_txstatus (u1_t txunit);

// 函数功能：中止指定发送单元的发送任务
// 参数说明：txunit - 发送单元编号  
// 返回值：无
// 调用时机：需要取消正在进行或计划中的发送任务时调用
void  ral_txabort (u1_t txunit);

// 函数功能：执行下行发送任务
// 参数说明：txjob - 发送任务结构体，s2ctx - S2E上下文，nocca - 是否跳过信道清除评估
// 返回值：发送结果状态码(RAL_TX_OK/RAL_TX_FAIL/RAL_TX_NOCA)
// 调用时机：需要发送下行数据包时调用
int   ral_tx  (txjob_t* txjob, s2ctx_t* s2ctx, int nocca);

// 函数功能：获取指定发送单元的备选天线位图
// 参数说明：txunit - 发送单元编号
// 返回值：备选天线的位图标识
// 调用时机：天线分集或多天线选择场景中调用
u1_t  ral_altAntennas (u1_t txunit);

//=============================================================================
// RAL内部接口和共享代码
//=============================================================================

// 函数功能：获取时间同步测量数据
// 参数说明：pps_en - PPS使能标志，last_xtime - 最后一次xtime读数指针，timesync - 时间同步数据输出
// 返回值：测量质量指标(数值越小质量越好)
// 调用时机：RAL内部时间同步机制中定期调用
int ral_getTimesync (u1_t pps_en, sL_t* last_xtime, timesync_t* timesync);

#endif // _ral_h_
