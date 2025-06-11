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

// 文件功能：LoRa基站时间同步机制实现 - 高精度时间同步和漂移校正系统
// 作用：管理MCU与SX130X芯片、GPS PPS信号、网络服务器时间之间的同步关系
// 核心功能：
// 1. MCU与SX130X芯片时钟同步，补偿晶振漂移
// 2. GPS PPS信号检测和时间基准校准
// 3. 网络服务器时间同步，建立全局时间基准
// 4. 时间漂移统计分析和质量评估
// 5. 多个发送单元间的时间协调

#include <stdlib.h>           // 标准库函数：malloc、qsort等
#include <math.h>             // 数学函数：round、fabs等
#include "s2conf.h"           // S2配置管理
#include "tc.h"               // 传输上下文管理
#include "timesync.h"         // 时间同步接口定义
#include "ral.h"              // 无线电抽象层接口

// 平台相关的最大漂移阈值定义
#if defined(CFG_smtcpico)
#define _MAX_DT 300           // SMTC PicoGW平台：最大漂移300ppm
#else
#define _MAX_DT 100           // 标准平台：最大漂移100ppm
#endif

// 时间同步质量控制参数
#define SYNC_QUAL_GOOD        100  // 被认为是良好的同步质量值
#define SYNC_QUAL_THRES        90  // 同步质量的截止分位数阈值
#define N_SYNC_QUAL            30  // 同步质量统计表的大小
#define MCU_DRIFT_THRES        90  // MCU同步质量的截止分位数阈值
#define PPS_DRIFT_THRES        80  // PPS同步质量的截止分位数阈值
#define N_DRIFTS               20  // MCU/PPS分位数统计表的大小
#define QUICK_RETRIES           3  // 快速重试次数

// 时间单位和精度控制参数
#define PPM       ((sL_t)1000000)  // 1秒的微秒数，用于ppm计算
#define iPPM_SCALE             10  // 整数ppm精度：保持漂移为十分之一ppm的整数
#define fPPM_SCALE           10.0  // 浮点ppm精度：对应的浮点数倍数
#define MIN_MCU_DRIFT_THRES         2*iPPM_SCALE   // 最小MCU漂移阈值(十分之一ppm)
#define MAX_MCU_DRIFT_THRES   _MAX_DT*iPPM_SCALE   // 最大MCU漂移阈值(十分之一ppm)

// PPS(每秒脉冲)信号相关参数
#define MAX_PPS_ERROR         1000  // PPS信号最大偏差(微秒)
#define MAX_PPS_OFFSET_CHANGE  50  // PPS偏移更新阈值(微秒)
#define NO_PPS_ALARM_INI       10  // 无PPS告警初始阈值(秒)
#define NO_PPS_ALARM_RATE     2.0  // 无PPS告警增长率
#define NO_PPS_ALARM_MAX     3600  // 无PPS告警最大阈值(秒)

// 其他时间控制参数
#define XTICKS_DECAY       100000  // FIFO中xticks的最大存活时间(微秒)
#define UTC_GPS_EPOCH_US 315964800 // UTC纪元相对于GPS纪元的偏移(秒)

// 时间转换和处理宏定义
#define ustimeRoundSecs(x) (((x) + PPM/2) / PPM * PPM)  // 将微秒时间四舍五入到秒
#define ustime2xtime(sync, _ustime) ((sync)->xtime + ((_ustime)-(sync)->ustime))  // MCU时间转SX130X时间
#define xtime2ustime(sync, _xtime)  ((sync)->ustime + ((_xtime)-(sync)->xtime))   // SX130X时间转MCU时间
// 在不同发送单元间转换xtime
#define xtime2xtime(src_sync, dst_sync, _xtime)                 \
    (((dst_sync)->xtime - (src_sync)->xtime) +                  \
     ((src_sync)->ustime - (dst_sync)->ustime) + (_xtime))


// 分位数统计结构体 - 用于漂移质量分析
struct quants {
    int qmin, q50, q80, qmax;      // 最小值、中位数、80%分位数、最大值
};

// 发送单元统计信息结构体 - 跟踪每个SX130X芯片的时钟漂移状态
static struct txunit_stats {
    int excessive_drift_cnt;       // 过度漂移计数器
    int drift_thres;              // 漂移阈值(MCU_DRIFT_THRES分位数)
    int mcu_drifts[N_DRIFTS];     // MCU漂移历史记录数组
    int mcu_drifts_widx;          // MCU漂移写入索引(环形缓冲区)
} txunit_stats[MAX_TXUNITS];      // 为每个发送单元维护统计信息

// 全局时间同步状态变量
static int         sum_mcu_drifts;    // txunit_stats[0].mcu_drifts的累计值
static int         pps_drifts[N_DRIFTS];     // PPS漂移历史记录数组
static int         pps_drifts_widx;          // PPS漂移写入索引
static int         pps_drifts_thres;         // PPS漂移阈值(PPS_DRIFT_THRES分位数)
static u4_t        no_pps_thres;             // 下次发出PPS错误告警的时间阈值
static ustime_t    ppsOffset;                // PPS信号在ustime_t时间轴上的偏移位置
                                            // -1:未知, 否则为0..1e6-1微秒范围内的值
