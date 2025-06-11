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
// 文件功能：无线电抽象层(RAL)主进程实现
// 作用说明：实现多无线电主从进程架构的主控端，负责：
//          - 管理多个从进程(每个控制一个SX130X芯片)
//          - 协调多个无线电的同步和调度
//          - 提供统一的RAL接口给上层应用
//          - 处理进程间通信和异常恢复
// 架构原理：采用主从进程分离设计，主进程处理业务逻辑，从进程专门控制硬件
//=============================================================================

#if defined(CFG_lgw1) && defined(CFG_ral_master_slave)

#define _GNU_SOURCE
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <wordexp.h>

#include "timesync.h"
#include "tc.h"
#include "sys.h"
#include "sys_linux.h"
#include "sx130xconf.h"
#include "ral.h"
#include "ralsub.h"

//=============================================================================
// 主从进程通信和恢复相关的时间常量定义
//=============================================================================
#define WAIT_SLAVE_PID_INTV rt_millis(500)  // 等待从进程PID的轮询间隔：500ms
#define RETRY_KILL_INTV     rt_millis(100)  // 重试杀死从进程的间隔：100ms
#define RETRY_PIPE_IO       500             // 管道IO重试次数限制：500次
#define PPM                 1000000         // 百万分率常数，用于时间计算

//=============================================================================
// 从进程状态结构体定义
// 功能：维护每个从进程的完整状态信息，包括通信、配置、监控等
//=============================================================================
typedef struct slave {
    tmr_t      tmr;               // 从进程监控定时器，用于检查进程状态和重启
    tmr_t      tsync;             // 时间同步定时器，定期请求从进程进行时间同步
    pid_t      pid;               // 从进程的进程ID
    aio_t*     dn;                // 下行管道(主->从)，用于发送命令到从进程
    aio_t*     up;                // 上行管道(从->主)，用于接收从进程的响应和数据
    u1_t       state;             // 从进程当前状态(运行/停止/重启等)
    u1_t       killCnt;           // 杀死尝试计数，用于逐步升级信号强度
    u1_t       restartCnt;        // 重启计数，防止频繁重启导致的无限循环
    u1_t       antennaType;       // 天线类型，用于天线分集和切换功能
    dbuf_t     sx1301confJson;    // SX130X配置JSON数据缓冲区
    chdefl_t   upchs;             // 分配给该从进程的上行信道配置
    int        last_expcmd;       // 上次期望的命令ID，用于处理异步响应
    // 读取溢出缓冲区：处理管道读取时的数据分片问题
    struct {
        u1_t buf[PIPE_BUF];       // 缓冲区数据
        int off;                  // 当前偏移位置
        int exp;                  // 期望的数据长度
    } rsb;
} slave_t;

//=============================================================================
// 主进程全局状态变量
//=============================================================================
static int       n_slaves;       // 从进程总数量
static slave_t*  slaves;         // 从进程状态数组
static pid_t     master_pid;     // 主进程PID，用于进程管理
static u4_t      region;         // 区域码，用于LBT(监听后发送)配置

// 前向声明：从进程重启函数
static void restart_slave (tmr_t* tmr);

