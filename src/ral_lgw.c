//=============================================================================
// 文件功能：SX1301/SX1302 LoRa网关芯片的无线电抽象层实现
// 作用说明：为SX1301/SX1302系列芯片提供RAL接口实现，包括：
//          - 参数映射转换(RPS与libloragw格式互转)
//          - 发送接收数据处理
//          - 时间同步机制
//          - 硬件配置管理
// 适用芯片：SX1301, SX1302系列LoRa网关集中器芯片
//=============================================================================

#if defined(CFG_lgw1)

#if (!defined(CFG_ral_lgw) && !defined(CFG_ral_master_slave)) || (defined(CFG_ral_lgw) && defined(CFG_ral_master_slave))
#error Exactly one of the two params must be set: CFG_ral_lgw CFG_ral_master_slave
#endif

#include "s2conf.h"
#include "tc.h"
#include "timesync.h"
#include "sys.h"
#include "sx130xconf.h"
#include "ral.h"
#include "lgw/loragw_reg.h"
#include "lgw/loragw_hal.h"
#if defined(CFG_sx1302)
#include "lgw/loragw_sx1302_timestamp.h"
extern timestamp_counter_t counter_us; // from loragw_sx1302.c
#endif // defined(CFG_sx1302)

//=============================================================================
// 配置常量定义
//=============================================================================
#define RAL_MAX_RXBURST 10  // 单次轮询最大接收数据包数量，防止接收队列溢出

//=============================================================================
// FSK调制参数常量定义
// 功能：定义FSK调制方式的固定参数
//=============================================================================
#define FSK_BAUD      50000  // FSK波特率：50kbps
#define FSK_FDEV      25     // FSK频偏：25kHz
#define FSK_PRMBL_LEN 5      // FSK前导码长度：5字节

//=============================================================================
// 扩频因子映射表
// 功能：将S2E内部SF枚举值映射到libloragw的DR枚举值
//=============================================================================
static const u2_t SF_MAP[] = {
    [SF12 ]= DR_LORA_SF12,    // SF12 -> libloragw DR枚举
    [SF11 ]= DR_LORA_SF11,    // SF11 -> libloragw DR枚举
    [SF10 ]= DR_LORA_SF10,    // SF10 -> libloragw DR枚举
    [SF9  ]= DR_LORA_SF9,     // SF9  -> libloragw DR枚举
    [SF8  ]= DR_LORA_SF8,     // SF8  -> libloragw DR枚举
    [SF7  ]= DR_LORA_SF7,     // SF7  -> libloragw DR枚举
    [FSK  ]= DR_UNDEFINED,    // FSK  -> 未定义(FSK不使用DR)
    [SFNIL]= DR_UNDEFINED,    // 无效SF -> 未定义
};

//=============================================================================
// 带宽映射表
// 功能：将S2E内部BW枚举值映射到libloragw的BW枚举值
//=============================================================================
static const u1_t BW_MAP[] = {
    [BW125]= BW_125KHZ,       // 125kHz -> libloragw BW枚举
    [BW250]= BW_250KHZ,       // 250kHz -> libloragw BW枚举
    [BW500]= BW_500KHZ,       // 500kHz -> libloragw BW枚举
    [BWNIL]= BW_UNDEFINED     // 无效BW -> 未定义
};

//=============================================================================
// 函数功能：libloragw扩频因子转换为S2E内部SF值
// 参数说明：lgw_sf - libloragw的扩频因子枚举值
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
// 函数功能：libloragw带宽转换为S2E内部BW值
// 参数说明：lgw_bw - libloragw的带宽枚举值
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
// 函数功能：将libloragw接收包参数转换为S2E的RPS格式
// 参数说明：p - libloragw接收包结构体指针
// 返回值：S2E内部RPS参数值
// 调用时机：接收数据包处理时调用，转换硬件参数为内部格式
//=============================================================================
rps_t ral_lgw2rps (struct lgw_pkt_rx_s* p) {
    return p->modulation == MOD_LORA           // 判断调制方式
        ? rps_make(to_sf(p->datarate), to_bw(p->bandwidth))  // LoRa: 组合SF和BW
        : FSK;                                 // FSK: 直接返回FSK标识
}