static sL_t        gpsOffset;                // GPS时间到xtime的偏移量
static int         syncLnsCnt;               // 与LNS时间同步尝试计数，0=未尝试
static tmr_t       syncLnsTmr;               // 与服务器时间同步的定时器
static ustime_t    lastReport;               // 上次时间同步状态报告的时间
static timesync_t  timesyncs[MAX_TXUNITS];   // 每个发送单元的时间同步状态
static timesync_t  ppsSync;                  // 最后一次良好的PPS同步状态
static s1_t        syncWobble;               // 同步抖动量
static u1_t        wsBufFull;                // WebSocket缓冲区满标志
static int         syncQual[N_SYNC_QUAL];    // 同步质量历史记录数组
static int         syncQual_widx;            // 同步质量写入索引
static int         syncQual_thres;           // 当前同步质量阈值

// 前向声明
static void onTimesyncLns (tmr_t* tmr);     // LNS时间同步定时器回调函数


// 函数功能：报告时间同步状态，输出详细的同步信息到日志
// 参数说明：force - 强制报告标志，1=立即报告，0=按时间间隔报告
// 返回值：无
// 调用时机：定期报告或调试时强制报告时间同步状态
static void timesyncReport (int force) {
    ustime_t now = rt_getTime();                               // 获取当前MCU时间
    if( !force && now < lastReport + TIMESYNC_REPORTS )       // 如果非强制且未到报告间隔
        return;                                                // 直接返回，不进行报告
    lastReport = now;                                          // 更新最后报告时间
    
    // 计算PPS信号对应的MCU时间(如果有PPS同步)
    uL_t pps_ustime = timesyncs[0].pps_xtime != 0 ? xtime2ustime(&timesyncs[0], timesyncs[0].pps_xtime) : 0;
    
    // 报告当前时间状态：MCU时间、UTC时间、GPS偏移、PPS偏移、同步质量
    LOG(MOD_SYN|INFO, "Time sync: NOW          ustime=0x%012lX utc=0x%lX gpsOffset=0x%lX ppsOffset=%ld syncQual=%d\n",
        now, rt_ustime2utc(now),  gpsOffset, ppsOffset, syncQual[0]);
    
    // 报告MCU与SX130X#0的同步状态
    LOG(MOD_SYN|INFO, "Time sync: MCU/SX130X#0 ustime=0x%012lX xtime=0x%lX pps_ustime=0x%lX pps_xtime=0x%lX",
        timesyncs[0].ustime, timesyncs[0].xtime, pps_ustime, timesyncs[0].pps_xtime);
    
    if( !ppsOffset )                                           // 如果没有PPS偏移信息
        return;                                                // 不继续报告PPS相关信息
    
    // 计算最后一次PPS同步对应的MCU时间
    pps_ustime = xtime2ustime(&timesyncs[0], ppsSync.pps_xtime);
    
    // 报告最后一次PPS同步状态
    LOG(MOD_SYN|INFO, "Time sync: Last PPS     ustime=0x%012lX xtime=0x%lX pps_ustime=0x%lX pps_xtime=0x%lX",
        ppsSync.ustime, ppsSync.xtime, pps_ustime, ppsSync.pps_xtime);
    
    if( !gpsOffset )                                           // 如果没有GPS偏移信息
        return;                                                // 不继续报告时间基准信息
    
    // 报告时间基准比较：系统UTC时间 vs SX130X GPS时间
    LOG(MOD_SYN|INFO, "Time ref:  Last PPS     sys->UTC=%>.6T  SX130X->GPS=%>.6T  leaps=%02lus diff=%~T",
        rt_ustime2utc(pps_ustime),                            // 系统时间转UTC
        ts_xtime2gpstime(ppsSync.pps_xtime) + UTC_GPS_EPOCH_US*PPM,  // SX130X时间转GPS再转UTC
        (ts_xtime2gpstime(ppsSync.pps_xtime) + UTC_GPS_EPOCH_US*PPM - rt_ustime2utc(pps_ustime) + PPM/2)/PPM,  // 闰秒差异
        (ts_xtime2gpstime(ppsSync.pps_xtime) + UTC_GPS_EPOCH_US*PPM - rt_ustime2utc(pps_ustime) + PPM/2)%PPM - PPM/2 );  // 精确时间差
}


// 函数功能：将浮点漂移率编码为整数ppm值
// 参数说明：drift - 浮点漂移率(相对于1.0的偏差)
// 返回值：编码后的整数ppm值(十分之一ppm精度)
// 调用时机：需要存储或比较漂移率时调用
static int encodeDriftPPM (double drift) {
    return (int)round((drift - 1.0) * PPM * iPPM_SCALE);     // 转换为相对偏差并按比例缩放
}

// 函数功能：将整数ppm值解码为浮点漂移率
// 参数说明：scaled_ppm - 缩放后的ppm值
// 返回值：解码后的浮点漂移率
// 调用时机：需要使用漂移率进行时间校正时调用
static double decodeDriftPPM (double scaled_ppm) {
    return 1.0 + scaled_ppm / (PPM * fPPM_SCALE);           // 还原为绝对漂移率
}

// 函数功能：将缩放的ppm值转换为标准ppm单位
// 参数说明：scaled_ppm - 缩放后的ppm值
// 返回值：标准ppm值
// 调用时机：显示或记录ppm值时调用
static double decodePPM (double scaled_ppm) {
    return scaled_ppm / fPPM_SCALE;                          // 除以缩放因子得到真实ppm
}

// 函数功能：整数绝对值比较函数，用于qsort排序
// 参数说明：a, b - 待比较的整数指针
// 返回值：绝对值比较结果：负数、0、正数
// 调用时机：对漂移数组进行排序以计算分位数时调用
static int cmp_abs_int (const void* a, const void* b) {
    return abs(*(int*)a) - abs(*(int*)b);                    // 按绝对值大小排序
}