//=============================================================================
// 函数功能：从从进程管道读取数据，处理命令响应和数据包
// 参数说明：slave - 从进程状态结构体指针
//          buf - 读取缓冲区
//          bufsize - 缓冲区大小
//          expcmd - 期望的命令ID，-1表示异步读取
//          expresp - 期望的响应结构体指针
// 返回值：1表示成功读取期望的响应，0表示未读取到或失败
// 调用时机：需要从从进程读取数据时调用，支持同步和异步模式
// 处理逻辑：解析管道数据，根据消息类型分发处理，支持数据分片重组
//=============================================================================
static int read_slave_pipe (slave_t* slave, u1_t* buf, int bufsize, int expcmd, struct ral_response* expresp) {
    u1_t slave_idx = (int)(slave-slaves);      // 计算从进程索引
    u1_t retries = 0;                          // 重试计数器
    u1_t expok = 0;                            // 期望响应是否已收到

    while(1) {
        int n = read(slave->up->fd, buf, bufsize);  // 从上行管道读取数据
        if( n == 0 ) {                         // 如果读取到EOF
            // EOF表示从进程已关闭管道，通常是进程异常退出
            LOG(MOD_RAL|ERROR, "Slave (%d) - EOF", slave_idx);
            rt_yieldTo(&slave->tmr, restart_slave);  // 触发从进程重启
            return expok;
        }
        if( n == -1 ) {                        // 如果读取出错
            if( errno == EAGAIN ) {            // 如果是非阻塞读取无数据
                if( expcmd == -1 )             // 如果是异步读取
                    return expok;              // 直接返回，不等待
                if( ++retries < 5 ) {          // 如果同步读取且重试次数未超限
                    rt_usleep(RETRY_PIPE_IO);  // 短暂等待后重试
                    continue;
                }
                LOG(MOD_RAL|WARNING, "Slave (%d) did not send reply data - expecting cmd=%d", slave_idx, expcmd);
                slave->last_expcmd = expcmd;   // 记录期望命令，等待异步响应
                return expok;
            }
            rt_fatal("Slave (%d) pipe read fail: %s", slave_idx, strerror(errno));  // 其他错误为致命错误
            // NOT REACHED
        }

        slave->restartCnt = 0;                 // 成功读取数据，重置重启计数
        int off = 0;                           // 数据处理偏移位置
        
        // 处理读取到的所有数据，可能包含多个消息
        while( off < n ) {
            int dlen = n - off;                // 剩余数据长度
            struct ral_header* hdr = (struct ral_header*)&buf[off];  // 消息头指针
            int consumed = 0;                  // 本次消息消耗的字节数

            // 处理上次未完成的分片数据
            if( slave->rsb.off ) {
                assert(slave->rsb.off<slave->rsb.exp);  // 确保分片状态正确
                int chunksz = min(slave->rsb.exp-slave->rsb.off, n-off);  // 计算本次复制的数据量
                memcpy(&slave->rsb.buf[slave->rsb.off], &buf[off], chunksz);  // 复制数据到分片缓冲区
                off += chunksz;                // 更新读取偏移
                slave->rsb.off += chunksz;     // 更新分片偏移
                if( slave->rsb.off < slave->rsb.exp ) {  // 如果分片仍未完整
                    continue;                  // 继续读取更多数据
                }
                // 分片完整，使用分片缓冲区的数据
                hdr = (struct ral_header*)slave->rsb.buf;
                dlen = slave->rsb.off;
            }

            // 根据消息类型进行处理
            if( expcmd >= 0 && hdr->cmd == expcmd ) {  // 处理期望的同步响应
                if( (slave->rsb.exp = sizeof(struct ral_response)) > dlen ) goto spill;  // 数据不完整，需要分片
                *expresp = *(struct ral_response*)hdr;  // 复制响应数据
                consumed = sizeof(*expresp);   // 记录消耗的字节数
                expok = 1;                     // 标记期望响应已收到
                slave->last_expcmd = expcmd = -1;  // 清除期望命令标记
            }
            else if( slave->last_expcmd >= 0 && hdr->cmd == slave->last_expcmd ) {  // 处理过期的异步响应
                if( (slave->rsb.exp = sizeof(struct ral_response)) > dlen ) goto spill;
                LOG(MOD_RAL|WARNING, "Slave (%d) responded to expired synchronous cmd: %d. Ignoring.", slave_idx, hdr->cmd);
                consumed = sizeof(struct ral_response);
                slave->last_expcmd = -1;       // 清除过期命令标记
            }
            else if( hdr->cmd == RAL_CMD_TIMESYNC ) {  // 处理时间同步响应
                if( (slave->rsb.exp= sizeof(struct ral_timesync_resp)) > dlen ) goto spill;
                struct ral_timesync_resp* resp = (struct ral_timesync_resp*)hdr;
                // 更新时间同步状态，获取下次同步延迟
                ustime_t delay = ts_updateTimesync(slave_idx, resp->quality, &resp->timesync);
                rt_setTimer(&slave->tsync, rt_micros_ahead(delay));  // 设置下次时间同步定时器
                consumed = sizeof(*resp);
            }
            else if( hdr->cmd == RAL_CMD_RX ) {        // 处理接收数据包
                if( (slave->rsb.exp = sizeof(struct ral_rx_resp)) > dlen ) goto spill;
                struct ral_rx_resp* resp = (struct ral_rx_resp*)hdr;
                // 获取下一个接收作业槽位
                rxjob_t* rxjob = !TC ? NULL : s2e_nextRxjob(&TC->s2ctx);
                if( rxjob != NULL ) {
                    // 复制接收数据到接收队列
                    memcpy(&TC->s2ctx.rxq.rxdata[rxjob->off], resp->rxdata, resp->rxlen);
                    rxjob->len = resp->rxlen;      // 设置数据包长度
                    rxjob->freq = resp->freq;      // 设置接收频率
                    rxjob->rctx = resp->rctx;      // 设置接收上下文(从进程ID)
                    rxjob->xtime = resp->xtime;    // 设置接收时间戳
                    rxjob->rssi = resp->rssi;      // 设置RSSI值
                    rxjob->snr = resp->snr;        // 设置SNR值
                    // 将RPS转换为数据速率
                    rxjob->dr = s2e_rps2dr(&TC->s2ctx, resp->rps);
                    if( rxjob->dr == DR_ILLEGAL ) {  // 如果数据速率转换失败
                        LOG(MOD_RAL|ERROR, "Unable to map to an up DR: %R", resp->rps);
                    } else {
                        s2e_addRxjob(&TC->s2ctx, rxjob);    // 添加接收作业到队列
                        s2e_flushRxjobs(&TC->s2ctx);        // 刷新接收作业给上层处理(XXX：可能需要优化)
                    }
                } else {
                    LOG(MOD_RAL|ERROR, "Slave (%d) has RX frame dropped - out of space", slave_idx);
                }
                consumed = sizeof(*resp);
            }
            else {
                // 收到意外的数据类型
                rt_fatal("Slave (%d) sent unexpected data: cmd=%d size=%d", slave_idx, hdr->cmd, dlen);
            }

            // 处理消息完成，清理状态
            if( slave->rsb.off ) {             // 如果使用了分片缓冲区
                slave->rsb.off = 0;            // 重置分片状态
            } else {
                off += consumed;               // 更新读取偏移
            }
            continue;

        spill:  // 数据分片处理：当前数据不完整，需要缓存等待更多数据
            if( sizeof(slave->rsb.buf)-slave->rsb.off < dlen ) {  // 检查分片缓冲区空间
                rt_fatal("Slave (%d) Cannot store data in slave->rsb.buf size=%d slave->rsb.off=%d", slave_idx, n-off, slave->rsb.off);
            } else {
                // 将不完整数据复制到分片缓冲区
                memcpy(&slave->rsb.buf[slave->rsb.off], hdr, dlen);
                slave->rsb.off += dlen;        // 更新分片偏移
                off += dlen;                   // 更新读取偏移
            }
        }
        assert(off==n);                        // 确保所有数据都已处理
    }
}

