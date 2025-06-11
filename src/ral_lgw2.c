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
// 文件功能：SX1301 V2版本LoRa网关芯片的无线电抽象层实现
// 作用说明：为SX1301 V2系列芯片提供RAL接口实现，包括：
//          - SX1301AR API封装和参数转换
//          - LoRa/FSK调制参数映射
//          - 时间同步和PPS支持
//          - 发送接收数据处理
//          - SPI通信接口封装
// 适用芯片：SX1301 V2系列LoRa网关集中器芯片
// 版本差异：与ral_lgw.c相比，使用不同的底层库(sx1301ar vs libloragw)
//=============================================================================

#if defined(CFG_lgw2)

#if defined(CFG_ral_master_slave)
#error ral_master_slave not compatible with lgw2  // LGW2版本不支持主从模式
#endif

#include "s2conf.h"       // S2配置管理
#include "tc.h"           // 流量控制模块
#include "timesync.h"     // 时间同步功能
#include "sys.h"          // 系统功能接口
#if defined(CFG_linux)
#include "sys_linux.h"    // Linux系统特定功能
#include <errno.h>        // 错误码定义
#endif // defined(CFG_linux)
#include "sx1301v2conf.h" // SX1301 V2配置管理
#include "ral.h"          // 无线电抽象层接口
#include "lgw2/sx1301ar_err.h"  // SX1301AR错误处理
#include "lgw2/sx1301ar_gps.h"  // SX1301AR GPS功能
#include "lgw2/spi_linuxdev.h"  // Linux SPI设备接口

//=============================================================================
// 全局状态变量
//=============================================================================
static u1_t       pps_en;        // PPS(每秒脉冲)使能标志
static s2_t       txpowAdjust;   // 发送功率调整值(按TXPOW_SCALE缩放)
static sL_t       last_xtime;    // 上次硬件时间戳(构建连续64位时间)
static tmr_t      rxpollTmr;     // 接收轮询定时器
static tmr_t      syncTmr;       // 时间同步定时器
static int        spiFd = -1;    // SPI设备文件描述符

//=============================================================================
// 函数功能：SPI读取操作包装函数
// 参数说明：header - SPI头部字节，address - 寄存器地址
//          data - 读取数据缓冲区，size - 读取字节数，status - 状态返回
// 返回值：操作结果，0表示成功，非0表示失败
// 调用时机：需要从SX1301寄存器读取数据时调用
//=============================================================================
static int _spi_read (u1_t header, u2_t address, u1_t* data, u4_t size, u1_t* status) {
    return spi_linuxdev_read(header, spiFd, address, data, size, status);  // 调用Linux SPI设备读取
}

//=============================================================================
// 函数功能：SPI写入操作包装函数
// 参数说明：header - SPI头部字节，address - 寄存器地址
//          data - 写入数据缓冲区，size - 写入字节数，status - 状态返回
// 返回值：操作结果，0表示成功，非0表示失败
// 调用时机：需要向SX1301寄存器写入数据时调用
//=============================================================================
static int _spi_write (u1_t header, u2_t address, const u1_t* data, u4_t size, u1_t* status) {
    return spi_linuxdev_write(header, spiFd, address, data, size, status);  // 调用Linux SPI设备写入
}

//=============================================================================
// 扩频因子映射表
// 功能：将S2E内部SF枚举值映射到SX1301AR库的MR枚举值
//=============================================================================
static const u2_t SF_MAP[] = {
    [SF12 ]= MR_SF12,      // SF12 -> SX1301AR MR枚举
    [SF11 ]= MR_SF11,      // SF11 -> SX1301AR MR枚举
    [SF10 ]= MR_SF10,      // SF10 -> SX1301AR MR枚举
    [SF9  ]= MR_SF9,       // SF9  -> SX1301AR MR枚举
    [SF8  ]= MR_SF8,       // SF8  -> SX1301AR MR枚举
    [SF7  ]= MR_SF7,       // SF7  -> SX1301AR MR枚举
    [FSK  ]= MR_UNDEFINED, // FSK  -> 未定义(FSK使用不同参数)
    [SFNIL]= MR_UNDEFINED, // 无效SF -> 未定义
};