// 函数功能：计算漂移数组的统计分位数
// 参数说明：drifts - 漂移数据数组
//          q - 输出的分位数结构体指针
//          thresQ - 阈值分位数百分比
//          auxQ - 辅助分位数百分比指针(输入输出)
// 返回值：指定阈值分位数的值
// 调用时机：分析漂移分布特征时调用
static int drift_stats (int* drifts, struct quants *q, int thresQ, int* auxQ) {
    int sorted_drifts[N_DRIFTS];                             // 临时排序数组
    memcpy(sorted_drifts, drifts, sizeof(sorted_drifts));    // 复制原数组
    qsort(sorted_drifts,N_DRIFTS,sizeof(sorted_drifts[0]), cmp_abs_int);  // 按绝对值排序
    
    // 计算各个分位数
    q->qmin = sorted_drifts[0];                              // 最小值(绝对值)
    q->q50  = sorted_drifts[N_DRIFTS/2];                     // 中位数
    q->q80  = sorted_drifts[(N_DRIFTS*80+50)/100];          // 80%分位数
    q->qmax = sorted_drifts[N_DRIFTS-1];                     // 最大值(绝对值)
    
    if( auxQ )                                               // 如果需要计算辅助分位数
        *auxQ = sorted_drifts[(*auxQ*N_DRIFTS+50)/100];      // 计算指定百分比的分位数
    
    return sorted_drifts[(thresQ*N_DRIFTS+50)/100];          // 返回阈值分位数
}

// 函数功能：记录漂移统计信息到日志并返回阈值
// 参数说明：msg - 日志消息前缀
//          drifts - 漂移数据数组
//          thresQ - 阈值分位数百分比
//          auxQ - 辅助分位数百分比指针
// 返回值：计算得出的阈值分位数
// 调用时机：需要分析和记录漂移分布时调用
static int log_drift_stats (str_t msg, int* drifts, int thresQ, int* auxQ) {
    struct quants q;                                         // 分位数统计结构体
    int thres = drift_stats(drifts, &q, thresQ, auxQ);      // 计算分位数统计
    
    // 记录详细的漂移统计信息到日志
    LOG(MOD_SYN|INFO, "%s: min: %+4.1fppm  q50: %+4.1fppm  q80: %+4.1fppm  max: %+4.1fppm - threshold q%d: %+4.1fppm",
        msg,
        q.qmin / fPPM_SCALE, q.q50 / fPPM_SCALE, q.q80 / fPPM_SCALE, q.qmax / fPPM_SCALE,  // 各分位数的ppm值
        thresQ, thres / fPPM_SCALE);                         // 阈值分位数和其ppm值
    
    return thres;                                            // 返回阈值
}


// 函数功能：标准化MCU时间跨度，补偿晶振漂移
// 参数说明：timespan - 原始MCU时间跨度(微秒)
// 返回值：标准化后的时间跨度(微秒)
// 调用时机：需要将MCU时间转换为标准时间时调用，消除本地晶振漂移影响
ustime_t ts_normalizeTimespanMCU (ustime_t timespan) {
    // 使用平均MCU漂移率对时间跨度进行校正
    // 校正公式：标准时间 = MCU时间 / 漂移率
    return (ustime_t)round(timespan / decodeDriftPPM((double)sum_mcu_drifts / N_DRIFTS));
    // 注释的版本是等价的计算方式，但上面的版本更清晰
    return (ustime_t)round( timespan / (1.0 + sum_mcu_drifts / (PPM*iPPM_SCALE) / N_DRIFTS) );
}