//=============================================================================
// 函数功能：异步管道读取回调函数
// 参数说明：aio - 异步IO对象指针
// 返回值：无
// 调用时机：从进程管道有数据可读时自动调用
// 执行逻辑：调用read_slave_pipe进行异步数据读取，不等待特定响应
//=============================================================================
static void pipe_read (aio_t* aio) {
    slave_t* slave = (slave_t*)aio->ctx;       // 从AIO上下文获取从进程状态
    u1_t buf[PIPE_BUF];                        // 临时读取缓冲区
    struct ral_response resp;                  // 临时响应结构体
    read_slave_pipe(slave, buf, sizeof(buf), -1, &resp);  // 异步读取(-1表示不等待特定命令)
}

//=============================================================================
// 函数功能：杀死所有从进程
// 参数说明：无
// 返回值：无
// 调用时机：主进程退出或需要重置所有从进程时调用
// 执行逻辑：向所有活动的从进程发送终止信号
//=============================================================================
static void killAllSlaves () {
    LOG(MOD_RAL|INFO, "Killing all slaves");
    for( int i=0; i<n_slaves; i++ ) {
        slave_t* slave = &slaves[i];
        if( is_slave_alive(slave) ) {          // 如果从进程仍然活动
            LOG(MOD_RAL|INFO, "Slave pid=%d idx=%d: Killing", slave->pid, i);
            kill(slave->pid, SIGKILL);         // 发送强制杀死信号
        }
    }
}

//=============================================================================
// 函数功能：检查从进程是否仍然存活
// 参数说明：slave - 从进程状态结构体指针
// 返回值：1表示存活，0表示已死亡
// 调用时机：需要确认从进程状态时调用
// 检查原理：使用waitpid的WNOHANG选项非阻塞检查进程状态
//=============================================================================
static int is_slave_alive (slave_t* slave) {
    if( slave->pid <= 0 )                     // 如果PID无效
        return 0;                             // 认为进程已死亡
    int status = 0;
    pid_t wpid = waitpid(slave->pid, &status, WNOHANG);  // 非阻塞等待子进程
    if( wpid == slave->pid ) {                // 如果进程已结束
        if( WIFEXITED(status) ) {             // 如果是正常退出
            LOG(MOD_RAL|INFO, "Slave pid=%d exited normally: status=%d", slave->pid, WEXITSTATUS(status));
        } else if( WIFSIGNALED(status) ) {    // 如果是被信号杀死
            LOG(MOD_RAL|INFO, "Slave pid=%d killed by signal: %d", slave->pid, WTERMSIG(status));
        } else {
            LOG(MOD_RAL|INFO, "Slave pid=%d died: status=0x%04X", slave->pid, status);
        }
        slave->pid = 0;                       // 清除PID标记
        return 0;                             // 进程已死亡
    }
    if( wpid == -1 && errno == ECHILD ) {    // 如果进程不存在
        LOG(MOD_RAL|DEBUG, "Slave pid=%d: No such process", slave->pid);
        slave->pid = 0;                       // 清除PID标记
        return 0;                             // 进程已死亡
    }
    return 1;                                 // 进程仍然存活
}

//=============================================================================
// 函数功能：重新检查从进程状态的定时器回调
// 参数说明：tmr - 定时器结构体指针
// 返回值：无
// 调用时机：定期检查从进程状态，确保从进程正常运行
// 执行逻辑：检查从进程是否存活，如果死亡则触发重启
//=============================================================================
static void recheck_slave (tmr_t* tmr) {
    slave_t* slave = memberof(slave_t, tmr, tmr);  // 从定时器获取从进程状态
    if( !is_slave_alive(slave) ) {            // 如果从进程已死亡
        restart_slave(tmr);                   // 触发重启
    } else {
        // 从进程仍然存活，设置下次检查定时器
        rt_setTimer(tmr, rt_micros_ahead(WAIT_SLAVE_PID_INTV));
    }
}

//=============================================================================
// 函数功能：执行从进程，设置环境并启动从进程程序
// 参数说明：idx - 从进程索引，rdfd - 读取文件描述符，wrfd - 写入文件描述符
// 返回值：无(函数不会返回，因为会执行exec)
// 调用时机：fork后在子进程中调用，设置从进程执行环境
// 执行逻辑：设置环境变量，重定向标准输入输出，执行station程序
//=============================================================================
static void execSlave (int idx, int rdfd, int wrfd) {
    // 关闭除了通信管道外的所有文件描述符
    for( int i=3; i<256; i++ ) {
        if( i != rdfd && i != wrfd )           // 保留通信管道
            close(i);                          // 关闭其他文件描述符
    }
    // 重定向标准输入输出到通信管道
    dup2(rdfd, 0);                            // 标准输入连接到从主进程读取管道
    dup2(wrfd, 1);                            // 标准输出连接到向主进程写入管道
    close(rdfd);                              // 关闭原始文件描述符
    close(wrfd);
    
    // 设置从进程环境变量
    char sidxs[16];
    snprintf(sidxs, sizeof(sidxs), "%d", idx);
    setenv("SLAVE_IDX", sidxs, 1);            // 设置从进程索引环境变量
    
    // 执行station程序作为从进程
    char* argv[] = { (char*)sys_exe, "--slave", NULL };
    execv(sys_exe, argv);                     // 执行station程序
    rt_fatal("execv failed: %s", strerror(errno));  // 如果exec失败
    // NOT REACHED
}

