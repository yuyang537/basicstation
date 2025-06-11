//=============================================================================
// 文件功能：LoRa网关仿真器模块 - 为BasicStation提供虚拟LoRa硬件模拟
// 作用说明：在测试环境中替代真实LoRa网关硬件，支持以下核心功能：
//          1. 双API兼容支持：同时模拟libloragw(LGW1)和sx1301ar(LGW2)硬件接口
//          2. Unix域套接字通信：通过套接字与外部仿真环境进行数据交换
//          3. 数据包收发仿真：模拟LoRa数据包的发送、接收和缓冲管理
//          4. CCA(Clear Channel Assessment)模拟：模拟信道评估和干扰检测
//          5. 时间戳仿真：模拟硬件时间戳和GPS同步功能
//          6. 空中时间计算：精确计算LoRa数据包的传输时间
//          7. 配置接口仿真：模拟所有硬件配置和参数设置API
//          8. 发送状态跟踪：模拟发送任务的调度、执行和完成状态
// 
// 技术特点：
//          - 基于异步IO的事件驱动架构，无阻塞操作
//          - 环形缓冲区设计，高效处理大量数据包
//          - 智能重连机制，自动恢复套接字连接
//          - 精确的空中时间计算，符合LoRaWAN规范
//          - CCA冲突检测，模拟真实射频环境
//          - 时间偏移机制，增强测试覆盖率
//          - 模块化配置系统，支持多种硬件配置
//=============================================================================

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

#if defined(CFG_lgwsim)                       // 仅在启用LoRa网关仿真配置时编译此模块
// LCOV_EXCL_START                           // 排除代码覆盖率统计，因为这是测试专用代码

//=============================================================================
// 系统头文件包含 - 提供基础系统功能
//=============================================================================
#include <stdio.h>                            // 标准输入输出操作
#include <time.h>                             // 时间处理函数
#include <unistd.h>                           // Unix标准函数
#include <fcntl.h>                            // 文件控制操作
#include <errno.h>                            // 错误码定义
#include <sys/socket.h>                       // 套接字通信功能
#include <sys/un.h>                           // Unix域套接字结构

//=============================================================================
// 硬件库头文件包含 - 根据配置选择对应的LoRa硬件接口
//=============================================================================
#if defined(CFG_lgw1)                         // 如果配置为LGW1(libloragw)
#include "lgw/loragw_reg.h"                   // LoRa网关寄存器定义
#include "lgw/loragw_hal.h"                   // LoRa网关硬件抽象层
#include "lgw/loragw_fpga.h"                  // FPGA相关定义
#include "lgw/loragw_lbt.h"                   // Listen Before Talk功能
#elif defined(CFG_lgw2)                       // 如果配置为LGW2(sx1301ar)
#include "lgw2/sx1301ar_hal.h"                // SX1301AR硬件抽象层
#include "lgw2/sx1301ar_err.h"                // SX1301AR错误定义
#include "lgw2/sx1301ar_gps.h"                // SX1301AR GPS功能
#include "lgw2/sx1301ar_dsp.h"                // SX1301AR DSP功能
#endif

//=============================================================================
// BasicStation模块头文件包含
//=============================================================================
#include "rt.h"                               // 实时任务和定时器系统
#include "s2e.h"                              // Station-to-Endpoint核心功能
#include "sys.h"                              // 系统服务接口

#include "sys_linux.h"                        // Linux平台特定功能

//=============================================================================
// 仿真器配置常量定义
//=============================================================================
#define MAX_CCA_INFOS   10                    // CCA信息最大数量：支持10个并发信道评估
#define MAGIC_CCA_FREQ  0xCCAFCCAF            // CCA魔数标识：用于识别CCA控制消息

#define RX_NPKTS 1000                         // 接收数据包缓冲区容量：1000个数据包

//=============================================================================
// 数据结构定义 - CCA(Clear Channel Assessment)信道评估
//=============================================================================

//=============================================================================
// 结构功能：单个CCA信道评估信息
// 成员说明：freq - 频率(Hz)，beg - 开始时间，end - 结束时间
// 用途说明：定义某个频率在特定时间段内的信道占用状态
//=============================================================================
struct cca_info {
    u4_t freq;                                // 信道频率(Hz) - 标识被评估的频率点
    sL_t beg;                                 // 占用开始时间(微秒) - 信道忙碌起始时刻
    sL_t end;                                 // 占用结束时间(微秒) - 信道忙碌结束时刻
};

//=============================================================================
// 结构功能：CCA信息批量传输消息
// 成员说明：magic - 消息类型标识，infos - CCA信息数组
// 用途说明：通过套接字传输多个CCA评估结果，模拟射频环境干扰
//=============================================================================
struct cca_msg {
    u4_t magic;                               // 魔数标识(0xCCAFCCAF) - 标识这是CCA控制消息
    struct cca_info infos[MAX_CCA_INFOS];     // CCA信息数组 - 包含多个频率的占用状态
};