//=============================================================================
// 带宽映射表
// 功能：将S2E内部BW枚举值映射到SX1301AR库的BW枚举值
//=============================================================================
static const u1_t BW_MAP[] = {
    [BW125]= BW_125K,      // 125kHz -> SX1301AR BW枚举
    [BW250]= BW_250K,      // 250kHz -> SX1301AR BW枚举
    [BW500]= BW_500K,      // 500kHz -> SX1301AR BW枚举
    [BWNIL]= BW_UNDEFINED  // 无效BW -> 未定义
};

//=============================================================================
// 函数功能：SX1301AR扩频因子转换为S2E内部SF值
// 参数说明：lgw_sf - SX1301AR的扩频因子枚举值
// 返回值：S2E内部SF枚举值，失败返回SFNIL
// 调用时机：接收数据包时，将硬件SF值转换为内部格式
//=============================================================================
static int to_sf (int lgw_sf) {
    for( u1_t sf=SF12; sf<=FSK; sf++ )        // 遍历所有SF值
        if( SF_MAP[sf] == lgw_sf )             // 查找匹配的映射
            return sf;                         // 返回对应的S2E SF值
    return SFNIL;                              // 未找到匹配项，返回无效值
}

//=============================================================================
// 函数功能：SX1301AR带宽转换为S2E内部BW值
// 参数说明：lgw_bw - SX1301AR的带宽枚举值
// 返回值：S2E内部BW枚举值，失败返回BWNIL
// 调用时机：接收数据包时，将硬件BW值转换为内部格式
//=============================================================================
static int to_bw (int lgw_bw) {
    for( u1_t bw=BW125; bw<=BW500; bw++ )     // 遍历所有BW值
        if( BW_MAP[bw] == lgw_bw )             // 查找匹配的映射
            return bw;                         // 返回对应的S2E BW值
    return BWNIL;                              // 未找到匹配项，返回无效值
}

//=============================================================================
// 函数功能：将SX1301AR接收包参数转换为S2E的RPS格式
// 参数说明：p - SX1301AR接收包结构体指针
// 返回值：S2E内部RPS参数值
// 调用时机：接收数据包处理时调用，转换硬件参数为内部格式
//=============================================================================
static rps_t ral_lgw2rps (sx1301ar_rx_pkt_t* p) {
    return p->modulation == MOD_LORA           // 判断调制方式
        ? rps_make(to_sf(p->modrate), to_bw(p->bandwidth))  // LoRa: 组合SF和BW
        : FSK;                                 // FSK: 直接返回FSK标识
}

//=============================================================================
// 函数功能：将S2E的RPS参数转换为SX1301AR发送包格式
// 参数说明：rps - S2E内部RPS参数，p - SX1301AR发送包结构体指针
// 返回值：无
// 调用时机：发送数据包前调用，将内部参数转换为硬件格式
//=============================================================================
static void ral_rps2lgw (rps_t rps, sx1301ar_tx_pkt_t* p) {
    assert(rps != RPS_ILLEGAL);                // 确保RPS参数有效
    if( rps_sf(rps) == FSK ) {                 // 如果是FSK调制
        p->modulation = MOD_FSK;               // 设置FSK调制
        p->modrate    = MR_57600;              // 设置FSK调制速率：57.6kbps
        p->f_dev      = 25;                    // 设置FSK频偏：25kHz
        p->preamble   = 5;                     // 设置FSK前导码长度：5字节
    } else {                                   // 如果是LoRa调制
        p->modulation = MOD_LORA;              // 设置LoRa调制
        p->modrate    = SF_MAP[rps_sf(rps)];   // 设置扩频因子
        p->bandwidth  = BW_MAP[rps_bw(rps)];   // 设置带宽
    }
}