//=============================================================================
// 函数功能：向从进程管道写入数据
// 参数说明：slave - 从进程状态结构体指针，data - 要写入的数据，len - 数据长度
// 返回值：成功返回1，失败返回0
// 调用时机：需要向从进程发送命令或数据时调用
// 执行逻辑：非阻塞写入数据到管道，处理管道满的情况
//=============================================================================
static int write_slave_pipe (slave_t* slave, void* data, int len) {
    assert(len < PIPE_BUF);                   // 确保数据长度在管道缓冲区限制内
    int retries = 0;                          // 重试计数器
    while(1) {
        int n = write(slave->dn->fd, data, len);  // 向下行管道写入数据
        if( n == len )                        // 如果写入成功
            return 1;                         // 返回成功
        if( n == -1 ) {                       // 如果写入失败
            if( errno == EPIPE ) {            // 如果管道已断开
                LOG(MOD_RAL|ERROR, "Slave (%d) - Broken pipe", (int)(slave-slaves));
                return 0;                     // 返回失败
            }
            if( errno == EAGAIN ) {           // 如果管道缓冲区满
                if( ++retries > 10 ) {        // 如果重试次数过多
                    LOG(MOD_RAL|ERROR, "Slave (%d) - Pipe full, too many retries", (int)(slave-slaves));
                    return 0;                 // 返回失败
                }
                rt_usleep(rt_millis(10));     // 等待10ms后重试
                continue;
            }
            rt_fatal("Slave (%d) pipe write fail: %s", (int)(slave-slaves), strerror(errno));
        }
        rt_fatal("Slave (%d) pipe write partial: %d/%d", (int)(slave-slaves), n, len);
    }
}

//=============================================================================
// 函数功能：向从进程发送配置命令
// 参数说明：slave - 从进程状态结构体指针
// 返回值：无
// 调用时机：从进程启动后需要配置硬件时调用
// 执行逻辑：构造配置请求，发送JSON配置和信道分配信息给从进程
//=============================================================================
static void send_config (slave_t* slave) {
    struct ral_config_req req = { .cmd = RAL_CMD_CONFIG, .rctx = 0 };  // 初始化配置请求
    strcpy(req.hwspec, "sx1301/1");           // 设置硬件规格
    int jlen = slave->sx1301confJson.bufsize; // 获取JSON配置数据长度
    if( jlen >= sizeof(req.json) )            // 检查JSON数据是否过大
        rt_fatal("JSON of sx1301conf to big for pipe: %d > %d", jlen, sizeof(req.json));
    if( jlen > 0 ) {                          // 如果有配置数据
        req.region = region;                  // 设置区域码
        req.jsonlen = jlen;                   // 设置JSON数据长度
        req.upchs = slave->upchs;             // 设置信道配置
        memcpy(req.json, slave->sx1301confJson.buf, jlen);  // 复制JSON数据
        LOG(MOD_RAL|INFO, "Master sending %d bytes of JSON sx1301conf to slave (%d)", jlen, (int)(slave-slaves));
        if( !write_slave_pipe(slave, &req, sizeof(req)) )  // 发送配置请求
            rt_fatal("Failed to send sx1301conf");     // 发送失败为致命错误
    }
}

//=============================================================================
// 函数功能：请求从进程进行时间同步的定时器回调
// 参数说明：tmr - 定时器结构体指针  
// 返回值：无
// 调用时机：定期向从进程请求时间同步测量
// 执行逻辑：发送时间同步请求给从进程
//=============================================================================
static void req_timesync (tmr_t* tmr) {
    slave_t* slave = memberof(slave_t, tmr, tsync);  // 从定时器获取从进程状态
    struct ral_timesync_req req = { .cmd = RAL_CMD_TIMESYNC, .rctx = 0 };  // 初始化时间同步请求
    if( !write_slave_pipe(slave, &req, sizeof(req)) )  // 发送时间同步请求
        rt_fatal("Failed to send ral_timesync_req");   // 发送失败为致命错误
}

