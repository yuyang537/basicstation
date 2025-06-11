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

// 文件功能：Station-to-Server协议实现 - LoRaWAN基站与网络服务器通信的核心模块
// 作用：处理上行数据包转发、下行数据包调度、区域参数配置、占空比控制等核心业务逻辑

#include <stdio.h>        // 标准输入输出函数
#include "s2conf.h"       // S2配置管理
#include "uj.h"           // JSON编解码器
#include "ral.h"          // 无线电抽象层接口
#include "s2e.h"          // S2E协议定义和数据结构
#include "kwcrc.h"        // 关键字CRC校验
#include "timesync.h"     // 时间同步功能

// 全局调试控制变量 - 用于开发和测试环境覆盖正常的监管限制
u1_t s2e_dcDisabled;      // 禁用占空比限制标志 - 测试/开发环境覆盖
u1_t s2e_ccaDisabled;     // 禁用载波侦听(LBT)标志 - 测试/开发环境覆盖  
u1_t s2e_dwellDisabled;   // 禁用驻留时间限制标志 - 测试/开发环境覆盖


// 外部内联函数声明 - 用于RPS(Radio Parameters Set)参数处理
extern inline int   rps_sf   (rps_t params);  // 提取扩频因子(Spreading Factor)
extern inline int   rps_bw   (rps_t params);  // 提取带宽(Bandwidth)
extern inline rps_t rps_make (int sf, int bw); // 构造RPS参数

// 前向声明 - 定时器回调函数
static void s2e_txtimeout (tmr_t* tmr);   // 发送超时处理函数
static void s2e_bcntimeout (tmr_t* tmr);  // 信标超时处理函数

// 函数功能：设置所有发送单元的占空比(Duty Cycle)时间戳
// 参数说明：s2ctx - S2E上下文指针
//          t - 要设置的时间戳值
// 返回值：无
// 调用时机：初始化或重置占空比控制时调用
static void setDC (s2ctx_t* s2ctx, ustime_t t) {
    for( u1_t u=0; u<MAX_TXUNITS; u++ ) {        // 遍历所有发送单元
        for( u1_t i=0; i<DC_NUM_BANDS; i++ )     // 遍历EU868频段的占空比控制
            s2ctx->txunits[u].dc_eu868bands[i] = t;  // 设置EU868频段占空比时间戳
        for( u1_t i=0; i<MAX_DNCHNLS; i++ )      // 遍历所有下行信道
            s2ctx->txunits[u].dc_perChnl[i] = t;    // 设置每信道占空比时间戳
    }
}

// 函数功能：重置占空比控制，设置当前时间为基准时间
// 参数说明：s2ctx - S2E上下文指针
//          dc_chnlRate - 信道占空比速率(倍数)
// 返回值：无
// 调用时机：区域配置更新或占空比参数变化时调用
static void resetDC (s2ctx_t* s2ctx, u2_t dc_chnlRate) {
    setDC(s2ctx, rt_getTime());               // 设置所有占空比时间戳为当前时间
    s2ctx->dc_chnlRate = dc_chnlRate;         // 更新信道占空比速率
}

// 函数功能：默认的发送许可检查函数(总是允许发送)
// 参数说明：s2ctx - S2E上下文指针
//          txjob - 发送任务指针
//          ccaDisabled - CCA(载波侦听)禁用标志指针
// 返回值：1表示允许发送，0表示不允许发送
// 调用时机：作为默认的canTx函数指针，在没有特定区域限制时使用
static int s2e_canTxOK (s2ctx_t* s2ctx, txjob_t* txjob, int* ccaDisabled) {
    return 1;  // 默认总是允许发送(无监管限制)
}


// 函数功能：初始化S2E(Station-to-Server)协议上下文
// 参数说明：s2ctx - S2E上下文指针，包含所有协议状态和配置
// 返回值：无
// 调用时机：基站启动时调用，在TC(Traffic Control)模块初始化阶段
void s2e_ini (s2ctx_t* s2ctx) {
    // 初始化JoinEUI过滤器(如果尚未分配内存)
    if( s2e_joineuiFilter == NULL )
        s2e_joineuiFilter = rt_mallocN(uL_t, 2*MAX_JOINEUI_RANGES+2);  // 分配JoinEUI范围过滤器内存，需要至少一个尾部0条目

    memset(s2ctx, 0, sizeof(*s2ctx));          // 清零整个上下文结构体
    txq_ini(&s2ctx->txq);                      // 初始化发送队列
    rxq_ini(&s2ctx->rxq);                      // 初始化接收队列

    s2ctx->canTx = s2e_canTxOK;                // 设置默认的发送许可检查函数
    for( u1_t i=0; i<DR_CNT; i++ )             // 初始化数据速率定义数组
        s2ctx->dr_defs[i] = RPS_ILLEGAL;       // 所有数据速率初始设为非法，等待区域配置
    setDC(s2ctx, USTIME_MIN);                  // 禁用占空比控制，直到有需要的区域配置

    // 初始化所有发送单元的定时器和状态
    for( int u=0; u < MAX_TXUNITS; u++ ) {
        rt_iniTimer(&s2ctx->txunits[u].timer, s2e_txtimeout);  // 初始化发送超时定时器
        s2ctx->txunits[u].timer.ctx = s2ctx;   // 设置定时器上下文为S2E上下文
        s2ctx->txunits[u].head = TXIDX_END;    // 初始化发送队列头为结束标记
    }
    rt_iniTimer(&s2ctx->bcntimer, s2e_bcntimeout);  // 初始化信标定时器
    s2ctx->bcntimer.ctx = s2ctx;               // 设置信标定时器上下文
}

// 函数功能：释放S2E协议上下文，清理所有资源
// 参数说明：s2ctx - S2E上下文指针
// 返回值：无
// 调用时机：基站关闭或重启时调用，清理协议状态
void s2e_free (s2ctx_t* s2ctx) {
    // 清理所有发送单元的定时器
    for( int u=0; u < MAX_TXUNITS; u++ )
        rt_clrTimer(&s2ctx->txunits[u].timer);  // 清除发送超时定时器
    rt_clrTimer(&s2ctx->bcntimer);             // 清除信标定时器
    memset(s2ctx, 0, sizeof(*s2ctx));         // 清零整个上下文结构体
    ts_iniTimesync();                          // 重新初始化时间同步
    ral_stop();                                // 停止无线电抽象层
}

// ================================================================================
//
// 接收(RX)部分 - 处理从无线电层接收的上行数据包
//
// ================================================================================
//
// 功能概述：从无线电层获取接收到的帧数据，填充接收队列(RXQ)，
// 然后尽可能多地将这些帧转发给网络服务器(LNS)
//

// 函数功能：获取接收队列中的下一个待处理接收任务
// 参数说明：s2ctx - S2E上下文指针
// 返回值：指向下一个接收任务的指针，如果队列为空则返回NULL
// 调用时机：需要处理接收队列中的数据包时调用
rxjob_t* s2e_nextRxjob (s2ctx_t* s2ctx) {
    return rxq_nextJob(&s2ctx->rxq);           // 从接收队列获取下一个任务
}


// 函数功能：将新接收的数据包添加到接收队列，并检测镜像帧
// 参数说明：s2ctx - S2E上下文指针
//          rxjob - 新接收的数据包任务指针
// 返回值：无
// 调用时机：无线电层接收到新的上行数据包时调用
void s2e_addRxjob (s2ctx_t* s2ctx, rxjob_t* rxjob) {
    // 将新接收的帧添加到接收队列
    // 检查镜像帧(在相邻频率上的反射信号)
    for( rxjob_t* p = &s2ctx->rxq.rxjobs[s2ctx->rxq.first]; p < rxjob; p++ ) {
        // 检查是否为重复帧：相同数据速率、长度和内容
        if( p->dr == rxjob->dr &&                                    // 数据速率相同
            p->len == rxjob->len &&                                  // 帧长度相同
            memcmp(&s2ctx->rxq.rxdata[p->off], &s2ctx->rxq.rxdata[rxjob->off], rxjob->len) == 0 ) {  // 帧内容相同
            // 检测到重复帧 - 丢弃镜像帧
            // 使用信号质量指标(8*SNR - RSSI)来判断哪个帧质量更好
            if( (8*rxjob->snr - rxjob->rssi) > (8*p->snr - p->rssi) ) {
                // 新帧质量更好，丢弃之前的帧p
                LOG(MOD_S2E|DEBUG, "Dropped mirror frame freq=%F snr=%5.1f rssi=%d (vs. freq=%F snr=%5.1f rssi=%d) - DR%d mic=%d (%d bytes)",
                    p->freq, p->snr/4.0, -p->rssi, rxjob->freq, rxjob->snr/4.0, -rxjob->rssi,
                    p->dr, (s4_t)rt_rlsbf4(&s2ctx->rxq.rxdata[p->off]+rxjob->len-4), p->len);

                rxq_commitJob(&s2ctx->rxq, rxjob);        // 提交新帧到队列
                rxjob = rxq_dropJob(&s2ctx->rxq, p);      // 从队列中丢弃旧帧
            } else {
                // 旧帧质量更好，丢弃新接收的帧(不提交到队列)
                LOG(MOD_S2E|DEBUG, "Dropped mirror frame freq=%F snr=%5.1f rssi=%d (vs. freq=%F snr=%5.1f rssi=%d) - DR%d mic=%d (%d bytes)",
                    rxjob-> freq, rxjob->snr/4.0, -rxjob->rssi, p->freq, p->snr/4.0, -p->rssi,
                    rxjob->dr, (s4_t)rt_rlsbf4(&s2ctx->rxq.rxdata[rxjob->off]+rxjob->len-4), rxjob->len);
            }
            return;  // 处理完重复帧，直接返回
        }
    }
    // 没有发现镜像帧，正常提交新帧到队列
    rxq_commitJob(&s2ctx->rxq, rxjob);
}

// 函数功能：刷新接收队列，将所有待处理的接收数据包转发给网络服务器
// 参数说明：s2ctx - S2E上下文指针
// 返回值：无
// 调用时机：定期调用或接收到新数据包后调用，将上行数据转发给LNS
void s2e_flushRxjobs (s2ctx_t* s2ctx) {
    while( s2ctx->rxq.first < s2ctx->rxq.next ) {  // 遍历接收队列中的所有待处理任务
        // 获取发送缓冲区 - 解析帧数据并检查过滤器
        ujbuf_t sendbuf = (*s2ctx->getSendbuf)(s2ctx, MIN_UPJSON_SIZE);  // 获取JSON编码缓冲区
        if( sendbuf.buf == NULL ) {            // 如果WebSocket没有可用空间
            // WebSocket缓冲区已满 - WS稍后会再次调用
            return;                            // 暂停处理，等待缓冲区可用
        }
        rxjob_t* j = &s2ctx->rxq.rxjobs[s2ctx->rxq.first++];  // 获取队列中的下一个接收任务
        dbuf_t lbuf = { .buf = NULL };         // 日志缓冲区初始化
        // 如果启用了详细日志，准备日志输出
        if( log_special(MOD_S2E|VERBOSE, &lbuf) )
            xprintf(&lbuf, "RX %F DR%d %R snr=%.1f rssi=%d xtime=0x%lX - ",
                    j->freq, j->dr, s2e_dr2rps(s2ctx, j->dr), j->snr/4.0, -j->rssi, j->xtime);

        uj_encOpen(&sendbuf, '{');             // 开始JSON对象编码
        // 解析LoRa帧并检查过滤器(JoinEUI过滤等)
        if( !s2e_parse_lora_frame(&sendbuf, &s2ctx->rxq.rxdata[j->off], j->len, lbuf.buf ? &lbuf : NULL) ) {
            // 帧未通过完整性检查或被过滤器阻止
            sendbuf.pos = 0;                   // 重置缓冲区位置
            continue;                          // 跳过此帧，处理下一个
        }
        if( lbuf.buf )                         // 如果有日志缓冲区
            log_specialFlush(lbuf.pos);        // 刷新日志输出
        
        // 计算参考时间(用于时间同步)
        double reftime = 0.0;
        if( s2ctx->muxtime ) {                 // 如果有多路复用时间基准
            reftime = s2ctx->muxtime +         // 基准时间加上
                ts_normalizeTimespanMCU(rt_getTime()-s2ctx->reftime) / 1e6;  // 标准化的时间差
        }
        
        // 编码上行信息JSON字段
        uj_encKVn(&sendbuf,
                  "RefTime",  'T', reftime,    // 参考时间戳
                  "DR",       'i', j->dr,      // 数据速率
                  "Freq",     'i', j->freq,    // 接收频率
                  "upinfo",   '{',             // 上行信息对象开始
                  /**/ "rctx",    'I', j->rctx,     // 接收上下文(天线信息)
                  /**/ "xtime",   'I', j->xtime,    // 扩展时间戳
                  /**/ "gpstime", 'I', ts_xtime2gpstime(j->xtime),  // GPS时间
                  /**/ "fts",     'i', j->fts,      // 精细时间戳
                  /**/ "rssi",    'i', -(s4_t)j->rssi,  // 接收信号强度(转为负值)
                  /**/ "snr",     'g', j->snr/4.0,  // 信噪比(除以4转为dB)
                  /**/ "rxtime",  'T', rt_getUTC()/1e6,  // 接收时间(UTC)
                  "}",                         // 上行信息对象结束
                  NULL);
        uj_encClose(&sendbuf, '}');            // 结束JSON对象编码
        
        // 检查JSON编码是否成功
        if( !xeos(&sendbuf) ) {                // 如果编码超出缓冲区大小
            LOG(MOD_S2E|ERROR, "JSON encoding exceeds available buffer space: %d", sendbuf.bufsize);
        } else {
            (*s2ctx->sendText)(s2ctx, &sendbuf);  // 通过WebSocket发送JSON数据到LNS
            assert(sendbuf.buf==NULL);         // 确认缓冲区已被发送函数接管
        }
    }
}



// ================================================================================
//
// 发送(TX)部分 - 处理下行数据包的调度和传输
//
// ================================================================================

// EU868频段占空比速率常量表 - 定义不同频段的占空比限制倍数
// 用于计算发送后的占空比阻塞时间：阻塞时间 = 发送时间 + 空中时间 * 速率倍数
static const u2_t DC_EU868BAND_RATE[] = {
    [DC_DECI ]=   10,    // 0.1%占空比频段：阻塞时间为空中时间的10倍
    [DC_CENTI]=  100,    // 1%占空比频段：阻塞时间为空中时间的100倍  
    [DC_MILLI]= 1000,    // 10%占空比频段：阻塞时间为空中时间的1000倍
};