//=============================================================================
// 函数功能：从RPS参数中提取带宽信息(RAL接口实现)
// 参数说明：rps - S2E内部RPS参数
// 返回值：SX1301AR带宽枚举值
// 调用时机：需要获取带宽信息进行硬件配置时调用
//=============================================================================
int ral_rps2bw (rps_t rps) {
    assert(rps != RPS_ILLEGAL);                // 确保RPS参数有效
    return BW_MAP[rps_bw(rps)];                // 返回对应的SX1301AR带宽值
}

//=============================================================================
// 函数功能：从RPS参数中提取扩频因子信息(RAL接口实现)
// 参数说明：rps - S2E内部RPS参数
// 返回值：SX1301AR扩频因子枚举值
// 调用时机：需要获取扩频因子信息进行硬件配置时调用
//=============================================================================
int ral_rps2sf (rps_t rps) {
    assert(rps != RPS_ILLEGAL);                // 确保RPS参数有效
    return SF_MAP[rps_sf(rps)];                // 返回对应的SX1301AR扩频因子值
}

//=============================================================================
// 函数功能：进行时间同步测量，获取MCU时间与SX1301硬件时间的对应关系
// 参数说明：pps_en - PPS(每秒脉冲)使能标志，用于GPS时间同步
//          last_xtime - 上次读取的硬件时间戳指针，用于构建连续64位时间
//          timesync - 输出时间同步测量结果
// 返回值：测量质量指标(数值越小表示测量质量越好)，失败返回INT_MAX
// 调用时机：定期调用以维持时间同步，支持精确的发送时间调度
// 算法原理：同时读取MCU时间和硬件计数器，计算时间差作为质量指标
// 注意事项：只有在LGW_GPS_EN使能时检测到PPS边沿才能获得有效的锁存值
//=============================================================================
int ral_getTimesync (u1_t pps_en, sL_t* last_xtime, timesync_t* timesync) {
    static u4_t last_pps_xticks;              // 上次PPS锁存的硬件时间戳
    u4_t pps_xticks;                           // 当前PPS锁存的硬件时间戳
    if( pps_en ) {                             // 如果PPS功能启用
        // 注意：只有在LGW_GPS_EN标志使能时检测到PPS边沿才能获得正确的锁存值
        // 如果LGW_GPS_EN设为0，锁存值似乎会丢失
        // 因此在>1秒延迟后的开始就读取锁存值
        if( sx1301ar_get_trigcnt(SX1301AR_BOARD_MASTER, &pps_xticks) != 0 )  // 获取PPS锁存计数器
            goto failed;                       // 获取失败，跳转到错误处理
    }
    u4_t hs_pps = 0;                           // 高速PPS计数器
    if( sx1301ar_get_trighs(SX1301AR_BOARD_MASTER, &hs_pps) != 0 ) hs_pps = 0;  // 获取高速PPS计数，失败则置0
    
    sx1301ar_tref_t tref = sx1301ar_init_tref();    // 初始化时间参考
    sx1301ar_set_xtal_err(0,tref);             // 设置晶振误差为0
    ustime_t t0 = rt_getTime();                // 测量开始时的MCU时间
    u4_t xticks = 0;                           // 硬件计数器当前值
    if( sx1301ar_get_instcnt(SX1301AR_BOARD_MASTER, &xticks) != 0 )  // 获取即时计数器值
        goto failed;                           // 获取失败，跳转到错误处理
    ustime_t t1 = rt_getTime();                // 测量结束时的MCU时间
    sL_t d = (s4_t)(xticks - *last_xtime);     // 计算硬件时间差(处理32位溢出)
    if( d < 0 ) {                              // 如果发生了时间回卷
        LOG(MOD_SYN|CRITICAL,
            "SX1301 time sync roll over - no update for a long time: xticks=0x%08x last_xtime=0x%lX",
            xticks, *last_xtime);              // 记录严重错误
        d += (sL_t)1<<32;                      // 修正32位溢出
    }
    timesync->xtime = *last_xtime += d;        // 更新连续64位硬件时间
    timesync->ustime = (t0+t1)/2;              // MCU时间取测量时间的中点
    timesync->pps_xtime = 0;                   // 将被设置如果pps_en有效且有有效的PPS观测
    if( pps_en && pps_xticks && last_pps_xticks != pps_xticks ) {  // 如果PPS有效且发生了变化
        timesync->pps_xtime = timesync->xtime + (s4_t)(pps_xticks - xticks);  // 计算PPS对应的xtime
        last_pps_xticks = pps_xticks;          // 更新上次PPS时间戳
    }
    LOG(MOD_SYN|XDEBUG, "SYNC: ustime=0x%012lX (Q=%3d): xticks=0x%08x xtime=0x%lX - PPS: pps_xticks=0x%08x (%u) pps_xtime=0x%lX (pps_en=%d)",
        timesync->ustime, (int)(t1-t0), xticks, timesync->xtime, pps_xticks, pps_xticks, timesync->pps_xtime, pps_en);
    return (int)(t1-t0);                       // 返回测量耗时作为质量指标
  failed:
    LOG(MOD_SYN|CRITICAL, "SX1301 time sync failed: %s", sx1301ar_err_message(sx1301ar_errno));  // 记录错误信息
    return INT_MAX;                            // 返回最大值表示测量失败
}

