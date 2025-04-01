/*
 * 模块概述
 * ========
 * 本模块实现了 LoRaWAN 基站的异步IO系统，提供高效的非阻塞IO操作支持。
 * 它基于select机制实现事件驱动的IO多路复用，支持同时处理多个IO通道。
 * 该模块是基站网络通信、设备控制等功能的基础设施。
 * 
 * 文件功能
 * ========
 * 本文件实现了异步IO系统的核心功能：
 * - 异步IO句柄的管理和分配
 * - 基于select的事件循环
 * - IO事件的回调处理
 * - 文件描述符的生命周期管理
 * 
 * 主要组件
 * ========
 * 1. IO句柄管理
 *    - aio_t结构体：封装IO操作相关信息
 *    - 句柄池管理：固定大小的句柄数组
 *    - 句柄分配和回收机制
 * 
 * 2. 事件循环系统
 *    - select多路复用
 *    - 读写事件处理
 *    - 定时器集成（基于timerfd）
 *    - 事件分发机制
 * 
 * 3. 回调接口
 *    - 读写回调函数注册
 *    - 上下文管理
 *    - 错误处理
 * 
 * 关键流程
 * ========
 * 1. IO操作流程
 *    - 句柄初始化和配置
 *    - 事件监听和触发
 *    - 回调函数执行
 *    - 资源清理
 * 
 * 2. 事件处理流程
 *    - 文件描述符集合准备
 *    - select等待
 *    - 事件检测和分发
 *    - 回调函数调用
 * 
 * 3. 资源管理流程
 *    - 句柄分配检查
 *    - 文件描述符设置
 *    - 资源释放
 * 
 * 注意事项
 * ========
 * 1. 性能考虑
 *    - select的扩展性限制
 *    - 句柄数量上限(N_AIO_HANDLES)
 *    - 回调函数执行时间
 * 
 * 2. 资源管理
 *    - 及时关闭未使用的句柄
 *    - 避免句柄泄漏
 *    - 正确处理回调上下文
 * 
 * 3. 错误处理
 *    - 系统调用失败处理
 *    - 资源耗尽情况
 *    - 回调函数异常
 */

#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include "rt.h"


enum { N_AIO_HANDLES = 10 };
static aio_t aioHandles[N_AIO_HANDLES];


// There aren't that many fd open thus recalc every time we go into select
//int aio_maxfd;
//fd_set aio_rdset;
//fd_set aio_wrset;

aio_t* aio_open(void* ctx, int fd, aiofn_t rdfn, aiofn_t wrfn) {
    assert(ctx != NULL);
    for( int i=0; i < N_AIO_HANDLES; i++ ) {
        if( NULL == aioHandles[i].ctx ) {
            aioHandles[i].ctx = ctx;
            aioHandles[i].fd  = fd;
            aioHandles[i].rdfn = rdfn;
            aioHandles[i].wrfn = wrfn;
            int flags;
            if( (flags = fcntl(fd, F_GETFD, 0)) == -1 ||
                fcntl(fd, F_SETFD, flags|FD_CLOEXEC) == -1 )
                LOG(MOD_AIO|ERROR, "fcntl(fd, F_SETFD, FD_CLOEXEC) failed: %s", strerror(errno));
            return &aioHandles[i];
        }
    }
    rt_fatal("Out of AIO handles");
    return NULL;
}


aio_t* aio_fromCtx(void* ctx) {
   for( int i=0; i < N_AIO_HANDLES; i++ ) {
        if( ctx == aioHandles[i].ctx )
            return &aioHandles[i];
   }
   return NULL;
}


void aio_close (aio_t* aio) {
    if( aio == NULL )
        return;
    if( aio->fd >= 0 ) {
        close(aio->fd);
        aio->fd = -1;
    }
    assert(aio >= aioHandles && aio < &aioHandles[N_AIO_HANDLES]);
    memset(aio, 0, sizeof(*aio));
}


void aio_set_rdfn (aio_t* aio, aiofn_t rdfn) {
    assert(aio->ctx != NULL && aio->fd >= 0);
    aio->rdfn = rdfn;
}


void aio_set_wrfn (aio_t* aio, aiofn_t wrfn) {
    assert(aio->ctx != NULL && aio->fd >= 0);
    aio->wrfn = wrfn;
}


#if defined(CFG_timerfd)
#include <sys/timerfd.h>

static int timerFD;
#endif // CFG_timerfd


void aio_loop () {
    while(1) {
        int n, maxfd;
        fd_set rdset;
        fd_set wrset;
        do {
            maxfd = -1;
            FD_ZERO(&rdset);
            FD_ZERO(&wrset);
            struct timeval *ptimeout = NULL;
#if defined(CFG_timerfd)
            ustime_t deadline = rt_processTimerQ();
            if( deadline != USTIME_MAX ) {
                struct itimerspec spec;
                memset(&spec, 0, sizeof(spec));
                spec.it_value.tv_sec = deadline / rt_seconds(1);
                spec.it_value.tv_nsec = (deadline % rt_seconds(1)) * 1000;
                if( timerfd_settime(timerFD, TFD_TIMER_ABSTIME, &spec, NULL) == -1 )
                    rt_fatal("timerfd_settime failed: %s", strerror(errno));      // LCOV_EXCL_LINE
                FD_SET(timerFD, &rdset);
                maxfd = max(maxfd, timerFD);
            }
#else // !defined(CFG_timerfd)
            struct timeval timeout;
            ustime_t ahead = rt_processTimerQ();
            if( ahead != USTIME_MAX ) {
                ptimeout = &timeout;
                timeout.tv_sec = ahead / rt_seconds(1);
                timeout.tv_usec = ahead % rt_seconds(1);
            }
#endif // !defined(CFG_timerfd)
            for( int i=0; i < N_AIO_HANDLES; i++ ) {
                aio_t* aio = &aioHandles[i];
                if( !aio->ctx )
                    continue;
                int fd = aio->fd;
                if( aio->rdfn ) FD_SET(fd, &rdset);
                if( aio->wrfn ) FD_SET(fd, &wrset);
                maxfd = max(maxfd, fd);
            }
            n = select(maxfd+1, &rdset, &wrset, NULL, ptimeout);
        } while( n == -1 && errno == EINTR );
#if defined(CFG_timerfd)
        if( FD_ISSET(timerFD, &rdset) ) {
            u1_t buf[8];
            int err;
            while( (err = read(timerFD, buf, sizeof(buf))) > 0 );
            if( err != -1 || errno != EAGAIN )
                rt_fatal("Failed to read timerfd: err=%d %s\n", err, strerror(errno));     // LCOV_EXCL_LINE
            rt_processTimerQ();
            n--;
        }
#endif // defined(CFG_timerfd)
        for( int i=0; n > 0 && i < N_AIO_HANDLES; i++ ) {
            aio_t* aio = &aioHandles[i];
            if( !aio->ctx )
                continue;
            if( FD_ISSET(aio->fd, &rdset) && aio->rdfn ) {
                aio->rdfn(aio);
                n--;
            }
            if( FD_ISSET(aio->fd, &wrset) && aio->wrfn ) {
                aio->wrfn(aio);
                n--;
            }
        }
    }
}


void aio_ini () {
   for( int i=0; i < N_AIO_HANDLES; i++ )
       aioHandles[i].fd = -1;
#if defined(CFG_timerfd)
    timerFD = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
    if( timerFD == -1 )
        rt_fatal("timerfd_create failed: %s", strerror(errno));      // LCOV_EXCL_LINE
#endif // defined(CFG_timerfd)
}

