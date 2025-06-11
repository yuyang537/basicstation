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
// 文件功能：无线电抽象层(RAL)从进程实现
// 作用说明：实现多无线电主从进程架构的从控端，专门负责：
//          - 直接控制单个SX130X LoRa网关芯片硬件
//          - 执行主进程发送的硬件操作命令
//          - 处理LoRa数据包的发送和接收
//          - 进行高精度时间同步测量
//          - 通过管道与主进程进行进程间通信
// 架构特点：每个从进程独立管理一个SX130X芯片，避免硬件访问冲突
// 设计优势：硬件故障隔离、独立重启恢复、专一化处理提升性能
//=============================================================================

#if defined(CFG_lgw1) && defined(CFG_ral_master_slave)

#include <limits.h>           // 系统限制常数定义，如PIPE_BUF管道缓冲区大小
#include <unistd.h>           // UNIX标准定义，提供read/write等系统调用
#include <errno.h>            // 错误码定义，提供errno全局变量和错误常数
#include <fcntl.h>            // 文件控制定义，提供O_NONBLOCK等标志位

#include "uj.h"               // JSON解析器，用于处理配置数据
#include "ral.h"              // RAL接口定义，提供统一的无线电抽象层API
#include "ralsub.h"           // RAL主从进程通信协议定义
#include "timesync.h"         // 时间同步机制，提供高精度时间测量
#include "sys_linux.h"        // Linux系统接口，提供系统级操作
#include "sx130xconf.h"       // SX130X芯片配置解析器
#include "lgw/loragw_hal.h"   // LoRa网关硬件抽象层，直接控制SX130X芯片

// 如果未定义数据包FIFO大小，使用默认值16
#if !defined(LGW_PKT_FIFO_SIZE)
#define LGW_PKT_FIFO_SIZE 16
#endif

//=============================================================================
// 从进程全局状态变量
//=============================================================================
static u1_t   pps_en;          // PPS(每秒脉冲)信号使能标志，用于GPS时间同步
static sL_t   last_xtime;      // 上次记录的扩展时间戳，用于时间连续性维护
static u4_t   region;          // 区域码，决定是否需要LBT(监听后发送)检测
static tmr_t  rxpoll_tmr;      // 接收轮询定时器，定期检查是否有新的接收数据
static aio_t* rd_aio;          // 读取AIO对象，用于异步读取主进程命令
static aio_t* wr_aio;          // 写入AIO对象，用于向主进程发送响应和数据
static s2_t   txpowAdjust;     // 发送功率调整值，按TXPOW_SCALE缩放
static struct lgw_pkt_rx_s pkt_rx[LGW_PKT_FIFO_SIZE];  // 接收数据包缓冲区数组

//=============================================================================
// 函数功能：向主进程管道写入数据，处理管道满和断开情况
// 参数说明：data - 要写入的数据指针，len - 数据长度
// 返回值：无
// 调用时机：需要向主进程发送响应、接收数据或状态信息时调用
// 执行逻辑：循环写入直到成功，处理管道满和断开的异常情况
// 安全机制：重试限制防止无限循环，管道断开时触发致命错误
//=============================================================================
static void pipe_write_data (void* data, int len) {
    assert(len < PIPE_BUF);                   // 确保数据长度在管道原子写入限制内
    int retries = 0;                          // 重试计数器
    while(1) {
        int n = write(wr_aio->fd, data, len); // 向写入管道发送数据
        if( n == len )                        // 如果完全写入成功
            return;                           // 直接返回
        if( errno == EPIPE )                  // 如果管道已断开(主进程死亡)
            rt_fatal("Slave (%d) - Broken pipe", sys_slaveIdx);  // 触发致命错误
        if( errno == EAGAIN ) {               // 如果管道缓冲区满
            if( ++retries > 5 ) {             // 如果重试次数超过限制
                // rt_fatal("Slave (%d) - Pipe full - master too slow", sys_slaveIdx);
                LOG(MOD_RAL|ERROR, "Slave (%d) - Pipe full - dropping message", sys_slaveIdx);
                return;                       // 丢弃消息而不是致命错误(避免系统崩溃)
            }
            rt_usleep(rt_millis(1));          // 等待1ms后重试
        }
    }
}