//=============================================================================
// 函数功能：时间同步定时器处理函数
// 参数说明：tmr - 定时器结构体指针
// 返回值：无
// 调用时机：定期调用以维持系统时间同步
// 执行逻辑：获取时间同步测量，更新时间同步状态，设置下次同步时间
//=============================================================================
static void synctime (tmr_t* tmr) {
    timesync_t timesync = {0};                 // 初始化时间同步结构体
    int quality = ral_getTimesync(pps_en, &last_xtime, &timesync);  // 进行时间同步测量
    ustime_t delay = ts_updateTimesync(0, quality, &timesync);      // 更新时间同步状态，获取下次延迟
    rt_setTimer(&syncTmr, rt_micros_ahead(delay));                  // 设置下次同步定时器
}

//=============================================================================
// 函数功能：获取备选天线配置(LGW2版本实现)
// 参数说明：txunit - 发送单元编号
// 返回值：备选天线位图(LGW2版本只有主板#0可以发送，无备选天线)
// 调用时机：天线分集或多天线选择场景中调用
//=============================================================================
u1_t ral_altAntennas (u1_t txunit) {
    // 只有主板#0可以发送 - 没有其他天线
    return 0;                                  // LGW2版本无备选天线
}

//=============================================================================
// 函数功能：执行下行数据包发送
// 参数说明：txjob - 发送任务结构体，包含所有发送参数
//          s2ctx - S2E上下文，包含发送数据
//          nocca - 是否跳过信道清除评估
// 返回值：发送结果状态码(RAL_TX_OK/RAL_TX_FAIL/RAL_TX_NOCA)
// 调用时机：需要发送下行数据包时调用
// 执行逻辑：将S2E发送参数转换为SX1301AR格式，调用硬件发送函数
//=============================================================================
int ral_tx (txjob_t* txjob, s2ctx_t* s2ctx, int nocca) {
    sx1301ar_tx_pkt_t pkt_tx = sx1301ar_init_tx_pkt();  // 初始化SX1301AR发送包结构体

    // 设置LoRa发送的默认参数
    pkt_tx.invert_pol = true;                  // 默认启用极性反转
    pkt_tx.no_header  = false;                 // 默认包含LoRa头部

    if( txjob->preamble == 0 ) {               // 如果未指定前导码长度
        if( txjob->txflags & TXFLAG_BCN ) {    // 如果是信标发送
            pkt_tx.tx_mode = TX_ON_GPS;        // 使用GPS时间触发
            pkt_tx.preamble = 10;              // 信标前导码长度10符号
            pkt_tx.invert_pol = false;         // 信标不反转极性
            pkt_tx.no_header  = true;          // 信标无LoRa头部
        } else {                               // 普通数据发送
            pkt_tx.tx_mode = TX_TIMESTAMPED;   // 使用时间戳触发
            pkt_tx.preamble = 8;               // 默认前导码长度8符号
        }
    } else {                                   // 如果指定了前导码长度
        pkt_tx.preamble = txjob->preamble;     // 使用指定的前导码长度
    }
    rps_t rps = s2e_dr2rps(s2ctx, txjob->dr);  // 将数据速率转换为RPS参数
    ral_rps2lgw(rps, &pkt_tx);                 // 将RPS转换为SX1301AR格式
    pkt_tx.freq_hz    = txjob->freq;           // 设置发送频率
    pkt_tx.count_us   = txjob->xtime;          // 设置发送时间戳
    pkt_tx.rf_chain   = 0;                     // 使用射频链0
    pkt_tx.rf_power   = (float)(txjob->txpow - txpowAdjust) / TXPOW_SCALE;  // 计算发送功率(考虑校准偏移)
    pkt_tx.coderate   = CR_4_5;                // 设置编码率4/5
    pkt_tx.no_crc     = !txjob->addcrc;        // 设置CRC标志
    pkt_tx.size       = txjob->len;            // 设置数据包长度
    memcpy(pkt_tx.payload, &s2ctx->txq.txdata[txjob->off], pkt_tx.size);  // 复制发送数据

    // 注意：当前SX1301AR API无法实现nocca参数
    if( sx1301ar_send(0, &pkt_tx) != 0 ) {     // 调用SX1301AR发送函数
        if( sx1301ar_errno == ERR_LBT_FORBIDDEN )  // 如果是LBT(Listen Before Talk)禁止
            return RAL_TX_NOCA;                // 返回信道接入失败
        LOG(MOD_RAL|ERROR, "sx1301ar_send failed: %s", sx1301ar_err_message(sx1301ar_errno));  // 记录发送错误
        return RAL_TX_FAIL;                    // 返回发送失败
    }
    return RAL_TX_OK;                          // 返回发送成功
}