//=============================================================================
// 全局变量定义 - 根据硬件库版本选择对应的数据结构
//=============================================================================
#if defined(CFG_lgw1)                         // LGW1版本全局变量
static struct lgw_pkt_tx_s tx_pkt;            // 发送数据包缓冲区 - 当前待发送的数据包
static struct lgw_pkt_rx_s rx_pkts[RX_NPKTS+1]; // 接收数据包环形缓冲区 - 存储接收到的数据包
static u1_t     ppsLatched;                   // PPS锁存状态 - GPS秒脉冲同步标志
#elif defined(CFG_lgw2)                       // LGW2版本全局变量
static sx1301ar_tx_pkt_t tx_pkt;              // 发送数据包缓冲区 - 当前待发送的数据包
static sx1301ar_rx_pkt_t rx_pkts[RX_NPKTS+1]; // 接收数据包环形缓冲区 - 存储接收到的数据包
#endif

//=============================================================================
// 仿真器全局状态变量
//=============================================================================
static sL_t     timeOffset;                   // 时间偏移量 - 用于与系统时间区分，增强测试覆盖
static sL_t     txbeg;                        // 发送开始时间 - 记录当前发送任务的开始时刻
static sL_t     txend;                        // 发送结束时间 - 记录当前发送任务的结束时刻
static int      rxblen  = sizeof(rx_pkts[0])*RX_NPKTS; // 接收缓冲区总字节数
static int      rx_ridx = 0;                  // 接收缓冲区读索引 - 指向下一个要读取的数据包
static int      rx_widx = 0;                  // 接收缓冲区写索引 - 指向下一个要写入的位置
static u4_t     rx_dsc = 0;                   // 接收丢弃计数器 - 缓冲区满时丢弃的字节数
static aio_t*   aio;                          // 异步IO对象指针 - 管理套接字通信
static tmr_t    conn_tmr;                     // 连接定时器 - 用于重连机制
static struct sockaddr_un sockAddr;           // Unix域套接字地址结构
static struct cca_msg     cca_msg;            // CCA消息缓冲区 - 存储最新的信道评估信息

//=============================================================================
// 兼容性全局变量 - 保持与真实硬件库的接口一致性
//=============================================================================
uint8_t lgwx_device_mode = 0;                 // 设备模式标识 - 兼容libloragw接口
uint8_t lgwx_beacon_len = 0;                  // 信标长度配置 - 用于Class B信标
uint8_t lgwx_beacon_sf = 0;                   // 信标扩频因子 - 信标传输参数
uint8_t lgwx_lbt_mode = 0;                    // LBT模式配置 - Listen Before Talk功能

//=============================================================================
// 环形缓冲区操作宏定义
//=============================================================================
// 宏功能：计算环形缓冲区的可用空间
// 参数说明：widx - 写索引，ridx - 读索引，len - 缓冲区总长度
// 返回值：可写入的字节数
#define rbfree(widx,ridx,len) (widx >= ridx ? len-widx : ridx-widx-1)

// 宏功能：计算环形缓冲区的已用空间
// 参数说明：widx - 写索引，ridx - 读索引，len - 缓冲区总长度  
// 返回值：已存储的字节数
#define rbused(widx,ridx,len) (widx >= ridx ? widx-ridx : len-ridx+widx)

//=============================================================================
// 函数功能：CCA(Clear Channel Assessment)信道评估检查
// 参数说明：txtime - 发送时间戳，txfreq - 发送频率
// 返回值：1表示信道可用，0表示信道被占用
// 调用时机：发送数据包前检查信道是否空闲
// 算法逻辑：遍历所有CCA信息，检查发送时间是否与任何占用时段冲突
//=============================================================================
static int cca (sL_t txtime, u4_t txfreq) {
    for( int i=0; i<MAX_CCA_INFOS; i++ ) {    // 遍历所有CCA信息条目
        u4_t freq = cca_msg.infos[i].freq;    // 获取当前条目的频率
        if( freq == 0 )                       // 如果频率为0，表示列表结束
            break;                            // 退出循环
        if( txfreq == freq &&                 // 如果发送频率与CCA频率匹配
            txtime >= cca_msg.infos[i].beg && // 且发送时间在占用时段内
            txtime <= cca_msg.infos[i].end ) {
            return 0;                         // 返回信道被占用
        }
    }
    return 1;                                 // 所有检查通过，返回信道可用
}

//=============================================================================
// 函数功能：获取仿真器内部时间戳(xticks)
// 参数说明：无
// 返回值：调整后的时间戳(微秒)
// 调用时机：需要获取硬件时间戳时调用
// 设计目的：创建与系统时间不同的时间基准，提高测试覆盖率
//=============================================================================
static sL_t xticks () {
    // 通过减去时间偏移量，使xticks与ustime_t不同，增加测试覆盖范围
    return sys_time() - timeOffset;           // 返回相对于偏移基准的当前时间
}