//=============================================================================
// 函数功能：将S2E的RPS参数转换为libloragw发送包格式
// 参数说明：rps - S2E内部RPS参数，p - libloragw发送包结构体指针
// 返回值：无
// 调用时机：发送数据包前调用，将内部参数转换为硬件格式
//=============================================================================
void ral_rps2lgw (rps_t rps, struct lgw_pkt_tx_s* p) {
    assert(rps != RPS_ILLEGAL);                // 确保RPS参数有效
    if( rps_sf(rps) == FSK ) {                 // 如果是FSK调制
        p->modulation = MOD_FSK;               // 设置FSK调制
        p->datarate   = FSK_BAUD;              // 设置FSK波特率
        p->f_dev      = FSK_FDEV;              // 设置FSK频偏
        p->preamble   = FSK_PRMBL_LEN;         // 设置FSK前导码长度
    } else {                                   // 如果是LoRa调制
        p->modulation = MOD_LORA;              // 设置LoRa调制
        p->datarate   = SF_MAP[rps_sf(rps)];   // 设置扩频因子
        p->bandwidth  = BW_MAP[rps_bw(rps)];   // 设置带宽
    }
}

//=============================================================================
// 函数功能：从RPS参数中提取带宽信息(RAL接口实现)
// 参数说明：rps - S2E内部RPS参数
// 返回值：libloragw带宽枚举值
// 调用时机：需要获取带宽信息进行硬件配置时调用
//=============================================================================
int ral_rps2bw (rps_t rps) {
    assert(rps != RPS_ILLEGAL);                // 确保RPS参数有效
    return BW_MAP[rps_bw(rps)];                // 返回对应的libloragw带宽值
}

//=============================================================================
// 函数功能：从RPS参数中提取扩频因子信息(RAL接口实现)
// 参数说明：rps - S2E内部RPS参数
// 返回值：libloragw扩频因子枚举值
// 调用时机：需要获取扩频因子信息进行硬件配置时调用
//=============================================================================
int ral_rps2sf (rps_t rps) {
    assert(rps != RPS_ILLEGAL);                // 确保RPS参数有效
    return SF_MAP[rps_sf(rps)];                // 返回对应的libloragw扩频因子值
}

//=============================================================================
// 函数功能：进行时间同步测量，获取MCU时间与SX130X硬件时间的对应关系
// 参数说明：pps_en - PPS(每秒脉冲)使能标志，用于GPS时间同步
//          last_xtime - 上次读取的硬件时间戳指针，用于构建连续64位时间
//          timesync - 输出时间同步测量结果
// 返回值：测量质量指标(数值越小表示测量质量越好)
// 调用时机：定期调用以维持时间同步，支持精确的发送时间调度
// 算法原理：同时读取MCU时间和硬件计数器，计算时间差作为质量指标
//=============================================================================
int ral_getTimesync (u1_t pps_en, sL_t* last_xtime, timesync_t* timesync) {
    static u4_t last_pps_xticks;              // 上次PPS锁存的硬件时间戳
    u4_t pps_xticks = 0;                       // 当前PPS锁存的硬件时间戳
#if !defined(CFG_sx1302)
    if( pps_en ) {                             // 如果PPS功能启用
        // 首先读取上次锁存的PPS值，时间同步间隔需要>1秒以便PPS可能发生
        // 读取上次锁存值 - PPS发生时的时间。如果没有PPS发生，返回LGW_GPS_EN设为1时的时间
        lgw_get_trigcnt(&pps_xticks);          // 获取PPS锁存的计数器值
        // lgw1有单一的xtick寄存器，要么是PPS锁存的，要么是自由运行的
        // 我们临时禁用PPS锁存以获得自由运行的xtick
        lgw_reg_w(LGW_GPS_EN, 0);              // 禁用PPS锁存，保持当前值
    }
#endif
    ustime_t t0 = rt_getTime();                // 测量开始时的MCU时间
    u4_t xticks = 0;                           // 硬件计数器当前值
    // 获取当前自由运行的xtick
#if defined(CFG_sx1302)
    timestamp_counter_get(&counter_us, &xticks, &pps_xticks);  // SX1302: 同时获取计数器和PPS
#else
    lgw_get_trigcnt(&xticks);                  // SX1301: 获取当前计数器值
#endif
    ustime_t t1 = rt_getTime();                // 测量结束时的MCU时间
    sL_t d = (s4_t)(xticks - *last_xtime);     // 计算硬件时间差(处理32位溢出)
    if( d < 0 ) {                              // 如果发生了时间回卷
        LOG(MOD_SYN|CRITICAL,
            "SX130x time sync roll over - no update for a long time: xticks=0x%08x last_xtime=0x%lX",
            xticks, *last_xtime);              // 记录严重错误
        d += (sL_t)1<<32;                      // 修正32位溢出
    }
    timesync->xtime = *last_xtime += d;        // 更新连续64位硬件时间
    timesync->ustime = (t0+t1)/2;              // MCU时间取测量时间的中点
    timesync->pps_xtime = 0;                   // 将被设置如果pps_en有效且有有效的PPS观测
    if( pps_en ) {                             // 如果PPS功能启用
        // PPS锁存现在将保持当前xticks
#if !defined(CFG_sx1302)
        lgw_reg_w(LGW_GPS_EN, 1);              // 重新启用PPS锁存
#endif
        // 捕获PPS丢失的行为：
        //  - pps_xticks = 0 和 pps_xticks = 常数 都是非法的PPS观测
        //  - 通过 timesync->pps_xtime = 0 通知上层
        if( pps_xticks && last_pps_xticks != pps_xticks ) {  // 如果PPS有效且发生了变化
            timesync->pps_xtime = timesync->xtime + (s4_t)(pps_xticks - xticks);  // 计算PPS对应的xtime
            last_pps_xticks = pps_xticks;     // 更新上次PPS时间戳
        }
    }
    LOG(MOD_SYN|XDEBUG, "SYNC: ustime=0x%012lX (Q=%3d): xticks=0x%08x xtime=0x%lX - PPS: pps_xticks=0x%08x (%u) pps_xtime=0x%lX (pps_en=%d)",
        timesync->ustime, (int)(t1-t0), xticks, timesync->xtime, pps_xticks, pps_xticks, timesync->pps_xtime, pps_en);
    return (int)(t1-t0);                       // 返回测量耗时作为质量指标
}