//=============================================================================
// 函数功能：重启从进程的主要处理函数
// 参数说明：tmr - 定时器结构体指针
// 返回值：无
// 调用时机：从进程死亡或需要重启时调用
// 执行逻辑：清理旧进程，创建新进程，设置通信管道，发送配置
// 重启策略：先尝试温和终止(SIGTERM)，然后强制杀死(SIGKILL)，最后创建新进程
//=============================================================================
static void restart_slave (tmr_t* tmr) {
    slave_t* slave = memberof(slave_t, tmr, tmr);  // 从定时器获取从进程状态
    pid_t pid = slave->pid;                   // 获取当前从进程PID
    int slaveIdx = (int)(slave-slaves);       // 计算从进程索引

    if( ++slave->restartCnt > 4 ) {           // 如果重启次数过多
        // 防止从进程频繁重启导致的无限循环
        rt_fatal("Slave %d restarted %d times without successful interaction",
                 slaveIdx, slave->restartCnt);
    }
    
    // 清理旧的定时器和管道
    rt_clrTimer(&slave->tmr);                 // 清除监控定时器
    rt_clrTimer(&slave->tsync);               // 清除时间同步定时器
    aio_close(slave->up);                     // 关闭上行管道
    aio_close(slave->dn);                     // 关闭下行管道
    slave->up = slave->dn = NULL;             // 清空管道指针

    if( is_slave_alive(slave) ) {             // 如果从进程仍然存活
        LOG(MOD_RAL|INFO, "Slave pid=%d idx=%d: Trying kill (cnt=%d)", slaveIdx, pid, slave->killCnt);
        // 逐步升级信号强度：SIGTERM -> SIGKILL
        int err = kill(pid, slave->killCnt <= 2 ? SIGTERM : SIGKILL);
        if( err == -1 && errno != ESRCH )     // 如果杀死失败且进程存在
            LOG(MOD_RAL|ERROR, "kill failed: %s", strerror(errno));
        slave->killCnt += 1;                  // 增加杀死尝试计数
        rt_setTimerCb(&slave->tmr, rt_micros_ahead(RETRY_KILL_INTV), restart_slave);  // 稍后重试
        return;
    }
    
    // 创建新的通信管道
    int up[2] = { -1, -1 };                   // 上行管道文件描述符对
    int dn[2] = { -1, -1 };                   // 下行管道文件描述符对
    if( pipe2(up, O_NONBLOCK) == -1 || pipe2(dn, O_NONBLOCK) == -1 ) {  // 创建非阻塞管道
        rt_fatal("Failed to create pipe: %s", strerror(errno));
    }
    // 设置管道的异步IO
    slave->up = aio_open(slave, up[0], pipe_read, NULL);    // 主进程读取端，有数据时调用pipe_read
    slave->dn = aio_open(slave, dn[1], NULL, NULL);        // 主进程写入端，仅用于O_CLOEXEC
    sys_flushLog();                           // 刷新日志缓冲区

    if( (pid = fork()) == 0 ) {               // 创建子进程
        // 这是子进程：执行从进程程序
        execSlave(slaveIdx, dn[0], up[1]);    // 设置环境并执行从进程
        // NOT REACHED
        assert(0);
    }
    else if( pid < 0 ) {                      // 如果fork失败
        rt_fatal("Fork failed: %s\n", strerror(errno));
    }
    
    // 主进程：完成从进程设置
    LOG(MOD_RAL|INFO, "Master has started slave: pid=%d idx=%d (attempt %d)", pid, slaveIdx, slave->restartCnt);
    close(up[1]);                             // 关闭主进程不需要的管道端
    close(dn[0]);
    slave->pid = pid;                         // 记录新的从进程PID
    send_config(slave);                       // 发送配置给新的从进程
    pipe_read(slave->up);                     // 开始异步读取从进程数据
    rt_yieldTo(&slave->tmr, recheck_slave);   // 启动从进程状态检查定时器
}

//=============================================================================
// 函数功能：获取备用天线单元列表，用于天线分集功能
// 参数说明：txunit - 主发送单元索引
// 返回值：位掩码表示可用的备用天线单元，每位对应一个从进程索引
// 调用时机：需要进行天线分集或备用天线切换时调用
// 执行逻辑：查找所有配置为全向天线的从进程，排除当前发送单元
//=============================================================================
u1_t ral_altAntennas (u1_t txunit) {
    // 检查发送单元有效性和天线类型
    if( txunit >= n_slaves || slaves[txunit].antennaType != SX130X_ANT_OMNI )
        return 0;                             // 无效单元或非全向天线，返回0
    u1_t v = 0;                               // 备用天线位掩码
    for( int sidx=0; sidx < n_slaves; sidx++ ) {  // 遍历所有从进程
        if( sidx == txunit || slaves[sidx].antennaType != SX130X_ANT_OMNI )
            continue;                         // 跳过当前单元和非全向天线
        v |= 1<<sidx;                         // 添加该从进程到备用天线列表
    }
    return v;                                 // 返回备用天线位掩码
}

//=============================================================================
// 函数功能：将发送单元ID转换为从进程状态指针，并进行有效性检查
// 参数说明：txunit - 发送单元索引，op - 操作名称(用于日志输出)
// 返回值：有效的从进程指针，失败返回NULL
// 调用时机：在所有发送相关操作前调用，确保发送单元有效且可用
// 检查项目：单元索引范围、从进程是否存活、通信管道是否正常
//=============================================================================
static slave_t* txunit2slave(u1_t txunit, str_t op) {
    if( txunit >= n_slaves ) {               // 检查发送单元索引是否有效
        LOG(MOD_RAL|ERROR, "Illegal radio txunit #%d - rejecting %s", txunit, op);
        return NULL;                          // 索引超出范围
    }
    slave_t* slave = &slaves[txunit];         // 获取从进程状态指针
    if( slave->dn == NULL ) {                 // 检查下行管道是否有效
        LOG(MOD_RAL|ERROR, "Slave #%d dead - rejecting %s", txunit, op);
        return NULL;                          // 从进程已死亡或未启动
    }
    return slave;                             // 返回有效的从进程指针
}