//=============================================================================
// 函数功能：计算LoRa数据包的空中传输时间
// 参数说明：datarate - 数据速率，bandwidth - 带宽，plen - 数据包长度
// 返回值：空中传输时间(微秒)
// 调用时机：发送数据包时计算传输持续时间，用于CCA和状态跟踪
// 转换逻辑：将硬件库的参数格式转换为S2E内部格式，然后计算空中时间
//=============================================================================
static u4_t airtime (int datarate, int bandwidth, int plen) {
    int sf, bw;                               // 内部格式的扩频因子和带宽变量
#if defined(CFG_lgw1)                         // LGW1版本的参数转换
    switch(bandwidth) {                       // 带宽参数转换
    case BW_125KHZ: bw = BW125; break;        // 125kHz带宽
    case BW_250KHZ: bw = BW250; break;        // 250kHz带宽  
    case BW_500KHZ: bw = BW500; break;        // 500kHz带宽
    }
    switch(datarate) {                        // 数据速率(扩频因子)转换
    case DR_LORA_SF12: sf = SF12; break;      // SF12扩频因子
    case DR_LORA_SF11: sf = SF11; break;      // SF11扩频因子
    case DR_LORA_SF10: sf = SF10; break;      // SF10扩频因子
    case DR_LORA_SF9 : sf = SF9 ; break;      // SF9扩频因子
    case DR_LORA_SF8 : sf = SF8 ; break;      // SF8扩频因子
    case DR_LORA_SF7 : sf = SF7 ; break;      // SF7扩频因子
    }
#elif defined(CFG_lgw2)                       // LGW2版本的参数转换
    switch(bandwidth) {                       // 带宽参数转换
    case BW_125K: bw = BW125; break;          // 125kHz带宽
    case BW_250K: bw = BW250; break;          // 250kHz带宽
    case BW_500K: bw = BW500; break;          // 500kHz带宽
    default: bw = BWNIL; break;               // 无效带宽处理
    }
    switch(datarate) {                        // 调制速率转换
    case MR_SF12: sf = SF12; break;           // SF12扩频因子
    case MR_SF11: sf = SF11; break;           // SF11扩频因子
    case MR_SF10: sf = SF10; break;           // SF10扩频因子
    case MR_SF9 : sf = SF9 ; break;           // SF9扩频因子
    case MR_SF8 : sf = SF8 ; break;           // SF8扩频因子
    case MR_SF7 : sf = SF7 ; break;           // SF7扩频因子
    }
#endif
    // 调用S2E模块的空中时间计算函数，使用转换后的参数
    return s2e_calcDnAirTime(rps_make(sf,bw), plen, /*addcrc*/0, /*preamble*/0);
}

//=============================================================================
// 异步IO回调函数声明
//=============================================================================
static void read_socket (aio_t* aio);        // 套接字读取回调函数
static void write_socket (aio_t* aio);       // 套接字写入回调函数

//=============================================================================
// 函数功能：尝试连接到硬件仿真器
// 参数说明：tmr - 连接定时器
// 返回值：无
// 调用时机：当套接字连接断开时调用
// 算法逻辑：关闭当前连接，尝试重新连接，直到成功
//=============================================================================
//=============================================================================
// 函数功能：尝试连接到硬件仿真器
// 参数说明：tmr - 连接定时器指针
// 返回值：无
// 调用时机：启动时或连接断开时调用，周期性重试连接
// 执行逻辑：关闭旧连接->创建套接字->连接服务器->初始化AIO->发送标识包
// 错误处理：连接失败时设置1秒后重试定时器
//=============================================================================
static void try_connecting (tmr_t* tmr) {
    if( aio ) {                               // 如果已有AIO连接
        aio_close(aio);                       // 关闭旧的AIO连接
        aio = NULL;                           // 重置AIO指针
    }
    int fd;                                   // 套接字文件描述符
    // 注意：希望使用SOCK_DGRAM但仅在python/asyncio 3.7中可用(当前为3.6.5)
    if( (fd = socket(PF_UNIX, SOCK_STREAM|SOCK_NONBLOCK, 0)) == -1 ) { // 创建非阻塞Unix域套接字
        LOG(MOD_SIM|ERROR, "LGWSIM: Failed to open unix domain socket '%s': %d (%s)", sockAddr.sun_path, errno, strerror(errno));
        goto retry;                           // 跳转到重试逻辑
    }
    if( connect(fd, (struct sockaddr*)&sockAddr, sizeof(sockAddr)) == -1 ) { // 连接到仿真服务器
        LOG(MOD_SIM|ERROR, "LGWSIM: Failed to connect to unix domain socket '%s': %d (%s)", sockAddr.sun_path, errno, strerror(errno));
        close(fd);                            // 关闭套接字
        goto retry;                           // 跳转到重试逻辑
    }
    aio = aio_open(&conn_tmr, fd, read_socket, write_socket); // 创建AIO对象，设置读写回调
    // 发送一个虚假数据包，用于标识套接字连接
    tx_pkt.tx_mode = 255;                     // 设置特殊发送模式标识
    tx_pkt.count_us = timeOffset;             // 设置时间偏移作为标识
    tx_pkt.freq_hz = timeOffset>>32;          // 设置频率为时间偏移高位
    tx_pkt.f_dev = max(0, sys_slaveIdx);      // 设置频偏为从站索引
    LOG(MOD_SIM|INFO, "LGWSIM: Connected txunit#%d timeOffset=0x%lX xticksNow=0x%lX", max(0, sys_slaveIdx), timeOffset, xticks());
    write_socket(aio);                        // 发送标识数据包
    read_socket(aio);                         // 启动接收处理
    return;                                   // 连接成功，返回

 retry:                                       // 重试标签
    rt_setTimer(tmr, rt_seconds_ahead(1));    // 设置1秒后重试定时器
}