#if defined(CFG_ral_lgw)
//=============================================================================
// 本地模式RAL实现的全局变量
// 功能：直接控制本地LoRa网关库，无需子进程
//=============================================================================
static u1_t       pps_en;        // PPS使能状态
static s2_t       txpowAdjust;   // 发送功率调整值(按TXPOW_SCALE缩放)
static sL_t       last_xtime;    // 上次硬件时间戳
static tmr_t      rxpollTmr;     // 接收轮询定时器
static tmr_t      syncTmr;       // 时间同步定时器

//=============================================================================
// 函数功能：时间同步定时器处理函数
// 参数说明：tmr - 定时器结构体指针
// 返回值：无
// 调用时机：定期调用以维持系统时间同步
// 执行逻辑：获取时间同步测量，更新时间同步状态，设置下次同步时间
//=============================================================================
static void synctime (tmr_t* tmr) {
    timesync_t timesync;                       // 时间同步测量结果
    int quality = ral_getTimesync(pps_en, &last_xtime, &timesync);  // 进行时间同步测量
    ustime_t delay = ts_updateTimesync(0, quality, &timesync);      // 更新时间同步状态，获取下次延迟
    rt_setTimer(&syncTmr, rt_micros_ahead(delay));                  // 设置下次同步定时器
}

//=============================================================================
// 函数功能：获取备选天线配置(本地模式实现)
// 参数说明：txunit - 发送单元编号
// 返回值：备选天线位图(本地模式始终返回0，无备选天线)
// 调用时机：天线分集或多天线选择场景中调用
//=============================================================================
u1_t ral_altAntennas (u1_t txunit) {
    return 0;                                  // 本地模式无备选天线
}