//=============================================================================
// 函数功能：记录原始数据包信息，用于调试和监控
// 参数说明：level - 日志级别，msg - 消息前缀，pkt_rx - 接收数据包结构体指针
// 返回值：无
// 调用时机：接收到数据包时调用，根据CRC状态和日志级别决定是否输出
// 日志内容：CRC状态、频率、信噪比、RSSI、调制参数、时间戳、数据内容
//=============================================================================
static void log_rawpkt(u1_t level, str_t msg, struct lgw_pkt_rx_s * pkt_rx) {
    LOG(MOD_RAL|level, "%s[CRC %s] %^.3F %.2f/%.1f %R (mod=%d/dr=%d/bw=%d) xtick=%08x (%u) %d bytes: %64H",
        msg,                                  // 消息前缀
        pkt_rx->status == STAT_CRC_OK ? "OK"  : "FAIL",  // CRC检查结果
        pkt_rx->freq_hz,                      // 接收频率(Hz)
        pkt_rx->snr,                          // 信噪比(dB)
#if defined(CFG_sx1302)                      // 根据芯片类型选择RSSI字段
        pkt_rx->rssis,                        // SX1302使用rssis字段
#else
        pkt_rx->rssi,                         // SX1301使用rssi字段
#endif
        ral_lgw2rps(pkt_rx),                  // 将硬件格式转换为RPS格式
        pkt_rx->modulation,                   // 调制方式
        pkt_rx->datarate,                     // 数据速率
        pkt_rx->bandwidth,                    // 带宽
        pkt_rx->count_us,                     // 硬件计数器时间戳(十六进制)
        pkt_rx->count_us,                     // 硬件计数器时间戳(十进制)
        pkt_rx->size,                         // 数据包长度
        pkt_rx->size, pkt_rx->payload         // 数据包内容(十六进制显示)
    );
}

//=============================================================================
// 函数功能：接收轮询定时器回调，定期检查和处理接收到的LoRa数据包
// 参数说明：tmr - 定时器结构体指针(未使用)
// 返回值：无
// 调用时机：RX_POLL_INTV间隔定时触发，持续监控硬件接收状态
// 执行逻辑：读取硬件缓冲区->CRC检查->格式转换->发送给主进程->设置下次定时器
// 处理策略：批量处理提升效率，CRC错误静默丢弃，数据过大记录错误
//=============================================================================
static void rx_polling (tmr_t* tmr) {
    int n;
    // 从硬件缓冲区读取接收数据包，最多LGW_PKT_FIFO_SIZE个
    while( (n = lgw_receive(LGW_PKT_FIFO_SIZE, pkt_rx)) != 0 ) {
        if( n < 0 || n > LGW_PKT_FIFO_SIZE ) { // 检查返回值有效性
            LOG(MOD_RAL|ERROR, "lgw_receive error: %d", n);
            break;                            // 硬件错误，停止处理
        }
        // 逐个处理接收到的数据包
        for( int i=0; i<n; i++ ) {
            struct lgw_pkt_rx_s* p = &pkt_rx[i];  // 获取当前数据包指针
            if( p->status != STAT_CRC_OK ) {  // 如果CRC校验失败
                if( log_shallLog(MOD_RAL|DEBUG) ) {  // 如果启用了调试日志
                    log_rawpkt(DEBUG, "", p); // 记录错误包用于调试
                }
                continue;                     // 静默忽略CRC错误的包
            }
            if( p->size > MAX_RXFRAME_LEN ) { // 检查数据包长度是否超限
                // 这通常不应该发生，因为调用者提供了最大帧长度空间(255字节)
                log_rawpkt(ERROR, "Dropped RX frame - frame size too large: ", p);
                continue;                     // 丢弃过大的数据包
            }
            
            // 构造接收响应结构体，发送给主进程
            struct ral_rx_resp resp;
            memset(&resp, 0, sizeof(resp));   // 清零响应结构体
            resp.rctx   = sys_slaveIdx;       // 设置响应上下文(从进程索引)
            resp.cmd    = RAL_CMD_RX;         // 设置命令类型为接收
            // 将硬件时间戳转换为扩展时间格式
            resp.xtime  = ts_xticks2xtime(p->count_us, last_xtime);
            resp.rps    = ral_lgw2rps(p);     // 转换调制参数为RPS格式
            resp.freq   = p->freq_hz;         // 设置接收频率
#if defined(CFG_sx1302)                      // 根据芯片类型处理RSSI
            resp.rssi  = (u1_t)-p->rssis;    // SX1302: 取负值并转换为无符号
#else
            resp.rssi  = (u1_t)-p->rssi;     // SX1301: 取负值并转换为无符号
#endif
            resp.snr    = (s1_t)(p->snr  *  4);  // SNR放大4倍并转换为有符号整数
            resp.rxlen  = p->size;            // 设置数据包长度
            memcpy(resp.rxdata, p->payload, p->size);  // 复制数据包内容

            if( log_shallLog(MOD_RAL|XDEBUG) ) {  // 如果启用了详细调试日志
                log_rawpkt(XDEBUG, "", p);    // 记录成功接收的包
            }

            pipe_write_data(&resp, sizeof(resp));  // 发送接收响应给主进程
        }
    }
    // 设置下次轮询定时器，使用微秒精度
    rt_setTimer(&rxpoll_tmr, rt_micros_ahead(RX_POLL_INTV));
}