//=============================================================================
// 函数功能：读取数据包回调函数
// 参数说明：aio - 异步IO对象
// 返回值：无
// 调用时机：当有数据可读时调用
// 算法逻辑：从套接字读取数据，处理丢弃字节，更新缓冲区索引
//=============================================================================
//=============================================================================
// 函数功能：套接字数据读取回调函数 - 处理从仿真服务器接收的数据包
// 参数说明：aio - 异步IO对象指针
// 返回值：无
// 调用时机：当套接字有数据可读时由AIO系统调用
// 执行逻辑：循环读取数据->处理缓冲区溢出->更新索引->检测CCA消息
// 缓冲区策略：环形缓冲区设计，支持字节级对齐和数据包边界处理
//=============================================================================
static void read_socket (aio_t* aio) {
    while(1) {                                // 持续读取数据直到无数据可读
        u1_t * rxbuf = &((u1_t*)rx_pkts)[rx_widx]; // 计算当前写入位置的缓冲区指针
        int rxlen = 4;                        // 默认读取长度(最小有效数据)
        
        if( rx_dsc ) {                        // 如果正在丢弃字节(缓冲区溢出处理)
            if( rx_dsc % sizeof(rx_pkts[0]) == 0 ) { // 如果已到达数据包边界
                LOG(MOD_SIM|ERROR, "LGWSIM(%s): RX buffer full. Dropping frame.", sockAddr.sun_path);
                rx_dsc = 0;                   // 重置丢弃计数器
                continue;                     // 继续下一轮循环
            } else {                          // 继续丢弃剩余字节
                rxlen = sizeof(rx_pkts[0]) - rx_dsc; // 计算需要丢弃的剩余字节数
            }
        } else if( (rxlen = rbfree(rx_widx, rx_ridx, rxblen)) == 0 ) { // 如果缓冲区已满
            rx_dsc = rx_widx  % sizeof(rx_pkts[0]); // 计算当前位置在数据包内的偏移
            rx_widx -= rx_dsc;                // 回退到数据包起始位置
            rxbuf = &((u1_t*)rx_pkts)[rx_widx]; // 重新计算缓冲区指针
            rxlen = sizeof(rx_pkts[0]) - rx_dsc; // 设置需要丢弃的字节数
        }
        
        int n = read(aio->fd, rxbuf, rxlen);  // 从套接字读取数据
        if( n == 0 ) {                        // 如果连接被远端关闭
            LOG(MOD_SIM|ERROR, "LGWSIM(%s) closed (recv)", sockAddr.sun_path);
            rt_yieldTo(&conn_tmr, try_connecting); // 启动重连机制
            return;                           // 退出函数
        }
        if( n==-1 ) {                         // 如果读取发生错误
            if( errno == EAGAIN )             // 如果是非阻塞操作的"再试一次"错误
                return;                       // 正常退出，等待下次可读事件
            LOG(MOD_SIM|ERROR, "LGWSIM(%s): Recv error: %d (%s)", sockAddr.sun_path, errno, strerror(errno));
            rt_yieldTo(&conn_tmr, try_connecting); // 启动重连机制
            return;                           // 退出函数
        }

        if( rx_dsc || rbfree(rx_widx, rx_ridx, rxblen) == 0 ) { // 如果处于丢弃模式或缓冲区仍满
            rx_dsc += n;                      // 累加已丢弃的字节数
            continue;                         // 继续下一轮循环
        } else {                              // 正常数据处理
            rx_widx = (rx_widx+n) % rxblen;   // 更新写入索引(环形缓冲区)
        }

        // 检查是否接收到CCA(Clear Channel Assessment)控制消息
        if( rbused(rx_widx, rx_ridx, rxblen) >= sizeof(rx_pkts[0]) && // 缓冲区有完整数据包
            rx_pkts[rx_ridx/sizeof(rx_pkts[0])].freq_hz == MAGIC_CCA_FREQ ){ // 且标记为CCA消息
            cca_msg = *(struct cca_msg*)&rx_pkts[rx_ridx/sizeof(rx_pkts[0])]; // 提取CCA信息
            rx_ridx = (rx_ridx+sizeof(rx_pkts[0])) % rxblen; // 更新读取索引
        }
    }
}