//=============================================================================
// 函数功能：查询指定发送单元的发送状态
// 参数说明：txunit - 发送单元编号
// 返回值：发送状态(TXSTATUS_IDLE/TXSTATUS_SCHEDULED/TXSTATUS_EMITTING)
// 调用时机：检查发送任务执行状态时调用
//=============================================================================
int ral_txstatus (u1_t txunit) {
    sx1301ar_tstat_t status;                   // SX1301AR状态值
    if( sx1301ar_tx_status(txunit, &status) != 0 ) {  // 查询发送状态
        LOG(MOD_RAL|ERROR, "sx1301ar_tx_status failed: %s", sx1301ar_err_message(sx1301ar_errno));  // 记录错误
        return TXSTATUS_IDLE;                  // 返回空闲状态
    }
    if( status == TX_SCHEDULED )               // 如果状态为已调度
        return TXSTATUS_SCHEDULED;             // 返回已调度状态
    if( status == TX_EMITTING )                // 如果状态为发射中
        return TXSTATUS_EMITTING;              // 返回发射中状态
    return TXSTATUS_IDLE;                      // 默认返回空闲状态
}

//=============================================================================
// 函数功能：中止指定发送单元的发送任务
// 参数说明：txunit - 发送单元编号
// 返回值：无
// 调用时机：需要取消正在进行或计划中的发送任务时调用
//=============================================================================
void ral_txabort (u1_t txunit) {
    if( sx1301ar_abort_tx(txunit) != 0 )       // 调用SX1301AR中止发送函数
        LOG(MOD_RAL|ERROR, "sx1301ar_abort_tx failed: %s", sx1301ar_err_message(sx1301ar_errno));  // 记录错误
}