// 函数功能：更新时间同步状态，处理MCU与SX130X芯片间的时间同步
// 参数说明：txunit - 发送单元编号(0-MAX_TXUNITS)
//          quality - 同步质量评分
//          curr - 当前时间同步测量结果指针
// 返回值：下次时间同步的延迟时间(微秒)
// 调用时机：从SX130X芯片获得新的时间测量值时调用，建立和维护时间基准
ustime_t ts_updateTimesync (u1_t txunit, int quality, const timesync_t* curr) {
    // 更新同步质量历史记录
    syncQual[syncQual_widx] = quality;                       // 记录当前同步质量
    syncQual_widx = (syncQual_widx + 1) % N_SYNC_QUAL;       // 更新环形缓冲区索引
    
    // 每轮完整采样后重新计算质量阈值
    if( syncQual_widx == 0 ) {                               // 完成一轮采样
        int sorted_qual[N_SYNC_QUAL];                        // 质量排序数组
        memcpy(sorted_qual, syncQual, sizeof(sorted_qual));  // 复制质量数据
        qsort(sorted_qual,N_SYNC_QUAL,sizeof(sorted_qual[0]),cmp_abs_int);  // 按绝对值排序
        int thres = sorted_qual[(N_SYNC_QUAL*SYNC_QUAL_THRES+50)/100];  // 计算阈值分位数
        
        // 记录质量统计信息
        LOG(MOD_SYN|INFO, "Time sync qualities: min=%d q%d=%d max=%d (previous q%d=%d)",
            sorted_qual[0], SYNC_QUAL_THRES, thres, sorted_qual[N_SYNC_QUAL-1], SYNC_QUAL_THRES, syncQual_thres);
        syncQual_thres = max(SYNC_QUAL_GOOD, abs(thres));    // 更新质量阈值，确保不低于良好标准
    }
    
    // 检查当前同步质量是否符合要求
    if( abs(quality) > syncQual_thres ) {                    // 如果质量不达标
        LOG(MOD_SYN|VERBOSE, "Time sync rejected: quality=%d threshold=%d", quality, syncQual_thres);
        return TIMESYNC_RADIO_INTV;                          // 拒绝此次同步，使用标准间隔
    }

    timesync_t* last = &timesyncs[txunit];                   // 获取上次同步状态
    if( last->ustime == 0 ) {                                // 如果是第一次调用
        // 第一次调用 - 仅设置基准状态
        *last = *curr;                                       // 保存当前状态作为基准
        return TIMESYNC_RADIO_INTV;                          // 返回标准同步间隔
    }
    
    // 计算时间差值，分析晶振漂移
    ustime_t dus = curr->ustime - last->ustime;              // MCU时间差
    sL_t dxc = curr->xtime - last->xtime;                    // SX130X时间差
    
    // 检查SX130X计数器是否正常递增
    if( dxc <= 0 ) {                                         // 如果时间没有前进或出现异常
        LOG(MOD_SYN|ERROR, "SX130X#%d trigger count not ticking or weird value: 0x%lX .. 0x%lX (dxc=%d)",
            txunit, last->xtime, curr->xtime, dxc);
        return TIMESYNC_RADIO_INTV;                          // 返回标准间隔，等待下次测量
    }
    
    // 检查测量间隔是否合适
    if( dus < TIMESYNC_RADIO_INTV/5 ) {                      // 如果测量间隔太短
        // 如果测量时间间隔太近，不考虑此次测量
        return TIMESYNC_RADIO_INTV;                          // 等待更长间隔的测量
    }
    
    // 计算并记录MCU晶振漂移
    struct txunit_stats* stats = &txunit_stats[txunit];      // 获取发送单元统计信息
    int drift_ppm = encodeDriftPPM( (double)dus/(double)dxc );  // 计算漂移率(编码为ppm)
    
    // 更新txunit#0的累计漂移(用于全局时间校正)
    if( txunit == 0 )                                        // 如果是主发送单元
        sum_mcu_drifts += drift_ppm - stats->mcu_drifts[stats->mcu_drifts_widx];  // 更新累计漂移
    
    // 记录漂移历史
    stats->mcu_drifts[stats->mcu_drifts_widx] = drift_ppm;   // 保存当前漂移值
    stats->mcu_drifts_widx = (stats->mcu_drifts_widx + 1) % N_DRIFTS;  // 更新环形缓冲区索引
    
    // 每轮完整采样后分析漂移统计
    if( stats->mcu_drifts_widx == 0 ) {                      // 完成一轮漂移采样
        // 计算并记录漂移统计信息
        int thres = log_drift_stats("MCU/SX130X drift stats", stats->mcu_drifts, MCU_DRIFT_THRES, NULL);
        stats->drift_thres = max(MIN_MCU_DRIFT_THRES, min(MAX_MCU_DRIFT_THRES, abs(thres)));  // 更新漂移阈值
        
        // 计算平均漂移率并记录
        double mean_ppm = decodePPM( ((double)sum_mcu_drifts) / N_DRIFTS);
        LOG(MOD_SYN|INFO, "Mean MCU drift vs SX130X#0: %.1fppm",  mean_ppm);
        
        // 如果有UTC偏移基准且没有PPS同步，根据漂移校正UTC偏移
        if( rt_utcOffset_ts != 0 && !ppsSync.pps_xtime) {     // 有UTC基准但无PPS
            rt_utcOffset -= (curr->ustime - rt_utcOffset_ts) * mean_ppm/PPM;  // 校正UTC偏移
            rt_utcOffset_ts = curr->ustime;                  // 更新校正基准时间
        }
    }
    
    // 检查当前漂移是否超过阈值
    if( abs(drift_ppm) > stats->drift_thres ) {             // 如果漂移过大
        stats->excessive_drift_cnt += 1;                    // 增加过度漂移计数
        
        // 定期报告过度漂移警告
        if( (stats->excessive_drift_cnt % QUICK_RETRIES) == 0 ) {
            LOG(MOD_SYN|WARNING, "Repeated excessive clock drifts between MCU/SX130X#%d (%d retries): %.1fppm (threshold %.1fppm)",
                txunit, stats->excessive_drift_cnt, drift_ppm/fPPM_SCALE, stats->drift_thres/fPPM_SCALE);
        }
        
        // 如果连续多次过度漂移，重置阈值避免卡在极低值
        if( stats->excessive_drift_cnt >= 2*QUICK_RETRIES )
            stats->drift_thres = MAX_MCU_DRIFT_THRES;        // 重置阈值 - 可能卡在了极低值
        return TIMESYNC_RADIO_INTV/2;                        // 加快同步频率
    }
    
    stats->excessive_drift_cnt = 0;                          // 重置过度漂移计数
    ustime_t delay = TIMESYNC_RADIO_INTV;                    // 设置默认延迟间隔

    // Only txunit#0 can have PPS or we're not tracking a PPS
    if( txunit != 0 )
        goto done;

    if( ppsSync.pps_xtime ) {
        // We are actually tracking PPS - complain if PPS lost
        s4_t no_pps_secs = (curr->xtime - ppsSync.pps_xtime + PPM/2) / PPM;
        if( no_pps_secs > no_pps_thres ) {
            LOG(MOD_SYN|WARNING, "No PPS pulse for ~%d secs", no_pps_secs);
            no_pps_thres = no_pps_thres >= NO_PPS_ALARM_MAX
                ? no_pps_thres + NO_PPS_ALARM_MAX
                : (u4_t)(no_pps_thres * NO_PPS_ALARM_RATE);
        }
    }
    // We update ppsSync only if we have two consecutive time syncs with valid PPS timestamps
    // and if they are apart ~1s - we might see weird values if no PPS pulse occurred during time sync span.
    if( !last->pps_xtime || !curr->pps_xtime ) {
        goto done;
    }
    if( curr->xtime - curr->pps_xtime > PPM+TX_MIN_GAP ) {
        LOG(MOD_SYN|XDEBUG, "PPS: Rejecting PPS (xtime/pps_xtime spread): curr->xtime=0x%lX   curr->pps_xtime=0x%lX   diff=%lu (>%u)",
            curr->xtime, curr->pps_xtime, curr->xtime - curr->pps_xtime, PPM+TX_MIN_GAP);
        goto done;  // no PPS since last time sync
    }
    sL_t err = (curr->pps_xtime - last->pps_xtime) % PPM;
    if( err < 0 ) err += PPM;
    if( err > MAX_PPS_ERROR && err < PPM-MAX_PPS_ERROR ) {
        LOG(MOD_SYN|XDEBUG, "PPS: Rejecting PPS (consecutive pps_xtime error): curr->pps_xtime=0x%lX   last->pps_xtime=0x%lX   diff=%lu",
            curr->pps_xtime, last->pps_xtime, curr->pps_xtime - last->pps_xtime);
        goto done;  // out of scope - probably no value latched
    }
    if( !ppsSync.pps_xtime )
        LOG(MOD_SYN|INFO, "First PPS pulse acquired");

    // Time sync in curr a new valid PPS reference point
    // Update PPS drift stats
    double pps_drift = (double)(curr->pps_xtime - last->pps_xtime)
        / (double)((curr->pps_xtime - last->pps_xtime + PPM/2) / PPM * PPM);
    pps_drifts[pps_drifts_widx] = encodeDriftPPM(pps_drift);
    pps_drifts_widx = (pps_drifts_widx + 1) % N_DRIFTS;
    if( pps_drifts_widx == 0 )
        pps_drifts_thres = log_drift_stats("PPS/SX130X drift stats", pps_drifts, PPS_DRIFT_THRES, NULL);

    ustime_t pps_ustime = xtime2ustime(curr, curr->pps_xtime);
    ustime_t off = pps_ustime % PPM;
    if( syncLnsCnt == 0 ) {
        ppsOffset = off;
        syncLnsCnt = 1;
        wsBufFull = 0;
        rt_yieldTo(&syncLnsTmr, onTimesyncLns);
        LOG(MOD_SYN|INFO, "Obtained initial PPS offset (%ld) - starting timesync with LNS", ppsOffset);
    }
    else if( abs(ppsOffset-off) > (stats->drift_thres * TIMESYNC_RADIO_INTV)/PPM  ) {
        LOG(MOD_SYN|XDEBUG, "Changed PPS offset: %ld => %ld (delta: %ld)", ppsOffset, off, off-ppsOffset);
        // Adjust ppsOffset to accout for MCU/PPS drift
        ppsOffset = off;
    }
    // Correct the fractional second of the UTC reference so it lines up with PPS
    ustime_t pps_utctime_us = rt_ustime2utc(pps_ustime) % PPM;
    rt_utcOffset += pps_utctime_us < PPM/2 ? -pps_utctime_us : PPM-pps_utctime_us;
    // Shift timesync into the middle of two PPS pulses
    // Avoid turning off PPS latching during SX130X sync procedure near the PPS.
    // We might miss a PPS pulse and a scheduled frame might not be sent.
    // Also wobble the sync time a bir otherwise we might track the value when enabling PPS latching
    // as PPS. This happens with a rate resembling 1Hz
    syncWobble *= -1;
    off = syncWobble *PPM/10 + PPM/2 - (curr->ustime - ppsOffset + delay) % PPM;
    delay += off + (off < 0 ? 0 : PPM);
    // Update time reference for conversions + update GPS offset based on # of seconds passed
    // ppsSync->pps_xtime and gpsOffset are pairs related to same point in time
    if( gpsOffset )
        gpsOffset += ustimeRoundSecs(curr->pps_xtime - ppsSync.pps_xtime);
    ppsSync = *curr;
  done:
    *last = *curr;
    return delay;
}


