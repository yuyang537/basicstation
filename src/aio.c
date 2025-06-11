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

/**
 * ============================================================================
 * AIO (Asynchronous I/O) - 异步IO系统
 * ============================================================================
 * 
 * 功能概述：
 * 本模块实现了BasicStation的核心异步IO事件循环系统，是整个系统的心脏。
 * 基于select/poll机制，将所有IO操作（网络、管道、文件）和定时器事件
 * 统一到单一事件循环中处理，实现高效的单线程异步编程模型。
 * 
 * 核心特性：
 * 1. 统一事件循环：集成IO事件和定时器事件
 * 2. 回调驱动：通过函数指针实现事件响应
 * 3. 多路复用：单线程管理多个文件描述符
 * 4. 非阻塞IO：防止任何IO操作阻塞主循环
 * 5. 定时器集成：支持timerfd或传统timeout机制
 * 
 * 设计架构：
 * - 事件驱动架构，所有操作通过回调函数异步执行
 * - 基于Linux select系统调用的多路复用IO
 * - 与运行时定时器系统(rt.c)深度集成
 * - 支持读写分离的回调函数设计
 * 
 * 使用场景：
 * - 网络连接管理（WebSocket、HTTP、TCP）
 * - 进程间通信（管道、FIFO）
 * - 串口通信（GPS、调试接口）
 * - 文件IO操作
 * - 系统信号处理
 */

#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include "rt.h"

// ============================================================================
// 常量定义和全局变量
// ============================================================================

enum { N_AIO_HANDLES = 10 };                            // AIO句柄池大小：支持最多10个并发IO对象
static aio_t aioHandles[N_AIO_HANDLES];                 // AIO句柄池：存储所有活跃的IO对象

// 注释掉的代码：原本考虑缓存fd_set以提高性能，但由于fd数量不多，每次重新计算更简单
//int aio_maxfd;
//fd_set aio_rdset;
//fd_set aio_wrset;

// ============================================================================
// AIO对象生命周期管理
// ============================================================================

/**
 * 函数功能：创建并注册新的异步IO对象
 * 参数说明：
 *   ctx - 上下文指针，用于回调函数中识别对象，不能为NULL
 *   fd  - 文件描述符，必须是有效的非阻塞fd
 *   rdfn - 读取回调函数，当fd可读时调用，可为NULL
 *   wrfn - 写入回调函数，当fd可写时调用，可为NULL
 * 返回值：分配的AIO对象指针，失败时程序终止
 * 调用时机：需要监控文件描述符IO事件时调用
 * 
 * 设计要点：
 * - 自动设置FD_CLOEXEC标志，防止子进程继承fd
 * - 使用简单的线性搜索分配句柄，适合小规模应用
 * - 句柄耗尽时直接终止程序，确保系统稳定性
 */
aio_t* aio_open(void* ctx, int fd, aiofn_t rdfn, aiofn_t wrfn) {
    assert(ctx != NULL);                                // 上下文指针必须有效
    
    // 在句柄池中寻找空闲位置
    for( int i=0; i < N_AIO_HANDLES; i++ ) {
        if( NULL == aioHandles[i].ctx ) {               // 找到空闲句柄
            // 初始化AIO对象
            aioHandles[i].ctx = ctx;                    // 设置上下文指针
            aioHandles[i].fd  = fd;                     // 设置文件描述符
            aioHandles[i].rdfn = rdfn;                  // 设置读回调函数
            aioHandles[i].wrfn = wrfn;                  // 设置写回调函数
            
            // 设置文件描述符标志：FD_CLOEXEC防止子进程继承
            int flags;
            if( (flags = fcntl(fd, F_GETFD, 0)) == -1 ||
                fcntl(fd, F_SETFD, flags|FD_CLOEXEC) == -1 )
                LOG(MOD_AIO|ERROR, "fcntl(fd, F_SETFD, FD_CLOEXEC) failed: %s", strerror(errno));
            
            return &aioHandles[i];                      // 返回分配的AIO对象
        }
    }
    
    // 句柄池已满，系统无法继续运行
    rt_fatal("Out of AIO handles");
    return NULL;
}

/**
 * 函数功能：根据上下文指针查找对应的AIO对象
 * 参数说明：ctx - 要查找的上下文指针
 * 返回值：找到的AIO对象指针，未找到返回NULL
 * 调用时机：需要根据上下文修改IO行为或关闭连接时调用
 * 
 * 设计要点：
 * - 线性搜索实现，适合小规模句柄池
 * - 上下文指针作为唯一标识符
 * - 常用于网络连接管理和状态同步
 */
aio_t* aio_fromCtx(void* ctx) {
   for( int i=0; i < N_AIO_HANDLES; i++ ) {
        if( ctx == aioHandles[i].ctx )                  // 匹配上下文指针
            return &aioHandles[i];                      // 返回找到的AIO对象
   }
   return NULL;                                         // 未找到匹配的对象
}