//=============================================================================
// 函数功能：接收轮询定时器处理函数，定期检查并处理接收到的数据包
// 参数说明：tmr - 定时器结构体指针
// 返回值：无
// 调用时机：定期调用以处理接收数据包
// 执行逻辑：批量接收数据包，转换格式，加入接收队列，刷新给上层
//=============================================================================
static void rxpolling (tmr_t* tmr) {
    while(1) {                                 // 持续轮询直到没有更多数据包
        sx1301ar_rx_pkt_t pkt_rx[SX1301AR_MAX_PKT_NB];  // 接收包数组
        u1_t n;                                // 实际接收的包数量
        
        if( sx1301ar_fetch(0, pkt_rx, SIZE_ARRAY(pkt_rx), &n) == -1 ) {  // 从SX1301AR获取接收包
            LOG(MOD_RAL|ERROR, "sx1301ar_fetch: %s", sx1301ar_err_message(sx1301ar_errno));  // 记录获取错误
            break;                             // 停止轮询
        }
        if( n==0 ) {                           // 如果没有新数据包
            break;                             // 停止轮询
        }
        for( int i=0; i<n; i++ ) {             // 遍历所有接收到的包
            rxjob_t* rxjob = !TC ? NULL : s2e_nextRxjob(&TC->s2ctx);  // 获取下一个接收作业槽位
            if( rxjob == NULL ) {              // 如果接收队列已满
                LOG(ERROR, "SX1301 RX frame dropped - out of space");  // 记录丢包错误
                continue;                      // 跳过此包，允许刷新接收作业
            }
            sx1301ar_rx_pkt_t* p = &pkt_rx[i]; // 获取当前处理的包
            if( p->status != STAT_CRC_OK ) {   // 如果CRC校验失败
                LOG(XDEBUG, "Dropped frame without CRC or with broken CRC");  // 记录调试信息
                continue;                      // 静默忽略坏CRC包
            }
            if( p->size > MAX_RXFRAME_LEN ) {  // 如果包长度超过限制
                // 这种情况不应该发生，因为调用者提供了最大帧长度的空间 - 255字节
                LOG(MOD_RAL|ERROR, "Frame size (%d) exceeds offered buffer (%d)", p->size, MAX_RXFRAME_LEN);
                continue;                      // 跳过此包
            }
            
            // 将接收数据复制到接收作业
            memcpy(&TC->s2ctx.rxq.rxdata[rxjob->off], p->payload, p->size);  // 复制数据载荷
            rxjob->len   = p->size;            // 设置包长度
            rxjob->freq  = p->freq_hz;         // 设置接收频率
            rxjob->xtime = ts_xticks2xtime(p->count_us, last_xtime);  // 转换硬件时间戳为xtime格式
            rxjob->rssi = 255;                 // 初始化RSSI为最大值
            for (int j = 0; j < SX1301AR_BOARD_RFCHAIN_NB; j++) {  // 遍历所有射频链
                // 查找最佳信号质量的射频链
                if(rxjob->rssi < -p->rsig[j].rssi_chan || !p->rsig[j].is_valid) {  // 如果信号质量不佳或无效
                    continue;                  // 跳过此射频链
                }
                rxjob->fts = p->rsig[j].fine_received ? p->rsig[j].fine_tmst : -1;  // 设置精细时间戳
                rxjob->rssi = (u1_t)-p->rsig[j].rssi_chan;  // 设置RSSI(转换为正值)
                rxjob->snr = p->rsig[j].snr*4; // 设置SNR(放大4倍保存为整数)
                rxjob->rctx = j;               // 设置接收上下文(射频链索引)
            }
            rps_t rps = ral_lgw2rps(p);        // 转换RPS参数
            rxjob->dr = s2e_rps2dr(&TC->s2ctx, rps);  // 将RPS转换为数据速率
            if( rxjob->dr == DR_ILLEGAL ) {    // 如果数据速率转换失败
                LOG(MOD_RAL|ERROR, "Unable to map to an up DR: %R", rps);  // 记录错误
                continue;                      // 跳过此包
            }
            s2e_addRxjob(&TC->s2ctx, rxjob);   // 将接收作业加入队列
        }
    }
    s2e_flushRxjobs(&TC->s2ctx);               // 刷新接收作业给上层处理
    rt_setTimer(tmr, rt_micros_ahead(RX_POLL_INTV));  // 设置下次轮询定时器
}