// 函数功能：计算LoRa数据包的空中传输时间(Air Time)
// 参数说明：rps - 无线电参数集(包含扩频因子和带宽)
//          plen - 数据包长度(字节)
//          nocrc - 是否不包含CRC(1=不包含，0=包含)
//          preamble - 前导码长度(符号数，0表示使用默认值8)
// 返回值：空中传输时间(微秒)
// 调用时机：计算发送时间、占空比控制时调用
// 算法来源：改编自lmic.c的实现，符合LoRa物理层规范
static ustime_t _calcAirTime (rps_t rps, u1_t plen, u1_t nocrc, u2_t preamble) {
    if( preamble == 0 )                        // 如果前导码长度为0
        preamble = 8;                          // 使用默认前导码长度8个符号
    if( rps == RPS_ILLEGAL )                   // 如果无线电参数非法
        return 0;                              // 返回0时间
    
    // 算法实现改编自lmic.c并进行了适配
    u1_t bw = rps_bw(rps);                     // 提取带宽：0,1,2 = 125,250,500kHz
    u1_t sf = rps_sf(rps);                     // 提取扩频因子：0=FSK, 1..6 = SF7..12
    
    // FSK调制的空中时间计算
    if( sf == FSK ) {
        // FSK: (数据长度+前导码5+同步字3+长度字段1+CRC2) * 8位/字节 * 1秒 / 50kbps
        return (plen+/*preamble*/5+/*syncword*/3+/*len*/1+/*crc*/2) * /*bits/byte*/8
            * rt_seconds(1) / /*kbit/s*/50000;
    }
    
    // LoRa调制的空中时间计算
    sf = 7 + (sf - SF7)*(SF8-SF7);             // 将枚举SF7..SF12映射为数值7..12
    u1_t sfx = 4*sf;                           // SF的4倍，用于后续计算
    u1_t q = sfx - (sf >= 11 && bw == 0 ? 8 : 0);  // 低数据速率优化调整
    u1_t ih = 0;                               // 基站从不使用隐式头部模式
    u1_t cr = 0;                               // 编码率：CR_4_5=0, CR_4_6=1, CR_4_7=2, CR_4_8=3
    
    // 计算有效载荷符号数
    int tmp = 8*plen - sfx + 28 + (nocrc?0:16) - (ih?20:0);  // 有效载荷位数计算
    if( tmp > 0 ) {                            // 如果有效载荷位数大于0
        tmp = (tmp + q - 1) / q;               // 向上取整除法：计算符号数
        tmp *= cr+5;                           // 应用编码率(4/5编码)
        tmp += 8;                              // 加上头部符号数
    } else {                                   // 如果有效载荷位数小于等于0
        tmp = 8;                               // 最少8个符号(仅头部)
    }
    
    // 加上前导码符号数：前导码时间 = 4.25个符号 + 用户指定的前导码长度
    tmp = (tmp<<2) + /*preamble: 4*4.25*/ 17 + /*preamble*/(4*preamble);
    
    // 带宽到频率的映射：
    // bw = 125000 = 15625 * 2^3
    //      250000 = 15625 * 2^4
    //      500000 = 15625 * 2^5
    // sf = 7..12
    //
    // 时间计算公式：osticks = tmp * OSTICKS_PER_SEC * 1<<sf / bw
    //
    // 优化：3 => 减少除数 125000/8 => 15625
    //      2 => tmp左移2位的计数器
    sfx = sf - (3+2) - bw;                     // 计算最终的移位量
    int div = 15625;                           // 基础除数
    if( sfx > 4 ) {                            // 如果移位量过大
        // 防止最后一步中32位有符号整数溢出
        div >>= sfx-4;                         // 减少除数
        sfx = 4;                               // 限制移位量
    }
    // 最终时间计算：(符号数 << 移位量) * 1秒 / 除数，加上舍入
    return (((ustime_t)tmp << sfx) * rt_seconds(1) + div/2) / div;
}

// 函数功能：计算下行数据包的空中传输时间
// 参数说明：rps - 无线电参数集
//          plen - 数据包长度
//          addcrc - 是否添加CRC(1=添加，0=不添加)
//          preamble - 前导码长度
// 返回值：空中传输时间(微秒)
// 调用时机：调度下行数据包时计算传输时间
ustime_t s2e_calcDnAirTime (rps_t rps, u1_t plen, u1_t addcrc, u2_t preamble) {
    return _calcAirTime(rps, plen, !addcrc, preamble);  // 注意：addcrc需要取反传给nocrc参数
}

// 函数功能：计算上行数据包的空中传输时间
// 参数说明：rps - 无线电参数集
//          plen - 数据包长度
// 返回值：空中传输时间(微秒)
// 调用时机：处理接收到的上行数据包时计算其传输时间
ustime_t s2e_calcUpAirTime (rps_t rps, u1_t plen) {
    return _calcAirTime(rps, plen, 0, 8);          // 上行包：包含CRC，使用8符号前导码
}

// 函数功能：发送下行传输确认消息给网络服务器
// 参数说明：s2ctx - S2E上下文指针
//          txjob - 已完成发送的任务指针
// 返回值：无
// 调用时机：下行数据包发送完成后调用，向LNS确认传输状态
static void send_dntxed (s2ctx_t* s2ctx, txjob_t* txjob) {
    if( txjob->deveui ) {                      // 只有包含设备EUI的任务才发送确认
        // 注意：dnsched类型的任务没有设置deveui字段，不报告dntxed
        ujbuf_t sendbuf = (*s2ctx->getSendbuf)(s2ctx, MIN_UPJSON_SIZE/2);  // 获取发送缓冲区
        if( sendbuf.buf == NULL ) {            // 如果缓冲区不可用
            LOG(MOD_S2E|ERROR, "%J - failed to send dntxed, no buffer space", txjob);
            return;                            // 无法发送确认消息
        }
        uj_encOpen(&sendbuf, '{');             // 开始JSON对象编码
        // 编码下行传输确认消息的各个字段
        uj_encKVn(&sendbuf,
                  "msgtype",   's', "dntxed",  // 消息类型：下行传输确认
                  "seqno",     'I', txjob->diid,    // 序列号(兼容旧服务器)
                  "diid",      'I', txjob->diid,    // 下行指令ID(新服务器)
                  "DR",        'i', txjob->dr,      // 数据速率
                  "Freq",      'u', txjob->freq,    // 发送频率
                  rt_deveui,   'E', txjob->deveui,  // 目标设备EUI
                  "rctx",      'i', txjob->txunit,  // 发送天线编号
                  "xtime",     'I', txjob->xtime,   // 扩展时间戳
                  "txtime",    'T', txjob->txtime/1e6,  // 发送时间(秒)
                  "gpstime",   'I', txjob->gpstime, // GPS时间
                  NULL);
        uj_encClose(&sendbuf, '}');            // 结束JSON对象编码
        (*s2ctx->sendText)(s2ctx, &sendbuf);   // 通过WebSocket发送确认消息
    }
    // 记录发送完成的详细日志信息
    LOG(MOD_S2E|INFO, "TX %J - %s: %F %.1fdBm ant#%d(%d) DR%d %R frame=%12.4H (%u bytes)",
        txjob, txjob->deveui ? "dntxed" : "on air",  // 区分确认消息和实际发送
        txjob->freq, (double)txjob->txpow/TXPOW_SCALE,  // 频率和功率
        txjob->txunit, ral_rctx2txunit(txjob->rctx),    // 发送/接收天线编号
        txjob->dr, s2e_dr2rps(s2ctx, txjob->dr),        // 数据速率和RPS参数
        txjob->len, &s2ctx->txq.txdata[txjob->off], txjob->len);  // 帧数据和长度
}


// 函数功能：更新多路复用时间基准，用于时间同步
// 参数说明：s2ctx - S2E上下文指针
//          muxstime - 多路复用服务器时间(秒)
//          now - 当前本地时间(微秒，0表示使用当前时间)
// 返回值：使用的本地时间戳(微秒)
// 调用时机：接收到时间同步消息时调用，建立本地时间与服务器时间的对应关系
ustime_t s2e_updateMuxtime(s2ctx_t* s2ctx, double muxstime, ustime_t now) {
    if( now == 0 )                             // 如果没有指定本地时间
        now = rt_getTime();                    // 获取当前本地时间
    s2ctx->muxtime = muxstime;                 // 保存多路复用服务器时间基准
    s2ctx->reftime = now;                      // 保存对应的本地时间基准
    return now;                                // 返回使用的本地时间戳
}

// 函数功能：将数据速率(DR)转换为无线电参数集(RPS)
// 参数说明：s2ctx - S2E上下文指针
//          dr - 数据速率编号(0-15)
// 返回值：对应的RPS参数，如果DR无效则返回RPS_ILLEGAL
// 调用时机：需要获取特定数据速率的无线电参数时调用
rps_t s2e_dr2rps (s2ctx_t* s2ctx, u1_t dr) {
    return dr < 16 ? s2ctx->dr_defs[dr] : RPS_ILLEGAL;  // 检查DR范围并返回对应的RPS定义
}

// 函数功能：将无线电参数集(RPS)转换为数据速率(DR)
// 参数说明：s2ctx - S2E上下文指针
//          rps - 无线电参数集
// 返回值：对应的数据速率编号，如果RPS无效则返回DR_ILLEGAL
// 调用时机：仅用于接收帧处理，将接收到的RPS映射为正确的上行数据速率
// 注意：此函数只映射到正确的上行数据速率
u1_t s2e_rps2dr (s2ctx_t* s2ctx, rps_t rps) {
    for( u1_t dr=0; dr<DR_CNT; dr++ ) {        // 遍历所有数据速率定义
        if( s2ctx->dr_defs[dr] == rps )        // 如果找到匹配的RPS
            return dr;                         // 返回对应的数据速率编号
    }
    return DR_ILLEGAL;                         // 没有找到匹配的数据速率
}


// 函数功能：检查下行频率的有效性并分配信道索引
// 参数说明：s2ctx - S2E上下文指针
//          ujd - JSON解码器指针
//          pfreq - 输出频率值的指针
//          pchnl - 输出信道索引的指针
// 返回值：无(通过指针参数返回结果)
// 调用时机：解析下行消息中的频率字段时调用
static void check_dnfreq (s2ctx_t* s2ctx, ujdec_t* ujd, u4_t* pfreq, u1_t* pchnl) {
    sL_t freq = uj_int(ujd);                   // 从JSON中解析频率值
    // 检查频率是否在允许的范围内
    if( freq < s2ctx->min_freq || freq > s2ctx->max_freq )
        uj_error(ujd, "Illegal frequency value: %ld - not in range %d..%d", freq, s2ctx->min_freq, s2ctx->max_freq);
    *pfreq = freq;                             // 设置输出频率值
    
    // 查找并分配下行信道索引给此频率
    // 此信道索引仅用于本地跟踪占空比控制
    int ch;
    for( ch=0; ch<MAX_DNCHNLS; ch++ ) {        // 遍历所有下行信道槽位
        if( s2ctx->dn_chnls[ch] == 0 )         // 如果找到空槽位
            break;                             // 跳出循环使用此槽位
        if( freq == s2ctx->dn_chnls[ch] ) {    // 如果频率已存在
            *pchnl = ch;                       // 返回现有的信道索引
            return;                            // 直接返回
        }
    }
    
    // 检测到新的下行频率
    if( ch == MAX_DNCHNLS ) {                  // 如果所有槽位都已占用
        // 永远不占用最后一个槽位 - 便于在溢出情况下优雅运行
        // 所有超出的信道的空中时间都记录到最后一个槽位中
        LOG(MOD_S2E|WARNING, "Out of space for DN channel frequencies");
    } else {                                   // 如果有可用槽位
        s2ctx->dn_chnls[ch] = freq;            // 在新槽位中记录频率
    }
    *pchnl = ch;                               // 返回分配的信道索引(可能是最后一个槽位)
}

// 函数功能：检查数据速率的有效性
// 参数说明：s2ctx - S2E上下文指针
//          ujd - JSON解码器指针
//          pdr - 输出数据速率的指针
// 返回值：无(通过指针参数返回结果，失败时抛出JSON错误)
// 调用时机：解析下行消息中的数据速率字段时调用
static void check_dr (s2ctx_t* s2ctx, ujdec_t* ujd, u1_t* pdr) {
    sL_t dr = uj_int(ujd);                     // 从JSON中解析数据速率值
    // 检查数据速率是否有效：范围检查和RPS定义检查
    if( dr < 0 || dr >= DR_CNT || s2ctx->dr_defs[dr] == RPS_ILLEGAL )
        uj_error(ujd, "Illegal datarate value: %d for region %s", dr, s2ctx->region_s);
    *pdr = dr;                                 // 设置输出数据速率值
}

// 函数功能：将频率映射到EU868频段的占空比类别
// 参数说明：freq - 频率值(Hz)
// 返回值：占空比频段类别(DC_DECI/DC_CENTI/DC_MILLI)
// 调用时机：EU868区域的占空比控制计算时调用
// 频段划分：869.4-869.65MHz(0.1%), 868.0-868.6MHz和869.7-870.0MHz(1%), 其他(10%)
static int freq2band (u4_t freq) {
    if( freq >= 869400000 && freq <= 869650000 )          // 869.4-869.65MHz频段
        return DC_DECI;                        // 0.1%占空比频段(最严格)
    if( (freq >= 868000000 && freq <= 868600000) || (freq >= 869700000 && freq <= 870000000) )  // 868.0-868.6MHz和869.7-870.0MHz频段
        return DC_CENTI;                       // 1%占空比频段(中等限制)
    return DC_MILLI;                           // 10%占空比频段(最宽松，默认)
}

// 函数功能：更新占空比(Duty Cycle)状态，计算发送后的阻塞时间
// 参数说明：s2ctx - S2E上下文指针
//          txj - 已完成发送的任务指针
// 返回值：无
// 调用时机：数据包发送完成后调用，更新相应频段和信道的占空比状态
static void update_DC (s2ctx_t* s2ctx, txjob_t* txj) {
    // EU868区域的频段占空比更新
    if( s2ctx->region == J_EU868 ) {           // 如果是EU868区域
        u1_t band = freq2band(txj->freq);      // 根据频率确定频段类别
        ustime_t* dcbands = s2ctx->txunits[txj->txunit].dc_eu868bands;  // 获取频段占空比数组
        ustime_t t = dcbands[band];            // 获取当前频段的阻塞时间
        // 更新频段占空比，除非被禁用或永久阻塞
        if( t != USTIME_MIN && t != USTIME_MAX ) {  // 检查是否禁用(MIN)或永久阻塞(MAX)
            // 计算新的阻塞时间：发送时间 + 空中时间 × 频段倍数
            dcbands[band] = t = txj->txtime + txj->airtime * DC_EU868BAND_RATE[band];
            LOG(MOD_S2E|XDEBUG, "DC EU band %d blocked until %>.3T (txtime=%>.3T airtime=%~T)",
                DC_EU868BAND_RATE[band], rt_ustime2utc(t), rt_ustime2utc(txj->txtime), (ustime_t)txj->airtime);
        }
    }
    
    // 每信道占空比更新(适用于所有区域)
    int dnchnl = txj->dnchnl;                  // 获取下行信道索引
    ustime_t* dclist = s2ctx->txunits[txj->txunit].dc_perChnl;  // 获取每信道占空比数组
    ustime_t t = dclist[dnchnl];               // 获取当前信道的阻塞时间
    // 更新信道占空比，除非被禁用或永久阻塞
    if( t != USTIME_MIN && t != USTIME_MAX ) { // 检查是否禁用(MIN)或永久阻塞(MAX)
        // 计算新的阻塞时间：发送时间 + 空中时间 × 信道倍数
        dclist[dnchnl] = t = txj->txtime + txj->airtime * s2ctx->dc_chnlRate;
        LOG(MOD_S2E|XDEBUG, "DC dnchnl %d blocked until %>.3T (txtime=%>.3T airtime=%~T)",
            dnchnl, rt_ustime2utc(t), rt_ustime2utc(txj->txtime), (ustime_t)txj->airtime);
    }
}

