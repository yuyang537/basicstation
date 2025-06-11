/*
 * --- Revised 3-Clause BSD License ---
 * Copyright Semtech Corporation 2022. All rights reserved.
 *
 * 远程Shell模块 - BasicStation远程管理和诊断系统
 * ================================================
 * 
 * 模块概述：
 * 本模块实现了BasicStation的远程Shell功能，允许通过LoRaWAN网络进行远程管理：
 * 1. 伪终端管理 - 创建和管理伪终端(PTY)，提供Shell交互环境
 * 2. 双向数据流 - 通过WebSocket二进制消息进行Shell输入输出传输
 * 3. 多会话支持 - 支持同时管理多个独立的远程Shell会话
 * 4. 进程生命周期 - 完整的Shell进程创建、监控、终止管理
 * 5. 缓冲区管理 - 高效的双向数据缓冲和流控制机制
 * 
 * 技术特点：
 * - 基于伪终端的真实Shell环境：使用POSIX PTY提供完整的终端功能
 * - 异步双向数据传输：基于AIO事件驱动的非阻塞数据流处理
 * - 智能缓冲区管理：动态压缩和流控制，防止内存溢出
 * - 会话状态跟踪：完整的会话生命周期管理和状态监控
 * - 网络透明传输：通过S2E协议的二进制消息进行数据传输
 * - 安全进程控制：进程组管理和安全的进程终止机制
 * 
 * 架构设计：
 * 远程Shell通过S2E协议接收来自LNS的rmtsh消息，创建伪终端并启动Shell进程，
 * 将Shell的输入输出通过WebSocket二进制消息与远程客户端进行双向传输，
 * 提供完整的远程终端访问能力，用于远程诊断、配置管理和系统维护。
 * 
 * 消息流程：
 * LNS rmtsh消息 → startRmtsh() → PTY+Shell进程 → 数据双向传输 → 远程终端
 * 
 * 安全考虑：
 * - 会话隔离：每个会话独立管理，避免相互干扰
 * - 资源限制：缓冲区大小限制，防止内存攻击
 * - 进程控制：安全的进程组终止，避免僵尸进程
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

#if !defined(CFG_no_rmtsh)         // 远程Shell功能编译控制 - 只在未禁用时编译
#define _XOPEN_SOURCE 600            // 启用POSIX.1-2001扩展，支持posix_openpt等函数
#define _DEFAULT_SOURCE              // 启用glibc默认扩展，支持cfmakeraw等函数

// 系统头文件 - 伪终端和进程管理所需
#include <errno.h>                   // 错误码定义
#include <unistd.h>                  // POSIX系统调用
#include <signal.h>                  // 信号处理
#include <termios.h>                 // 终端控制接口
#include <fcntl.h>                   // 文件控制操作
#include <sys/types.h>               // 系统数据类型定义
#include <sys/ioctl.h>               // 设备控制接口
#include <sys/wait.h>                // 进程等待接口

// BasicStation模块头文件
#include "s2conf.h"                  // S2配置系统
#include "s2e.h"                     // S2E协议引擎
#include "uj.h"                      // JSON编解码器
#include "kwcrc.h"                   // 关键字CRC哈希
#include "tc.h"                      // TC连接管理

// 远程Shell缓冲区配置常量
#define UPBUFSZ   4096               // 上行缓冲区大小：从Shell读取的数据(4KB)
#define DNBUFSZ   4096               // 下行缓冲区大小：向Shell写入的数据(4KB)
#define UPBUFHI   (UPBUFSZ/2)        // 上行缓冲区高水位：触发压缩的阈值(2KB)
#define DNBUFHI   (DNBUFSZ/2)        // 下行缓冲区高水位：触发压缩的阈值(2KB)
#define WS_CHUNKS MIN_UPJSON_SIZE    // WebSocket数据块大小：最小JSON消息大小

// 远程Shell会话状态结构体
// 每个活跃的远程Shell会话都有一个对应的rmtsh_t实例
typedef struct rmtsh {
    str_t    user;                   // 会话用户名：标识会话所属用户
    pid_t    pid;                    // Shell进程ID：0表示会话未启动
    aio_t*   aio;                    // 异步IO对象：管理PTY主端的读写操作
    char     upbuf[UPBUFSZ];         // 上行数据缓冲区：从Shell读取到的输出数据
    char     dnbuf[DNBUFSZ];         // 下行数据缓冲区：要发送给Shell的输入数据
    int      upfill, upsink;         // 上行缓冲区管理：fill=数据末尾位置，sink=已发送位置
    int      dnfill, dnsink;         // 下行缓冲区管理：fill=数据末尾位置，sink=已写入位置
    ustime_t mtime;                  // 最后活动时间：用于会话超时检测和状态监控
} rmtsh_t;

// 远程Shell会话表：支持同时运行多个独立的Shell会话
// MAX_RMTSH定义了系统支持的最大并发Shell会话数量
static rmtsh_t rmtshTable[MAX_RMTSH];

// 函数前向声明
static void stopRmtsh (rmtsh_t* rmtsh);  // 停止远程Shell会话


// 函数功能：处理从Shell进程读取上行数据的异步IO回调函数
// 参数说明：aio - 异步IO对象，包含PTY主端文件描述符和远程Shell上下文
// 返回值：无
// 调用时机：Shell进程有输出数据可读时由AIO事件循环自动调用
// 处理逻辑：读取Shell输出→缓冲管理→网络传输→缓冲区压缩
static void up_read (aio_t* aio) {
    rmtsh_t* rmtsh = aio->ctx;               // 获取远程Shell会话上下文
    int n;                                   // 读取字节数
    while(1) {                               // 循环读取所有可用数据
        // 从PTY主端读取Shell进程的输出数据
        n = read(aio->fd, rmtsh->upbuf+rmtsh->upfill, UPBUFSZ-rmtsh->upfill);
        if( n == -1 ) {                      // 读取出错处理
            if( errno == EAGAIN ) {          // 如果是非阻塞读取无数据
                return;                      // 正常退出，等待下次可读事件
            }
            LOG(ERROR, "Failed to read from rmsh#%d (pid=%d): %s", (int)(rmtsh-rmtshTable), rmtsh->pid, strerror(errno));
            n = 0;                           // 将错误当作EOF处理
        }
        if( n == 0 ) {                       // EOF表示Shell进程已退出
            stopRmtsh(rmtsh);                // 停止远程Shell会话
            return;
        }
        rmtsh->mtime = rt_getTime();         // 更新最后活动时间
        rmtsh->upfill += n;                  // 更新缓冲区填充位置
        
        // 网络传输处理：将Shell输出发送给远程客户端
        if( TC ) {                           // 如果TC连接存在
            n = min(WS_CHUNKS, 1+rmtsh->upfill-rmtsh->upsink);  // 计算可发送的数据量
            if( n == 1 )                     // 如果只有会话ID字节没有数据
                continue;                    // 不发送空数据帧(会被误解为EOF)
            ujbuf_t sendbuf = (*TC->s2ctx.getSendbuf)(&TC->s2ctx, n);  // 获取发送缓冲区
            if( sendbuf.buf != NULL ) {      // 如果成功获取缓冲区
                sendbuf.buf[0] = rmtsh - rmtshTable;  // 第一个字节为会话索引
                memcpy(sendbuf.buf+1, &rmtsh->upbuf[rmtsh->upsink], n-1);  // 复制Shell输出数据
                sendbuf.pos = n;             // 设置数据长度
                (*TC->s2ctx.sendBinary)(&TC->s2ctx, &sendbuf);  // 发送二进制数据给远程客户端
                rmtsh->upsink += n-1;        // 更新已发送位置
            } else {
                LOG(WARNING, "No enough WS space to sent command");  // WebSocket缓冲区不足
            }
        } else {
            // 无网络连接时丢弃数据，防止缓冲区溢出
            if( rmtsh->upfill >= UPBUFHI )   // 如果缓冲区超过高水位
                rmtsh->upsink  = UPBUFHI;    // 强制释放一半缓冲区空间
        }
        
        // 缓冲区压缩：当缓冲区使用过半且有已发送数据时进行压缩
        if( rmtsh->upfill >= UPBUFHI && rmtsh->upsink > 0 ) {
            memcpy(&rmtsh->upbuf[0], &rmtsh->upbuf[rmtsh->upsink], rmtsh->upfill-rmtsh->upsink);  // 将未发送数据移到缓冲区开头
            rmtsh->upfill -= rmtsh->upsink;  // 调整填充位置
            rmtsh->upsink = 0;               // 重置已发送位置
        }
    }
}


// 函数功能：处理向Shell进程写入下行数据的异步IO回调函数
// 参数说明：aio - 异步IO对象，包含PTY主端文件描述符和远程Shell上下文
// 返回值：无
// 调用时机：PTY主端可写且有待写入数据时由AIO事件循环自动调用
// 处理逻辑：写入Shell输入→错误处理→缓冲区清理→禁用写事件
static void dn_write (aio_t* aio) {
    rmtsh_t* rmtsh = aio->ctx;               // 获取远程Shell会话上下文
    int n;                                   // 写入字节数
    
    // 循环写入所有待发送的下行数据
    while( rmtsh->dnfill > rmtsh->dnsink ) { // 当还有未写入的数据时
        // 向PTY主端写入数据，Shell进程将从PTY从端读取
        n = write(aio->fd, rmtsh->dnbuf+rmtsh->dnsink, rmtsh->dnfill-rmtsh->dnsink);
        if( n == -1 ) {                      // 写入出错处理
            if( errno == EAGAIN ) {          // 如果是非阻塞写入缓冲区满
                aio_set_wrfn(aio, dn_write); // 重新注册写回调，等待下次可写事件
                return;
            }
            stopRmtsh(rmtsh);                // 其他错误则停止Shell会话
            return;
        }
        rmtsh->mtime = rt_getTime();         // 更新最后活动时间
        rmtsh->dnsink += n;                  // 更新已写入位置
    }
    
    // 所有数据写入完成，清理缓冲区状态
    rmtsh->dnfill = rmtsh->dnsink = 0;       // 重置缓冲区指针
    aio_set_wrfn(aio, NULL);                 // 禁用写事件监听，避免无意义的回调
}


// 函数功能：向下行缓冲区填入数据并触发写入操作
// 参数说明：rmtsh - 远程Shell会话指针，data - 要填入的数据，len - 数据长度
// 返回值：无
// 调用时机：收到来自远程客户端的输入数据时调用
// 处理逻辑：缓冲区检查→空间压缩→数据填入→触发写入
static void dn_fill (rmtsh_t* rmtsh, u1_t* data, int len) {
    // 检查缓冲区空间是否足够
    if( rmtsh->dnfill + len > DNBUFSZ ) {    // 如果新数据会导致缓冲区溢出
        // 尝试压缩缓冲区以释放空间
        if( rmtsh->dnsink > 0 ) {            // 如果有已写入的数据可以释放
            memcpy(&rmtsh->dnbuf[0], &rmtsh->dnbuf[rmtsh->dnsink], rmtsh->dnfill-rmtsh->dnsink);  // 将未写入数据移到缓冲区开头
            rmtsh->dnfill -= rmtsh->dnsink;  // 调整填充位置
            rmtsh->dnsink = 0;               // 重置已写入位置
        }
        // 压缩后仍然无法容纳新数据，表示下行数据量过大
        if( rmtsh->dnfill + len > DNBUFSZ ) {
            LOG(ERROR, "Remote shell down stream buffer overflow");  // 记录缓冲区溢出错误
            stopRmtsh(rmtsh);                // 为安全起见停止Shell会话
            return;
        }
    }
    
    // 将新数据追加到下行缓冲区
    memcpy(&rmtsh->dnbuf[rmtsh->dnfill], data, len);  // 复制数据到缓冲区末尾
    rmtsh->dnfill += len;                    // 更新填充位置
    dn_write(rmtsh->aio);                    // 触发异步写入操作
}


// 函数功能：停止远程Shell会话并清理所有相关资源
// 参数说明：rmtsh - 要停止的远程Shell会话指针
// 返回值：无
// 调用时机：Shell进程退出、网络错误、缓冲区溢出或收到停止命令时调用
// 处理逻辑：发送EOF→终止进程组→清理僵尸进程→关闭IO→重置状态
static void stopRmtsh (rmtsh_t* rmtsh) {
    if( rmtsh->aio == NULL ) {               // 如果会话未启动
        return;                              // 直接返回，无需处理
    }
    
    // 发送EOF信号给远程客户端，通知Shell会话结束
    if( rmtsh->aio && TC ) {                 // 如果异步IO存在且有TC连接
        // 发送只包含会话ID的空数据包表示EOF
        ujbuf_t sendbuf = (*TC->s2ctx.getSendbuf)(&TC->s2ctx, MIN_UPJSON_SIZE);
        if( sendbuf.buf != NULL ) {          // 如果成功获取发送缓冲区
            sendbuf.buf[0] = rmtsh - rmtshTable;  // 设置会话索引
            sendbuf.pos = 1;                 // 只有一个字节的数据
            (*TC->s2ctx.sendBinary)(&TC->s2ctx, &sendbuf);  // 发送EOF信号
        }
    }
    
    // 终止Shell进程及其所有子进程
    if( rmtsh->pid ) {                       // 如果Shell进程存在
        kill(-rmtsh->pid, SIGKILL);          // 向整个进程组发送SIGKILL信号(负号表示进程组)
        while( waitpid(-1, NULL, WNOHANG) > 0 );  // 清理所有僵尸子进程
    }
    
    LOG(NOTICE, "Rmtsh#%d stopped (pid=%d)", (int)(rmtsh - rmtshTable), rmtsh->pid);  // 记录会话停止日志
    
    // 清理会话状态和资源
    rmtsh->pid = 0;                          // 清除进程ID
    aio_close(rmtsh->aio);                   // 关闭异步IO对象和PTY文件描述符
    rmtsh->aio = NULL;                       // 清空异步IO指针
    rmtsh->upfill = rmtsh->upsink = 0;       // 重置上行缓冲区状态
    rmtsh->dnfill = rmtsh->dnsink = 0;       // 重置下行缓冲区状态
}


// 函数功能：启动远程Shell会话，创建伪终端和Shell进程
// 参数说明：rmtsh - 远程Shell会话指针，user - 用户名，term - 终端类型
// 返回值：无
// 调用时机：收到来自LNS的rmtsh启动命令时调用
// 处理逻辑：伪终端创建→进程fork→子进程Shell设置→父进程IO管理
static void startRmtsh (rmtsh_t* rmtsh, str_t user, str_t term) {
    if( rmtsh->aio != NULL ) {               // 如果会话已经在运行
        return;                              // 直接返回，避免重复启动
    }
    
    // 创建伪终端对(PTY master/slave)
    int pty_master = -1;                     // PTY主端文件描述符
    int pty_slave = -1;                      // PTY从端文件描述符
    
    // 依次执行PTY创建的所有步骤，任何失败都会跳转到错误处理
    if( (pty_master = posix_openpt(O_RDWR|O_NONBLOCK)) == -1 ||  // 创建PTY主端(非阻塞模式)
        grantpt(pty_master) == -1 ||         // 授权PTY从端(设置正确的权限)
        unlockpt(pty_master) == -1 ||        // 解锁PTY从端(允许访问)
        (pty_slave = open(ptsname(pty_master), O_RDWR)) == -1 ) {  // 打开PTY从端设备文件
        close(pty_master);                   // 清理已创建的资源
        LOG(ERROR, "Setting up pseudo terminal (%s) failed: %s", pty_master==-1 ? "master":"slave", strerror(errno));
        return;
    }
    
    // 创建子进程运行Shell
    int rc = fork();                         // 创建子进程
    if( rc == -1 ) {                         // fork失败处理
        LOG(ERROR, "Forking into subshell failed: %s", strerror(errno));
        return;
    }
    
    if( rc == 0 ) {                          // 子进程分支：设置Shell环境
        // 子进程不需要PTY主端，关闭它
        close(pty_master);
        
        // 设置终端环境变量，告诉Shell使用什么终端类型
        setenv("TERM", term, 1);             // 设置TERM环境变量(如"xterm", "dumb"等)
        
        // 将PTY从端设置为RAW模式，确保Shell能正确处理终端控制字符
        struct termios tsettings;            // 终端设置结构体
        tcgetattr(pty_slave, &tsettings);    // 获取当前终端设置
        cfmakeraw(&tsettings);               // 配置为RAW模式(禁用行缓冲、回显等)
        tcsetattr(pty_slave, TCSANOW, &tsettings);  // 立即应用新设置
        
        // 将标准输入输出错误重定向到PTY从端
        if( dup2(pty_slave, STDIN_FILENO ) != STDIN_FILENO  ||   // 重定向标准输入
            dup2(pty_slave, STDOUT_FILENO) != STDOUT_FILENO ||   // 重定向标准输出
            dup2(pty_slave, STDERR_FILENO) != STDERR_FILENO ) {  // 重定向标准错误
                rt_fatal("Rmtsh subprocess failed to setup stdio: %s", strerror(errno));
        }
        
        // 创建新的会话并设置控制终端
        setsid();                            // 创建新会话，成为会话领导者
        // 将PTY从端设置为控制终端(Shell程序需要控制终端来正确管理输出)
        ioctl(0, TIOCSCTTY, 1);              // TIOCSCTTY: 设置控制终端
        
        // 执行Shell程序，替换当前进程映像
        if( execlp("sh","sh",NULL) == -1 ) { // 执行"/bin/sh"Shell
            rt_fatal("Rmtsh subprocess exec failed: %s", strerror(errno));
        }
    }
    
    // 父进程分支：管理Shell会话
    close(pty_slave);                        // 父进程不需要PTY从端，关闭它
    
    // 设置会话状态
    free((void*)rmtsh->user);                // 释放旧的用户名内存
    rmtsh->user = rt_strdup(user);           // 保存新的用户名
    rmtsh->mtime = rt_getTime();             // 设置会话创建时间
    rmtsh->pid = rc;                         // 保存Shell进程ID
    
    // 建立异步IO管理PTY主端的读写操作
    rmtsh->aio = aio_open(rmtsh, pty_master, up_read, NULL);  // 注册上行读取回调
    up_read(rmtsh->aio);                     // 立即尝试读取数据
    
    LOG(NOTICE, "Rmtsh#%d started (pid=%d)", (int)(rmtsh - rmtshTable), rmtsh->pid);  // 记录会话启动日志
}


// 函数功能：处理来自LNS的远程Shell控制消息(rmtsh)
// 参数说明：s2ctx - S2E上下文指针，D - JSON解码器指针
// 返回值：无
// 调用时机：S2E消息分发器收到msgtype为"rmtsh"的消息时调用
// 消息字段：user(用户名)、start(启动会话索引)、stop(停止会话索引)、term(终端类型)
void s2e_handleRmtsh (s2ctx_t* s2ctx, ujdec_t* D) {
    ujcrc_t field;                           // JSON字段类型
    int start_idx = -1;                      // 要启动的会话索引(-1表示不启动)
    int stop_idx = -1;                       // 要停止的会话索引(-1表示不停止)
    str_t user = NULL;                       // 会话用户名
    str_t term = "dumb";                     // 终端类型，默认为"dumb"(简单终端)
    
    // 解析JSON消息的所有字段
    while( (field = uj_nextField(D)) ) {     // 遍历所有JSON字段
        switch(field) {
        case J_msgtype: {                    // 消息类型字段
            uj_skipValue(D);                 // 跳过已知的消息类型
            break;
        }
        case J_user: {                       // 用户名字段
            user = uj_str(D);                // 提取用户名字符串
            break;
        }
        case J_start: {                      // 启动会话字段
            start_idx = uj_intRange(D, 0, MAX_RMTSH);  // 提取会话索引(0到MAX_RMTSH-1)
            break;
        }
        case J_stop: {                       // 停止会话字段
            stop_idx = uj_intRange(D, 0, MAX_RMTSH);   // 提取会话索引(0到MAX_RMTSH-1)
            break;
        }
        case J_term: {                       // 终端类型字段
            term = uj_str(D);                // 提取终端类型字符串(如"xterm", "vt100"等)
            break;
        }
        case J_MuxTime: {                    // 时间复用字段
            s2e_updateMuxtime(s2ctx, uj_num(D), 0);  // 更新S2E时间复用状态
            break;
        }
        default: {                           // 未知字段处理
            LOG(MOD_S2E|WARNING, "Unknown field in 'rmtsh' message - ignored: %s", D->field.name);
            uj_skipValue(D);                 // 跳过未知字段值
            break;
        }
        }
    }
    // 执行停止会话操作
    if( stop_idx >= 0 ) {                    // 如果指定了要停止的会话索引
        LOG(DEBUG, "Rmtsh stop received idx=%d", stop_idx);
        stopRmtsh(&rmtshTable[stop_idx]);    // 停止指定的Shell会话
    }
    
    // 执行启动会话操作
    if( start_idx >= 0 ) {                   // 如果指定了要启动的会话索引
        LOG(DEBUG, "Rmtsh start received user=%s idx=%d", user, start_idx);
        startRmtsh(&rmtshTable[start_idx], user, term);  // 启动指定的Shell会话
    }
    
    // 发送当前所有会话的状态响应给LNS
    if( TC == NULL )                         // 如果TC连接不存在
        return;                              // 直接返回(理论上不应该发生，因为刚收到rmtsh请求)

    ujbuf_t sendbuf = (*TC->s2ctx.getSendbuf)(&TC->s2ctx, MIN_UPJSON_SIZE);  // 获取发送缓冲区
    if( sendbuf.buf == NULL ) {              // 如果无法获取缓冲区
        LOG(MOD_S2E|ERROR, "Failed to send 'rmtsh' response, no buffer space");
        return;
    }
    
    // 构建JSON响应消息
    uj_encOpen(&sendbuf, '{');               // 开始JSON根对象
    uj_encKVn(&sendbuf,
              "msgtype",   's', "rmtsh",     // 消息类型为"rmtsh"
              "rmtsh",     '[', 0, NULL);    // 开始rmtsh状态数组
    
    // 遍历所有会话并添加状态信息
    for( int i=0; i<MAX_RMTSH; i++ ) {       // 遍历所有可能的Shell会话槽位
        rmtsh_t* rmtsh = &rmtshTable[i];     // 获取会话指针
        uj_encOpen(&sendbuf, '{');           // 开始会话状态对象
        uj_encKVn(&sendbuf,
                  "user",      's', rmtsh->user==NULL ? "" : rmtsh->user,        // 用户名(空字符串表示未设置)
                  "started",   'b', (rmtsh->aio != NULL),                       // 会话是否已启动(布尔值)
                  "age",       'i', rmtsh->mtime==0 ? -1 : (int)((rt_getTime() - rmtsh->mtime)/1000000),  // 会话存活时间(秒)
                  "pid",       'i', rmtsh->pid,                                  // Shell进程ID(0表示未启动)
              NULL);
        uj_encClose(&sendbuf, '}');          // 结束会话状态对象
    }
    uj_encClose(&sendbuf, ']');              // 结束rmtsh状态数组
    uj_encClose(&sendbuf, '}');              // 结束JSON根对象
    (*s2ctx->sendText)(s2ctx, &sendbuf);     // 发送JSON响应给LNS
    LOG(MOD_S2E|VERBOSE, "Rmtsh response sent");  // 记录响应发送日志
}


// 函数功能：处理来自LNS的远程Shell二进制数据(用户输入)
// 参数说明：s2ctx - S2E上下文指针，data - 二进制数据，len - 数据长度
// 返回值：1表示已处理，0表示未处理
// 调用时机：S2E协议引擎收到WebSocket二进制消息时调用
// 数据格式：第一字节为会话索引，后续字节为用户输入数据
int s2e_onBinary (s2ctx_t* s2ctx, u1_t* data, ujoff_t len) {
    if( len == 0 ) {                         // 如果数据长度为0
        return 1;                            // 直接返回已处理(空数据包通常表示保活)
    }
    
    // 验证会话索引的有效性
    if( data[0] >= MAX_RMTSH ) {             // 如果会话索引超出范围
        LOG(MOD_S2E|ERROR, "Illegal rmtsh session: %d", data[0]);
        return 1;                            // 记录错误并返回已处理
    }
    
    // 获取对应的Shell会话
    rmtsh_t* rmtsh = &rmtshTable[data[0]];   // 根据索引获取会话指针
    if( rmtsh->aio == NULL ) {               // 如果会话未启动
        LOG(MOD_S2E|ERROR, "Dropping data for stopped rmtsh#%d", data[0]);
        return 1;                            // 丢弃数据并返回已处理
    }
    
    // 将用户输入数据发送给Shell进程
    dn_fill(rmtsh, data+1, len-1);           // 跳过第一字节(会话索引)，发送剩余数据
    return 1;                                // 返回已处理
}


#endif // !defined(CFG_no_rmtsh)           // 远程Shell功能编译控制结束