//=============================================================================
// 函数功能：执行数据包发送操作，支持信标模式和数据模式
// 参数说明：txjob - 发送作业结构体，s2ctx - S2E上下文，nocca - 是否禁用CCA检测
// 返回值：发送结果状态码(RAL_TX_OK/RAL_TX_FAIL/RAL_TX_NOCA)
// 调用时机：上层需要发送LoRa数据包时调用
// 执行逻辑：构造发送请求，发送给从进程，根据区域设置决定是否等待CCA结果
// 注意事项：当前libloragw API无法实现nocca功能
//=============================================================================
int ral_tx (txjob_t* txjob, s2ctx_t* s2ctx, int nocca) {
    // NOTE: nocca not possible to implement with current libloragw API
    slave_t* slave = txunit2slave(txjob->txunit, "tx");  // 获取有效的从进程
    if( slave == NULL )
        return RAL_TX_FAIL;                   // 从进程无效，发送失败
    
    // 构造发送请求结构体
    struct ral_tx_req req;                    
    memset(&req, 0, sizeof(req));             // 清零请求结构体
    req.cmd = nocca ? RAL_CMD_TX_NOCCA : RAL_CMD_TX;  // 设置命令类型(是否需要CCA)
    req.rctx = txjob->rctx;                   // 设置请求上下文
    // 构造RPS参数：将数据速率转换为RPS格式，如果是信标则添加BCN标志
    req.rps = (s2e_dr2rps(s2ctx, txjob->dr)
               | (txjob->txflags & TXFLAG_BCN ? RPS_BCN : 0));
    req.xtime = txjob->xtime;                 // 设置发送时间戳
    req.freq = txjob->freq;                   // 设置发送频率
    req.txpow = txjob->txpow;                 // 设置发送功率
    req.addcrc = txjob->addcrc;               // 设置是否添加CRC
    req.txlen = txjob->len;                   // 设置数据包长度
    // 复制发送数据
    memcpy(req.txdata, &s2ctx->txq.txdata[txjob->off], txjob->len);
    
    // 发送请求给从进程
    if( !write_slave_pipe(slave, &req, sizeof(req)) )
        return RAL_TX_FAIL;                   // 管道写入失败
    
    if( region == 0 )                         // 如果区域码为0(无LBT要求)
        return RAL_TX_OK;                     // 直接返回成功，不等待CCA结果
    
    // 等待从进程的CCA/LBT结果响应
    struct ral_response resp;
    u1_t buf[PIPE_BUF];                       // 临时缓冲区
    if( !read_slave_pipe(slave, buf, PIPE_BUF, RAL_CMD_TX, &resp) )
        return TXSTATUS_IDLE;                 // 读取响应失败
    return resp.status;                       // 返回CCA/LBT检测结果
}

//=============================================================================
// 函数功能：查询指定发送单元的发送状态
// 参数说明：txunit - 发送单元索引
// 返回值：发送状态(TXSTATUS_IDLE/TXSTATUS_SCHEDULED/TXSTATUS_EMITTING)
// 调用时机：需要检查数据包发送进度或硬件发送状态时调用
// 执行逻辑：向从进程发送状态查询请求，等待状态响应
//=============================================================================
int ral_txstatus (u1_t txunit) {
    slave_t* slave = txunit2slave(txunit, "tx");  // 获取有效的从进程
    if( slave == NULL )
        return TXSTATUS_IDLE;                 // 从进程无效，返回空闲状态
    
    // 构造发送状态查询请求
    struct ral_txstatus_req req = { .cmd = RAL_CMD_TXSTATUS, .rctx = txunit };
    if( !write_slave_pipe(slave, &req, sizeof(req)) )  // 发送查询请求
        return TXSTATUS_IDLE;                 // 发送失败，返回空闲状态
    
    // 等待从进程的状态响应
    struct ral_response resp;
    u1_t buf[PIPE_BUF];                       // 临时缓冲区
    if( !read_slave_pipe(slave, buf, PIPE_BUF, RAL_CMD_TXSTATUS, &resp) )
        return TXSTATUS_IDLE;                 // 读取响应失败
    return resp.status;                       // 返回实际发送状态
}

//=============================================================================
// 函数功能：中止指定发送单元的当前发送操作
// 参数说明：txunit - 发送单元索引
// 返回值：无
// 调用时机：需要紧急停止正在进行的发送操作时调用
// 执行逻辑：向从进程发送中止命令，不等待响应
//=============================================================================
void ral_txabort (u1_t txunit) {
    slave_t* slave = txunit2slave(txunit, "tx");  // 获取有效的从进程
    if( slave == NULL )
        return;                               // 从进程无效，直接返回
    
    // 构造发送中止请求
    struct ral_txstatus_req req = { .cmd = RAL_CMD_TXABORT, .rctx = txunit };
    write_slave_pipe(slave, &req, sizeof(req));  // 发送中止命令(不等待响应)
}