//=============================================================================
// 函数功能：写入数据包回调函数
// 参数说明：aio - 异步IO对象
// 返回值：无
// 调用时机：当有数据可写时调用
// 算法逻辑：向套接字写入数据，处理错误情况
//=============================================================================
static void write_socket (aio_t* aio) {
    int n = write(aio->fd, &tx_pkt, sizeof(tx_pkt));
    if( n == 0 ) {
        LOG(MOD_SIM|ERROR, "LGWSIM(%s) closed (send)", sockAddr.sun_path);
        rt_yieldTo(&conn_tmr, try_connecting);
        return;
    }
    if( n==-1 ) {
        if( errno == EAGAIN )
            return;
        LOG(MOD_SIM|ERROR, "LGWSIM(%s): Send error: %d (%s)", sockAddr.sun_path, errno, strerror(errno));
        rt_yieldTo(&conn_tmr, try_connecting);
        return;
    }
    assert(n == sizeof(tx_pkt));
    aio_set_wrfn(aio, NULL);
}

//=============================================================================
// 函数功能：发送数据包
// 参数说明：pkt_data - 发送数据包
// 返回值：发送结果
// 调用时机：发送数据包时调用
// 算法逻辑：检查信道可用性，设置发送参数，调用写入回调函数
//=============================================================================
int lgw_send (struct lgw_pkt_tx_s pkt_data) {
    sL_t t = xticks();
    txbeg = t + (s4_t)((u4_t)pkt_data.count_us - (u4_t)t);
    txend = txbeg + airtime(pkt_data.datarate, pkt_data.bandwidth, pkt_data.size);
    if( !cca(txbeg, pkt_data.freq_hz) )
        return LGW_LBT_ISSUE;
    tx_pkt = pkt_data;
    if( !aio || aio->ctx == NULL || aio->fd == 0 )
        return LGW_HAL_ERROR;
    aio_set_wrfn(aio, write_socket);
    write_socket(aio);
    return LGW_HAL_SUCCESS;
}

//=============================================================================
// 函数功能：获取发送状态
// 参数说明：select - 状态选择，code - 状态代码
// 返回值：状态获取结果
// 调用时机：获取发送状态时调用
// 算法逻辑：根据发送时间戳判断发送状态
//=============================================================================
int lgw_status (uint8_t select, uint8_t *code) {
    sL_t t = xticks();
    if( t <= txbeg )
        *code = TX_SCHEDULED;
    else if( t <= txend )
        *code = TX_EMITTING;
    else
        *code = TX_FREE;
    return LGW_HAL_SUCCESS;
}

//=============================================================================
// 函数功能：中止发送任务
// 参数说明：无
// 返回值：中止结果
// 调用时机：中止发送任务时调用
// 算法逻辑：重置发送时间
//=============================================================================
int lgw_abort_tx (void) {
    txbeg = txend = 0;
    return LGW_HAL_SUCCESS;
}

//=============================================================================
// 函数功能：停止硬件仿真器
// 参数说明：无
// 返回值：停止结果
// 调用时机：停止硬件仿真器时调用
// 算法逻辑：清除连接定时器，重置发送时间，关闭套接字
//=============================================================================
int lgw_stop (void) {
    rt_clrTimer(&conn_tmr);
    txbeg = txend = 0;
    aio_close(aio);
    aio = NULL;
    return LGW_HAL_SUCCESS;
}

//=============================================================================
// 函数功能：获取触发计数
// 参数说明：trig_cnt_us - 触发计数存储数组
// 返回值：触发计数获取结果
// 调用时机：获取触发计数时调用
// 算法逻辑：获取当前时间戳，减去系统时间，得到触发计数
//=============================================================================
int lgw_get_trigcnt(uint32_t* trig_cnt_us) {
    sL_t t = xticks();
    if( ppsLatched )
        t -= sys_utc()%1000000;
    trig_cnt_us[0] = t;
    return LGW_HAL_SUCCESS;
}

//=============================================================================
// 函数功能：启动硬件仿真器
// 参数说明：无
// 返回值：启动结果
// 调用时机：启动硬件仿真器时调用
// 算法逻辑：初始化套接字地址，设置时间偏移量，启动连接定时器
//=============================================================================
int lgw_start () {
    const char* sockPath = getenv("LORAGW_SPI");
    if( aio )
        return LGW_HAL_ERROR;
    memset(&cca_msg, 0, sizeof(cca_msg));
    memset(&sockAddr, 0, sizeof(sockAddr));
    // Make xticks different from ustime to cover more test ground.
    // xticks start at ~(1<<28) whenever a radio simulation starts.
    timeOffset = sys_time() - 0x10000000;
    sockAddr.sun_family = AF_UNIX;
    snprintf(sockAddr.sun_path, sizeof(sockAddr.sun_path), "%s", sockPath);
    rt_yieldTo(&conn_tmr, try_connecting);
    return LGW_HAL_SUCCESS;
}