// 函数功能：计算发送功率，根据频率范围选择合适的功率级别
// 参数说明：s2ctx - S2E上下文指针
//          txjob - 发送任务指针
// 返回值：计算得出的发送功率值
// 调用时机：准备发送数据包时调用，确定使用的发送功率
static s2_t calcTxpow (s2ctx_t* s2ctx, txjob_t* txjob) {
    s2_t txpow = s2ctx->txpow;                 // 使用默认发送功率
    // 首先检查上界 - 如果频率范围为0则对所有发送频率都为false
    if( txjob->freq <= s2ctx->txpow2_freq[1] && txjob->freq >= s2ctx->txpow2_freq[0] ) {
        txpow = s2ctx->txpow2;                 // 在特定频率范围内使用备选功率
    }
    // 如果有更复杂的监管要求，可以使用：switch( s2ctx->region ) { .. }
    return txpow;                              // 返回计算得出的发送功率
}

// 函数功能：更新发送任务的空中时间和发送功率
// 参数说明：s2ctx - S2E上下文指针
//          txjob - 发送任务指针
// 返回值：无
// 调用时机：发送任务参数变化时调用，重新计算相关参数
static void updateAirtimeTxpow (s2ctx_t* s2ctx, txjob_t* txjob) {
    // 重新计算空中时间：根据数据速率、长度、CRC和前导码设置
    txjob->airtime = s2e_calcDnAirTime(s2e_dr2rps(s2ctx, txjob->dr), txjob->len, txjob->addcrc, txjob->preamble);
    txjob->txpow = calcTxpow(s2ctx, txjob);    // 重新计算发送功率
}

// 函数功能：计算发送任务的优先级，考虑备选时间和天线的惩罚
// 参数说明：txjob - 发送任务指针
// 返回值：计算得出的优先级值(数值越大优先级越高)
// 调用时机：发送队列排序和调度决策时调用
static int calcPriority (txjob_t* txjob) {
    int prio = txjob->prio;                    // 从基础优先级开始
    // 如果有备选发送时间或Class C重试，降低优先级
    if( txjob->rx2freq || ((txjob->txflags & TXFLAG_CLSC) && txjob->retries < CLASS_C_BACKOFF_MAX) )
        prio -= PRIO_PENALTY_ALTTXTIME;        // 备选时间惩罚
    // 如果有备选天线选项，降低优先级
    if( txjob->altAnts )
        prio -= PRIO_PENALTY_ALTANTENNA;       // 备选天线惩罚
    return prio;                               // 返回调整后的优先级
}


// 函数功能：切换到备选(更晚的)发送时间，如果有可用的备选方案
// 参数说明：s2ctx - S2E上下文指针
//          txjob - 发送任务指针
//          earliest - 最早允许的发送时间
// 返回值：1表示找到备选时间，0表示没有备选方案
// 调用时机：当前发送时间不可用时调用，尝试寻找备选发送时机
// 注意：此函数也会在参数变化时更新空中时间和发送功率
static int altTxTime (s2ctx_t* s2ctx, txjob_t* txjob, ustime_t earliest) {
    // Class C设备处理：支持连续监听，可以灵活调整发送时间
    if( (txjob->txflags & TXFLAG_CLSC) ) {
      again:                                   // 重试标签，用于Class C的多次尝试
        if( txjob->rx2freq ) {                 // 如果有RX2备选频率
            // 从RX1切换到RX2 - 可以随时发送，因为我们向前移动
            // 这样不太可能与RX1时隙冲突
            txjob->txtime   = earliest - CLASS_C_BACKOFF_BY;  // 设置最早可能的时间
            txjob->xtime    = ts_ustime2xtime(txjob->txunit, txjob->txtime);  // 转换为扩展时间
            txjob->retries  = 0;               // 重置重试计数
            txjob->freq     = txjob->rx2freq;  // 切换到RX2频率
            txjob->dr       = txjob->rx2dr;    // 切换到RX2数据速率
            txjob->dnchnl   = txjob->dnchnl2;  // 切换到RX2信道索引
            txjob->rx2freq  = 0;               // 使RX2无效，避免重复使用
            updateAirtimeTxpow(s2ctx, txjob);  // 更新空中时间和发送功率
            if( txjob->xtime == 0 ) {          // 如果时间同步失败
                LOG(MOD_S2E|VERBOSE, "%J - class C dropped - no time sync to SX130X yet", txjob);
                return 0;                      // 无法发送，返回失败
            }
        }
        if( txjob->retries > CLASS_C_BACKOFF_MAX ) {  // 如果重试次数超过限制
            LOG(MOD_S2E|VERBOSE, "%J - class C out of TX tries (%d in %~T)",
                txjob, txjob->retries, txjob->retries*CLASS_C_BACKOFF_BY);
            return 0;                          // 没有更多备选发送机会
        }
        // 将发送时间向后推迟一个退避间隔并重新检查
        // 这里不需要高精度，因为Class C设备总是在监听
        txjob->retries += 1;                   // 增加重试计数
        txjob->xtime += CLASS_C_BACKOFF_BY;    // 推迟扩展时间
        txjob->txtime += CLASS_C_BACKOFF_BY;   // 推迟发送时间
        if( txjob->txtime < earliest )         // 如果仍然太早
            goto again;                        // 继续重试
        return 1;                              // 找到合适的发送时间
    }
    
    // Class B设备处理：ping时隙模式
    if( (txjob->txflags & TXFLAG_PING) ) {
        // Class B ping时隙 - 服务器当前只提供一个时间槽
        LOG(MOD_S2E|VERBOSE, "%J - class B ping has no alternate TX time", txjob);
        return 0;                              // 没有备选发送时间
    }
    
    // Class A设备处理：标准的RX1/RX2窗口
    if( txjob->rx2freq == 0 ) {                // 如果没有RX2备选频率
        LOG(MOD_S2E|VERBOSE, "%J - class A has no more alternate TX time", txjob);
        return 0;                              // 没有更多备选发送时间
    }
    // 切换到RX2窗口(RX1后1秒)
    txjob->freq     = txjob->rx2freq;          // 使用RX2频率
    txjob->dr       = txjob->rx2dr;            // 使用RX2数据速率
    txjob->dnchnl   = txjob->dnchnl2;          // 使用RX2信道索引
    txjob->txtime  += rt_seconds(1);           // 发送时间推迟1秒(RX2窗口)
    txjob->xtime   += rt_seconds(1);           // 扩展时间推迟1秒
    txjob->rx2freq  = 0;                       // 使RX2无效，避免重复使用
    updateAirtimeTxpow(s2ctx, txjob);          // 更新空中时间和发送功率
    if( txjob->txtime < earliest ) {           // 如果RX2时间仍然太晚
        LOG(MOD_S2E|VERBOSE, "%J - too late for RX2 by %~T", txjob, earliest - txjob->txtime);
        return 0;                              // 错过了RX2窗口
    }
    LOG(MOD_S2E|VERBOSE, "%J - trying RX2 %F DR%d", txjob, txjob->freq, txjob->dr);
    return 1;                                  // 成功切换到RX2
}

// 函数功能：检查EU868区域的频段占空比是否允许发送
// 参数说明：s2ctx - S2E上下文指针
//          txjob - 发送任务指针
//          ccaDisabled - CCA禁用标志指针(输出参数)
// 返回值：1表示可以发送，0表示被占空比阻塞
// 调用时机：EU868区域发送许可检查时调用
static int s2e_canTxEU868 (s2ctx_t* s2ctx, txjob_t* txjob, int* ccaDisabled) {
    ustime_t txtime = txjob->txtime;           // 获取计划发送时间
    // 根据频率获取对应频段的占空比阻塞时间
    ustime_t band_exp = s2ctx->txunits[txjob->txunit].dc_eu868bands[freq2band(txjob->freq)];
    if( txtime >= band_exp ) {                 // 如果发送时间晚于阻塞结束时间
        *ccaDisabled = 0;                      // 需要进行CCA检查
        return 1;                              // 频段占空比允许发送
    }
    // 频段占空比不允许发送
    LOG(MOD_S2E|VERBOSE, "%J %F - no DC in band: txtime=%>.3T free=%>.3T",
        txjob, txjob->freq, rt_ustime2utc(txtime), rt_ustime2utc(band_exp));
    return 0;                                  // 被频段占空比阻塞
}

// 函数功能：检查每信道占空比是否允许发送
// 参数说明：s2ctx - S2E上下文指针
//          txjob - 发送任务指针
//          ccaDisabled - CCA禁用标志指针(输出参数)
// 返回值：2表示需要CCA检查，1表示无需CCA，0表示被占空比阻塞
// 调用时机：所有区域的每信道占空比检查时调用
static int s2e_canTxPerChnlDC (s2ctx_t* s2ctx, txjob_t* txjob, int* ccaDisabled) {
    ustime_t txtime = txjob->txtime;           // 获取计划发送时间
    // 获取该信道的占空比阻塞结束时间
    ustime_t chfree = s2ctx->txunits[txjob->txunit].dc_perChnl[txjob->dnchnl];
    if( txtime >= chfree ) {                   // 如果发送时间晚于信道阻塞结束时间
        *ccaDisabled = 0;                      // 需要进行CCA检查
        return 2;                              // 信道占空比允许发送，需要CCA
    }
    // 信道占空比不允许发送
    LOG(MOD_S2E|VERBOSE, "%J %F - no DC in channel: txtime=%>.3T until=%>.3T",
        txjob, txjob->freq, rt_ustime2utc(txtime), rt_ustime2utc(chfree));
    return 0;

    ustime_t band_exp = s2ctx->txunits[txjob->txunit].dc_eu868bands[freq2band(txjob->freq)];
    if( txtime >= band_exp )
        return 1;   // clear channel analysis not required
    // No DC in band
    LOG(MOD_S2E|VERBOSE, "%J %F - no DC in band: txtime=%>.3T free=%>.3T",
        txjob, txjob->freq, rt_ustime2utc(txtime), rt_ustime2utc(band_exp));
    return 0;
}


// 函数功能：将发送任务添加到发送队列，按发送时间排序插入
// 参数说明：s2ctx - S2E上下文指针
//          txjob - 要添加的发送任务指针
//          relocate - 是否为重新调度的任务(1=重新调度，0=新任务)
//          now - 当前时间(微秒)
// 返回值：1表示成功添加，0表示添加失败(被丢弃)
// 调用时机：接收到下行消息需要发送时调用
// 
// 算法逻辑：
// 1. 对于新任务：检查时间限制，寻找备选发送时间
// 2. 对于每个天线：从首选天线开始，然后尝试备选天线
// 3. 检查占空比限制和与正在进行的发送的冲突
// 4. 按发送时间插入队列，如果成为队列头则重置处理定时器
//
int s2e_addTxjob (s2ctx_t* s2ctx, txjob_t* txjob, int relocate, ustime_t now) {
    ustime_t earliest = now + TX_AIM_GAP;      // 计算最早可发送时间(当前时间+目标间隔)
    u1_t txunit;                               // 发送单元/天线编号
    if( !relocate ) {                          // 如果是来自LNS的新任务(非重新调度)
        ustime_t txtime = txjob->txtime;       // 获取计划发送时间
        txunit = txjob->txunit = ral_rctx2txunit(txjob->rctx);  // 将接收上下文转换为发送单元
        txjob->altAnts = ral_altAntennas(txunit);  // 获取备选天线掩码
        updateAirtimeTxpow(s2ctx, txjob);      // 更新空中时间和发送功率

        // 检查发送时间是否过于超前
        if( txtime > now + TX_MAX_AHEAD ) {    // 如果发送时间超过最大提前量
            LOG(MOD_S2E|WARNING, "%J - Tx job too far ahead: %~T", txjob, txtime-now);
            return 0;                          // 丢弃过早的发送任务
        }

        // 检查发送时间是否过晚，如果是则尝试寻找备选时间
        if( txtime < earliest  &&  !altTxTime(s2ctx, txjob, earliest) )
            return 0;                          // 无法找到合适的发送时间，丢弃任务
        goto start;                            // 跳转到发送单元选择逻辑
    }
  check_alt: {                              // 备选天线检查标签
        u1_t alts = txjob->altAnts;            // 获取备选天线掩码
        if( alts==0 ) {                        // 如果没有更多备选天线
            // 尝试更晚的发送时间
            if( !altTxTime(s2ctx, txjob, earliest) ) {  // 寻找备选发送时间
                LOG(MOD_S2E|WARNING, "%J - unable to place frame", txjob);
                return 0;                      // 无法安排发送，丢弃任务
            }
            // 重置天线选项，使用新的发送时间重新尝试
            txunit = txjob->txunit = ral_rctx2txunit(txjob->rctx);  // 重新获取首选天线
            txjob->altAnts = ral_altAntennas(txunit);  // 重置备选天线掩码
        } else {
            // 尝试寻找备选天线
            txunit = 0;                        // 从天线0开始搜索
            while( (alts & (1<<txunit)) == 0 ) // 寻找掩码中下一个可用的天线
                txunit += 1;                   // 检查下一个天线
            txjob->txunit = txunit;            // 设置选中的天线
            txjob->altAnts &= ~(1<<txunit);    // 从备选掩码中移除已尝试的天线
        }
    }
  start: {                                  // 发送单元处理开始标签
        int ccaDisabled = 0;                   // CCA禁用标志
        // 检查占空比限制是否允许发送
        if( !s2e_dcDisabled && !(*s2ctx->canTx)(s2ctx, txjob, &ccaDisabled) )
            goto check_alt;                    // 占空比不允许，尝试备选天线
        
        ustime_t txtime = txjob->txtime;       // 获取发送时间
        txidx_t* pidx = &s2ctx->txunits[txunit].head;  // 获取发送队列头指针
        txidx_t  idx  = pidx[0];               // 获取队列头索引
        txjob_t* curr = txq_idx2job(&s2ctx->txq, idx);  // 获取队列头任务
        
        // 检查是否与正在进行的发送冲突
        if( curr && (curr->txflags & TXFLAG_TXING) && 
            txtime < curr->txtime + curr->airtime + TX_MIN_GAP ) {
            // 与当前正在发送的任务时间冲突
            LOG(MOD_S2E|DEBUG, "%J - frame colliding with ongoing TX on ant#%d", txjob, txunit);
            goto check_alt;                    // 尝试备选天线
        }
        
        // 按发送时间升序插入队列
        do {
            if( idx == TXIDX_END  ||  txtime < curr->txtime ) {  // 找到插入位置
                // 找到合适位置 - 插入或追加
                assert(txjob->next == TXIDX_NIL);  // 确保新任务的next指针为空
                txjob->next = idx;             // 设置新任务的下一个节点
                pidx[0] = txq_job2idx(&s2ctx->txq, txjob);  // 将新任务链接到队列
                
                if( pidx == &s2ctx->txunits[txunit].head ) {  // 如果新任务成为队列头
                    // 重新启动发送定时器以处理新的队列头
                    rt_yieldTo(&s2ctx->txunits[txunit].timer, s2e_txtimeout);
                }
                return 1;                      // 成功添加到队列
            }
            idx = (pidx = &curr->next)[0];     // 移动到下一个节点
            curr = txq_idx2job(&s2ctx->txq, idx);  // 获取下一个任务
        } while(1);                           // 持续寻找插入位置
    }
}


