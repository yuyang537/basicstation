/*
 * S2E - Station-to-Engine协议层头文件
 * ===================================
 * 功能：BasicStation的核心LoRaWAN协议处理引擎
 * 核心：帧解析、队列管理、区域参数、信标生成、时间管理
 */

#ifndef _s2e_h_
#define _s2e_h_

#include "s2conf.h"  // S2配置管理
#include "sys.h"     // 系统核心服务
#include "uj.h"      // JSON解析器
#include "xq.h"      // 扩展队列管理
#include "ws.h"      // WebSocket通信

// 全局过滤器
extern uL_t* s2e_joineuiFilter;  // JoinEUI过滤器：过滤不需要的Join请求
extern u4_t  s2e_netidFilter[4]; // NetID过滤器：网络ID位掩码过滤

// LoRa帧处理API
int  s2e_parse_lora_frame(ujbuf_t* buf, const u1_t* frame , int len, dbuf_t* lbuf);
     // 解析LoRa帧：将原始LoRa帧解析为JSON格式
void s2e_make_beacon (uint8_t* layout, sL_t epoch_secs, int infodesc, double lat, double lon, uint8_t* buf);
     // 生成信标：创建Class B信标帧

// LoRa调制参数枚举
enum { SF12, SF11, SF10, SF9, SF8, SF7, FSK, SFNIL };  // 扩频因子：LoRa调制的扩频因子选项
enum { BW125, BW250, BW500, BWNIL };                   // 带宽：LoRa调制的带宽选项
enum { RPS_DNONLY = 0x20 };                           // 仅下行：仅用于下行传输的RPS标志
enum { RPS_BCN = 0x40 };                              // 信标：信标传输的RPS标志
enum { RPS_ILLEGAL = 0xFF };                          // 非法：无效的RPS值
enum { RPS_FSK = FSK };                               // FSK：FSK调制类型

typedef u1_t rps_t;  // 无线参数集：编码调制参数的8位值

// RPS参数操作内联函数
inline int   rps_sf   (rps_t params) { return params &  0x7; }        // 提取扩频因子：从RPS中提取SF值
inline int   rps_bw   (rps_t params) { return (params >> 3) & 0x3; }  // 提取带宽：从RPS中提取BW值
inline rps_t rps_make (int sf, int bw) { return (sf&7) | ((bw&3)<<3); } // 构造RPS：由SF和BW构造RPS值

// 无线发送状态枚举
enum {
    TXSTATUS_IDLE,       // 空闲状态：无发送操作
    TXSTATUS_SCHEDULED,  // 已调度：发送已调度但未开始
    TXSTATUS_EMITTING,   // 发送中：正在发送数据
};

// 发送作业标志枚举
enum {
    TXFLAG_TXING     = 0x01,  // 发送中：正在执行发送操作
    TXFLAG_TXCHECKED = 0x02,  // 已检查：发送状态已检查
    TXFLAG_CLSA      = 0x04,  // Class A：Class A下行窗口
    TXFLAG_PING      = 0x08,  // Ping槽：Class B Ping槽传输
    TXFLAG_CLSC      = 0x10,  // Class C：Class C连续接收窗口
    TXFLAG_BCN       = 0x20,  // 信标：Class B信标传输
};

// 发送条件枚举
enum {
    TXCOND_CANTX = 0,  // 可发送：无限制可以发送
    TXCOND_CCA,        // CCA检查：仅在信道空闲时可发送
    TXCOND_NOCA,       // 无信道访问：无法访问信道
    TXCOND_NODC,       // 无占空比：绝对没有占空比
};

// 优先级惩罚常量
enum { PRIO_PENALTY_ALTTXTIME  =  10 };  // 备用发送时间惩罚：使用备用时间的优先级惩罚
enum { PRIO_PENALTY_ALTANTENNA =  10 };  // 备用天线惩罚：使用备用天线的优先级惩罚
enum { PRIO_PENALTY_CCA        =   8 };  // CCA惩罚：需要CCA检查的优先级惩罚
enum { PRIO_BEACON             = 128 };  // 信标优先级：信标传输的高优先级

// 占空比和信道常量
enum { DC_DECI, DC_CENTI, DC_MILLI, DC_NUM_BANDS }; // 占空比频段：十分之一、百分之一、千分之一占空比频段
enum { MAX_DNCHNLS = 48 };                          // 最大下行信道：支持的最大下行信道数
enum { MAX_UPCHNLS = MAX_130X * 10 };               // 最大上行信道：每个芯片10个信道
enum { DR_CNT = 16 };                               // 数据速率数量：支持的数据速率总数
enum { DR_ILLEGAL = 16 };                           // 非法数据速率：无效的DR值

/**
 * struct s2txunit - S2发送单元结构
 * 功能：管理单个发送单元的占空比和调度
 */
typedef struct s2txunit {
    ustime_t dc_eu868bands[DC_NUM_BANDS];  // EU868频段占空比：各频段的占空比限制时间
    ustime_t dc_perChnl[MAX_DNCHNLS+1];    // 每信道占空比：每个信道的占空比限制时间
    txidx_t  head;                         // 队列头：发送队列的头部索引
    tmr_t    timer;                        // 定时器：占空比和调度定时器
} s2txunit_t;

// 信标状态枚举
enum {
    BCNING_OK     = 0x00,  // 信标正常：信标功能启用且运行正常
    BCNING_NOTIME = 0x01,  // 无时间：缺少GPS/PPS时间同步
    BCNING_NOPOS  = 0x02   // 无位置：缺少网关位置信息
};