//=============================================================================
// 函数功能：设置GPS启用状态
// 参数说明：reg_value - 启用状态
// 返回值：设置结果
// 调用时机：设置GPS启用状态时调用
// 算法逻辑：设置PPS锁存状态
//=============================================================================
int lgw_reg_w (uint16_t register_id, int32_t reg_value) {
    assert(register_id == LGW_GPS_EN);
    ppsLatched = reg_value;
    return LGW_HAL_SUCCESS;
}

//=============================================================================
// 函数功能：设置板载配置
// 参数说明：conf - 板载配置
// 返回值：配置结果
// 调用时机：设置板载配置时调用
// 算法逻辑：根据配置参数设置内部状态
//=============================================================================
int lgw_board_setconf (struct lgw_conf_board_s conf) {
    return LGW_HAL_SUCCESS;
}

//=============================================================================
// 函数功能：设置射频链路配置
// 参数说明：rf_chain - 射频链路，conf - 射频配置
// 返回值：配置结果
// 调用时机：设置射频链路配置时调用
// 算法逻辑：检查输入范围，检查是否支持，检查是否启用，设置内部配置
//=============================================================================
int lgw_rxrf_setconf (uint8_t rf_chain, struct lgw_conf_rxrf_s conf) {
    /* check input range (segfault prevention) */
    if (rf_chain >= LGW_RF_CHAIN_NB) {
        LOG(MOD_SIM|ERROR, "ERROR: NOT A VALID RF_CHAIN NUMBER\n");
        return LGW_HAL_ERROR;
    }

    /* check if radio type is supported */
    if ((conf.type != LGW_RADIO_TYPE_SX1255) && (conf.type != LGW_RADIO_TYPE_SX1257)) {
        LOG(MOD_SIM|ERROR, "ERROR: NOT A VALID RADIO TYPE\n");
        return LGW_HAL_ERROR;
    }

    /* check if TX notch filter frequency is supported */
    if ((conf.tx_enable == true) && ((conf.tx_notch_freq < LGW_MIN_NOTCH_FREQ) || (conf.tx_notch_freq > LGW_MAX_NOTCH_FREQ))) {
        LOG(MOD_SIM|ERROR, "WARNING: NOT A VALID TX NOTCH FILTER FREQUENCY [%u..%u]Hz\n", LGW_MIN_NOTCH_FREQ, LGW_MAX_NOTCH_FREQ);
        conf.tx_notch_freq = 0;
    }

    /* set internal config according to parameters */
    // rf_enable[rf_chain] = conf.enable;
    // rf_rx_freq[rf_chain] = conf.freq_hz;
    // rf_rssi_offset[rf_chain] = conf.rssi_offset;
    // rf_radio_type[rf_chain] = conf.type;
    // rf_tx_enable[rf_chain] = conf.tx_enable;
    // rf_tx_notch_freq[rf_chain] = conf.tx_notch_freq;

    LOG(MOD_SIM|INFO, "Note: rf_chain %d configuration; en:%d freq:%d rssi_offset:%f radio_type:%d tx_enable:%d tx_notch_freq:%u\n",
        rf_chain, conf.enable, conf.freq_hz, conf.rssi_offset, conf.type, conf.tx_enable, conf.tx_notch_freq);

    return LGW_HAL_SUCCESS;
}

const uint8_t ifmod_config[LGW_IF_CHAIN_NB] = LGW_IFMODEM_CONFIG;

#define LGW_RF_RX_BANDWIDTH_125KHZ  925000      /* for 125KHz channels */
#define LGW_RF_RX_BANDWIDTH_250KHZ  1000000     /* for 250KHz channels */
#define LGW_RF_RX_BANDWIDTH_500KHZ 1100000 /* for 500KHz channels */

int32_t lgw_bw_getval(int x) {
    switch (x) {
        case BW_500KHZ: return 500000;
        case BW_250KHZ: return 250000;
        case BW_125KHZ: return 125000;
        case BW_62K5HZ: return 62500;
        case BW_31K2HZ: return 31200;
        case BW_15K6HZ: return 15600;
        case BW_7K8HZ : return 7800;
        default: return -1;
    }
}