//=============================================================================
// 函数功能：信道分配回调函数，将分配的信道配置到相应的从进程
// 参数说明：ctx - 上下文指针(包含芯片数量)，ch - 信道分配信息，flag - 分配阶段标志
// 返回值：无
// 调用时机：ral_challoc进行信道分配时自动调用
// 执行逻辑：根据分配阶段，将信道频率和RPS配置保存到对应的从进程状态中
//=============================================================================
static void slave_challoc_cb (void* ctx, challoc_t* ch, int flag) {
    if( ctx == NULL ) return;                 // 上下文无效，直接返回
    int n1301 = *(int*)ctx;                   // 获取芯片数量
    switch( flag ) {                          // 根据分配阶段执行不同操作
    case CHALLOC_START: {                     // 分配开始阶段
        break;                                // 无需特殊处理
    }
    case CHALLOC_CHIP_START: {                // 芯片分配开始阶段
        break;                                // 无需特殊处理
    }
    case CHALLOC_CH: {                        // 信道分配阶段
        if( ch->chip > n1301 ) break;         // 芯片索引超出范围，跳过
        // 将分配的信道频率和RPS保存到对应从进程的信道配置中
        slaves[ch->chip].upchs.freq[ch->chan] = ch->chdef.freq;  // 设置信道频率
        slaves[ch->chip].upchs.rps[ch->chan] = ch->chdef.rps;    // 设置信道RPS参数
        break;
    }
    case CHALLOC_CHIP_DONE: {                 // 芯片分配完成阶段
        break;                                // 无需特殊处理
    }
    case CHALLOC_DONE: {                      // 所有分配完成阶段
        break;                                // 无需特殊处理
    }
    }
}

//=============================================================================
// 函数功能：配置RAL模块，解析JSON配置并分配给各从进程
// 参数说明：hwspec - 硬件规格字符串，cca_region - CCA区域码，json - JSON配置，
//          jsonlen - JSON长度，upchs - 上行信道定义
// 返回值：成功返回1，失败返回0
// 调用时机：系统启动时或需要重新配置硬件时调用
// 执行逻辑：解析JSON配置，分配信道，将配置发送给各从进程
// 配置策略：支持配置复制和天线分集功能
//=============================================================================
int ral_config (str_t hwspec, u4_t cca_region, char* json, int jsonlen, chdefl_t* upchs) {
    // 检查硬件规格是否支持
    if( strncmp(hwspec, "sx1301/", 7) != 0 ) {
        LOG(MOD_RAL|ERROR, "Unsupported hwspec=%s", hwspec);
        return 0;                             // 不支持的硬件规格
    }
    
    // 释放之前的配置内存
    for( int i=0; i<n_slaves; i++ )
        dbuf_free(&slaves[i].sx1301confJson);

    // 解析JSON配置
    ujdec_t D;                                // JSON解码器
    uj_iniDecoder(&D, json, jsonlen);         // 初始化解码器
    if( uj_decode(&D) ) {                     // 解码JSON
        LOG(MOD_RAL|ERROR, "Parsing of sx1301 channel setup JSON failed");
        return 0;                             // JSON解析失败
    }
    if( uj_null(&D) ) {                       // 检查是否为空配置
        LOG(MOD_RAL|ERROR, "sx1301_conf is null but a hw setup IS required - no fallbacks");
        return 0;                             // 配置为空但硬件需要配置
    }
    
    // 解析配置数组，为每个从进程分配配置
    uj_enterArray(&D);                        // 进入JSON数组
    int slaveIdx, n1301=0;                    // 从进程索引和配置数量计数
    while( (slaveIdx = uj_nextSlot(&D)) >= 0 ) {  // 遍历数组元素
        n1301 = slaveIdx+1;                   // 更新配置数量
        if( slaveIdx < n_slaves ) {           // 如果从进程索引有效
            // 复制该从进程的JSON配置数据
            slaves[slaveIdx].sx1301confJson = dbuf_dup(uj_skipValue(&D));
        } else {
            uj_skipValue(&D);                 // 跳过多余的配置
        }
    }
    uj_exitArray(&D);                         // 退出JSON数组
    uj_assertEOF(&D);                         // 确保JSON解析完整
    
    if( n1301 == 0 ) {                        // 检查是否有有效配置
        LOG(MOD_RAL|ERROR, "sx1301_conf is empty but a hw setup IS required - no fallbacks");
        return 0;                             // 无有效配置
    }

    // 执行信道分配
    ral_challoc(upchs, slave_challoc_cb, &n1301);  // 调用信道分配算法

    // 验证硬件规格与配置数量的一致性
    str_t s = hwspec+7;                       // 跳过"sx1301/"前缀
    int specn = rt_readDec(&s);               // 读取硬件规格中的数量
    if( specn != n1301 ) {                    // 检查数量是否一致
        LOG(MOD_RAL|ERROR, "hwspec=%s and size of sx1301_conf array (%d) not in sync", hwspec, n1301);
        return 0;                             // 硬件规格与配置不匹配
    }
    if( n1301 > n_slaves ) {                  // 检查配置数量是否超出硬件能力
        LOG(MOD_RAL|ERROR, "Region plan asks for hwspec=%s which exceeds actual hardware: sx1301/%d", hwspec, n_slaves);
        return 0;                             // 配置要求超出实际硬件
    }
    
    // 处理配置复制和天线分集
    if( n1301 < n_slaves ) {                  // 如果配置少于从进程数量
        if( n_slaves % n1301 != 0 ) {         // 检查是否能整数倍复制
            LOG(MOD_RAL|WARNING, "Region plan hwspec '%s' cannot be replicated onto routers 'sx1301/%d' - router is underutilized",
                hwspec, n_slaves);            // 无法整数倍复制，路由器未充分利用
        } else {
            // 复制配置到剩余的从进程，实现天线分集
            for( int si=n1301, sj=0; si < n_slaves; si++, sj=(sj+1)%n1301 ) {
                slaves[si].upchs = slaves[sj].upchs;              // 复制信道配置
                slaves[si].sx1301confJson = dbuf_dup(slaves[sj].sx1301confJson);  // 复制JSON配置
            }
            LOG(MOD_RAL|WARNING, "Region plan hwspec '%s' replicated %d times onto slaves 'sx1301/%d' - assuming antenna diversity",
                hwspec, n_slaves/n1301, n_slaves);  // 配置已复制，假设天线分集
        }
    } else {
        LOG(MOD_RAL|INFO, "Region plan hwspec '%s' mapped to %d slaves 'sx1301/1'", hwspec, n_slaves);
    }
    
    // 保存区域码并发送配置给各从进程
    region = cca_region;                      // 保存CCA区域码
    for( int i=0; i < n_slaves; i++ ) {       // 遍历所有从进程
        send_config(&slaves[i]);              // 发送配置给每个从进程
    }
    return 1;                                 // 配置成功
}