// 函数功能：将GPS时间转换为指定发送单元的SX130X时间
// 参数说明：txunit - 目标发送单元编号
//          gpstime - GPS时间戳
// 返回值：对应的SX130X时间戳，失败返回0
// 调用时机：需要将GPS时间基准转换为SX130X芯片时间时调用
sL_t ts_gpstime2xtime (u1_t txunit, sL_t gpstime) {
    // 检查必要的同步条件
    if( txunit >= MAX_TXUNITS || !timesyncs[txunit].xtime || !ppsSync.pps_xtime || ppsOffset < 0 || !gpsOffset ) {
        LOG(MOD_SYN|ERROR, "Cannot convert GPS time - missing %s time sync",
            !timesyncs[txunit].xtime ? "SX130X"        // 缺少SX130X同步
            : !ppsSync.pps_xtime || ppsOffset ? "PPS"   // 缺少PPS同步
            : !gpsOffset ? "GPS" : "?");               // 缺少GPS偏移
        return 0;                                      // 转换失败
    }
    
    // 检查PPS同步是否过期
    if( timesyncs[0].xtime - ppsSync.pps_xtime > PPS_VALID_INTV ) {
        LOG(MOD_SYN|ERROR, "Failed to convert gpstime to xtime - last PPS sync to old: %~T",
            timesyncs[0].xtime - ppsSync.pps_xtime);
        return 0;                                      // PPS同步过期，转换失败
    }
    
    // 执行GPS时间到SX130X时间的转换
    sL_t xtime = gpstime - gpsOffset + ppsSync.pps_xtime;  // 计算基于PPS基准的xtime
    return txunit==0 ? xtime : xtime2xtime(&ppsSync, &timesyncs[txunit], xtime);  // 转换到目标发送单元
}