// 函数功能：分析发送队列并决定下一个发送动作，实现发送状态机
// 参数说明：s2ctx - S2E上下文指针
//          txunit - 发送单元/天线编号
// 返回值：下次应该调用此函数的时间(微秒)，USTIME_MAX表示队列为空
// 调用时机：定时器超时或队列变化时调用，重新评估发送动作
//
// 状态机逻辑：
// 1. 正在发送的任务会经历完整的发送状态直到出队
//    - 进入并通过发送状态：重新计算时间、检查信道、提交到无线电
//    - 检查帧是否正在发射(防止无线电故障、时钟翻转)
//    - 发送结束时考虑下一个任务
// 2. 如果队列头任务时间太远，等待直到可发送
//
// 返回值建议下次调用的时间
//
ustime_t s2e_nextTxAction (s2ctx_t* s2ctx, u1_t txunit) {
    ustime_t now = rt_getTime();               // 获取当前时间
    txidx_t *phead = &s2ctx->txunits[txunit].head;  // 获取发送队列头指针
 again:                                        // 重新处理队列头标签
    if( phead[0] == TXIDX_END )                // 如果队列为空
        return USTIME_MAX;                     // 返回最大时间值，表示无需调度
    
    txjob_t* curr = txq_idx2job(&s2ctx->txq, phead[0]);  // 获取队列头任务
    ustime_t txdelta = curr->txtime - now;     // 计算距离发送时间的时间差

    if( (curr->txflags & TXFLAG_TXING) ) {     // 如果任务正在发送状态
        // 队列头任务处于发送中状态
        ustime_t txend = curr->txtime + curr->airtime;  // 计算发送结束时间
        if( now >= txend ) {                   // 如果发送已完成
            // 发送完成 - 移除任务
            LOG(MOD_S2E|DEBUG, "Tx done diid=%ld", curr->diid);
            if( !(curr->txflags & TXFLAG_TXCHECKED) ) {  // 如果尚未检查
                update_DC(s2ctx, curr);        // 更新占空比状态
                curr->txflags |= TXFLAG_TXCHECKED;  // 标记为已检查
                send_dntxed(s2ctx, curr);      // 发送传输确认消息
            }
            txq_unqJob(&s2ctx->txq, phead);    // 从队列中移除任务
            txq_freeJob(&s2ctx->txq, curr);    // 释放任务内存
            goto again;                        // 处理下一个任务
        }
        // 帧仍在发送中 - 在发送结束时返回
        if( !(curr->txflags & TXFLAG_TXCHECKED) ) {  // 如果尚未检查发送状态
            if( txdelta > -TXCHECK_FUDGE )     // 如果还未到检查时间
                return curr->txtime + TXCHECK_FUDGE;  // 返回检查时间
            int txs = ral_txstatus(txunit);    // 获取无线电发送状态
            if( txs != TXSTATUS_EMITTING ) {   // 如果无线电未在发射
                // Something went wrong - should be emitting
                LOG(MOD_S2E|ERROR, "%J - radio is not emitting frame - abandoning TX, trying alternative", curr);
                ral_txabort(txunit);           // 中止无线电发送
                curr->txflags &= ~TXFLAG_TXING; // 清除发送中标志
                goto check_alt;                // 尝试备选方案
            }
            // 看起来正在发射中
            update_DC(s2ctx, curr);            // 更新占空比状态
            
            
            
            
            
            
            

            curr->txflags |= TXFLAG_TXCHECKED;  // 设置发送检查完成标志
            // 在这里发送dntxed而不是在@txend，给网络服务器更多时间更新/通知复用器(如入网)
            send_dntxed(s2ctx, curr);          // 发送下行传输确认消息
        }
        return txend;                          // 返回发送结束时间
    }
    if( txdelta < TX_MIN_GAP ) {               // 如果距离发送时间太近
        // 错过了发送开始时间 - 尝试备选方案或丢弃帧
        LOG(MOD_S2E|ERROR, "%J - missed TX time: txdelta=%~T min=%~T", curr, txdelta, TX_MIN_GAP);
      check_alt:                               // 备选方案检查标签
        txq_unqJob(&s2ctx->txq, phead);        // 从队列中移除当前任务
        if( !s2e_addTxjob(s2ctx, curr, /*relocate*/1, now) )  // 注意：可能改变队列头！(需要重新加载)
            txq_freeJob(&s2ctx->txq, curr);    // 如果无法重新放置，释放任务
        goto again;                            // 重新检查队列头
    }
    // 发送时间太远 队列头是可发送的 - 是时候给无线电提供数据了吗？
    if( txdelta > TX_AIM_GAP ) {               // 如果距离发送时间还太远
        LOG(MOD_S2E|DEBUG, "%J - next TX start ahead by %~T (%>.6T)",
            curr, txdelta, rt_ustime2utc(curr->txtime));
        return curr->txtime - TX_AIM_GAP;      // 返回提前准备时间
    }

    // 基于最新时间同步数据重新计算精确的xtime
    if( curr->gpstime ) {                      // 如果有GPS时间戳
        // 根据最新时间同步数据重新计算xtime
        curr->xtime = ts_gpstime2xtime(txunit, curr->gpstime);  // GPS时间转扩展时间
        curr->txtime = ts_xtime2ustime(curr->xtime);            // 扩展时间转本地时间
        txdelta = curr->txtime - now;          // 重新计算时间差
    }
    else if( ral_xtime2txunit(curr->xtime) != txunit ) {  // 如果扩展时间对应的发送单元不匹配
        curr->xtime = ts_xtime2xtime(curr->xtime, txunit);  // 转换到正确的发送单元
    }
    if( curr->xtime == 0 ) {                   // 如果时间转换失败
        LOG(MOD_S2E|ERROR, "%J - time sync problems - trying alternative", curr);
        goto check_alt;                       // 尝试备选方案
    }
    // 发送时间足够接近，可以做决定了
    // 检查信道访问权限
    int ccaDisabled = s2e_ccaDisabled;         // 获取CCA禁用状态
    if( !s2e_dcDisabled && !(*s2ctx->canTx)(s2ctx, curr, &ccaDisabled) )  // 检查占空比和发送权限
        goto check_alt;                       // 如果不能发送，尝试备选方案

    // 检查与后续帧的冲突并权衡优先级
    // 假设如果当前较早的发送任务没有被占空比阻塞，
    // 那么较晚的发送任务也不会被阻塞
    ustime_t txend = curr->txtime + curr->airtime;  // 计算当前任务结束时间
    txjob_t* other_txjob = curr;               // 初始化其他任务指针
    int prio = calcPriority(curr);             // 计算当前任务优先级
    do {
        other_txjob = txq_idx2job(&s2ctx->txq, other_txjob->next);  // 获取下一个任务
        if( other_txjob == NULL )              // 如果没有更多任务
            break;                             // 退出循环
        if( txend < other_txjob->txtime - TX_MIN_GAP )  // 如果没有时间重叠
            break;  // 无重叠，退出检查
        int oprio = calcPriority(other_txjob); // 计算其他任务优先级
        if( prio < oprio ) {                   // 如果当前任务优先级较低
            LOG(MOD_S2E|ERROR, "%J - Hindered by %J %~T later: prio %d<%d - trying alternative",
                curr, other_txjob, other_txjob->txtime - curr->txtime, prio, oprio);
            goto check_alt;                   // 被高优先级任务阻挡，尝试备选方案
        }
    } while(1);

    // 记录详细的发送信息用于调试
    LOG(MOD_S2E|VERBOSE, "%J - starting TX in %~T: %F %.1fdBm ant#%d(%d) DR%d %R frame=%12.4H (%u bytes)",
        curr, txdelta,                         // 任务信息和时间延迟
        curr->freq, (double)curr->txpow/TXPOW_SCALE,  // 频率和发送功率
        curr->txunit, ral_rctx2txunit(curr->rctx),     // 发送/接收天线编号
        curr->dr, s2e_dr2rps(s2ctx, curr->dr), // 数据速率和RPS参数
        curr->len, &s2ctx->txq.txdata[curr->off], curr->len);  // 帧数据和长度

    int txerr = ral_tx(curr, s2ctx, ccaDisabled);  // 调用无线电层开始发送
    if( txerr != RAL_TX_OK ) {                 // 如果发送启动失败
        if( txerr == RAL_TX_NOCA ) {           // 如果是信道繁忙
            LOG(MOD_S2E|ERROR, "%J - channel busy - trying alternative", curr);
        } else {                               // 其他无线电层错误
            LOG(MOD_S2E|ERROR, "%J - radio layer failed to TX - trying alternative", curr);
        }
        goto check_alt;                       // 尝试备选方案
    }
    curr->txflags |= TXFLAG_TXING;             // 设置发送中标志

    // 移除所有重叠的后续发送任务并寻找备选方案(天线/发送时间)
    // 如果没有备选方案则丢弃发送任务
    while(1) {
        txjob_t* next_txjob = txq_idx2job(&s2ctx->txq, curr->next);  // 获取下一个任务
        if( next_txjob == NULL || txend < next_txjob->txtime - TX_MIN_GAP )  // 如果没有下一个任务或无重叠
            break;  // 无下一个任务或无重叠，退出循环
        LOG(MOD_S2E|INFO, "%J - displaces %J due to %~T overlap", curr, next_txjob, next_txjob->txtime - TX_MIN_GAP - txend);
        txq_unqJob(&s2ctx->txq, &curr->next); // 从队列中移除重叠的任务
        if( !s2e_addTxjob(s2ctx, next_txjob, /*relocate*/1, now) )  // 注意：可能改变next指针！
            txq_freeJob(&s2ctx->txq, next_txjob);  // 如果无法重新放置，释放任务
    }
    return curr->txtime + TXCHECK_FUDGE;       // 返回发送检查时间(稍微延后)
}



// 函数功能：发送定时器超时回调函数
// 参数说明：tmr - 定时器指针，包含S2E上下文
// 返回值：无
// 调用时机：发送定时器到期时由运行时系统调用
static void s2e_txtimeout (tmr_t* tmr) {
    s2ctx_t* s2ctx = tmr->ctx;             // 获取S2E上下文
    // 通过指针偏移计算发送单元索引(从定时器地址反推发送单元)
    u1_t txunit = (s2txunit_t*)((u1_t*)tmr - offsetof(s2txunit_t, timer)) - s2ctx->txunits;
    ustime_t t = s2e_nextTxAction(s2ctx, txunit);  // 处理发送动作并获取下次调用时间
    if( t == USTIME_MAX )                  // 如果队列为空
        return;                            // 不需要设置定时器
    rt_setTimer(tmr, t);                   // 设置下次定时器到期时间
}


// 函数功能：信标定时器超时回调函数，处理LoRaWAN信标的周期性发送
// 参数说明：tmr - 定时器指针，包含S2E上下文
// 返回值：无
// 调用时机：信标发送间隔到期时由运行时系统调用
static void s2e_bcntimeout (tmr_t* tmr) {
    s2ctx_t* s2ctx = tmr->ctx;             // 获取S2E上下文
    ustime_t now = rt_getTime();           // 获取当前时间
    sL_t xtime = ts_ustime2xtime(0, now);  // 将本地时间转换为扩展时间
    sL_t gpstime = ts_xtime2gpstime(xtime); // 将扩展时间转换为GPS时间
    double lat, lon;                       // 纬度和经度变量
    int latlon_ok = sys_getLatLon(&lat, &lon);  // 获取GPS位置信息
    
    // 确定信标状态：需要时间和位置信息才能正常发送信标
    u1_t state = (gpstime ? BCNING_OK:BCNING_NOTIME) | (latlon_ok?BCNING_OK:BCNING_NOPOS);

    // 检查信标状态是否发生变化
    if( state != s2ctx->bcn.state ) {
        str_t msg = state==BCNING_OK
            ? "Beaconing resumed - recovered GPS data: %s %s"     // 信标恢复消息
            : "Beaconing suspend - missing GPS data: %s %s";     // 信标暂停消息
        u1_t change = state ^ s2ctx->bcn.state;  // 计算状态变化位掩码
        LOG(MOD_S2E|INFO, msg, (change&BCNING_NOTIME)?"time":"", (change&BCNING_NOPOS)?"position":"");
        s2ctx->bcn.state = state;              // 更新信标状态
    }
    if( state != BCNING_OK ) {                 // 如果缺少时间或位置信息
        // 没有PPS信号或尚未时间同步 - 稍后重试
        rt_setTimer(tmr, now + rt_seconds(10)); // 10秒后重试
        return;                                // 暂停信标处理
    }

    // 计算下一个信标发送时间，选择即将到来的128秒GPS时间倍数，至少1秒后
    ustime_t ahead = BEACON_INTVL - gpstime % BEACON_INTVL;  // 计算距下一个信标周期的时间
    sL_t gpstxtime = gpstime + ahead;          // 计算信标发送的GPS时间
    txjob_t* txjob = txq_reserveJob(&s2ctx->txq);  // 预留一个发送任务结构
    if( txjob == NULL ) {                      // 如果发送队列已满
        LOG(MOD_S2E|ERROR, "Out of TX jobs - cannot send beacon");
        goto nextbcn;                          // 跳转到下次信标调度
    }
    int ctrl = s2ctx->bcn.ctrl;                // 获取信标控制参数
    int bcn_len = s2ctx->bcn.layout[2];        // 获取信标帧长度
    u1_t* p = txq_reserveData(&s2ctx->txq, bcn_len);  // 预留信标数据缓冲区
    if( p == NULL ) {                          // 如果数据缓冲区不足
        LOG(MOD_S2E|ERROR, "Out of TX data space - cannot send beacon");
        goto nextbcn;                          // 跳转到下次信标调度
    }
    sL_t epoch = gpstxtime/BEACON_INTVL;       // 计算信标时期数(每128秒一个时期)
    txjob->gpstime = gpstxtime;                // 设置发送的GPS时间
    txjob->xtime   = ts_gpstime2xtime(0, txjob->gpstime);  // 转换为扩展时间格式
    txjob->txtime  = ts_xtime2ustime(txjob->xtime);        // 转换为本地时间格式
    txjob->freq    = s2ctx->bcn.freqs[epoch % (ctrl>>4)];  // 根据时期选择信标频率
    txjob->dr      = ctrl & 0xF;               // 提取数据速率(控制字低4位)
    txjob->addcrc  = false;                    // 信标不需要添加CRC(MAC层已包含)
    txjob->txflags = TXFLAG_BCN;               // 标记为信标发送类型
    txjob->prio    = PRIO_BEACON;              // 设置信标优先级
    txjob->len     = bcn_len;                  // 设置信标数据长度
    s2e_make_beacon(s2ctx->bcn.layout, epoch*128, 0, lat, lon, p);  // 生成信标帧内容

    txq_commitJob(&s2ctx->txq, txjob);         // 提交发送任务到队列
    if( !s2e_addTxjob(s2ctx, txjob, /*initial placement*/0, now) )  // 添加到发送调度
        txq_freeJob(&s2ctx->txq, txjob);       // 如果调度失败则释放任务

  nextbcn:
    // 休眠直到下一个信标提前800ms触发
    ahead += BEACON_INTVL - rt_millis(800);    // 计算到下次信标的时间间隔(提前800ms)
    rt_setTimer(tmr, now + ahead);             // 设置下次信标定时器
}

