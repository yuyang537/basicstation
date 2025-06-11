//=============================================================================
// 文件功能：命令FIFO模块 - 为BasicStation提供外部命令输入接口
// 作用说明：通过命名管道(FIFO)接收外部命令，支持以下核心功能：
//          1. FIFO管道监听：监控命名管道文件的读取事件
//          2. 命令解析处理：解析文本命令和JSON格式的消息
//          3. 智能重连机制：自动检测FIFO状态并重新打开连接
//          4. 日志级别控制：支持动态修改系统日志输出级别
//          5. JSON消息转发：将JSON命令转发到LNS服务器
//          6. 异步IO处理：基于事件驱动的非阻塞命令处理
//          7. 错误恢复机制：处理FIFO断开和重连的各种异常情况
//          8. 缓冲区管理：支持命令行拼接和多行命令处理
//
// 技术特点：
//          - 基于命名管道的进程间通信，支持多个客户端
//          - 事件驱动的异步处理架构，无阻塞操作
//          - 智能的FIFO状态检测和自动重连机制
//          - 支持文本命令和JSON格式两种输入方式
//          - 内置的日志级别动态控制功能
//          - 与TC模块集成，支持WebSocket消息转发
//          - 完善的错误处理和资源清理机制
//          - 定时器驱动的重连策略，确保服务可用性
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

#if defined(CFG_linux)                       // 仅在Linux系统下编译此模块

#include <unistd.h>                          // UNIX标准定义，提供read/write等系统调用
#include <fcntl.h>                           // 文件控制定义，提供open标志位和操作函数
#include <sys/stat.h>                        // 文件状态定义，提供stat结构体和文件类型检查
#include <errno.h>                           // 错误码定义，提供errno全局变量

#include "s2conf.h"                          // S2配置系统，提供配置参数定义
#include "rt.h"                              // 运行时系统，提供定时器和时间管理
#include "tc.h"                              // 传输连接模块，提供WebSocket通信接口


//=============================================================================
// 模块全局状态变量
//=============================================================================
static str_t  fifo;                         // FIFO文件路径，存储命名管道的完整路径
static aio_t* aio;                           // 异步IO对象指针，用于FIFO的事件驱动读取
static int    fill;                          // 当前命令行缓冲区填充长度
static char   cmdline[PIPE_BUF];            // 命令行缓冲区，大小为系统管道缓冲区大小
static tmr_t  reopen_tmr;                   // 重连定时器，用于定期尝试重新打开FIFO


// 函数前向声明
static int fifo_reopen ();

//=============================================================================
// 函数功能：FIFO重连超时处理回调函数
// 参数说明：tmr - 定时器对象指针，NULL表示立即触发
// 返回值：无
// 调用时机：定时器到期或需要立即重连时调用
// 执行逻辑：尝试重新打开FIFO，失败则设置下次重试定时器
//=============================================================================
static void reopen_timeout (tmr_t* tmr) {
    if( tmr == NULL || !fifo_reopen() )      // 如果是立即调用或重连失败
        rt_setTimer(&reopen_tmr, rt_micros_ahead(CMD_REOPEN_FIFO_INTV));  // 设置重连定时器
}