// 函数功能：将SX130X时间转换为GPS时间
// 参数说明：xtime - SX130X时间戳
// 返回值：对应的GPS时间戳，失败返回0
// 调用时机：需要将SX130X芯片时间转换为GPS时间基准时调用
sL_t ts_xtime2gpstime (sL_t xtime) {
    if( ppsSync.pps_xtime == 0 ) {                     // 如果没有PPS同步基准
        return 0;                                      // 无法转换
    }
    
    xtime = ts_xtime2xtime(xtime, 0);                  // 转换到txunit#0的时间基准
    if( !xtime || xtime - ppsSync.pps_xtime > PPS_VALID_INTV ) {  // 检查转换结果和PPS有效性
        LOG(MOD_SYN|ERROR, "Failed to convert xtime to gpstime - last PPS sync too old: %~T",
            xtime - ppsSync.pps_xtime);
        return 0;                                      // PPS同步过期，转换失败
    }
    
    return gpsOffset + xtime - ppsSync.pps_xtime;      // 计算GPS时间
}


// 函数功能：将MCU时间转换为指定发送单元的SX130X时间
// 参数说明：txunit - 目标发送单元编号
//          ustime - MCU时间戳(微秒)
// 返回值：对应的SX130X时间戳，失败返回0
// 调用时机：需要将MCU系统时间转换为SX130X芯片时间时调用
sL_t ts_ustime2xtime (u1_t txunit, ustime_t ustime) {
    if( txunit >= MAX_TXUNITS || timesyncs[txunit].xtime == 0 )  // 检查发送单元有效性和同步状态
        return 0; // 无法转换
    const timesync_t* sync = &timesyncs[txunit];       // 获取时间同步状态
    return ustime2xtime(sync, ustime);                 // 执行MCU到SX130X时间转换
}


// 函数功能：将SX130X时间转换为MCU时间
// 参数说明：xtime - SX130X时间戳
// 返回值：对应的MCU时间戳(微秒)，失败返回0
// 调用时机：需要将SX130X芯片时间转换为MCU系统时间时调用
ustime_t ts_xtime2ustime (sL_t xtime) {
    int txunit = ral_xtime2txunit(xtime);              // 从xtime中提取发送单元编号
    if( txunit >= MAX_TXUNITS || timesyncs[txunit].xtime == 0 ) {  // 检查发送单元有效性
        LOG(MOD_SYN|ERROR, "Cannot convert xtime=0x%lX - missing SX130X#%d time sync",
            timesyncs[txunit].xtime, txunit);
        return 0;                                      // 没有对应的时间同步状态
    }
    
    const timesync_t* sync = &timesyncs[txunit];       // 获取时间同步状态
    if( ral_xtime2sess(xtime) != ral_xtime2sess(sync->xtime) ) {  // 检查会话一致性
        LOG(MOD_SYN|ERROR, "Cannot convert xtime=0x%lX - obsolete session: %d (current %d)",
            xtime, ral_xtime2sess(xtime), ral_xtime2sess(sync->xtime));
        return 0;                                      // 会话不匹配，xtime已过期
    }
    
    return xtime2ustime(sync, xtime);                  // 执行SX130X到MCU时间转换
}


// 函数功能：在不同发送单元之间转换SX130X时间
// 参数说明：xtime - 源发送单元的SX130X时间戳
//          dst_txunit - 目标发送单元编号
// 返回值：转换到目标发送单元的SX130X时间戳，失败返回0
// 调用时机：需要在多个SX130X芯片间进行时间协调时调用
sL_t ts_xtime2xtime (sL_t xtime, u1_t dst_txunit) {
    int src_txunit = ral_xtime2txunit(xtime);          // 从xtime中提取源发送单元编号
    if( src_txunit == dst_txunit )                     // 如果源和目标是同一个发送单元
        return xtime;                                  // 直接返回原时间
    
    // 检查源和目标发送单元的时间同步状态
    if( src_txunit >= MAX_TXUNITS ||
        timesyncs[src_txunit].xtime == 0 ||
        timesyncs[dst_txunit].xtime == 0 ) {
        LOG(MOD_SYN|ERROR, "Cannot convert xtime=%ld from txunit#%d to txunit#%d", xtime, src_txunit, dst_txunit);
        return 0; // 无法转换
    }
    
    // 执行发送单元间的时间转换
    const timesync_t* src_sync = &timesyncs[src_txunit];  // 源发送单元同步状态
    const timesync_t* dst_sync = &timesyncs[dst_txunit];  // 目标发送单元同步状态
    return xtime2xtime(src_sync, dst_sync, xtime);     // 基于同步状态进行转换
}