// 函数功能：检查指定数据速率范围内是否存在快速LoRa调制(250kHz或500kHz带宽)
// 参数说明：s2ctx - S2E上下文指针
//          minDR, maxDR - 数据速率范围
//          rpsp - 输出参数，返回找到的快速LoRa RPS
// 返回值：true表示存在快速LoRa，false表示不存在
// 调用时机：处理路由配置时检查是否支持快速LoRa数据速率
static bool hasFastLora(s2ctx_t* s2ctx, int minDR, int maxDR, rps_t* rpsp) {
    for( int dr=minDR; dr<=maxDR; dr++ ) {     // 遍历数据速率范围
        rps_t rps = s2e_dr2rps(s2ctx, dr);    // 获取数据速率对应的RPS
        if( rps_bw(rps) == BW250 || rps_bw(rps) == BW500 ) {  // 检查是否为快速带宽
            *rpsp = rps;                       // 返回找到的快速LoRa RPS
            return true;                       // 找到快速LoRa
        }
    }
    return false;                              // 未找到快速LoRa
}

// 函数功能：检查指定数据速率范围内是否存在FSK调制
// 参数说明：s2ctx - S2E上下文指针
//          minDR, maxDR - 数据速率范围
// 返回值：true表示存在FSK，false表示不存在
// 调用时机：处理路由配置时检查是否支持FSK调制
static bool hasFSK(s2ctx_t* s2ctx, int minDR, int maxDR) {
    for( int dr=minDR; dr<=maxDR; dr++ ) {     // 遍历数据速率范围
        if( s2e_dr2rps(s2ctx, dr) == RPS_FSK ) // 检查是否为FSK调制
            return true;                       // 找到FSK
    }
    return false;                              // 未找到FSK
}

// 函数功能：检查指定数据速率范围内是否存在125kHz带宽的LoRa调制
// 参数说明：s2ctx - S2E上下文指针
//          minDR, maxDR - 数据速率范围
//          min_rps, max_rps - 输出参数，返回找到的最小和最大RPS
// 返回值：true表示存在125kHz LoRa，false表示不存在
// 调用时机：处理路由配置时检查125kHz LoRa支持情况
static bool any125kHz(s2ctx_t* s2ctx, int minDR, int maxDR, rps_t* min_rps, rps_t* max_rps) {
    *min_rps = *max_rps = RPS_ILLEGAL;         // 初始化输出参数为非法值
    bool any125kHz = false;                    // 是否找到125kHz LoRa标志
    for( int dr=minDR; dr<=maxDR; dr++ ) {     // 遍历数据速率范围
        rps_t rps = s2e_dr2rps(s2ctx, dr);    // 获取数据速率对应的RPS
        if( rps != RPS_FSK && rps_bw(rps) == BW125 ) {  // 检查是否为125kHz LoRa
            any125kHz = true;                  // 找到125kHz LoRa
            *min_rps = rps;                    // 设置最小RPS(最低的数据速率)
            if( *max_rps == RPS_ILLEGAL ) *max_rps = rps;  // 设置最大RPS(第一次找到时)
        }
    }
    return any125kHz;                          // 返回是否找到125kHz LoRa
}

// 函数功能：向上行信道定义数组中插入一个信道配置
// 参数说明：upchs - 上行信道定义结构指针
//          idx - 插入位置索引
//          freq - 信道频率(Hz)
//          bw - 带宽类型
//          minSF, maxSF - 最小和最大扩频因子
// 返回值：无
// 调用时机：处理路由配置中的上行信道列表时调用
inline static void upch_insert (chdefl_t* upchs, uint idx, u4_t freq, u1_t bw, u1_t minSF, u2_t maxSF) {
    if( idx >= MAX_UPCHNLS ) return;           // 检查索引范围，防止越界
    upchs->freq[idx] = freq;                   // 设置信道频率
    upchs->rps[idx].bw = bw;                   // 设置信道带宽
    upchs->rps[idx].minSF = minSF;             // 设置最小扩频因子
    upchs->rps[idx].maxSF = maxSF;             // 设置最大扩频因子
}