//=============================================================================
// 函数功能：发送时间同步响应给主进程
// 参数说明：无
// 返回值：无
// 调用时机：收到主进程的时间同步请求时调用
// 执行逻辑：调用时间同步测量函数，构造响应，发送给主进程
//=============================================================================
static void sendTimesync () {
    struct ral_timesync_resp resp;            // 时间同步响应结构体
    resp.rctx = sys_slaveIdx;                 // 设置响应上下文
    resp.cmd = RAL_CMD_TIMESYNC;              // 设置命令类型
    // 执行时间同步测量，获取质量指标和同步数据
    resp.quality = ral_getTimesync(pps_en, &last_xtime, &resp.timesync);
    pipe_write_data(&resp, sizeof(resp));     // 发送响应给主进程
}

//=============================================================================
// 函数功能：管道读取回调函数，处理来自主进程的各种命令
// 参数说明：aio - 异步IO对象指针
// 返回值：无
// 调用时机：主进程向管道发送数据时自动调用
// 执行逻辑：循环读取管道数据，根据命令类型分发处理
// 命令类型：发送状态查询、发送中止、时间同步、数据发送、配置、停止
//=============================================================================
static void pipe_read (aio_t* aio) {
    u1_t buf[PIPE_BUF];                       // 管道读取缓冲区
    while(1) {
        int n = read(aio->fd, buf, sizeof(buf));  // 从管道读取数据
        if( n == 0 ) {                        // 如果读取到EOF
            // 主进程关闭了管道，通常表示主进程退出
            LOG(MOD_RAL|INFO, "EOF from master (%d)", sys_slaveIdx);
            exit(2);                          // 从进程也退出
            return;
        }
        if( n == -1 ) {                       // 如果读取出错
            if( errno == EAGAIN )             // 如果是非阻塞读取无数据
                return;                       // 直接返回等待下次调用
            rt_fatal("Slave pipe read fail: %s", strerror(errno));  // 其他错误为致命
        }
        
        // 解析并处理读取到的命令数据
        int off = 0;                          // 数据处理偏移位置
        while( off < n ) {
            struct ral_header* req = (struct ral_header*)&buf[off];  // 获取命令头
            assert(n >= off + sizeof(*req));  // 确保至少有完整的命令头
            
            // 处理发送状态查询命令
            if( n >= off + sizeof(struct ral_txstatus_req) && req->cmd == RAL_CMD_TXSTATUS ) {
                off += sizeof(struct ral_txstatus_req);  // 更新处理偏移
                struct ral_response* resp = (struct ral_response*)req;  // 复用请求内存作为响应
                u1_t ret=TXSTATUS_IDLE, status;       // 默认状态为空闲
#if defined(CFG_sx1302)                      // SX1302芯片状态查询
                int err = lgw_status(0, TX_STATUS, &status);  // 查询发送状态
#else                                        // SX1301芯片状态查询
                int err = lgw_status(TX_STATUS, &status);     // 查询发送状态
#endif
                /**/ if (err != LGW_HAL_SUCCESS)  { LOG(MOD_RAL|ERROR, "lgw_status failed"); }  // 查询失败
                else if( status == TX_SCHEDULED ) { ret = TXSTATUS_SCHEDULED; }  // 发送已调度
                else if( status == TX_EMITTING  ) { ret = TXSTATUS_EMITTING; }   // 正在发送
                resp->status = ret;               // 设置响应状态
                pipe_write_data(resp, sizeof(*resp));  // 发送状态响应
                continue;
            }
            // 处理发送中止命令
            else if( n >= off + sizeof(struct ral_txabort_req) && req->cmd == RAL_CMD_TXABORT) {
                off += sizeof(struct ral_txabort_req);  // 更新处理偏移
#if defined(CFG_sx1302)                      // SX1302芯片发送中止
                lgw_abort_tx(0);              // 中止射频链0的发送
#else                                        // SX1301芯片发送中止
                lgw_abort_tx();               // 中止发送
#endif
                continue;
            }
            // 处理时间同步请求命令
            else if( n >= off + sizeof(struct ral_timesync_req) && req->cmd == RAL_CMD_TIMESYNC) {
                off += sizeof(struct ral_timesync_req);  // 更新处理偏移
                sendTimesync();               // 执行时间同步并发送响应
                continue;
            }
            // 处理数据发送命令(支持CCA和无CCA两种模式)
            else if( n >= off + sizeof(struct ral_tx_req) && (req->cmd == RAL_CMD_TX_NOCCA || req->cmd == RAL_CMD_TX  )) {
                off += sizeof(struct ral_tx_req);  // 更新处理偏移
                struct ral_tx_req* txreq = (struct ral_tx_req*)req;  // 转换为发送请求
                struct lgw_pkt_tx_s pkt_tx;   // 硬件发送数据包结构体

                // 设置发送参数的默认值
                pkt_tx.invert_pol = true;     // 默认使用极性反转
                pkt_tx.no_header  = false;    // 默认包含LoRa头部

                if( (txreq->rps & RPS_BCN) ) {  // 如果是信标模式发送
                    pkt_tx.tx_mode = ON_GPS;      // 使用GPS触发模式
                    pkt_tx.preamble = 10;         // 信标使用10字节前导码
                    pkt_tx.invert_pol = false;    // 信标不使用极性反转
                    pkt_tx.no_header  = true;     // 信标没有LoRa头部
                } else {                      // 如果是数据模式发送
                    pkt_tx.tx_mode = TIMESTAMPED; // 使用时间戳触发模式
                    pkt_tx.preamble = 8;          // 数据包使用8字节前导码
                }
                ral_rps2lgw(txreq->rps, &pkt_tx);     // 转换RPS参数为硬件格式
                pkt_tx.freq_hz    = txreq->freq;      // 设置发送频率
                pkt_tx.count_us   = txreq->xtime;     // 设置发送时间戳
                pkt_tx.rf_chain   = 0;                // 使用射频链0
                pkt_tx.rf_power   = (float)(txreq->txpow - txpowAdjust)/TXPOW_SCALE;  // 计算发送功率
                pkt_tx.coderate   = CR_LORA_4_5;      // 使用4/5编码率
                pkt_tx.no_crc     = !txreq->addcrc;   // 根据请求决定是否添加CRC
                pkt_tx.size       = txreq->txlen;     // 设置数据包长度
                memcpy(pkt_tx.payload, txreq->txdata, txreq->txlen);  // 复制发送数据
                
                // 调用硬件发送函数
#if defined(CFG_sx1302)                      // SX1302芯片发送
                int err = lgw_send(&pkt_tx);  // 传递指针
#else                                        // SX1301芯片发送
                int err = lgw_send(pkt_tx);   // 传递结构体
#endif
                if( region == 0 ) {           // 如果无需LBT检测
                    continue;                 // 直接继续，不发送响应
                }
                
                // 构造并发送CCA/LBT检测结果响应
                struct ral_response* resp = (struct ral_response*)req;  // 复用请求内存
                u1_t ret = RAL_TX_OK;         // 默认发送成功
                if( err == LGW_HAL_SUCCESS ) {        // 如果硬件操作成功
                    ret = RAL_TX_OK;          // 发送成功
                } else if( err == LGW_LBT_ISSUE ) {   // 如果LBT检测失败
                    ret = RAL_TX_NOCA;        // 信道忙，无法发送
                } else {                      // 其他错误
                    LOG(MOD_RAL|ERROR, "lgw_send failed");
                    ret = RAL_TX_FAIL;        // 发送失败
                }
                resp->status = ret;           // 设置响应状态
                pipe_write_data(resp, sizeof(*resp));  // 发送响应
                continue;
            }
            // 处理配置命令
            else if( n >= off + sizeof(struct ral_config_req) && req->cmd == RAL_CMD_CONFIG) {
                off += sizeof(struct ral_config_req);  // 更新处理偏移
                struct ral_config_req* confreq = (struct ral_config_req*)req;  // 转换为配置请求
                struct sx130xconf sx1301conf;     // SX130X配置结构体
                int status = 0;               // 配置状态码
                
                // 执行配置的多个步骤，任何一步失败都会导致致命错误
                // 注意：sx1301conf_start可能需要相当长的时间(如果启用LBT最多8秒!)
                if( (status = !sx130xconf_parse_setup(&sx1301conf, sys_slaveIdx, confreq->hwspec, confreq->json, confreq->jsonlen)) ||  // 解析配置
                    (status = !sx130xconf_challoc(&sx1301conf, &confreq->upchs)   << 1) ||  // 分配信道
                    (status = !sys_runRadioInit(sx1301conf.device)                << 2) ||  // 初始化无线电
                    (status = !sx130xconf_start(&sx1301conf, confreq->region)     << 3) )   // 启动配置
                    rt_fatal("Slave radio start up failed with status 0x%02x", status);  // 配置失败为致命错误
                
                // 检查PPS配置的有效性
                if( sx1301conf.pps && sys_slaveIdx ) {  // 如果启用PPS但不是从进程0
                    LOG(MOD_RAL|ERROR, "Only slave#0 may have PPS enabled");
                    sx1301conf.pps = 0;       // 强制禁用PPS
                }
                
                // 保存配置参数到全局变量
                pps_en = sx1301conf.pps;      // 保存PPS使能状态
                region = confreq->region;     // 保存区域码
                txpowAdjust = sx1301conf.txpowAdjust;  // 保存功率调整值
                last_xtime = ts_newXtimeSession(sys_slaveIdx);  // 初始化新的时间会话
                
                // 启动接收轮询定时器
                rt_yieldTo(&rxpoll_tmr, rx_polling);
                // 发送初始时间同步响应
                sendTimesync();
                continue;
            }
            // 处理停止命令
            else if( n >= off + sizeof(struct ral_stop_req) && req->cmd == RAL_CMD_STOP) {
                off += sizeof(struct ral_stop_req);  // 更新处理偏移
                last_xtime = 0;               // 清除时间状态
                rt_clrTimer(&rxpoll_tmr);     // 清除接收轮询定时器
                lgw_stop();                   // 停止硬件
                continue;
            }
            else {
                // 收到意外的命令数据
                rt_fatal("Master sent unexpected data: cmd=%d size=%d", req->cmd, n-off);
            }
        }
        assert(off==n);                       // 确保所有数据都已处理(请求不应被分片)
    }
}