// 函数功能：将32位SX130X时钟计数器转换为向LNS报告的xtime
// 参数说明：xticks - 32位SX130X时钟计数值
//          last_xtime - 上次已知的有效xtime值
// 返回值：转换后的完整xtime值，失败返回0
// 调用时机：仅在有libloragw访问权限的进程中调用(即非ral_master进程)
// 注意：处理32位计数器的溢出问题，以及FIFO中滞留帧的时间戳
sL_t ts_xticks2xtime (u4_t xticks, sL_t last_xtime) {
    // 时间同步应该足够频繁，不应看到从正数到负数的溢出
    // (需要2^31微秒约35分钟才会溢出)
    // 但是我们可能会看到小的负数，因为时间同步可能比
    // 卡在SX130X FIFO中的帧时间戳稍新一些
    sL_t d;
    if( (d = (s4_t)(xticks - last_xtime)) < -XTICKS_DECAY ) {  // 如果时间差异过大
        LOG(MOD_SYN|CRITICAL,
            "SX130X RX time roll over - no update for a long time: xticks=0x%X last_xtime=0x%lX",
            xticks, last_xtime);
        return 0;                                      // 计数器溢出，返回失败
    }
    return last_xtime + d;                             // 返回更新后的xtime
}


// 函数功能：为指定发送单元创建新的xtime会话标识
// 参数说明：txunit - 发送单元编号
// 返回值：包含会话标识和发送单元编号的xtime基值
// 调用时机：需要区分SX130X时间戳会话时调用(如重新连接TC时)
// 作用：SX130X计数器重启时，必须拒绝重启前的旧时间戳
sL_t ts_newXtimeSession (u1_t txunit) {
    // 这个函数用于区分SX130X时间戳的不同会话
    // 如果有新会话(当前是重新连接到TC)，SX130X计数器会重启
    // 来自TC的带有重启前时间戳的旧时间戳必须被拒绝
    sL_t ext = ((sL_t)rand() & RAL_XTSESS_MASK) << RAL_XTSESS_SHIFT;  // 生成随机会话标识
    if( !ext ) ext = (sL_t)1<<RAL_XTSESS_SHIFT;        // 会话标识不能为0
    ext |= ((sL_t)txunit & RAL_TXUNIT_MASK) << RAL_TXUNIT_SHIFT;  // 加入发送单元编号
    return ext;                                        // 返回完整的会话基值
}


// 函数功能：初始化时间同步模块，每次启动新会话时运行
// 参数说明：无
// 返回值：无
// 调用时机：程序启动或重启时调用，重置所有时间同步状态
void ts_iniTimesync () {
    ppsOffset = -1;                                    // 重置PPS偏移为未知状态
    gpsOffset = 0;                                     // 重置GPS偏移
    no_pps_thres = NO_PPS_ALARM_INI;                   // 重置PPS告警阈值
    memset(&ppsSync, 0, sizeof(ppsSync));              // 清零PPS同步状态 - 从未见过PPS
    memset(&txunit_stats, 0, sizeof(txunit_stats));    // 清零发送单元统计信息
    
    // 初始化所有发送单元的漂移阈值
    for( int i=0; i<MAX_TXUNITS; i++ )
        txunit_stats[i].drift_thres = MAX_MCU_DRIFT_THRES;  // 设置为最大漂移阈值
    
    syncWobble = -1;                                   // 重置同步抖动标志
    pps_drifts_widx = 0;                               // 重置PPS漂移写入索引
    syncQual_thres = INT_MAX;                          // 重置同步质量阈值为最大值
    syncLnsCnt = 0;                                    // 重置LNS同步计数
    lastReport = 0;                                    // 重置最后报告时间
    sum_mcu_drifts = 0;                                // 重置MCU漂移累计值
    memset(timesyncs, 0, sizeof(timesyncs));           // 清零所有发送单元的时间同步状态
    rt_clrTimer(&syncLnsTmr);                          // 清除LNS同步定时器
}


// ================================================================================
//
// 与LNS的时间同步 - 维护gpsOffset全局时间基准
//
// ================================================================================


// 函数功能：联系服务器获取到GPS纪元的时间同步
// 参数说明：tmr - 定时器指针(定时器回调函数)
// 返回值：无
// 调用时机：定时器触发时调用，重复执行直到找到时间同步解决方案
// 作用：向LNS发送时间同步请求，建立本地时间与GPS时间的关联
static void onTimesyncLns (tmr_t* tmr) {
    timesyncReport(0);                                 // 报告当前时间同步状态
    
    // 检查时间同步的前提条件
    if( TC == NULL || ppsOffset < 0 || gpsOffset ) {
        // 未连接 || 尚无SX130X/PPS同步 || 已有GPS纪元同步
        rt_setTimer(tmr, rt_micros_ahead(TIMESYNC_LNS_PAUSE));  // 暂停一段时间后重试
        return;
    }
    
    s2ctx_t* s2ctx = &TC->s2ctx;                       // 获取S2E上下文
    ujbuf_t sendbuf = (*s2ctx->getSendbuf)(s2ctx, MIN_UPJSON_SIZE/2);  // 获取发送缓冲区
    
    // 检查缓冲区可用性
    if( sendbuf.buf == NULL ) {                        // 如果没有可用缓冲区
        if( !wsBufFull )                               // 首次遇到缓冲区满
            LOG(MOD_SYN|ERROR, "Failed to send timesync to server - no buffer space");
        rt_setTimer(tmr, rt_micros_ahead(TIMESYNC_LNS_RETRY)); // 稍后重试
        wsBufFull = 1;                                 // 标记缓冲区满状态
        return;
    }
    
    wsBufFull = 0;                                     // 重置缓冲区满标志
    
    // 构造时间同步请求JSON消息
    uj_encOpen(&sendbuf, '{');                         // 开始JSON对象
    uj_encKVn(&sendbuf,
              "msgtype",   's', "timesync",            // 消息类型：时间同步
              "txtime",    'I', rt_getTime(),          // 发送时间戳
              NULL);
    uj_encClose(&sendbuf, '}');                        // 结束JSON对象
    
    (*s2ctx->sendText)(s2ctx, &sendbuf);               // 发送时间同步请求到服务器
    
    // 计算下次发送的延迟时间
    ustime_t delay = syncLnsCnt % TIMESYNC_LNS_BURST ? TIMESYNC_LNS_RETRY : TIMESYNC_LNS_PAUSE;
    syncLnsCnt += 1;                                   // 增加同步尝试计数
    rt_setTimer(tmr, rt_micros_ahead(delay));          // 设置下次定时器触发时间
    LOG(MOD_SYN|DEBUG, "Timesync #%d sent to server", syncLnsCnt);
}