// 函数功能：处理路由配置消息(router_config)，设置基站的区域参数和信道配置
// 参数说明：s2ctx - S2E上下文指针
//          D - JSON解码器指针
// 返回值：1表示配置成功，0表示配置失败
// 调用时机：收到来自LNS的router_config消息时调用，初始化基站区域设置
static int handle_router_config (s2ctx_t* s2ctx, ujdec_t* D) {
    char hwspec[MAX_HWSPEC_SIZE] = { 0 };      // 硬件规格字符串缓冲区
    ujbuf_t sx130xconf = { .buf=NULL };        // SX130x芯片配置缓冲区
    ujcrc_t field;                             // JSON字段类型
    u1_t ccaDisabled=0, dcDisabled=0, dwellDisabled=0;   // 字段不存在时的默认值
    s2_t max_eirp = 100 * TXPOW_SCALE;         // 特殊值 - 表示未请求设置
    int jlistlen = 0;                          // JoinEUI过滤列表长度
    chdefl_t upchs = {{0}};                    // 上行信道定义结构
    int chslots = 0;                           // 已配置的信道槽位数
    s2bcn_t bcn = { 0 };                       // 信标配置结构

    s2ctx->txpow = 14 * TXPOW_SCALE;           // 内置默认发送功率(14dBm)

    while( (field = uj_nextField(D)) ) {       // 遍历JSON配置字段
        switch(field) {
        case J_freq_range: {                   // 频率范围字段
            uj_enterArray(D);                  // 进入频率范围数组[min, max]
            s2ctx->min_freq = (uj_nextSlot(D), uj_uint(D));  // 最小频率(Hz)
            s2ctx->max_freq = (uj_nextSlot(D), uj_uint(D));  // 最大频率(Hz)
            uj_exitArray(D);                   // 退出频率范围数组
            break;
        }
        case J_DRs: {                          // 数据速率定义字段
            int dr = 0;                        // 数据速率索引
            uj_enterArray(D);                  // 进入数据速率数组
            while( uj_nextSlot(D) >= 0 ) {     // 遍历每个数据速率定义
                uj_enterArray(D);              // 进入单个DR定义[sf, bw, dnonly]
                int sfin   = (uj_nextSlot(D), uj_int(D));  // 扩频因子索引(0=FSK)
                int bwin   = (uj_nextSlot(D), uj_int(D));  // 带宽(125/250/500)
                int dnonly = (uj_nextSlot(D), uj_int(D));  // 是否仅下行
                uj_exitArray(D);               // 退出单个DR定义
                if( sfin < 0 ) {               // 如果扩频因子无效
                    s2ctx->dr_defs[dr] = RPS_ILLEGAL;  // 标记为非法数据速率
                } else {
                    // 目前我们不跟踪/检查dnonly标志
                    int bw = bwin==125 ? BW125 : bwin==250 ? BW250 : BW500;  // 带宽映射
                    int sf = 12-sfin;          // 扩频因子转换(SF7-12对应sfin 5-0)
                    rps_t rps = (sfin==0 ? FSK : rps_make(sf,bw)) | (dnonly ? RPS_DNONLY : 0);  // 构造RPS
                    s2ctx->dr_defs[dr] = rps;  // 存储数据速率定义
                }
                dr = min(DR_CNT-1, dr+1);      // 递增DR索引，不超过最大值
            }
            uj_exitArray(D);                   // 退出数据速率数组
            break;
        }
        case J_upchannels: {                   // 上行信道配置字段
            uj_enterArray(D);                  // 进入上行信道数组
            while( uj_nextSlot(D) >= 0 ) {     // 遍历每个上行信道
                if( chslots > MAX_UPCHNLS-1 ) { // 检查信道数量限制
                    uj_skipValue(D);           // 跳过超出限制的信道
                    continue;
                }
                uj_enterArray(D);              // 进入单个信道定义[freq, minDR, maxDR]
                u4_t freq = (uj_nextSlot(D), uj_int(D));  // 信道频率(Hz)
                int insert = chslots;          // 插入位置(保持频率排序)
                while( insert > 0 && upchs.freq[insert-1] > freq ) {  // 找到正确的插入位置
                    upch_insert(&upchs, insert, upchs.freq[insert-1],  // 向后移动较大频率的信道
                        BWNIL, upchs.rps[insert-1].minSF, upchs.rps[insert-1].maxSF);
                    insert--;                  // 向前查找插入位置
                }
                int minDR = (uj_nextSlot(D), uj_intRange(D, 0, 8-1)); // 目前所有上行信道DR必须在0-7范围内
                int maxDR = (uj_nextSlot(D), uj_intRange(D, 0, 8-1)); // 目前所有上行信道DR必须在0-7范围内
                upch_insert(&upchs, insert, freq, BWNIL, minDR, maxDR);  // 插入新信道
                uj_exitArray(D);               // 退出单个信道定义
                chslots++;                     // 递增已配置的信道数
            }
            uj_exitArray(D);                   // 退出上行信道数组
            break;
        }
        case J_NetID: {                       // 网络ID过滤器配置字段
            if( !uj_null(D) ) {               // 如果提供了NetID列表
                for( int i=0; i<4; i++ )      // 清零过滤器位图
                    s2e_netidFilter[i] = 0;
                uj_enterArray(D);              // 进入NetID数组
                while( uj_nextSlot(D) >= 0 ) { // 遍历每个NetID
                    u4_t netid = uj_uint(D);   // 获取NetID值
                    s2e_netidFilter[(netid >> 5) & 3] |= 1 << (netid & 0x1F);  // 设置对应位图位
                }
                uj_exitArray(D);               // 退出NetID数组
            } else {                           // 如果NetID为null
                for( int i=0; i<4; i++ )      // 允许所有NetID(全部位设置为1)
                    s2e_netidFilter[i] = 0xffFFffFF;
            }
            break;
        }
        case J_JoinEUI: {                      // JoinEUI字段(新版本名称)
            rt_joineui = "JoinEUI";            // 设置运行时字段名称
            rt_deveui  = "DevEUI";             // 设置运行时字段名称
            // FALL THRU                       // 继续执行下面的处理
        }
        case J_JoinEui: {                      // JoinEui字段(旧版本名称)
            for( int i=0; i<2*MAX_JOINEUI_RANGES; i++ )  // 清零JoinEUI过滤器数组
                s2e_joineuiFilter[i] = 0;
            if( !uj_null(D) ) {               // 如果提供了JoinEUI范围列表
                uj_enterArray(D);              // 进入JoinEUI范围数组
                int off;                       // 范围索引
                while( (off = uj_nextSlot(D)) >= 0 ) {  // 遍历每个范围
                    uj_enterArray(D);          // 进入单个范围[low, high]
                    if( off < MAX_JOINEUI_RANGES ) {  // 检查范围数量限制
                        s2e_joineuiFilter[2*off+0] = (uj_nextSlot(D), uj_int(D));  // 范围下限
                        s2e_joineuiFilter[2*off+1] = (uj_nextSlot(D), uj_int(D));  // 范围上限
                    } else {                   // 超出最大范围数量
                        LOG(MOD_S2E|ERROR, "Too many Join EUI filter ranges - max %d supported", MAX_JOINEUI_RANGES);
                    }
                    uj_exitArray(D);           // 退出单个范围
                }
                uj_exitArray(D);               // 退出JoinEUI范围数组
                jlistlen = min(off, MAX_JOINEUI_RANGES);  // 记录实际范围数量
                s2e_joineuiFilter[2*jlistlen] = 0; // 终止列表标记
                
            }
            break;
        }
        case J_region: {                       // 区域配置字段
            const char* region_s = uj_str(D);  // 获取区域字符串
            ujcrc_t region = D->str.crc;       // 获取区域CRC值用于快速比较
            switch( region ) {                 // 根据区域类型设置参数
            case J_EU863: {                   // 非标准的过时命名
                region = J_EU868;             // 转换为标准EU868
                region_s = "EU868";           // 更新区域字符串
                // FALL THRU                  // 继续执行EU868处理
            }
            case J_EU868: {                   // 欧洲868MHz频段(标准名称)
                s2ctx->canTx  = s2e_canTxEU868;  // 设置EU868占空比检查函数
                s2ctx->txpow  = 16 * TXPOW_SCALE;  // 默认发送功率16dBm
                s2ctx->txpow2 = 27 * TXPOW_SCALE;  // 高功率频段27dBm
                s2ctx->txpow2_freq[0] = 869400000; // 高功率频段起始频率
                s2ctx->txpow2_freq[1] = 869650000; // 高功率频段结束频率
                resetDC(s2ctx, 3600/100);  // 100s/1h累计开机时间在PSA下约2.78%
                break;
            }
            case J_IL915: {                   // 以色列915MHz频段
                s2ctx->txpow  = 14 * TXPOW_SCALE;  // 默认发送功率14dBm
                s2ctx->txpow2 = 20 * TXPOW_SCALE;  // 高功率频段20dBm
                s2ctx->txpow2_freq[0] = 916200000; // 高功率频段起始频率
                s2ctx->txpow2_freq[1] = 916400000; // 高功率频段结束频率
                resetDC(s2ctx, 100);      // 1%占空比限制
                break;
            }
            case J_KR920: {                   // 韩国920MHz频段
                s2ctx->ccaEnabled = 1;        // 启用载波侦听
                s2ctx->canTx = s2e_canTxPerChnlDC;  // 使用每信道占空比检查
                s2ctx->txpow = 23 * TXPOW_SCALE;   // 默认发送功率23dBm
                resetDC(s2ctx, 50);       // 2%占空比限制
                break;
            }
            case J_AS923JP: {                 // 非标准的过时命名(日本)
                region = J_AS923_1;          // 转换为标准AS923-1
                region_s = "AS923-1";        // 更新区域字符串
                // FALL THRU                 // 继续执行AS923-1处理
            }
            case J_AS923_1: {                 // 亚洲923MHz频段-1(标准名称)
                s2ctx->ccaEnabled = 1;        // 启用载波侦听
                s2ctx->canTx = s2e_canTxPerChnlDC;  // 使用每信道占空比检查
                s2ctx->txpow = 13 * TXPOW_SCALE;   // 默认发送功率13dBm
                resetDC(s2ctx, 10);       // 10%占空比限制
                break;
            }
            case J_US902: {                   // 非标准的过时命名(美国)
                region = J_US915;            // 转换为标准US915
                region_s = "US915";          // 更新区域字符串
                // FALL THRU                 // 继续执行US915处理
            }
            case J_US915: {                   // 美国915MHz频段(标准名称)
                s2ctx->txpow = 26 * TXPOW_SCALE;   // 默认发送功率26dBm
                break;                       // 无占空比限制
            }
            case J_AU915: {                   // 澳大利亚915MHz频段
                s2ctx->txpow = 30 * TXPOW_SCALE;   // 默认发送功率30dBm
                break;                       // 无占空比限制
            }
            default: {                        // 未识别的区域
                LOG(MOD_S2E|WARNING, "Unrecognized region: %s - ignored", region_s);
                s2ctx->txpow = 14 * TXPOW_SCALE;   // 使用默认功率
                region = 0;                  // 标记为未知区域
                break;
            }
            }
            snprintf(s2ctx->region_s, sizeof(s2ctx->region_s), "%s", region_s);  // 保存区域字符串
            s2ctx->region = region;            // 保存区域CRC值
            break;
        }
        case J_max_eirp: {                    // 请求特定的最大EIRP值 - 下面检查是否生效
            max_eirp = (s2_t)(uj_num(D) * TXPOW_SCALE);  // 转换为内部功率单位
            break;
        }
        case J_MuxTime: {                      // 多路复用时间字段
            s2e_updateMuxtime(s2ctx, uj_num(D), 0);  // 更新多路复用时间基准
            rt_utcOffset = s2ctx->muxtime*1e6 - s2ctx->reftime;  // 计算UTC偏移
            rt_utcOffset_ts = s2ctx->reftime;  // 记录偏移计算时间戳
            break;
        }
        case J_hwspec: {                       // 硬件规格字段
            str_t s = uj_str(D);              // 获取硬件规格字符串
            if( D->str.len > sizeof(hwspec)-1 )  // 检查字符串长度
                uj_error(D, "Hardware specifier is too long");
            strcpy(hwspec, s);                // 复制硬件规格字符串
            break;
        }
#if defined(CFG_prod)
        case J_nocca:                              // 禁用CCA(载波侦听)控制字段
        case J_nodc:                               // 禁用占空比控制字段
        case J_nodwell:                            // 禁用驻留时间控制字段
        case J_device_mode: {                      // 设备模式控制字段
            LOG(MOD_S2E|WARNING, "Feature not supported in production level code (router_config) - ignored: %s", D->field.name);
            uj_skipValue(D);                       // 生产版本中忽略这些调试字段
            break;
        }
#else // !defined(CFG_prod)
        case J_nocca: {                            // 禁用载波侦听(CCA)字段
            ccaDisabled = uj_bool(D) ? 2 : 1;      // 2=强制禁用, 1=不禁用
            break;
        }
        case J_nodc: {                             // 禁用占空比控制字段
            dcDisabled = uj_bool(D) ? 2 : 1;       // 2=强制禁用, 1=不禁用
            break;
        }
        case J_nodwell: {                          // 禁用驻留时间限制字段
            dwellDisabled = uj_bool(D) ? 2 : 1;    // 2=强制禁用, 1=不禁用
            break;
        }
        case J_device_mode: {                      // 设备模式切换字段
            sys_deviceMode = uj_bool(D) ? 1 : 0;   // 1=设备模式, 0=基站模式
            break;
        }
#endif // !defined(CFG_prod)
        case J_sx1301_conf:                        // SX1301芯片配置字段(旧版本)
        case J_SX1301_conf:                        // SX1301芯片配置字段(大写版本)
        case J_sx1302_conf:                        // SX1302芯片配置字段(新版本)
        case J_SX1302_conf:                        // SX1302芯片配置字段(大写版本)
        case J_radio_conf: {                       // 通用射频配置字段
            // 在RAL层处理这些芯片特定的配置
            sx130xconf = uj_skipValue(D);          // 保存配置内容供RAL层使用
            break;
        }
        case J_msgtype: {                          // 消息类型字段
            // 静默忽略的字段
            uj_skipValue(D);                       // 跳过已知的消息类型字段
            break;
        }
        case J_bcning: {                           // 信标配置字段
            if( uj_null(D) )                       // 如果信标配置为null
                break;                             // 跳过信标配置
            uj_enterObject(D);                     // 进入信标配置对象
            while( (field = uj_nextField(D)) ) {   // 遍历信标配置字段
                switch(field) {
                case J_DR: {                       // 信标数据速率字段
                    bcn.ctrl = (uj_uint(D) & 0xF) | (bcn.ctrl & 0xF0);  // 设置数据速率(低4位)
                    break;
                }
                case J_layout: {                   // 信标帧布局字段
                    uj_enterArray(D);              // 进入布局数组[网络数据,GPS数据,总长度]
                    bcn.layout[0] = (uj_nextSlot(D), uj_uint(D));  // 网络特定数据长度
                    bcn.layout[1] = (uj_nextSlot(D), uj_uint(D));  // GPS数据长度
                    bcn.layout[2] = (uj_nextSlot(D), uj_uint(D));  // 信标帧总长度
                    uj_exitArray(D);               // 退出布局数组
                    break;
                }
                case J_freqs: {                    // 信标频率列表字段
                    uj_enterArray(D);              // 进入频率数组
                    int off = 0;                   // 频率数组索引
                    while( uj_nextSlot(D) >= 0 ) { // 遍历每个频率值
                        if( off < SIZE_ARRAY(bcn.freqs) ) {  // 检查数组边界
                            bcn.freqs[off++] = uj_int(D);    // 存储频率值
                        } else {
                            LOG(MOD_S2E|ERROR, "Too many beacon frequencies: %d - max %d supported", off, SIZE_ARRAY(bcn.freqs));
                        }
                    }
                    uj_exitArray(D);               // 退出频率数组
                    bcn.ctrl = (bcn.ctrl & 0xF) | (off<<4);  // 设置频率数量(高4位)
                    break;
                }
                default: {
                    LOG(MOD_S2E|WARNING, "Unknown field in router_config.bcning - ignored: %s (0x%X)", D->field.name, D->field.crc);
                    uj_skipValue(D);               // 跳过未知的信标字段
                    break;
                }
                }
            }
            uj_exitObject(D);                      // 退出信标配置对象
            break;
        }
        default: {
            LOG(MOD_S2E|WARNING, "Unknown field in router_config - ignored: %s (0x%X)", D->field.name, D->field.crc);
            uj_skipValue(D);
            break;
        }
        }
    }
    if( !hwspec[0] ) {                         // 检查硬件规格字符串是否存在
        LOG(MOD_S2E|ERROR, "No 'hwspec' in 'router_config' message");
        return 0;                              // 缺少硬件规格，配置失败
    }
    if( sx130xconf.buf == NULL ) {             // 检查SX130x芯片配置是否存在
        LOG(MOD_S2E|ERROR, "No 'sx1301_conf' or 'sx1302_conf' in 'router_config' message");
        return 0;                              // 缺少芯片配置，配置失败
    }
    int chdefs = chslots;                      // 保存已定义的信道数量
    for( int chslot=0; chslot<chdefs && upchs.freq[chslot]; chslot++ ) {  // 遍历每个已配置的上行信道
        int minDR = upchs.rps[chslot].minSF;   // 获取该信道的最小数据速率
        int maxDR = upchs.rps[chslot].maxSF;   // 获取该信道的最大数据速率
        rps_t rps0 = RPS_ILLEGAL;              // 初始化RPS变量
        rps_t rps1 = RPS_ILLEGAL;              // 初始化RPS变量
        if( any125kHz(s2ctx, minDR, maxDR, &rps0, &rps1) ) {  // 检查是否支持125kHz LoRa
            upch_insert(&upchs, chslot,         // 插入125kHz LoRa信道定义
                upchs.freq[chslot], BW125, rps_sf(rps0), rps_sf(rps1));
        }
        rps0 = RPS_ILLEGAL;                    // 重置RPS变量
        if( hasFastLora(s2ctx, minDR, maxDR, &rps0) ) {  // 检查是否支持快速LoRa(250/500kHz)
            upch_insert(&upchs, upchs.rps[chslot].bw == BWNIL ? chslot : chslots++,  // 插入快速LoRa信道
                upchs.freq[chslot], rps_bw(rps0), rps_sf(rps0), rps_sf(rps0));
        }
        if( hasFSK(s2ctx, minDR, maxDR) ) {     // 检查是否支持FSK调制
            upch_insert(&upchs, upchs.rps[chslot].bw == BWNIL ? chslot : chslots++,  // 插入FSK信道
            upchs.freq[chslot], 0, FSK, FSK);   // FSK使用带宽0和特殊的SF值
        }
    }
    ts_iniTimesync();                          // 初始化时间同步模块
    if( !ral_config(hwspec,                    // 调用RAL层配置硬件
                    s2ctx->ccaEnabled ? s2ctx->region : 0,  // 如果启用CCA则传递区域码
                    sx130xconf.buf, sx130xconf.bufsize,     // SX130x芯片配置数据
                    &upchs) ) {                // 上行信道定义
        return 0;                              // RAL配置失败
    }
    // 如果服务器提供了设置，则覆盖本地设置
    if( ccaDisabled   ) s2e_ccaDisabled   = ccaDisabled   & 2;    // 应用CCA禁用设置
    if( dcDisabled    ) s2e_dcDisabled    = dcDisabled    & 2;    // 应用占空比禁用设置
    if( dwellDisabled ) s2e_dwellDisabled = dwellDisabled & 2;    // 应用驻留时间禁用设置
    if( max_eirp != 100*TXPOW_SCALE ) {        // 如果提供了最大EIRP设置
        if( s2ctx->region==0 || max_eirp < s2ctx->txpow ) {  // 检查功率限制条件
            // 如果未指定区域则使用max_eirp，无论值多少
            // 如果指定了区域(未知区域=14dBm)则只允许降低功率
            s2ctx->txpow = max_eirp;           // 设置主发送功率
        }
        if( max_eirp < s2ctx->txpow2 ) {       // 检查次要功率限制
            s2ctx->txpow2 = max_eirp;          // 设置次要发送功率
        }
    }
    LOG(MOD_S2E|INFO, "Configuring for region: %s%s -- %F..%F",  // 输出区域配置信息
        s2ctx->region_s, s2ctx->ccaEnabled ? " (CCA)":"", s2ctx->min_freq, s2ctx->max_freq);
    if( log_shallLog(MOD_S2E|INFO) ) {         // 如果启用了INFO级别日志
        for( int dr=0; dr<16; dr++ ) {         // 遍历所有数据速率定义
            int rps = s2ctx->dr_defs[dr];      // 获取数据速率对应的RPS
            if( rps == RPS_ILLEGAL ) {         // 如果数据速率未定义
                LOG(MOD_S2E|INFO, "  DR%-2d undefined", dr);
            } else {                           // 如果数据速率已定义
                LOG(MOD_S2E|INFO, "  DR%-2d %R %s", dr, rps, rps & RPS_DNONLY ? "(DN only)" : "");
            }
        }
        LOG(MOD_S2E|INFO,                      // 输出发送功率配置
            "  TX power: %.1f dBm EIRP",
            s2ctx->txpow/(double)TXPOW_SCALE);
        if( s2ctx->txpow2_freq[0] ) {          // 如果配置了频率特定功率
            LOG(MOD_S2E|INFO, "            %.1f dBm EIRP for %F..%F",
                s2ctx->txpow2/(double)TXPOW_SCALE, s2ctx->txpow2_freq[0], s2ctx->txpow2_freq[1]);
        }
        LOG(MOD_S2E|INFO, "  %s list: %d entries", rt_joineui, jlistlen);  // 输出JoinEUI列表信息
        LOG(MOD_S2E|INFO, "  NetID filter: %08X-%08X-%08X-%08X",           // 输出NetID过滤器状态
            s2e_netidFilter[3], s2e_netidFilter[2], s2e_netidFilter[1], s2e_netidFilter[0]);
        LOG(MOD_S2E|INFO, "  Dev/test settings: nocca=%d nodc=%d nodwell=%d",  // 输出调试设置状态
            (s2e_ccaDisabled!=0), (s2e_dcDisabled!=0), (s2e_dwellDisabled!=0));
    }
    if( (bcn.ctrl&0xF0) != 0 ) {               // 如果指定了至少一个信标频率
        // 至少指定了一个信标频率
        LOG(MOD_S2E|INFO, "Beaconing every %~T on %F(%d) @ DR%d (frame layout %d/%d/%d)",
            BEACON_INTVL, bcn.freqs[0],        // 信标间隔和第一个频率
            (bcn.ctrl>>4), bcn.ctrl & 0xF,     // 频率数量和数据速率
            bcn.layout[0], bcn.layout[1], bcn.layout[2]);  // 帧布局参数
        s2ctx->bcn = bcn;                      // 保存信标配置到上下文
        s2e_bcntimeout(&s2ctx->bcntimer);      // 启动信标定时器
    }
    return 1;                                  // 配置成功
}


// 函数功能：处理下行帧消息(dnframe) - 已过时的消息格式
// 参数说明：s2ctx - S2E上下文指针
//          D - JSON解码器指针
// 返回值：无
// 调用时机：收到来自LNS的dnframe消息时调用
// 注意：这是过时的消息格式，新服务器使用dnmsg格式，携带更多上下文信息
void handle_dnframe (s2ctx_t* s2ctx, ujdec_t* D) {
    ustime_t now = rt_getTime();               // 获取当前时间
    txjob_t* txjob = txq_reserveJob(&s2ctx->txq);  // 预留一个发送任务结构
    if( txjob == NULL ) {                      // 如果发送队列已满
        LOG(MOD_S2E|ERROR, "Out of TX jobs - dropping incoming message");
        return;                                // 丢弃消息
    }
    int flags = 0;                             // 字段解析状态标志
    ujcrc_t field;                             // JSON字段类型
    while( (field = uj_nextField(D)) ) {       // 遍历JSON字段
        switch(field) {
        case J_msgtype: {                          // 消息类型字段
            uj_skipValue(D);                       // 跳过已知的消息类型
            break;
        }
        case J_DR: {                               // 数据速率字段
            check_dr(s2ctx, D, &txjob->dr);        // 验证并设置数据速率
            flags |= 0x01;                         // 标记DR字段已解析
            break;
        }
        case J_Freq: {                             // 频率字段
            check_dnfreq(s2ctx, D, &txjob->freq, &txjob->dnchnl);  // 验证并设置频率
            flags |= 0x02;                         // 标记频率字段已解析
            break;
        }
        case J_DevEUI:                             // 设备EUI字段(标准名称)
        case J_DevEui: {                           // 设备EUI字段(旧版本名称)
            txjob->deveui = uj_eui(D);             // 解析设备EUI
            flags |= 0x04;                         // 标记DevEUI字段已解析
            break;
        }
        case J_xtime: {                            // 扩展时间字段
            txjob->xtime = uj_int(D);              // 设置发送时间(xtime格式)
            flags |= 0x08;                         // 标记xtime字段已解析
            break;
        }
        case J_asap: {                             // 尽快发送标志字段
            if( uj_bool(D) )                       // 如果设置为true
                txjob->txflags |= TXFLAG_CLSC;     // 设置Class C标志(立即发送)
            break;
        }
        case J_seqno:   // 旧服务器使用的字段名(如果过时可删除)
        case J_diid: {  // 新服务器使用的字段名
            txjob->diid = uj_int(D);               // 下行消息ID
            flags |= 0x10;                         // 标记diid字段已解析
            break;
        }
        case J_MuxTime: {                          // 多路复用时间字段
            s2e_updateMuxtime(s2ctx, uj_num(D), now);  // 更新时间基准
            break;
        }
        case J_pdu: {                              // PDU数据字段
            uj_str(D);                             // 解析十六进制字符串
            int xlen = D->str.len/2;               // 计算二进制长度
            u1_t* p = txq_reserveData(&s2ctx->txq, xlen);  // 预留数据缓冲区
            if( p == NULL ) {                      // 如果缓冲区不足
                uj_error(D, "Out of TX data space");
                return;                            // 返回错误
            }
            txjob->len = uj_hexstr(D, p, xlen);    // 转换十六进制为二进制
            flags |= 0x20;                         // 标记PDU字段已解析
            break;
        }
        case J_rctx: {                             // 射频上下文字段
            txjob->rctx = uj_int(D);               // 设置射频上下文
            flags |= 0x40;                         // 标记rctx字段已解析
            break;
        }
        default: {
            LOG(MOD_S2E|WARNING, "Unknown field in dnframe - ignored: %s", D->field.name);
            uj_skipValue(D);                       // 跳过未知字段
            break;
        }
        }
    }
    if( (flags & 0x40) == 0 ) {                    // 如果没有提供rctx字段
        txjob->rctx = ral_xtime2rctx(txjob->xtime);  // 从xtime推导射频上下文
        flags |= 0x40;                             // 标记rctx字段已设置
    }
    if( flags != 0x7F ) {                          // 检查所有必需字段是否齐全(0x7F = 0x01|0x02|0x04|0x08|0x10|0x20|0x40)
        LOG(MOD_S2E|WARNING, "Some mandatory fields are missing (flags=0x%X)", flags);
        return;                                    // 缺少必需字段，丢弃消息
    }
    txjob->txtime = ts_xtime2ustime(txjob->xtime); // 转换扩展时间为本地时间
    if( txjob->xtime == 0 || txjob->txtime == 0 ) {  // 检查时间转换是否有效
        LOG(MOD_S2E|ERROR, "%J - dropped due to time conversion problems (MCU/GPS out of sync, obsolete input) - xtime=%ld", txjob, txjob->xtime);
        return;  // 非法/过时的xtime，丢弃消息
    }
    txq_commitJob(&s2ctx->txq, txjob);             // 提交发送任务到队列
    if( !s2e_addTxjob(s2ctx, txjob, /*initial placement*/0, now) )  // 尝试添加到发送调度
        txq_freeJob(&s2ctx->txq, txjob);           // 如果调度失败则释放任务
}