int lgw_rxif_setconf (uint8_t if_chain, struct lgw_conf_rxif_s conf) {
    int32_t bw_hz;
    uint32_t rf_rx_bandwidth;
    uint8_t ifmod_config[LGW_IF_CHAIN_NB] = LGW_IFMODEM_CONFIG;
    // uint8_t fsk_sync_word_size = 3; /* default number of bytes for FSK sync word */
    uint64_t fsk_sync_word = 0xC194C1; /* default FSK sync word (ALIGNED RIGHT, MSbit first) */

    /* check input range (segfault prevention) */
    if (if_chain >= LGW_IF_CHAIN_NB) {
        LOG(MOD_SIM|ERROR, "ERROR: %d NOT A VALID IF_CHAIN NUMBER\n", if_chain);
        return LGW_HAL_ERROR;
    }

    /* if chain is disabled, don't care about most parameters */
    if (conf.enable == false) {
        LOG(MOD_SIM|INFO, "Note: if_chain %d disabled\n", if_chain);
        return LGW_HAL_SUCCESS;
    }

    if (conf.rf_chain >= LGW_RF_CHAIN_NB) {
        LOG(MOD_SIM|ERROR, "ERROR: INVALID RF_CHAIN TO ASSOCIATE WITH A LORA_STD IF CHAIN\n");
        return LGW_HAL_ERROR;
    }
    /* check if IF frequency is optimal based on channel and radio bandwidths */
    switch (conf.bandwidth) {
        case BW_250KHZ:
            rf_rx_bandwidth = LGW_RF_RX_BANDWIDTH_250KHZ; /* radio bandwidth */
            break;
        case BW_500KHZ:
            rf_rx_bandwidth = LGW_RF_RX_BANDWIDTH_500KHZ; /* radio bandwidth */
            break;
        default:
            /* For 125KHz and below */
            rf_rx_bandwidth = LGW_RF_RX_BANDWIDTH_125KHZ; /* radio bandwidth */
            break;
    }
    bw_hz = lgw_bw_getval(conf.bandwidth); /* channel bandwidth */
    if ((conf.freq_hz + ((bw_hz==-1)?LGW_REF_BW:bw_hz)/2) > ((int32_t)rf_rx_bandwidth/2)) {
        LOG(MOD_SIM|ERROR, "ERROR: IF FREQUENCY %d TOO HIGH\n", conf.freq_hz);
        return LGW_HAL_ERROR;
    } else if ((conf.freq_hz - ((bw_hz==-1)?LGW_REF_BW:bw_hz)/2) < -((int32_t)rf_rx_bandwidth/2)) {
        LOG(MOD_SIM|ERROR, "ERROR: IF FREQUENCY %d TOO LOW\n", conf.freq_hz);
        return LGW_HAL_ERROR;
    }

    /* check parameters according to the type of IF chain + modem,
    fill default if necessary, and commit configuration if everything is OK */
    switch (ifmod_config[if_chain]) {
        case IF_LORA_STD:
            /* fill default parameters if needed */
            if (conf.bandwidth == BW_UNDEFINED) {
                conf.bandwidth = BW_250KHZ;
            }
            if (conf.datarate == DR_UNDEFINED) {
                conf.datarate = DR_LORA_SF9;
            }
            /* check BW & DR */
            if (!IS_LORA_BW(conf.bandwidth)) {
                LOG(MOD_SIM|ERROR, "ERROR: BANDWIDTH NOT SUPPORTED BY LORA_STD IF CHAIN\n");
                return LGW_HAL_ERROR;
            }
            if (!IS_LORA_STD_DR(conf.datarate)) {
                LOG(MOD_SIM|ERROR, "ERROR: DATARATE NOT SUPPORTED BY LORA_STD IF CHAIN\n");
                return LGW_HAL_ERROR;
            }
            /* set internal configuration  */
            // if_enable[if_chain] = conf.enable;
            // if_rf_chain[if_chain] = conf.rf_chain;
            // if_freq[if_chain] = conf.freq_hz;
            // lora_rx_bw = conf.bandwidth;
            // lora_rx_sf = (uint8_t)(DR_LORA_MULTI & conf.datarate); /* filter SF out of the 7-12 range */
            // if (SET_PPM_ON(conf.bandwidth, conf.datarate)) {
            //     lora_rx_ppm_offset = true;
            // } else {
            //     lora_rx_ppm_offset = false;
            // }
            LOG(MOD_SIM|INFO, "Note: LoRa 'std' if_chain %d configuration; en:%d rf_chain:%d freq:%d bw:%d dr:%d\n",
                if_chain, conf.enable, conf.rf_chain, conf.freq_hz, conf.bandwidth, (uint8_t)(DR_LORA_MULTI & conf.datarate));
            break;

        case IF_LORA_MULTI:
            /* fill default parameters if needed */
            if (conf.bandwidth == BW_UNDEFINED) {
                conf.bandwidth = BW_125KHZ;
            }
            if (conf.datarate == DR_UNDEFINED) {
                conf.datarate = DR_LORA_MULTI;
            }
            /* check BW & DR */
            if (conf.bandwidth != BW_125KHZ) {
                LOG(MOD_SIM|ERROR, "ERROR: BANDWIDTH NOT SUPPORTED BY LORA_MULTI IF CHAIN\n");
                return LGW_HAL_ERROR;
            }
            if (!IS_LORA_MULTI_DR(conf.datarate)) {
                LOG(MOD_SIM|ERROR, "ERROR: DATARATE(S) NOT SUPPORTED BY LORA_MULTI IF CHAIN\n");
                return LGW_HAL_ERROR;
            }
            /* set internal configuration  */
            // if_enable[if_chain] = conf.enable;
            // if_rf_chain[if_chain] = conf.rf_chain;
            // if_freq[if_chain] = conf.freq_hz;
            // lora_multi_sfmask[if_chain] = (uint8_t)(DR_LORA_MULTI & conf.datarate); /* filter SF out of the 7-12 range */
            LOG(MOD_SIM|INFO, "Note: LoRa 'multi' if_chain %d configuration; en:%d rf_chain:%d freq:%d SF_mask:0x%02x\n",
                if_chain, conf.enable, conf.rf_chain, conf.freq_hz, (uint8_t)(DR_LORA_MULTI & conf.datarate));
            break;

        case IF_FSK_STD:
            /* fill default parameters if needed */
            if (conf.bandwidth == BW_UNDEFINED) {
                conf.bandwidth = BW_250KHZ;
            }
            if (conf.datarate == DR_UNDEFINED) {
                conf.datarate = 64000; /* default datarate */
            }
            /* check BW & DR */
            if(!IS_FSK_BW(conf.bandwidth)) {
                LOG(MOD_SIM|ERROR, "ERROR: BANDWIDTH NOT SUPPORTED BY FSK IF CHAIN\n");
                return LGW_HAL_ERROR;
            }
            if(!IS_FSK_DR(conf.datarate)) {
                LOG(MOD_SIM|ERROR, "ERROR: DATARATE NOT SUPPORTED BY FSK IF CHAIN\n");
                return LGW_HAL_ERROR;
            }
            /* set internal configuration  */
            // if_enable[if_chain] = conf.enable;
            // if_rf_chain[if_chain] = conf.rf_chain;
            // if_freq[if_chain] = conf.freq_hz;
            // fsk_rx_bw = conf.bandwidth;
            // fsk_rx_dr = conf.datarate;
            if (conf.sync_word > 0) {
                // fsk_sync_word_size = conf.sync_word_size;
                fsk_sync_word = conf.sync_word;
            }
            LOG(MOD_SIM|INFO, "Note: FSK if_chain %d configuration; en:%d rf_chain:%d freq:%d bw:%d dr:%d (%d real dr) sync:0x%0X\n",
                if_chain, conf.enable, conf.rf_chain, conf.freq_hz, conf.bandwidth, conf.datarate, LGW_XTAL_FREQU/(LGW_XTAL_FREQU/conf.datarate), fsk_sync_word);
            break;

        default:
            LOG(MOD_SIM|ERROR, "ERROR: IF CHAIN %d TYPE NOT SUPPORTED\n", if_chain);
            return LGW_HAL_ERROR;
    }
    return LGW_HAL_SUCCESS;
}