//=============================================================================
// 函数功能：启动从进程的主入口函数
// 参数说明：rdfd - 读取文件描述符(从主进程接收命令)，wrfd - 写入文件描述符(向主进程发送响应)
// 返回值：无(函数不会返回，进入事件循环)
// 调用时机：从进程程序启动时调用，建立与主进程的通信并进入事件循环
// 执行逻辑：建立AIO管道->初始化定时器->启动异步读取->进入事件循环
//=============================================================================
void sys_startupSlave (int rdfd, int wrfd) {
    // 建立异步IO对象，使用rxpoll_tmr作为虚拟上下文
    rd_aio = aio_open(&rxpoll_tmr, rdfd, pipe_read, NULL);  // 读取端，有数据时调用pipe_read
    wr_aio = aio_open(&rxpoll_tmr, wrfd, NULL, NULL);       // 写入端，仅用于写入操作
    rt_iniTimer(&rxpoll_tmr, NULL);           // 初始化接收轮询定时器
    pipe_read(rd_aio);                        // 启动初始的管道读取
    LOG(MOD_RAL|INFO, "Slave LGW (%d) - started.", sys_slaveIdx);  // 记录从进程启动
    aio_loop();                               // 进入异步IO事件循环，永不返回
    // NOT REACHED
    assert(0);                                // 不应该到达这里
}

#endif // defined(CFG_lgw1) && defined(CFG_ral_master_slave)