//=============================================================================
// 函数功能：执行下行数据包发送
// 参数说明：txjob - 发送任务结构体，包含所有发送参数
//          s2ctx - S2E上下文，包含发送数据
//          nocca - 是否跳过信道清除评估(当前libloragw API无法实现)
// 返回值：发送结果状态码(RAL_TX_OK/RAL_TX_FAIL/RAL_TX_NOCA)
// 调用时机：需要发送下行数据包时调用
// 执行逻辑：将S2E发送参数转换为libloragw格式，调用硬件发送函数
//=============================================================================
int ral_tx (txjob_t* txjob, s2ctx_t* s2ctx, int nocca) {
    struct lgw_pkt_tx_s pkt_tx;                // libloragw发送包结构体
    memset(&pkt_tx, 0, sizeof(pkt_tx));        // 清零初始化

    // 设置LoRa发送的默认参数
    pkt_tx.invert_pol = true;                  // 默认启用极性反转
    pkt_tx.no_header  = false;                 // 默认包含LoRa头部

    if( txjob->preamble == 0 ) {               // 如果未指定前导码长度
        if( txjob->txflags & TXFLAG_BCN ) {    // 如果是信标发送
            pkt_tx.tx_mode = ON_GPS;           // 使用GPS时间触发
            pkt_tx.preamble = 10;              // 信标前导码长度10符号
            pkt_tx.invert_pol = false;         // 信标不反转极性
            pkt_tx.no_header  = true;          // 信标无LoRa头部
        } else {                               // 普通数据发送
            pkt_tx.tx_mode = TIMESTAMPED;      // 使用时间戳触发
            pkt_tx.preamble = 8;               // 默认前导码长度8符号
        }
    } else {                                   // 如果指定了前导码长度
        pkt_tx.preamble = txjob->preamble;     // 使用指定的前导码长度
    }
    rps_t rps = s2e_dr2rps(s2ctx, txjob->dr);  // 将数据速率转换为RPS参数
    ral_rps2lgw(rps, &pkt_tx);                 // 将RPS转换为libloragw格式
    pkt_tx.freq_hz    = txjob->freq;           // 设置发送频率
    pkt_tx.count_us   = txjob->xtime;          // 设置发送时间戳
    pkt_tx.rf_chain   = 0;                     // 使用射频链0
    pkt_tx.rf_power   = (float)(txjob->txpow - txpowAdjust) / TXPOW_SCALE;  // 计算发送功率(考虑校准偏移)
    pkt_tx.coderate   = CR_LORA_4_5;           // 设置编码率4/5
    pkt_tx.no_crc     = !txjob->addcrc;        // 设置CRC标志
    pkt_tx.size       = txjob->len;            // 设置数据包长度
    memcpy(pkt_tx.payload, &s2ctx->txq.txdata[txjob->off], pkt_tx.size);  // 复制发送数据

    // 注意：当前libloragw API无法实现nocca参数
#if defined(CFG_sx1302)
    int err = lgw_send(&pkt_tx);               // SX1302发送(传递指针)
#else
    int err = lgw_send(pkt_tx);                // SX1301发送(传递值)
#endif
    if( err != LGW_HAL_SUCCESS ) {             // 如果发送失败
        if( err != LGW_LBT_ISSUE ) {           // 如果不是LBT问题
            LOG(MOD_RAL|ERROR, "lgw_send failed");  // 记录一般发送错误
            return RAL_TX_FAIL;                // 返回发送失败
        }
        return RAL_TX_NOCA;                    // 返回信道接入失败
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
    u1_t status;                               // libloragw状态值
#if defined(CFG_sx1302)
    int err = lgw_status(txunit, TX_STATUS, &status);  // SX1302查询状态(支持多单元)
#else
    int err = lgw_status(TX_STATUS, &status);  // SX1301查询状态(单一单元)
#endif
    if (err != LGW_HAL_SUCCESS) {              // 如果查询失败
        LOG(MOD_RAL|ERROR, "lgw_status failed");  // 记录错误
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
#if defined(CFG_sx1302)
    lgw_abort_tx(txunit);                      // SX1302中止发送(支持多单元)
#else
    lgw_abort_tx();                            // SX1301中止发送(单一单元)
#endif
}

//=============================================================================
// 函数功能：记录原始接收数据包的详细信息
// 参数说明：level - 日志级别，msg - 日志消息前缀，pkt_rx - 接收包结构体指针
// 返回值：无
// 调用时机：调试时记录接收包的详细参数信息
//=============================================================================
static void log_rawpkt(u1_t level, str_t msg, struct lgw_pkt_rx_s * pkt_rx) {
    LOG(MOD_RAL|level, "%s[CRC %s] %^.3F %.2f/%.1f %R (mod=%d/dr=%d/bw=%d) xtick=%08x (%u) %d bytes: %64H",
        msg,                                   // 消息前缀
        pkt_rx->status == STAT_CRC_OK ? "OK"  : "FAIL",  // CRC状态
        pkt_rx->freq_hz,                       // 接收频率
        pkt_rx->snr,                           // 信噪比
#if defined(CFG_sx1302)
        pkt_rx->rssis,                         // SX1302: RSSIS值
#else
        pkt_rx->rssi,                          // SX1301: RSSI值
#endif
        ral_lgw2rps(pkt_rx),                   // 转换后的RPS参数
        pkt_rx->modulation,                    // 调制方式
        pkt_rx->datarate,                      // 数据速率
        pkt_rx->bandwidth,                     // 带宽
        pkt_rx->count_us,                      // 硬件时间戳(重复显示)
        pkt_rx->count_us,                      // 硬件时间戳
        pkt_rx->size,                          // 数据包大小
        pkt_rx->size, pkt_rx->payload          // 数据包内容(十六进制显示)
    );
}

//=============================================================================
// 函数功能：接收轮询定时器处理函数，定期检查并处理接收到的数据包
// 参数说明：tmr - 定时器结构体指针
// 返回值：无
// 调用时机：定期调用以处理接收数据包
// 执行逻辑：批量接收数据包，转换格式，加入接收队列，刷新给上层
//=============================================================================
static void rxpolling (tmr_t* tmr) {
    int rounds = 0;                            // 轮询轮次计数
    while(rounds++ < RAL_MAX_RXBURST) {        // 限制单次轮询的最大包数
        struct lgw_pkt_rx_s pkt_rx;            // libloragw接收包结构体
        int n = lgw_receive(1, &pkt_rx);       // 尝试接收1个数据包
        if( n < 0 || n > 1 ) {                 // 如果接收函数返回错误
            LOG(MOD_RAL|ERROR, "lgw_receive error: %d", n);  // 记录接收错误
            break;                             // 停止轮询
        }
        if( n==0 ) {                           // 如果没有新数据包
            break;                             // 停止轮询
        }

        rxjob_t* rxjob = !TC ? NULL : s2e_nextRxjob(&TC->s2ctx);  // 获取下一个接收作业槽位
        if( rxjob == NULL ) {                  // 如果接收队列已满
            log_rawpkt(ERROR, "Dropped RX frame - out of space: ", &pkt_rx);  // 记录丢包错误
            break;                             // 允许刷新接收作业
        }
        if( pkt_rx.status != STAT_CRC_OK ) {   // 如果CRC校验失败
            if( log_shallLog(MOD_RAL|DEBUG) ) { // 如果启用了DEBUG日志
                log_rawpkt(DEBUG, "", &pkt_rx); // 记录调试信息
            }
            continue;                          // 静默忽略坏CRC包
        }
        if( pkt_rx.size > MAX_RXFRAME_LEN ) {  // 如果包长度超过限制
            // 这种情况不应该发生，因为调用者提供了最大帧长度的空间 - 255字节
            log_rawpkt(ERROR, "Dropped RX frame - frame size too large: ", &pkt_rx);  // 记录错误
            continue;                          // 跳过此包
        }

        // 将接收数据复制到接收作业
        memcpy(&TC->s2ctx.rxq.rxdata[rxjob->off], pkt_rx.payload, pkt_rx.size);  // 复制数据载荷
        rxjob->len   = pkt_rx.size;            // 设置包长度
        rxjob->freq  = pkt_rx.freq_hz;         // 设置接收频率
        rxjob->xtime = ts_xticks2xtime(pkt_rx.count_us, last_xtime);  // 转换硬件时间戳为xtime格式
#if defined(CFG_sx1302)
        rxjob->rssi  = (u1_t)-pkt_rx.rssis;    // SX1302: 设置RSSI(转换为正值)
#else
        rxjob->rssi  = (u1_t)-pkt_rx.rssi;     // SX1301: 设置RSSI(转换为正值)
#endif
        rxjob->snr   = (s1_t)(pkt_rx.snr*4);   // 设置SNR(放大4倍保存为整数)
        rps_t rps = ral_lgw2rps(&pkt_rx);      // 转换RPS参数
        rxjob->dr = s2e_rps2dr(&TC->s2ctx, rps);  // 将RPS转换为数据速率
        if( rxjob->dr == DR_ILLEGAL ) {        // 如果数据速率转换失败
            log_rawpkt(ERROR, "Dropped RX frame - unable to map to an up DR: ", &pkt_rx);  // 记录错误
            continue;                          // 跳过此包
        }

        if( log_shallLog(MOD_RAL|XDEBUG) ) {   // 如果启用了扩展调试日志
            log_rawpkt(XDEBUG, "", &pkt_rx);   // 记录详细调试信息
        }

        s2e_addRxjob(&TC->s2ctx, rxjob);       // 将接收作业加入队列
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
    if( strcmp(hwspec, "sx1301/1") != 0 ) {    // 检查硬件规格是否支持
        LOG(MOD_RAL|ERROR, "Unsupported hwspec=%s", hwspec);  // 记录不支持的硬件规格
        return 0;                              // 返回失败
    }
    ujdec_t D;                                 // JSON解码器
    uj_iniDecoder(&D, json, jsonlen);          // 初始化JSON解码器
    if( uj_decode(&D) ) {                      // 如果JSON解析失败
        LOG(MOD_RAL|ERROR, "Parsing of sx130x channel setup JSON failed");  // 记录解析错误
        return 0;                              // 返回失败
    }
    if( uj_null(&D) ) {                        // 如果配置为null
        LOG(MOD_RAL|ERROR, "sx130x_conf is null but a hw setup IS required - no fallbacks");  // 记录错误
        return 0;                              // 返回失败
    }
    uj_enterArray(&D);                         // 进入JSON数组
    int ok=0, slaveIdx;                        // 成功标志和从机索引
    while( (slaveIdx = uj_nextSlot(&D)) >= 0 ) {  // 遍历配置数组
        dbuf_t json = uj_skipValue(&D);        // 获取配置项的JSON数据
        if( slaveIdx == 0 ) {                  // 只处理第一个配置项(本地模式)
            struct sx130xconf sx130xconf;      // SX130X配置结构体
            int status = 0;                    // 状态码

            // 按阶段执行配置，记录每个阶段的失败状态
            if( (status = !sx130xconf_parse_setup(&sx130xconf, -1, hwspec, json.buf, json.bufsize) << 0) ||  // 解析配置
                (status = !sx130xconf_challoc(&sx130xconf, upchs)    << 1) ||  // 分配信道
                (status = !sys_runRadioInit(sx130xconf.device)       << 2) ||  // 运行初始化脚本
                (status = !sx130xconf_start(&sx130xconf, cca_region) << 3) ) { // 启动硬件
                LOG(MOD_RAL|ERROR, "ral_config failed with status 0x%02x", status);  // 记录具体失败阶段
            } else {
                // 无线电启动成功，保存配置参数
                txpowAdjust = sx130xconf.txpowAdjust;   // 保存发送功率调整值
                pps_en = sx130xconf.pps;                // 保存PPS使能状态
                last_xtime = ts_newXtimeSession(0);     // 初始化新的时间会话
                rt_yieldTo(&rxpollTmr, rxpolling);      // 启动接收轮询定时器
                rt_yieldTo(&syncTmr, synctime);         // 启动时间同步定时器
                ok = 1;                                 // 标记成功
            }
        }
    }
    uj_exitArray(&D);                          // 退出JSON数组
    uj_assertEOF(&D);                          // 确保JSON结束
    return ok;                                 // 返回配置结果
}

//=============================================================================
// 函数功能：初始化无线电抽象层(本地模式)
// 参数说明：无
// 返回值：无
// 调用时机：系统启动时，在配置无线电硬件之前调用
// 执行逻辑：初始化时间状态和定时器，为后续配置做准备
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
// 执行逻辑：停止定时器，重置状态，关闭libloragw
//=============================================================================
void ral_stop() {
    rt_clrTimer(&syncTmr);                     // 清除时间同步定时器
    last_xtime = 0;                            // 重置硬件时间戳
    rt_clrTimer(&rxpollTmr);                   // 清除接收轮询定时器
    lgw_stop();                                // 停止libloragw
}

#endif // defined(CFG_ral_lgw)
#endif // defined(CFG_lgw1)