//=============================================================================
// 函数功能：初始化RAL主进程模块，启动所有从进程
// 参数说明：无
// 返回值：无
// 调用时机：系统启动时调用，建立主从进程架构
// 执行逻辑：检测配置文件，分配从进程状态，设置信号处理，启动所有从进程
// 初始化过程：配置文件检查 -> 内存分配 -> 信号设置 -> 从进程启动
//=============================================================================
void ral_ini () {
    // 检测从进程配置文件数量
    int slaveCnt = 0;                         // 从进程数量计数
    while(1) {
        char cfname[64];                      // 配置文件名缓冲区
        snprintf(cfname, sizeof(cfname), "slave-%d.conf", slaveCnt);  // 构造配置文件名
        dbuf_t b = sys_checkFile(cfname);     // 检查配置文件是否存在
        if( b.buf == NULL )                   // 如果文件不存在
            break;                            // 结束检测
        free(b.buf);                          // 释放文件内容缓冲区
        slaveCnt += 1;                        // 增加从进程计数
    }
    
    // 验证从进程数量
    if( slaveCnt == 0 || slaveCnt > MAX_TXUNITS )
        rt_fatal("%s 'slave-N.conf' files found  (N=0,1,..,%d)",
                 slaveCnt ? "Too many" : "No", MAX_TXUNITS-1);  // 配置文件数量不合法

    // 分配从进程状态数组
    assert(slaves == NULL);                   // 确保尚未初始化
    n_slaves = slaveCnt;                      // 设置从进程数量
    slaves = rt_mallocN(slave_t, n_slaves);   // 分配从进程状态数组内存
    
    // 解析各从进程的配置文件
    int allok = 1;                            // 所有配置是否成功的标志
    for( int sidx=0; sidx < n_slaves; sidx++ ) {  // 遍历所有从进程
        struct sx130xconf sx1301conf;         // 临时配置结构体
        // 解析从进程配置(使用默认的最小配置)
        if( !sx130xconf_parse_setup(&sx1301conf, sidx, "sx1301/1", "{}", 2) ) {
            allok = 0;                        // 配置解析失败
        } else {
            slaves[sidx].antennaType = sx1301conf.antennaType;  // 保存天线类型
        }
        slaves[sidx].last_expcmd = -1;        // 初始化期望命令为无
    }
    if( !allok )                              // 如果有配置失败
        rt_fatal("Failed to load/parse some slave config files");  // 致命错误

    // 设置进程管理和信号处理
    master_pid = getpid();                    // 记录主进程PID
    atexit(killAllSlaves);                    // 注册退出时清理函数
    signal(SIGPIPE, SIG_IGN);                 // 忽略管道破裂信号

    // 初始化并启动所有从进程
    for( int i=0; i<n_slaves; i++ ) {         // 遍历所有从进程槽位
        rt_iniTimer(&slaves[i].tmr, NULL);    // 初始化监控定时器
        rt_iniTimer(&slaves[i].tsync, req_timesync);  // 初始化时间同步定时器
        rt_yieldTo(&slaves[i].tmr, restart_slave);    // 启动从进程(通过重启函数)
    }
}

//=============================================================================
// 函数功能：停止RAL模块，优雅关闭所有从进程
// 参数说明：无
// 返回值：无
// 调用时机：系统关闭或需要停止RAL服务时调用
// 执行逻辑：向所有从进程发送停止命令，清理时间同步定时器
//=============================================================================
void ral_stop () {
    // 构造停止命令
    struct ral_timesync_req req = { .cmd = RAL_CMD_STOP, .rctx = 0 };
    
    // 向所有从进程发送停止命令
    for( int slaveIdx=0; slaveIdx < n_slaves; slaveIdx++ ) {  // 遍历所有从进程
        slave_t* slave = &slaves[slaveIdx];   // 获取从进程状态
        rt_clrTimer(&slave->tsync);           // 清除时间同步定时器
        write_slave_pipe(slave, &req, sizeof(req));  // 发送停止命令
    }
}

#endif // defined(CFG_lgw1) && defined(CFG_ral_master_slave)