// 函数功能：处理下行消息(dnmsg) - 新的消息格式，携带更丰富的上下文信息
// 参数说明：s2ctx - S2E上下文指针
//          D - JSON解码器指针
// 返回值：无
// 调用时机：收到来自LNS的dnmsg消息时调用，这是推荐的新格式
void handle_dnmsg (s2ctx_t* s2ctx, ujdec_t* D) {
    ustime_t now = rt_getTime();               // 获取当前时间
    txjob_t* txjob = txq_reserveJob(&s2ctx->txq);  // 预留一个发送任务结构
    if( txjob == NULL ) {                      // 如果发送队列已满
        LOG(MOD_S2E|ERROR, "Out of TX jobs - dropping incoming message");
        return;                                // 丢弃消息
    }
    int flags = 0;                             // 字段解析状态标志
    ujcrc_t field;                             // JSON字段类型
    while( (field = uj_nextField(D)) ) {       // 遍历JSON字段
        switch(field) {
        case J_msgtype: {
            uj_skipValue(D);
            break;
        }
        case J_DevEUI:                             // 设备EUI字段(标准名称)
        case J_DevEui: {                           // 设备EUI字段(旧版本名称)
            txjob->deveui = uj_eui(D);             // 解析设备EUI
            flags |= 0x01;                         // 标记DevEUI字段已解析
            break;
        }
        case J_dC: {                               // 设备类别字段
            int txflags = 0, dc = uj_intRange(D, 0, 2);  // 解析设备类别(0-2)
            switch(dc) {
                case 0: txflags = TXFLAG_CLSA; break;  // Class A设备
                case 1: txflags = TXFLAG_PING; break;  // Class B设备(ping slot)
                case 2: txflags = TXFLAG_CLSC; break;  // Class C设备
            }
            txjob->txflags = txflags;              // 设置发送标志
            flags |= 0x02;                         // 标记dC字段已解析
            break;
        }
        case J_seqno:   // 旧服务器使用的字段名(如果过时可删除)
        case J_diid: {  // 新服务器使用的字段名
            txjob->diid = uj_int(D);               // 下行消息ID
            flags |= 0x04;                         // 标记diid字段已解析
            break;
        }
        case J_pdu: {                              // PDU数据字段
            uj_str(D);                             // 解析十六进制字符串
            int xlen = D->str.len/2;               // 计算二进制长度
            if( xlen > 255 ) {                     // 检查PDU长度限制
                uj_error(D, "TX pdu too large. Maximum is 255 bytes.");
                return;                            // PDU过大，返回错误
            }
            u1_t* p = txq_reserveData(&s2ctx->txq, xlen);  // 预留数据缓冲区
            if( p == NULL ) {                      // 如果缓冲区不足
                uj_error(D, "Out of TX data space");
                return;                            // 返回错误
            }
            txjob->len = uj_hexstr(D, p, xlen);    // 转换十六进制为二进制
            flags |= 0x08;                         // 标记PDU字段已解析
            break;
        }
        case J_RxDelay: {                          // 接收延迟字段
            // 将零映射为一(LoRaWAN规范要求RxDelay最小为1)
            txjob->rxdelay = max(1, uj_intRange(D, 0, 15));  // 设置接收延迟(1-15秒)
            flags |= 0x10;                         // 标记RxDelay字段已解析
            break;
        }
        case J_priority: {                         // 优先级字段
            txjob->prio = uj_intRange(D, 0, 255);  // 设置发送优先级(0-255)
            break;
        }
        case J_dnmode: {                           // 下行模式字段
            // 当前不需要用于决策
            //str_t mode = uj_str(D); // "updn" 或 "dn"
            //dnmode = (mode[0]=='d' && mode[1]=='n');
            uj_skipValue(D);                       // 跳过此字段
            break;
        }
        case J_xtime: {  // 扩展时间字段(0表示不存在)
            txjob->xtime = uj_int(D);              // 设置发送时间(xtime格式)
            break;
        }
        case J_DR: {                               // 数据速率字段(用于即时发送)
            txjob->rxdelay = 0;                    // 即时发送时延迟为0
            flags |= 0x10;  // 设置rxdelay标志 - RxDelay隐式为0
            // 继续执行下面的RX1DR处理
        }
        case J_RX1DR: {                            // RX1窗口数据速率字段
            check_dr(s2ctx, D, &txjob->dr);        // 验证并设置RX1数据速率
            flags |= 0x0100;                       // 标记RX1DR字段已解析
            break;
        }
        case J_Freq:                               // 频率字段(通用名称)
        case J_RX1Freq: {                          // RX1窗口频率字段
            check_dnfreq(s2ctx, D, &txjob->freq, &txjob->dnchnl);  // 验证并设置RX1频率
            flags |= 0x0200;                       // 标记RX1Freq字段已解析
            break;
        }
        case J_RX2DR: {                            // RX2窗口数据速率字段
            check_dr(s2ctx, D, &txjob->rx2dr);     // 验证并设置RX2数据速率
            flags |= 0x0400;                       // 标记RX2DR字段已解析
            break;
        }
        case J_RX2Freq: {                          // RX2窗口频率字段
            check_dnfreq(s2ctx, D, &txjob->rx2freq, &txjob->dnchnl2);  // 验证并设置RX2频率
            flags |= 0x0800;                       // 标记RX2Freq字段已解析
            break;
        }
        case J_MuxTime: {                          // 多路复用时间字段
            s2e_updateMuxtime(s2ctx, uj_num(D), now);  // 更新时间基准
            break;
        }
        case J_rctx: {                             // 射频上下文字段
            txjob->rctx = uj_int(D);               // 设置射频上下文
            flags |= 0x1000;                       // 标记rctx字段已解析
            break;
        }
        case J_gpstime: {  // GPS时间字段(微秒精度)
            txjob->gpstime = uj_uint(D);           // 设置GPS时间戳
            break;
        }
        case J_preamble: {                         // 前导码长度字段
            txjob->preamble = uj_uint(D);          // 设置前导码符号数
            break;
        }
        case J_addcrc: {                           // CRC添加标志字段
            txjob->addcrc = uj_uint(D);            // 设置是否添加CRC校验
            break;
        }
        default: {
            LOG(MOD_S2E|WARNING, "Unknown field in dnmsg - ignored: %s", D->field.name);
            uj_skipValue(D);                       // 跳过未知字段
            break;
        }
        }
    }
    if ( (flags & 0x10) != 0x10) {             // 如果RxDelay字段缺失
        // 将零映射为一(LoRaWAN规范要求)
        txjob->rxdelay = 1;                    // 设置默认接收延迟为1秒
        flags |= 0x10;                         // 标记RxDelay字段已设置
        LOG(MOD_S2E|WARNING, "RxDelay mapped to 1 as it was not present!");
    }
    if( (flags & 0x1F) != 0x1F ||              // 检查基本必需字段(DevEUI, dC, diid, pdu, RxDelay)
        // flags & 0x300 必须是 {0x000,0x300} - RX1DR/RX1Freq要么都存在要么都不存在
        ((1 << ((flags >> 8) & 3)) & ((1<<3)|(1<<0))) == 0 ||
        // flags & 0xC00 必须是 {0x000,0xC00} - RX2DR/RX2Freq要么都存在要么都不存在
        ((1 << ((flags >> 10) & 3)) & ((1<<3)|(1<<0))) == 0 ) {
        LOG(MOD_S2E|WARNING, "Some mandatory fields are missing (flags=0x%X)", flags);
        return;                                // 字段不完整，丢弃消息
    }
    if( (flags & 0x1000) == 0 && txjob->xtime ) {  // 如果没有rctx但有xtime
        // 没有射频上下文但有扩展时间 - 从xtime推导射频单元
        // 如果没有提供xtime字段，rctx默认为零
        txjob->rctx = ral_xtime2rctx(txjob->xtime);  // 从扩展时间提取射频上下文
    }
    txjob->txunit = ral_rctx2txunit(txjob->rctx);   // 从射频上下文获取发送单元索引

    if( (txjob->txflags & TXFLAG_PING) ) {     // 如果是Class B设备的ping slot发送
        txjob->xtime  = ts_gpstime2xtime(txjob->txunit, txjob->gpstime);  // GPS时间转扩展时间
        txjob->txtime = ts_xtime2ustime(txjob->xtime);                    // 扩展时间转本地时间
    }
    else {                                     // Class A或Class C设备发送
        if( txjob->xtime != 0 ) {              // 如果提供了基准扩展时间
            txjob->xtime += txjob->rxdelay * 1000000;  // 添加接收延迟(秒转微秒)
            txjob->txtime = ts_xtime2ustime(txjob->xtime);  // 转换为本地时间
        }
        if( txjob->freq == 0 ) {               // 如果没有RX1频率，切换到RX2
            // 切换到RX2窗口:
            //  Class A设备 - 没有提供RX1参数
            //  Class C自发下行 - 没有提供RX1参数
            if( txjob->rx2freq == 0 ) {        // 如果RX2频率也没有
                LOG(MOD_S2E|WARNING, "Ignoring 'dnmsg' with neither RX1/RX2 frequencies");
                return;                        // 既没有RX1也没有RX2，丢弃消息
            }
            if( !altTxTime(s2ctx, txjob, now+TX_AIM_GAP) ) {  // 尝试使用RX2备选时间
                LOG(MOD_S2E|WARNING, "Ignoring 'dnmsg' with no viable RX2");
                return;                        // RX2不可行，丢弃消息
            }
        }
    }
    if( txjob->xtime == 0 || txjob->txtime == 0 ) {  // 检查时间转换是否成功
        LOG(MOD_S2E|ERROR, "%J - dropped due to time conversion problems (MCU/GPS out of sync, obsolete input) - xtime=%ld", txjob, txjob->xtime);
        return;  // 时间转换失败，可能是MCU/GPS不同步或输入过时
    }
    txq_commitJob(&s2ctx->txq, txjob);         // 提交发送任务到队列
    if( !s2e_addTxjob(s2ctx, txjob, /*initial placement*/0, now) )  // 添加到发送调度
        txq_freeJob(&s2ctx->txq, txjob);       // 如果调度失败则释放任务
}


// 函数功能：处理下行调度消息(dnsched)，支持批量发送任务的精确时间调度
// 参数说明：s2ctx - S2E上下文指针
//          D - JSON解码器指针
// 返回值：无
// 调用时机：收到来自LNS的dnsched消息时调用，用于Class B/C设备的精确时间调度
void handle_dnsched (s2ctx_t* s2ctx, ujdec_t* D) {
    ustime_t now = rt_getTime();               // 获取当前时间作为参考
    ujcrc_t field;                             // JSON字段类型
    while( (field = uj_nextField(D)) ) {       // 遍历JSON字段
        switch(field) {
        case J_msgtype: {                      // 消息类型字段
            uj_skipValue(D);                   // 跳过已知的消息类型
            break;
        }
        case J_MuxTime: {                      // 多路复用时间字段
            s2e_updateMuxtime(s2ctx, uj_num(D), now);  // 更新时间基准
            break;
        }
        case J_schedule: {                     // 调度数组字段
            int slot;                          // 调度槽位索引
            uj_enterArray(D);                  // 进入调度数组
            while( (slot = uj_nextSlot(D)) >= 0 ) {  // 遍历每个调度槽位
                txjob_t* txjob = txq_reserveJob(&s2ctx->txq);  // 预留发送任务结构
                if( txjob == NULL ) {          // 如果发送队列已满
                    uj_error(D, "Out of TX jobs - stopping parsing of 'dnsched' message");
                    return;                    // 停止解析并返回
                }
                int flags = 0;                 // 必需字段标志位
                uj_enterObject(D);             // 进入调度对象
                while( (field = uj_nextField(D)) ) {   // 遍历调度对象的字段
                    switch(field) {
                    case J_diid: {  // 下行消息ID字段(新服务器使用此字段名)
                        txjob->diid = uj_int(D);    // 设置下行消息标识符
                        break;
                    }
                    case J_priority: {              // 优先级字段
                        txjob->prio = uj_intRange(D, 0, 255);  // 设置发送优先级(0-255)
                        break;
                    }
                    case J_DR: {                    // 数据速率字段
                        check_dr(s2ctx, D, &txjob->dr);  // 验证并设置数据速率
                        flags |= 0x01;              // 标记DR字段已解析
                        break;
                    }
                    case J_Freq: {                  // 频率字段
                        check_dnfreq(s2ctx, D, &txjob->freq, &txjob->dnchnl);  // 验证并设置频率
                        flags |= 0x02;              // 标记频率字段已解析
                        break;
                    }
                    case J_ontime: {   // GPS时间字段(秒精度) - 当前不支持小数
                        txjob->gpstime = rt_seconds(uj_uint(D));  // 秒转换为微秒
                        flags |= 0x04;              // 标记时间字段已解析
                        break;
                    }
                    case J_gpstime: {  // GPS时间字段(微秒精度)
                        txjob->gpstime = uj_uint(D);  // 直接设置GPS微秒时间
                        flags |= 0x04;              // 标记时间字段已解析
                        break;
                    }
                    case J_xtime: {    // 扩展时间发送字段
                        txjob->xtime = uj_uint(D);  // 设置扩展时间
                        flags |= 0x04;              // 标记时间字段已解析
                        break;
                    }
                    case J_pdu: {                   // PDU数据字段
                        uj_str(D);                  // 解析十六进制字符串
                        int xlen = D->str.len/2;    // 计算二进制长度
                        u1_t* p = txq_reserveData(&s2ctx->txq, xlen);  // 预留数据缓冲区
                        if( p == NULL ) {           // 如果缓冲区不足
                            uj_error(D, "Out of TX data space");
                            return;                 // 返回错误
                        }
                        txjob->len = uj_hexstr(D, p, xlen);  // 转换十六进制为二进制
                        flags |= 0x08;              // 标记PDU字段已解析
                        break;
                    }
                    case J_rctx: {                  // 射频上下文字段
                        txjob->rctx = uj_int(D);    // 设置射频上下文
                        break;
                    }
                    case J_preamble: {              // 前导码长度字段
                        txjob->preamble = uj_uint(D);  // 设置前导码符号数
                        break;
                    }
                    case J_addcrc: {                // CRC添加标志字段
                        txjob->addcrc = uj_uint(D); // 设置是否添加CRC校验
                        break;
                    }
                    default: {
                        LOG(MOD_S2E|WARNING, "Unknown field in dnsched.schedule[%d] - ignored: %s", slot, D->field.name);
                        uj_skipValue(D);            // 跳过未知字段
                        break;
                    }
                    }
                }
                if( flags != 0xF ) {                   // 检查必需字段(DR, Freq, 时间, PDU)
                    LOG(MOD_S2E|WARNING, "Some mandatory fields in dnsched.schedule[%d] are missing (flags=0x%X)", slot, flags);
                } else {                            // 所有必需字段都存在
                    u1_t txunit = ral_rctx2txunit(txjob->rctx);  // 从射频上下文获取发送单元
                    txjob->txunit = txunit;         // 设置发送单元索引
                    if( txjob->gpstime ) {          // 如果提供了GPS时间
                        txjob->xtime = ts_gpstime2xtime(txunit, txjob->gpstime);  // GPS时间转扩展时间
                        txjob->txtime = ts_xtime2ustime(txjob->xtime);             // 扩展时间转本地时间
                        txjob->txflags = TXFLAG_PING;  // 标记为Class B ping slot发送
                    } else {                        // 如果使用扩展时间
                        txjob->txtime = ts_xtime2ustime(txjob->xtime);  // 扩展时间转本地时间
                        txjob->txflags = TXFLAG_CLSA;  // 标记为Class A发送
                    }
                    if( txjob->txtime != 0 ) {      // 如果时间转换成功
                        LOG(MOD_S2E|INFO, "DNSCHED diid=%ld %>T %~T DR%-2d %F - %d bytes",
                            txjob->diid, rt_ustime2utc(txjob->txtime), txjob->txtime-now, txjob->dr, txjob->freq, txjob->len);
                        txq_commitJob(&s2ctx->txq, txjob);  // 提交发送任务到队列
                        if( !s2e_addTxjob(s2ctx, txjob, /*initial placement*/0, now) )  // 添加到发送调度
                            txq_freeJob(&s2ctx->txq, txjob);  // 如果调度失败则释放任务
                    } else {                        // 时间转换失败
                        LOG(MOD_S2E|ERROR, "DNSCHED failed to convert %stime: %ld",
                            txjob->gpstime ? "gps":"x",  // 显示失败的时间类型
                            txjob->gpstime ? txjob->gpstime : txjob->xtime);  // 显示失败的时间值
                    }
                }
                uj_exitObject(D);                   // 退出调度对象
            }
            uj_exitArray(D);
            break;
        }
        default: {
            LOG(MOD_S2E|WARNING, "Unknown field in dnsched - ignored: %s", D->field.name);
            uj_skipValue(D);
            break;
        }
        }
    }
}