// 函数功能：服务器强制设置推断的GPS时间，直接建立时间基准
// 参数说明：xtime - SX130X时间戳
//          gpstime - 对应的GPS时间戳
// 返回值：无
// 调用时机：服务器直接提供GPS时间标签时调用，跳过时间推断过程
void ts_setTimesyncLns (ustime_t xtime, sL_t gpstime) {
    ustime_t ustime = ts_xtime2ustime(xtime);          // 转换为MCU时间
    if( ustime == 0 || (xtime = ts_xtime2xtime(xtime, 0)) == 0 )  // 转换到txunit#0
        return;                                        // 转换失败，直接返回
    
    ustime_t gps_us = gpstime % PPM;                   // 提取GPS时间的微秒部分
    ppsOffset = (ustime - gps_us) % PPM;               // 计算PPS偏移
    gpsOffset = gpstime;                               // 设置GPS时间偏移
    
    // 设置PPS同步状态
    ppsSync.pps_xtime = xtime;                         // PPS对应的SX130X时间
    ppsSync.xtime = xtime;                             // 同步点的SX130X时间
    ppsSync.ustime = ustime;                           // 同步点的MCU时间
    
    LOG(MOD_SYN|INFO, "Server time sync: xtime=0x%lX gpstime=0x%lX ppsOffset=%ld gpsOffset=0x%lX",
        xtime, gpstime, ppsOffset, gpsOffset);
}


// Server reported back a timestamp - infer GPS second label for a specific PPS edge
void ts_processTimesyncLns (ustime_t txtime, ustime_t rxtime, sL_t gpstime) {
    if( ppsOffset < 0 || rxtime - txtime >= 2*PPM || gpsOffset )
        return;    // need ppsOffset || roundtrip too long || we already have a solution
    if( sys_modePPS == PPS_FUZZY ) {
        // In this timing mode the PPS of the gateway and the PPS of the server are not aligned.
        // This mode facilitates beaconing while not perfectly aligned to an absolute GPS time.
        sL_t xtime = ustime2xtime(&timesyncs[0], (txtime + rxtime)/2);
        LOG(MOD_SYN|INFO, "Timesync with LNS - fuzzy PPS: tx/rx=0x%lX..0x%lX xtime=0x%lX gpsOffset=0x%lX", txtime, rxtime, xtime, gpsOffset);
        ts_setTimesyncLns(xtime, gpstime);
        return;
    }
    txtime -= ppsOffset;
    rxtime -= ppsOffset;
    sL_t tx_s   = txtime / PPM;
    sL_t rx_s   = rxtime / PPM;
    sL_t gps_us = gpstime % PPM;
    sL_t gps_s  = gpstime - gps_us;
    sL_t us_s = 0;
    int  cnt = 0;
    // Try all combinations of server offset from PPS and all
    // possible seconds on the gateway side from TX start to receive time.
    // If only one solution makes sense then save the seconds offset from
    // monotonic ustime to GPS time.
    for( sL_t try_s=tx_s; try_s <= rx_s; try_s++ ) {
        ustime_t candidate = try_s*PPM + gps_us;
        if( candidate >= txtime && candidate <= rxtime ) {
            us_s = try_s*PPM + ppsOffset;  // possible solution
            cnt++;
        }
    }
    LOG(MOD_SYN|VERBOSE, "Timesync LNS: tx/rx:0x%lX..0x%lX (%~T) us/gps:0x%lX/0x%lX (pps offset=%ld) - %d solutions",
        txtime, rxtime, rxtime-txtime, us_s, gpstime, ppsOffset, cnt);
    if( cnt != 1 )
        return;

    // Only one solution - calculate the GPS time label
    //    us_s (localtime) equivalent to gps_s (GPS seconds since epoch)
    // Translate into a seconds offset
    const timesync_t* sync0 = &timesyncs[0];
    sL_t pps_xtime_inferred = ustime2xtime(sync0, us_s);    // inferred PPS pulse in xtime (subject to ustime->xtime error)
    sL_t delta  = ustimeRoundSecs(pps_xtime_inferred - ppsSync.pps_xtime);  // seconds between last latched PPS and inferred
    sL_t pps_xtime = ppsSync.pps_xtime + delta;
    sL_t jitter = pps_xtime - pps_xtime_inferred;
    if( abs(jitter) * iPPM_SCALE > txunit_stats[0].drift_thres ) {
        LOG(MOD_SYN|ERROR, "Timesync LNS: Too much drift between last PPS and inferred PPS: %ldus", jitter);
        return;
    }
    gpsOffset = gps_s - delta;
    LOG(MOD_SYN|INFO, "Timesync with LNS: gpsOffset=0x%lX", gpsOffset);
    timesyncReport(1);
}

// --------------------------------------------------------------------------------
//
// Time sync health data
//
// --------------------------------------------------------------------------------