//=============================================================================
// 函数功能：配置无线电硬件，解析配置参数并启动硬件
// 参数说明：hwspec - 硬件规格字符串，cca_region - CCA区域代码
//          json - 硬件配置JSON数据，jsonlen - JSON数据长度
//          upchs - 上行信道配置
// 返回值：成功返回1，失败返回0
// 调用时机：RAL初始化后，需要配置具体硬件参数时调用
// 执行逻辑：验证硬件规格，解析JSON配置，配置硬件，启动接收和时间同步
//=============================================================================
int ral_config (str_t hwspec, u4_t cca_region, char* json, int jsonlen, chdefl_t* upchs) {
    struct sx1301v2conf sx1301v2conf;          // SX1301 V2配置结构体
    if( !sx1301v2conf_parse_setup(&sx1301v2conf, -1, hwspec, json, jsonlen) )  // 解析配置参数
        return 0;                              // 解析失败，返回错误
    str_t device = sx1301v2conf.boards[0].device;  // 获取SPI设备路径
    for( int i=0; i < SX1301AR_MAX_BOARD_NB; i++ ) {  // 遍历所有板卡配置
        if( sx1301v2conf.boards[i].boardConf.board_type == BRD_TYPE_UNKNOWN )  // 如果板卡类型未知
            continue;                          // 跳过此板卡
        if( sx1301v2conf.boards[i].device[0] && strcmp(device, sx1301v2conf.boards[i].device) != 0 ) {  // 检查设备路径一致性
            LOG(MOD_RAL|ERROR, "Multiple SPI devices not (yet) supported: %s and %s",
                device, sx1301v2conf.boards[i].device);  // 记录多设备错误
            goto errexit;                      // 跳转到错误处理
        }
        s2_t fpga_version, dsp_version;        // FPGA和DSP版本号
        str_t v = sx1301ar_version_info(i, &fpga_version, &dsp_version);  // 获取版本信息
        LOG(MOD_RAL|INFO, "Board#%d sx1301ar library version: %s", i, v);  // 记录版本信息
        sx1301v2conf.boards[i].boardConf.spi_read  = _spi_read;   // 设置SPI读取函数
        sx1301v2conf.boards[i].boardConf.spi_write = _spi_write;  // 设置SPI写入函数
    }
    ral_stop();                                // 停止之前的无线电操作

#if defined(CFG_linux)
    u4_t pids[1];                              // 进程ID数组
    int n = sys_findPids(device, pids, SIZE_ARRAY(pids));  // 查找使用该设备的进程
    if( n > 0 )                                // 如果设备被其他进程占用
        rt_fatal("Radio device '%s' in use by process: %d%s", device, pids[0], n>1?".. (and others)":"");  // 致命错误
#endif // defined(CFG_linux)

#if !defined(CFG_variant_testsim)
    int err;                                   // 错误码
    if( (err = spi_linuxdev_open(device, /*default speed*/-1, &spiFd)) != 0 ) {  // 打开SPI设备
        LOG(MOD_RAL|ERROR, "Failed to open SPI device '%s': ret=%d errno=%s", device, err, strerror(errno));  // 记录打开错误
        goto errexit;                          // 跳转到错误处理
    }
    // 配置SPI设备主从模式(仅ATMEL CPU需要)
    //   SPI 0: HOST <-> FPGA
    //   SPI 1: HOST/DSP <-> Flash    -- 以便DSP可以访问闪存进行启动
    if( (err = spi_set_mode(0, SPI_MODE_MASTER)) != 0 ||  // 设置SPI0为主模式
        (err = spi_set_mode(1, SPI_MODE_SLAVE)) != 0 ) {   // 设置SPI1为从模式
        LOG(MOD_RAL|ERROR, "Failed to set mode for SPI device '%s': %s", device, err);  // 记录模式设置错误
        goto errexit;                          // 跳转到错误处理
    }
#endif

    if( !sys_runRadioInit(sx1301v2conf.boards[0].device) ||  // 运行无线电初始化脚本
        !sx1301v2conf_challoc(&sx1301v2conf, upchs) ||      // 分配信道配置
        !sx1301v2conf_start(&sx1301v2conf, cca_region) ) {  // 启动硬件配置
        goto errexit;                          // 任何步骤失败都跳转到错误处理
    }
    // 无线电启动成功，保存配置参数
    txpowAdjust = sx1301v2conf.boards[0].txpowAdjusts[0];  // 保存发送功率调整值
    pps_en = sx1301v2conf.boards[0].pps;       // 保存PPS使能状态
    last_xtime = ts_newXtimeSession(0);        // 初始化新的时间会话
    rt_yieldTo(&rxpollTmr, rxpolling);         // 启动接收轮询定时器
    rt_yieldTo(&syncTmr, synctime);            // 启动时间同步定时器

    LOG(MOD_RAL|INFO, "Station device: %s (PPS capture %sabled)", device, pps_en ? "en":"dis");  // 记录设备状态
    return 1;                                  // 返回成功

  errexit:                                     // 错误处理标签
    if( spiFd >= 0 ) {                         // 如果SPI设备已打开
#if !defined(CFG_variant_testsim)
        (void)spi_linuxdev_close(spiFd);       // 关闭SPI设备
#endif
        spiFd = -1;                            // 重置文件描述符
    }
    return 0;                                  // 返回失败
}