// 函数功能：处理时间同步消息(timesync)，同步基站与LNS的时间基准
// 参数说明：s2ctx - S2E上下文指针
//          D - JSON解码器指针
// 返回值：无
// 调用时机：收到来自LNS的timesync消息时调用，用于建立精确的时间同步
void handle_timesync (s2ctx_t* s2ctx, ujdec_t* D) {
    ustime_t rxtime = rt_getTime();            // 记录接收时间戳
    ustime_t txtime = 0;                       // LNS发送时间戳
    ustime_t xtime  = 0;                       // 扩展时间戳
    sL_t     gpstime = 0;                      // GPS时间戳
    ujcrc_t  field;                            // JSON字段类型
    while( (field = uj_nextField(D)) ) {       // 遍历JSON字段
        switch(field) {
        case J_msgtype: {                      // 消息类型字段
            uj_skipValue(D);                   // 跳过已知的消息类型
            break;
        }
        case J_xtime: {                        // 扩展时间字段
            xtime = uj_int(D);                 // 解析扩展时间值
            break;
        }
        case J_txtime: {                       // 发送时间字段
            txtime = uj_int(D);                // 解析LNS发送时间
            break;
        }
        case J_gpstime: {                      // GPS时间字段
            gpstime = uj_int(D);               // 解析GPS时间值
            break;
        }
        case J_MuxTime: {                      // 多路复用时间字段
            s2e_updateMuxtime(s2ctx, uj_num(D), rxtime);  // 更新多路复用时间基准
            break;
        }
        default: {
            LOG(MOD_S2E|WARNING, "Unknown field in timesync - ignored: %s", D->field.name);
            uj_skipValue(D);
            break;
        }
        }
    }
    // 处理时间同步数据
    if( xtime )                                // 如果有扩展时间数据
        ts_setTimesyncLns(xtime, gpstime);     // 设置LNS时间同步基准
    if( txtime && gpstime )                    // 如果有完整的时间戳对
        ts_processTimesyncLns(txtime, rxtime, gpstime);  // 处理时间同步计算
}


// 函数功能：处理时间查询消息(getxtime)，返回当前系统的各种时间信息
// 参数说明：s2ctx - S2E上下文指针
//          D - JSON解码器指针
// 返回值：无
// 调用时机：收到来自LNS的getxtime消息时调用，用于时间同步验证和调试
void handle_getxtime (s2ctx_t* s2ctx, ujdec_t* D) {
    // 不需要必需字段 - 跳过所有内容
    ujcrc_t field;                             // JSON字段类型
    double  muxtime = 0;                       // 多路复用时间
    while( (field = uj_nextField(D)) ) {
        switch(field) {
        case J_msgtype: {
            uj_skipValue(D);
            break;
        }
        case J_MuxTime: {
            muxtime = uj_num(D);
            break;
        }
        default: {
            LOG(MOD_S2E|WARNING, "Unknown field in getxtime - ignored: %s", D->field.name);
            uj_skipValue(D);
            break;
        }
        }
    }
    // 获取发送缓冲区并构建响应消息
    ujbuf_t sendbuf = (*s2ctx->getSendbuf)(s2ctx, MIN_UPJSON_SIZE);  // 获取发送缓冲区
    if( sendbuf.buf == NULL ) {            // 如果WebSocket无可用空间
        // WebSocket空间不足 - WS稍后会重新调用
        return;                            // 暂时返回，等待重试
    }
    ustime_t ustime = rt_getTime();        // 获取当前本地时间
    uj_encOpen(&sendbuf, '{');             // 开始JSON对象编码
    uj_encKVn(&sendbuf,                    // 编码多个键值对
              "msgtype",  's', "getxtime",     // 消息类型
              "MuxTime",  'T', muxtime,        // 多路复用时间
              "ustime",   'T', ustime/1e6,     // 本地时间(秒)
              "UTCtime",  'T', rt_ustime2utc(ustime)/1e6,  // UTC时间(秒)
              "xtimes",   '[', 0);             // 开始扩展时间数组
    // 为每个发送单元生成扩展时间
    for( int txunit=0; txunit<MAX_TXUNITS; txunit++ ) {
        sL_t xtime = ts_ustime2xtime(txunit, ustime);  // 计算该单元的扩展时间
        uj_encInt(&sendbuf, xtime);        // 添加到数组
    }
    uj_encClose(&sendbuf, ']');            // 结束扩展时间数组
    uj_encClose(&sendbuf, '}');            // 结束JSON对象
    if( !xeos(&sendbuf) ) {                // 检查编码是否成功
        LOG(MOD_S2E|ERROR, "JSON encoding exceeds available buffer space: %d", sendbuf.bufsize);
    } else {
        (*s2ctx->sendText)(s2ctx, &sendbuf);  // 发送响应消息
        assert(sendbuf.buf==NULL);         // 确认缓冲区已被回收
    }
}


// 函数功能：处理远程命令执行消息(runcmd)，允许LNS远程执行基站命令
// 参数说明：s2ctx - S2E上下文指针
//          D - JSON解码器指针
// 返回值：无
// 调用时机：收到来自LNS的runcmd消息时调用，用于远程管理和维护
void handle_runcmd (s2ctx_t* s2ctx, ujdec_t* D) {
    ujcrc_t field;                             // JSON字段类型
    char* argv[MAX_CMDARGS+2] = { NULL };      // 命令参数数组(预留空间)
    int argc = 1;                              // 参数计数器(从1开始，为命令名留位置)
    while( (field = uj_nextField(D)) ) {       // 遍历JSON字段
        switch(field) {
        case J_msgtype: {                      // 消息类型字段
            uj_skipValue(D);                   // 跳过已知的消息类型
            break;
        }
        case J_command: {                      // 命令名字段
            argv[0] = uj_str(D);               // 设置命令名(argv[0])
            break;
        }
        case J_arguments: {                    // 参数数组字段
            uj_enterArray(D);                  // 进入参数数组
            while( uj_nextSlot(D) >= 0 ) {     // 遍历每个参数
                if( argc <= MAX_CMDARGS )      // 检查参数数量限制
                    argv[argc] = uj_str(D);    // 添加参数到argv数组
                argc++;                        // 增加参数计数
            }
            uj_exitArray(D);                   // 退出参数数组
            break;
        }
        default: {
            LOG(MOD_S2E|WARNING, "Unknown field in runcmd - ignored: %s", D->field.name);
            uj_skipValue(D);
            break;
        }
        }
    }
    // 验证命令参数的完整性和合法性
    if( !argv[0] ) {                           // 检查是否提供了命令名
        LOG(MOD_S2E|ERROR, "No command provided - runcmd ignored");
        return;                                // 无命令名，忽略请求
    }
    if( argc > MAX_CMDARGS+1 ) {               // 检查参数数量是否超限
        LOG(MOD_S2E|WARNING, "Too many arguments (max %d but got %d) - runcmd ignored", MAX_CMDARGS, argc-1);
        return;                                // 参数过多，忽略请求
    }
    argv[argc] = NULL;                         // 以NULL结束argv数组(execv要求)
    sys_execCommand(0, (str_t*)argv);          // 执行命令: 0=分离进程，不等待完成
}



// --------------------------------------------------------------------------------
//
// 解码传入的JSON记录
//
// --------------------------------------------------------------------------------

// 函数功能：处理来自LNS的JSON消息，解析并分发到相应的处理函数
// 参数说明：s2ctx - S2E上下文指针
//          json - JSON消息字符串
//          jsonlen - JSON消息长度
// 返回值：1表示成功处理，0表示处理失败
// 调用时机：从LNS接收到JSON消息时调用，是所有下行消息的入口点
int s2e_onMsg (s2ctx_t* s2ctx, char* json, ujoff_t jsonlen) {
    ujdec_t D;                                 // JSON解码器
    uj_iniDecoder(&D, json, jsonlen);          // 初始化JSON解码器
    ujcrc_t msgtype = uj_msgtype(&D);          // 提取消息类型
    if( uj_decode(&D) ) {                      // 解码JSON消息
        LOG(MOD_S2E|ERROR, "Parsing of JSON message failed - ignored");
        return 1;                              // 解析失败，返回成功避免重连
    }
    // 检查是否在配置完成前收到了下行消息
    if( s2ctx->region == 0 && (msgtype == J_dnmsg || msgtype == J_dnsched || msgtype == J_dnframe) ) {
        // 可能发生在消息仍在队列中的情况
        LOG(MOD_S2E|WARNING, "Received '%.*s' before 'router_config' - dropped", D.str.len, D.str.beg);
        return 1;                              // 丢弃早期消息
    }
    // 所有JSON数据必须是每帧一个对象
    uj_nextValue(&D);                          // 移动到下一个值
    uj_enterObject(&D);                        // 进入对象
    int ok = 1;                                // 处理结果标志

    switch(msgtype) {                          // 根据消息类型分发处理
    case 0: {                                   // 无消息类型
        LOG(MOD_S2E|ERROR, "No msgtype - ignored");
        break;
    }
    case J_router_config: {                     // 路由配置消息
        ok = handle_router_config(s2ctx, &D);   // 处理路由配置
        if( ok ) sys_inState(SYSIS_TC_CONNECTED);  // 配置成功则进入连接状态
        break;
    }
    case J_dnframe: {                           // 下行帧消息(已过时)
        LOG(MOD_S2E|ERROR, "Received obsolete 'dnframe' message!");
        handle_dnframe(s2ctx, &D);              // 处理下行帧
        break;
    }
    case J_dnmsg: {                             // 下行消息(新格式)
        handle_dnmsg(s2ctx, &D);                // 处理下行消息
        break;
    }
    case J_dnsched: {                           // 下行调度消息
        handle_dnsched(s2ctx, &D);              // 处理下行调度
        break;
    }
    case J_timesync: {                          // 时间同步消息
        handle_timesync(s2ctx, &D);             // 处理时间同步
        break;
    }
    case J_getxtime: {                          // 获取扩展时间消息
        handle_getxtime(s2ctx, &D);             // 处理扩展时间请求
        break;
    }
    case J_runcmd: {                            // 运行命令消息
        handle_runcmd(s2ctx, &D);               // 处理远程命令
        break;
    }
    case J_rmtsh: {                             // 远程shell消息
        s2e_handleRmtsh(s2ctx, &D);             // 处理远程shell
        break;
    }
    case J_error: {
        ujcrc_t  field;
        while( (field = uj_nextField(&D)) ) {
            switch(field) {
            case J_error: {
                LOG(MOD_S2E|WARNING, "LNS ERROR Msg: %s", uj_str(&D));
                break;
            }
            default: {
                uj_skipValue(&D);
                break;
            }
            }
        }
        break;
    }
    default: {
        // Platform specific commands
        if( !s2e_handleCommands(msgtype, s2ctx, &D) )
            uj_error(&D, "Unknown msgtype: %.*s", D.str.len, D.str.beg);
        break;
    }
    }
    uj_exitObject(&D);
    uj_assertEOF(&D);
    return ok;
}


#if defined(CFG_no_rmtsh)
// 函数功能：处理远程shell消息(rmtsh) - 未实现版本
// 参数说明：s2ctx - S2E上下文指针
//          D - JSON解码器指针
// 返回值：无
// 调用时机：当远程shell功能被禁用时的占位函数
void s2e_handleRmtsh (s2ctx_t* s2ctx, ujdec_t* D) {
    uj_error(D, "Rmtsh not implemented");      // 返回未实现错误
}

// 函数功能：处理二进制消息 - 未实现版本
// 参数说明：s2ctx - S2E上下文指针
//          data - 二进制数据指针
//          datalen - 数据长度
// 返回值：0表示未处理
// 调用时机：当远程shell功能被禁用时的占位函数
int s2e_onBinary (s2ctx_t* s2ctx, u1_t* data, ujoff_t datalen) {
    LOG(MOD_S2E|ERROR, "Ignoring rmtsh binary data (%d bytes)", datalen);
    return 0;                                  // 忽略二进制数据
}
#endif