/**
 * 函数功能：关闭AIO对象并释放相关资源
 * 参数说明：aio - 要关闭的AIO对象指针，可以为NULL
 * 返回值：无
 * 调用时机：连接断开、错误发生或正常关闭时调用
 * 
 * 设计要点：
 * - 安全关闭文件描述符，避免重复关闭
 * - 完全清零结构体，防止野指针问题
 * - 支持NULL参数，简化调用者代码
 */
void aio_close (aio_t* aio) {
    if( aio == NULL )                                   // 处理NULL输入
        return;
        
    // 安全关闭文件描述符
    if( aio->fd >= 0 ) {
        close(aio->fd);                                 // 关闭文件描述符
        aio->fd = -1;                                   // 标记为已关闭
    }
    
    // 验证AIO对象在有效范围内
    assert(aio >= aioHandles && aio < &aioHandles[N_AIO_HANDLES]);
    
    // 清零整个结构体，释放句柄供重用
    memset(aio, 0, sizeof(*aio));
}

// ============================================================================
// AIO回调函数管理
// ============================================================================

/**
 * 函数功能：动态设置或更改读取回调函数
 * 参数说明：
 *   aio - AIO对象指针，必须是有效的活跃对象
 *   rdfn - 新的读取回调函数，NULL表示取消读取监控
 * 返回值：无
 * 调用时机：状态机切换、协议升级或临时禁用读取时调用
 * 
 * 设计要点：
 * - 支持运行时动态修改回调函数
 * - 常用于协议握手阶段的状态切换
 * - NULL回调表示暂停监控该方向的IO事件
 */
void aio_set_rdfn (aio_t* aio, aiofn_t rdfn) {
    assert(aio->ctx != NULL && aio->fd >= 0);           // 验证AIO对象有效性
    aio->rdfn = rdfn;                                   // 设置新的读取回调函数
}

/**
 * 函数功能：动态设置或更改写入回调函数
 * 参数说明：
 *   aio - AIO对象指针，必须是有效的活跃对象
 *   wrfn - 新的写入回调函数，NULL表示取消写入监控
 * 返回值：无
 * 调用时机：有数据要发送或发送完成时调用
 * 
 * 设计要点：
 * - 支持运行时动态修改回调函数
 * - 常用于发送缓冲区管理和流控制
 * - NULL回调表示当前无数据需要发送
 */
void aio_set_wrfn (aio_t* aio, aiofn_t wrfn) {
    assert(aio->ctx != NULL && aio->fd >= 0);           // 验证AIO对象有效性
    aio->wrfn = wrfn;                                   // 设置新的写入回调函数
}

// ============================================================================
// 定时器集成支持
// ============================================================================

#if defined(CFG_timerfd)
#include <sys/timerfd.h>

static int timerFD;                                     // Linux timerfd：高精度定时器文件描述符
#endif // CFG_timerfd

// ============================================================================
// 核心事件循环
// ============================================================================

/**
 * 函数功能：主事件循环，永不返回的系统心脏
 * 参数说明：无
 * 返回值：无（永不返回）
 * 调用时机：系统初始化完成后调用，成为主线程的永久循环
 * 
 * 架构设计：
 * 1. 定时器处理：集成运行时定时器系统，支持微秒级精度
 * 2. IO多路复用：使用select监控所有注册的文件描述符
 * 3. 事件分发：根据IO状态调用相应的回调函数
 * 4. 错误处理：优雅处理中断和错误情况
 * 
 * 性能优化：
 * - 每次循环重新计算fd_set，避免状态同步复杂性
 * - 使用EINTR重试机制，确保信号处理的正确性
 * - 支持timerfd和传统timeout两种定时器模式
 * 
 * 执行流程：
 * 1. 处理定时器队列，获取下次超时时间
 * 2. 构建读写fd_set，包含所有活跃文件描述符
 * 3. 调用select等待IO事件或定时器超时
 * 4. 处理定时器事件（如果使用timerfd）
 * 5. 分发IO事件到相应的回调函数
 * 6. 重复循环
 */