/**
 * struct s2bcn - S2信标结构
 * 功能：管理Class B信标的配置和状态
 */
typedef struct s2bcn {
    u1_t     state;        // 状态：跟踪信标失败状态
    u1_t     ctrl;         // 控制：0x0F=>DR，0xF0=频率数量
    u1_t     layout[3];    // 布局：时间偏移、信息描述偏移、信标长度
    u4_t     freqs[8];     // 频率：1个或最多8个信标频率
} s2bcn_t;

/**
 * struct s2ctx - S2上下文主结构
 * 功能：S2E协议处理的核心上下文，包含所有状态和配置
 */
typedef struct s2ctx {
    // WebSocket通信函数指针（连接到TC/WebSocket）
    dbuf_t (*getSendbuf) (struct s2ctx* s2ctx, int minsize);     // 获取发送缓冲区
    void   (*sendText)   (struct s2ctx* s2ctx, dbuf_t* buf);     // 发送文本数据
    void   (*sendBinary) (struct s2ctx* s2ctx, dbuf_t* buf);     // 发送二进制数据
    int    (*canTx)      (struct s2ctx* s2ctx, txjob_t* txjob, int* ccaDisabled); // 区域相关的发送检查

    u1_t     ccaEnabled;     // CCA启用：此区域是否使用CCA检测
    rps_t    dr_defs[DR_CNT]; // 数据速率定义：每个DR的RPS参数映射
    u2_t     dc_chnlRate;    // 信道占空比：信道占空比限制百分比
    u4_t     dn_chnls[MAX_DNCHNLS+1]; // 下行信道：下行传输可用的频率列表
    u4_t     min_freq;       // 最小频率：区域允许的最小频率
    u4_t     max_freq;       // 最大频率：区域允许的最大频率
    s2_t     txpow;          // 默认发送功率：标准TX功率设置
    s2_t     txpow2;         // 特殊发送功率：特定频率范围的TX功率/0表示不适用
    u4_t     txpow2_freq[2]; // 功率2频率范围：txpow2适用的频率范围/0,0表示无范围
    ujcrc_t  region;         // 区域：区域标识符的CRC值
    char     region_s[16];   // 区域字符串：区域名称的字符串形式
    txq_t    txq;            // 发送队列：TX作业队列
    rxq_t    rxq;            // 接收队列：RX作业队列
    double   muxtime;        // MUXS时间：来自MUXS的时间戳
    ustime_t reftime;        // 参考时间：muxtime到达时的本地时间
    s2txunit_t txunits[MAX_TXUNITS]; // 发送单元：多个发送单元的数组
    s2bcn_t    bcn;          // 信标定义：Class B信标配置
    tmr_t      bcntimer;     // 信标定时器：信标传输定时器
} s2ctx_t;

// 全局开关：跨TC会话启用/禁用特定功能（也支持交互式操作，无需重置/更改参数）
extern u1_t s2e_dcDisabled;    // 禁用占空比：忽略占空比限制（测试/开发覆盖）
extern u1_t s2e_ccaDisabled;   // 禁用CCA：忽略繁忙信道（测试/开发覆盖）
extern u1_t s2e_dwellDisabled; // 禁用驻留时间：忽略驻留时间限制（测试/开发覆盖）

// S2E核心API函数
rps_t    s2e_dr2rps (s2ctx_t*, u1_t dr);              // DR转RPS：将数据速率转换为无线参数
u1_t     s2e_rps2dr (s2ctx_t*, rps_t rps);            // RPS转DR：将无线参数转换为数据速率
ustime_t s2e_calcUpAirTime (rps_t rps, u1_t plen);    // 计算上行空中时间：计算数据包传输时间
ustime_t s2e_calcDnAirTime (rps_t rps, u1_t plen, u1_t lcrc, u2_t preamble); // 计算下行空中时间
ustime_t s2e_updateMuxtime(s2ctx_t* s2ctx, double muxstime, ustime_t now);    // 更新MUXS时间：now=0时使用rt_getTime()

// S2E生命周期管理
void     s2e_ini          (s2ctx_t*);                  // 初始化S2上下文：设置默认值和状态
void     s2e_free         (s2ctx_t*);                  // 释放S2上下文：清理资源和内存
void     s2e_enableDC     (s2ctx_t*, u2_t chnlRate);   // 启用占空比：设置信道占空比限制
void     s2e_disableDC    (s2ctx_t*);                  // 禁用占空比：关闭占空比检查

// 队列管理API
rxjob_t* s2e_nextRxjob    (s2ctx_t*);                  // 获取下一个RX作业：从RX队列取出作业
void     s2e_addRxjob     (s2ctx_t*, rxjob_t* rxjob);  // 添加RX作业：将RX作业加入队列
void     s2e_flushRxjobs  (s2ctx_t*);                  // 刷新RX作业：清空RX队列

// 消息处理API
int      s2e_onMsg        (s2ctx_t*, char* json, ujoff_t jsonlen);    // 处理消息：处理来自LNS的JSON消息
int      s2e_onBinary     (s2ctx_t*, u1_t* data, ujoff_t datalen);    // 处理二进制：处理二进制数据

// 调度和命令处理API
ustime_t s2e_nextTxAction (s2ctx_t*, u1_t txunit);                    // 下一个TX动作：获取下次发送时间
int      s2e_handleCommands (ujcrc_t msgtype, s2ctx_t* s2ctx, ujdec_t* D);  // 处理命令：处理各种S2E命令
void     s2e_handleRmtsh    (s2ctx_t* s2ctx, ujdec_t* D);             // 处理远程Shell：处理rmtsh命令

#endif // _s2e_h_