//=============================================================================
// 函数功能：初始化无线电抽象层(本地模式，LGW2版本)
// 参数说明：无
// 返回值：无
// 调用时机：系统启动时，在配置无线电硬件之前调用
// 执行逻辑：初始化时间状态和定时器，为后续配置做准备
// 注意事项：LoRa网关库在本地运行，无需子进程
//=============================================================================
void ral_ini() {
    last_xtime = 0;                            // 重置硬件时间戳
    rt_iniTimer(&rxpollTmr, rxpolling);        // 初始化接收轮询定时器
    rt_iniTimer(&syncTmr, synctime);           // 初始化时间同步定时器
}

//=============================================================================
// 函数功能：停止无线电抽象层，关闭所有硬件和定时器
// 参数说明：无
// 返回值：无
// 调用时机：系统关闭时或需要重新配置无线电硬件时调用
// 执行逻辑：停止SX1301AR，关闭SPI设备，停止定时器，重置状态
//=============================================================================
void ral_stop() {
    sx1301ar_stop(SX1301AR_MAX_BOARD_NB);      // 停止所有SX1301AR板卡
    if( spiFd >= 0 ) {                         // 如果SPI设备已打开
        (void)spi_linuxdev_close(spiFd);       // 关闭SPI设备
        spiFd = -1;                            // 重置文件描述符
    }
    last_xtime = 0;                            // 重置硬件时间戳
    rt_clrTimer(&rxpollTmr);                   // 清除接收轮询定时器
    rt_clrTimer(&syncTmr);                     // 清除时间同步定时器
}

#endif // defined(CFG_lgw2)