//=============================================================================
// 函数功能：FIFO数据读取回调函数 - 处理从FIFO接收的命令数据
// 参数说明：_aio - 异步IO对象指针
// 返回值：无
// 调用时机：FIFO有数据可读时由AIO系统自动调用
// 执行逻辑：循环读取数据->解析命令行->分发处理->更新缓冲区
// 命令类型：日志级别控制命令、JSON格式的LNS消息
//=============================================================================
static void fifo_read(aio_t* _aio) {
    assert(aio == _aio);                     // 验证AIO对象一致性
    int n;                                   // 读取的字节数
    while(1) {                               // 持续读取直到无数据可读
        n = read(aio->fd, cmdline+fill, sizeof(cmdline)-fill);  // 从FIFO读取数据到缓冲区
        if( n == -1 ) {                      // 如果读取发生错误
            if( errno == EAGAIN )            // 如果是非阻塞操作的"再试一次"错误
                return;                      // 正常退出，等待下次可读事件
            LOG(ERROR, "Failed to read CMD from '%s': %s", fifo, strerror(errno));  // 记录读取错误
            n = 0;                           // 将错误视为EOF处理
        }
        if( n == 0 ) {                       // 如果读取到EOF(客户端关闭连接)
            // FIFO写端关闭，需要重新打开FIFO等待新的连接
            aio_close(aio);                  // 关闭当前AIO连接
            aio = NULL;                      // 清空AIO指针
            reopen_timeout(NULL);            // 立即触发重连
            return;                          // 退出函数
        }
        n = fill += n;                       // 更新缓冲区填充长度
        int i=0;                             // 命令解析索引
        while( i<n ) {                       // 遍历所有接收到的数据
            if( cmdline[i] != '\n' ) {       // 如果不是换行符
                i++;                         // 继续查找下一个字符
                continue;
            }
            cmdline[i] = 0;                  // 将换行符替换为字符串结束符
            str_t err = NULL;                // 错误信息指针

            if( cmdline[0] != '{' ) {        // 如果不是JSON格式(非'{'开头)
                // 处理内置的文本命令，主要是日志级别控制
                int lvl = log_str2level(cmdline);  // 尝试解析为日志级别
                if( lvl >= 0 ) {             // 如果是有效的日志级别
                    log_setLevel(lvl);       // 设置新的日志级别
                } else {                     // 如果不是有效的日志级别命令
                    // 当前仅支持日志级别命令，其他文本命令视为未知
                    err = "Unknown fifo command";  // 设置错误信息
                }
            }
            else if( TC ) {                  // 如果是JSON格式且TC模块已连接
                // 将JSON命令转发到LNS服务器
                ujbuf_t sendbuf = (*TC->s2ctx.getSendbuf)(&TC->s2ctx, i);  // 获取发送缓冲区
                if( sendbuf.buf != NULL ) {  // 如果成功获取到发送缓冲区
                    memcpy(sendbuf.buf, cmdline, i);  // 复制JSON命令到发送缓冲区
                    sendbuf.pos = i;         // 设置数据长度
                    LOG(INFO, "CMD sent: %.40s%s", cmdline, i>40?"..":"");  // 记录发送的命令(截断显示)
                    (*TC->s2ctx.sendText)(&TC->s2ctx, &sendbuf);  // 通过WebSocket发送到LNS
                } else {                     // 如果无法获取发送缓冲区
                    err = "Not enough WS space to sent command";  // WebSocket发送缓冲区满
                }
            } else {                         // 如果是JSON格式但TC模块未连接
                err = "Command dropped - not connected right now";  // 当前未连接到LNS
            }
            if( err ) {                      // 如果有错误信息
                LOG(ERROR,"%s: %.20s%s", err, cmdline, i>20?"..":"");  // 记录错误和命令内容
            }
            fill = n -= i+1;                 // 计算剩余未处理的数据长度
            memcpy(cmdline, cmdline+i+1, n); // 将剩余数据移动到缓冲区开头
            i = 0;                           // 重置解析索引
        }
    }
}


//=============================================================================
// 函数功能：关闭FIFO连接和清理资源
// 参数说明：无
// 返回值：无
// 调用时机：程序退出或需要关闭FIFO时调用
// 执行逻辑：检查AIO连接状态并安全关闭
//=============================================================================
static void fifo_close () {
    if( aio == NULL )                        // 如果AIO连接已经关闭
        return;                              // 直接返回
    aio_close(aio);                          // 关闭AIO连接
    aio = NULL;                              // 清空AIO指针
}


//=============================================================================
// 函数功能：重新打开FIFO连接
// 参数说明：无
// 返回值：成功返回1，失败返回0
// 调用时机：初始连接或重连时调用
// 执行逻辑：检查FIFO文件->打开文件->设置AIO->注册清理
// 状态检查：使用stat系统调用验证文件存在且为FIFO类型
//=============================================================================
static int fifo_reopen () {
    struct stat st;                          // 文件状态结构体
    int fd;                                  // 文件描述符

    if( aio ) {                              // 如果当前有AIO连接
        aio_close(aio);                      // 先关闭现有连接
        aio = NULL;                          // 清空AIO指针
    }
    if( stat(fifo, &st) == -1  || (st.st_mode & S_IFMT) != S_IFIFO )  // 检查文件状态
        return 0;                            // 文件不存在或不是FIFO类型，返回失败
    if( (fd = open(fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) == -1 ) {  // 以只读非阻塞模式打开FIFO
        LOG(ERROR, "Failed to open cmd FIFO '%s': %s", fifo, strerror(errno));  // 记录打开失败错误
        return 0;                            // 返回失败
    }
    // 使用FIFO路径作为AIO上下文的虚拟指针
    aio = aio_open(&fifo, fd, fifo_read, NULL);  // 创建AIO对象，设置读回调为fifo_read
    atexit(fifo_close);                      // 注册程序退出时的清理函数
    fifo_read(aio);                          // 立即尝试读取数据(处理可能已存在的数据)
    return 1;                                // 返回成功
}


//=============================================================================
// 函数功能：启用命令FIFO功能的外部接口
// 参数说明：file - FIFO文件路径字符串
// 返回值：无
// 调用时机：系统初始化时调用，启用外部命令输入功能
// 执行逻辑：保存文件路径->初始化定时器->启动首次连接尝试
//=============================================================================
void sys_enableCmdFIFO (str_t file) {
    fifo = file;                             // 保存FIFO文件路径
    rt_iniTimer(&reopen_tmr, reopen_timeout);  // 初始化重连定时器
    reopen_timeout(&reopen_tmr);             // 立即启动首次连接尝试
}

#endif // defined(CFG_linux)