int lgw_txgain_setconf (struct lgw_tx_gain_lut_s* conf) {
    int i;

    /* Check LUT size */
    if ((conf->size < 1) || (conf->size > TX_GAIN_LUT_SIZE_MAX)) {
        LOG(MOD_SIM|ERROR, "ERROR: TX gain LUT must have at least one entry and  maximum %d entries\n", TX_GAIN_LUT_SIZE_MAX);
        return LGW_HAL_ERROR;
    }

    // txgain_lut.size = conf->size;

    for (i = 0; i < conf->size; i++) {
        /* Check gain range */
        if (conf->lut[i].dig_gain > 3) {
            LOG(MOD_SIM|ERROR, "ERROR: TX gain LUT: SX1301 digital gain must be between 0 and 3\n");
            return LGW_HAL_ERROR;
        }
        if (conf->lut[i].dac_gain != 3) {
            LOG(MOD_SIM|ERROR, "ERROR: TX gain LUT: SX1257 DAC gains != 3 are not supported\n");
            return LGW_HAL_ERROR;
        }
        if (conf->lut[i].mix_gain > 15) {
            LOG(MOD_SIM|ERROR, "ERROR: TX gain LUT: SX1257 mixer gain must not exceed 15\n");
            return LGW_HAL_ERROR;
        } else if (conf->lut[i].mix_gain < 8) {
            LOG(MOD_SIM|ERROR, "ERROR: TX gain LUT: SX1257 mixer gains < 8 are not supported\n");
            return LGW_HAL_ERROR;
        }
        if (conf->lut[i].pa_gain > 3) {
            LOG(MOD_SIM|ERROR, "ERROR: TX gain LUT: External PA gain must not exceed 3\n");
            return LGW_HAL_ERROR;
        }

        // /* Set internal LUT */
        // txgain_lut.lut[i].dig_gain = conf->lut[i].dig_gain;
        // txgain_lut.lut[i].dac_gain = conf->lut[i].dac_gain;
        // txgain_lut.lut[i].mix_gain = conf->lut[i].mix_gain;
        // txgain_lut.lut[i].pa_gain  = conf->lut[i].pa_gain;
        // txgain_lut.lut[i].rf_power = conf->lut[i].rf_power;
    }

    return LGW_HAL_SUCCESS;
}

int lgw_lbt_setconf (struct lgw_conf_lbt_s conf) {
    return LGW_HAL_SUCCESS;
}

str_t lgw_version_info () {
    return "LGW Simulation";
}

#if defined(CFG_smtcpico)
int lgw_connect (const char *com_path) {
    return LGW_HAL_SUCCESS;
}
#endif

#endif

// LCOV_EXCL_STOP
#endif // CFG_lgwsim