void aio_loop () {
    while(1) {                                          // 永不退出的主循环
        int n, maxfd;
        fd_set rdset;
        fd_set wrset;
        
        do {
            // ================================================================
            // 第一阶段：定时器处理和超时计算
            // ================================================================
            
            maxfd = -1;                                 // 重置最大文件描述符
            FD_ZERO(&rdset);                           // 清空读取fd_set
            FD_ZERO(&wrset);                           // 清空写入fd_set
            struct timeval *ptimeout = NULL;           // 超时指针，NULL表示无限等待
            
#if defined(CFG_timerfd)
            // Linux timerfd模式：使用高精度内核定时器
            ustime_t deadline = rt_processTimerQ();    // 处理定时器队列，获取下次截止时间
            if( deadline != USTIME_MAX ) {             // 如果有待处理的定时器
                struct itimerspec spec;
                memset(&spec, 0, sizeof(spec));        // 清零定时器规格
                
                // 将微秒时间戳转换为timespec格式
                spec.it_value.tv_sec = deadline / rt_seconds(1);              // 秒部分
                spec.it_value.tv_nsec = (deadline % rt_seconds(1)) * 1000;    // 纳秒部分
                
                // 设置绝对时间定时器
                if( timerfd_settime(timerFD, TFD_TIMER_ABSTIME, &spec, NULL) == -1 )
                    rt_fatal("timerfd_settime failed: %s", strerror(errno));  // LCOV_EXCL_LINE
                
                FD_SET(timerFD, &rdset);               // 将timerfd加入读取监控
                maxfd = max(maxfd, timerFD);           // 更新最大fd
            }
#else // !defined(CFG_timerfd)
            // 传统timeout模式：使用select的超时参数
            struct timeval timeout;
            ustime_t ahead = rt_processTimerQ();       // 处理定时器队列，获取等待时间
            if( ahead != USTIME_MAX ) {                // 如果有待处理的定时器
                ptimeout = &timeout;                   // 设置超时指针
                timeout.tv_sec = ahead / rt_seconds(1);         // 秒部分
                timeout.tv_usec = ahead % rt_seconds(1);        // 微秒部分
            }
#endif // !defined(CFG_timerfd)

            // ================================================================
            // 第二阶段：构建文件描述符集合
            // ================================================================
            
            // 遍历所有AIO对象，构建fd_set
            for( int i=0; i < N_AIO_HANDLES; i++ ) {
                aio_t* aio = &aioHandles[i];
                if( !aio->ctx )                        // 跳过未使用的句柄
                    continue;
                    
                int fd = aio->fd;
                if( aio->rdfn ) FD_SET(fd, &rdset);    // 有读回调则监控读事件
                if( aio->wrfn ) FD_SET(fd, &wrset);    // 有写回调则监控写事件
                maxfd = max(maxfd, fd);                // 更新最大文件描述符
            }
            
            // ================================================================
            // 第三阶段：等待IO事件
            // ================================================================
            
            // 调用select等待IO事件或超时
            n = select(maxfd+1, &rdset, &wrset, NULL, ptimeout);
            
        } while( n == -1 && errno == EINTR );          // EINTR重试：处理信号中断
        
        // ================================================================
        // 第四阶段：处理定时器事件
        // ================================================================
        
#if defined(CFG_timerfd)
        // 处理timerfd事件
        if( FD_ISSET(timerFD, &rdset) ) {
            u1_t buf[8];                               // 读取缓冲区
            int err;
            
            // 读取timerfd数据（消费事件）
            while( (err = read(timerFD, buf, sizeof(buf))) > 0 );
            
            // 检查读取错误（EAGAIN是正常的）
            if( err != -1 || errno != EAGAIN )
                rt_fatal("Failed to read timerfd: err=%d %s\n", err, strerror(errno));     // LCOV_EXCL_LINE
            
            rt_processTimerQ();                        // 处理到期的定时器
            n--;                                       // 减少待处理事件计数
        }
#endif // defined(CFG_timerfd)

        // ================================================================
        // 第五阶段：分发IO事件
        // ================================================================
        
        // 遍历所有AIO对象，分发IO事件
        for( int i=0; n > 0 && i < N_AIO_HANDLES; i++ ) {
            aio_t* aio = &aioHandles[i];
            if( !aio->ctx )                            // 跳过未使用的句柄
                continue;
                
            // 处理读事件
            if( FD_ISSET(aio->fd, &rdset) && aio->rdfn ) {
                aio->rdfn(aio);                        // 调用读回调函数
                n--;                                   // 减少待处理事件计数
            }
            
            // 处理写事件
            if( FD_ISSET(aio->fd, &wrset) && aio->wrfn ) {
                aio->wrfn(aio);                        // 调用写回调函数
                n--;                                   // 减少待处理事件计数
            }
        }
        
        // 循环继续，返回第一阶段处理下一轮事件
    }
}

// ============================================================================
// 系统初始化
// ============================================================================

/**
 * 函数功能：初始化AIO异步IO系统
 * 参数说明：无
 * 返回值：无
 * 调用时机：系统启动早期，在进入主循环之前调用
 * 
 * 初始化内容：
 * 1. 清空AIO句柄池，将所有fd标记为无效
 * 2. 创建timerfd（如果启用CFG_timerfd）
 * 3. 设置非阻塞和CLOEXEC标志
 * 
 * 设计要点：
 * - 所有fd初始化为-1，表示未使用状态
 * - timerfd使用CLOCK_MONOTONIC时钟，不受系统时间调整影响
 * - 设置非阻塞标志，确保read操作不会阻塞事件循环
 */
void aio_ini () {
    // 初始化AIO句柄池
    for( int i=0; i < N_AIO_HANDLES; i++ )
        aioHandles[i].fd = -1;                         // 将所有fd标记为无效
        
#if defined(CFG_timerfd)
    // 创建高精度定时器文件描述符
    timerFD = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
    if( timerFD == -1 )
        rt_fatal("timerfd_create failed: %s", strerror(errno));      // LCOV_EXCL_LINE
#endif // defined(CFG_timerfd)
}

