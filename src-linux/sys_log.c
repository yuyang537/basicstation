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

/*
 * BasicStation Linux平台日志系统实现
 * 
 * 架构特点：
 * - 多线程异步写入：主线程负责日志格式化，后台线程负责磁盘写入
 * - 日志轮转管理：支持文件大小限制和自动轮转功能
 * - 标准输出重定向：捕获stdout/stderr并转换为结构化日志
 * - 缓冲优化：使用内存缓冲区减少磁盘IO频率
 * - 线程安全：使用互斥锁和条件变量保证并发安全
 * - 故障降级：磁盘写入失败时自动降级到stderr输出
 */

#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include "rt.h"
#include "sys.h"
#include "sys_linux.h"

// 日志系统配置常量
#define LOG_LAG         100  // 毫秒：延迟刷新时间，避免频繁小写入
#define LOG_OUTSIZ     8192  // 字节：输出缓冲区大小，平衡内存使用和IO效率
#define LOG_HIGHWATER (LOG_OUTSIZ/2)  // 字节：高水位标记，达到此阈值立即刷新
#define MAX_LOGHDR      64   // 字节：日志头部最大长度，用于stdout重定向

// 日志文件配置结构体指针：由上层传入，包含路径、大小限制、轮转数量等
static struct logfile* logfile;

// 异步写入系统的核心组件
static tmr_t delay;               // 延迟定时器：控制批量写入的延迟时间
static char  outbuf[LOG_OUTSIZ];  // 输出缓冲区：累积日志数据减少磁盘IO
static int   outfill = 0;         // 缓冲区填充位置：当前已使用的缓冲区空间

// 标准输出重定向系统：捕获printf等输出并转为结构化日志
static aio_t* stdout_aio;         // 异步IO对象：管理stdout/stderr管道
static char   stdout_buf[MAX_LOGHDR+PIPE_BUF];  // 标准输出缓冲区：包含日志头部空间
static int    stdout_idx = MAX_LOGHDR;           // 缓冲区写入位置：预留头部空间

// 多线程同步机制：保证日志写入的线程安全
static pthread_mutex_t  mxfill  = PTHREAD_MUTEX_INITIALIZER;  // 缓冲区填充互斥锁
static pthread_mutex_t  mxcond  = PTHREAD_MUTEX_INITIALIZER;  // 条件变量互斥锁
static pthread_cond_t   condvar = PTHREAD_COND_INITIALIZER;   // 唤醒写入线程的条件变量
static pthread_t        thr;                                  // 后台写入线程句柄
static int              thrUp = 0;                            // 线程状态标志：1表示线程已启动

// 原始stderr文件描述符：故障时的降级输出目标
static int orig_stderr = STDERR_FILENO;

// 前向声明：内部函数
static void addLog (const char *logline, int len);

// 标准输出读取处理函数：将stdout/stderr输出转换为结构化日志
// 参数: aio - 异步IO对象，管理stdout重定向管道
// 功能: 读取管道数据，按行分割并格式化为日志消息
static void stdout_read(aio_t* aio) {
    while(1) {
        // 尝试从管道读取数据到缓冲区
        int n = read(aio->fd, &stdout_buf[stdout_idx], sizeof(stdout_buf)-stdout_idx);
        
        if( n == 0 ) {
            // EOF：通常表示进程退出，理论上不应该发生
            LOG(ERROR, "Stdout pipe - EOF");
            return;
        }
        
        if( n == -1 ) {
            // 读取错误：EAGAIN表示暂无数据，其他为真实错误
            if( errno != EAGAIN )
                LOG(ERROR, "Stdout pipe read fail: %s", strerror(errno));
            return;
        }
        
        // 查找完整行：避免在行中间截断数据
        stdout_idx += n;                     // 更新缓冲区填充位置
        int end = stdout_idx;
        
        // 从后向前查找最近的换行符
        while( end > MAX_LOGHDR && stdout_buf[end-1] != '\n' )
            end -= 1;
            
        // 缓冲区满且无换行符：强制刷新避免数据丢失
        if( end == MAX_LOGHDR && stdout_idx >= sizeof(stdout_buf) )
            end = stdout_idx;
            
        if( end > MAX_LOGHDR ) {
            // 构造完整的日志消息：日志头部 + 标准输出内容
            dbuf_t lbuf;
            if( log_special(MOD_SIO|INFO, &lbuf) ) {
                int n = min(MAX_LOGHDR, lbuf.pos);       // 日志头部长度
                char* p = &stdout_buf[MAX_LOGHDR-n];     // 头部插入位置
                char* e = &stdout_buf[end];              // 内容结束位置
                
                memcpy(p, lbuf.buf, n);                  // 复制日志头部
                
                // 确保行末有换行符：保证日志格式正确
                if( e[-1] != '\n') {
                    if( e < &stdout_buf[sizeof(stdout_buf)] ) {
                        e[0] = '\n';                     // 有空间：追加换行符
                        e += 1;
                    } else {
                        e[-1] = '\n';                    // 无空间：覆盖最后字符
                    }
                }
                
                addLog(p, e-p);                          // 输出完整的日志行
            }
            
            // 处理缓冲区中的剩余数据：移动到缓冲区开始位置
            n = stdout_idx - end;
            if( n > 0 )
                memcpy(&stdout_buf[MAX_LOGHDR], &stdout_buf[end], n);
            stdout_idx = MAX_LOGHDR+n;
        }
    }
}

// 日志数据写入函数：处理文件轮转和实际的磁盘写入
// 参数: data - 日志数据指针，len - 数据长度
// 功能: 检查文件大小，执行轮转，写入数据，处理错误降级
static void writeLogData (const char *data, int len) {
    // 检查日志文件配置：未配置时直接输出到stderr
    if( !logfile || !logfile->path ) {
      log2stderr:
        if( write(orig_stderr, data, len) == -1 )
            sys_fatal(FATAL_NOLOGGING);               // 连stderr都失败，系统致命错误
        return;
    }
    
    // 检查当前日志文件大小：决定是否需要轮转
    struct stat st = { .st_size = 0 };
    if( stat(logfile->path, &st) == -1 && errno != ENOENT ) {
        fprintf(stderr,"Failed to stat log file %s: %s\n", logfile->path, strerror(errno));
        goto log2stderr;                              // 文件状态检查失败，降级到stderr
    }
    
    // 文件大小超限：执行日志轮转
    if( st.st_size >= logfile->size ) {
        int flen = strlen(logfile->path);
        char fn[flen + 15];                          // 轮转文件名缓冲区
        struct timespec min_ctim;                    // 最早创建时间
        int logfno = -1;                             // 目标轮转编号
        
        strcpy(fn, logfile->path);
        
        // 查找最佳轮转文件：优先使用不存在的编号，否则使用最旧的文件
        for( int i=0; i<logfile->rotate; i++ ) {
            snprintf(fn+flen, 15, ".%d", i);         // 生成轮转文件名
            
            if( stat(fn, &st) == -1 ) {
                if( errno != ENOENT )
                    fprintf(stderr,"Failed to stat log file %s: %s\n", fn, strerror(errno));
                logfno = i;                          // 文件不存在，使用此编号
                break;
            }
            
            // 记录最旧的文件：基于创建时间判断
            if( logfno < 0 || min_ctim.tv_sec > st.st_ctim.tv_sec ) {
                min_ctim.tv_sec = st.st_ctim.tv_sec;
                logfno = i;
            }
        }
        
        // 删除目标轮转文件：为新文件腾出空间
        if( unlink(fn) == -1 && errno != ENOENT )
            fprintf(stderr,"Failed to unlink log file %s: %s\n", fn, strerror(errno));
            
        // 重命名当前日志文件：完成轮转操作
        if( rename(logfile->path, fn) == -1 ) {
            fprintf(stderr,"Failed to rename log file %s => %s: %s\n", logfile->path, fn, strerror(errno));
            // 重命名失败：删除当前文件，从新开始
            if( unlink(logfile->path) == -1 )
                fprintf(stderr,"Failed to unlink log file %s: %s\n", logfile->path, strerror(errno));
        }
    }
    
    // 打开日志文件：创建或追加模式，设置适当的权限
    int fd = open(logfile->path, O_CREAT|O_APPEND|O_WRONLY, S_IRUSR|S_IWUSR|S_IRGRP);
    if( fd == -1 ) {
        fprintf(stderr,"Failed to open log file %s: %s\n", logfile->path, strerror(errno));
        goto log2stderr;                              // 文件打开失败，降级到stderr
    }
    
    // 写入日志数据：检查完整性，处理部分写入
    int n;
    if( (n = write(fd, data, len)) != len ) {
        fprintf(stderr,"Partial write to log file %s: %s\n", logfile->path, strerror(errno));
        close(fd);
        goto log2stderr;                              // 写入不完整，降级到stderr
    }
    
    close(fd);                                       // 关闭文件描述符
}

// 添加日志到输出队列：线程安全的日志添加接口
// 参数: logline - 日志内容，len - 内容长度(0表示强制刷新)
// 功能: 根据线程状态选择同步或异步写入模式
static void addLog (const char *logline, int len) {
    // 后台线程未启动：直接同步写入，用于初始化阶段
    if( !thrUp ) {
        writeLogData(logline, len);
        return;
    }
    
    // 异步模式：添加到缓冲区，由后台线程处理
    pthread_mutex_lock(&mxfill);                     // 保护缓冲区填充操作
    
    int k = min(LOG_OUTSIZ-outfill, len);            // 计算可写入的数据量
    memcpy(outbuf + outfill, logline, k);           // 复制数据到缓冲区
    outfill += k;                                    // 更新填充位置
    
    // 决定是否立即通知后台线程：强制刷新或达到高水位
    int notify = (len == 0 || outfill >= LOG_HIGHWATER);
    
    pthread_mutex_unlock(&mxfill);                   // 释放缓冲区锁
    
    if( notify ) {
        // 立即通知后台线程：紧急或缓冲区快满
        pthread_cond_signal(&condvar);
    } else if( delay.next == TMR_NIL ) {
        // 设置延迟定时器：批量写入优化，减少磁盘IO频率
        rt_setTimer(&delay, rt_millis_ahead(LOG_LAG));
    }
}

// 延迟定时器回调：触发批量写入操作
// 参数: tmr - 定时器对象(未使用)
// 功能: 检查缓冲区状态，有数据时唤醒后台线程
static void on_delay (tmr_t* tmr) {
    pthread_mutex_lock(&mxfill);                     // 检查缓冲区状态
    if( outfill )                                    // 有待写入数据
        pthread_cond_signal(&condvar);               // 唤醒后台线程
    pthread_mutex_unlock(&mxfill);
}

// 后台日志写入线程：专门负责磁盘IO操作
// 功能: 等待条件信号，批量写入缓冲区数据，处理缓冲区重排
static void thread_log (void) {
    pthread_mutex_lock(&mxcond);                     // 获取条件变量锁
    
    while(1) {
        // 等待唤醒信号：来自addLog或定时器
        pthread_cond_wait(&condvar, &mxcond);
        
        // 获取当前缓冲区数据量：原子操作避免数据竞争
        pthread_mutex_lock(&mxfill);
        int len = outfill;
        pthread_mutex_unlock(&mxfill);
        
        if( len ) {
            // 执行磁盘写入：在锁外进行，避免阻塞前台线程
            writeLogData(outbuf, len);
            
            // 更新缓冲区状态：处理写入期间新增的数据
            pthread_mutex_lock(&mxfill);
            if( len < outfill ) {
                // 部分写入：移动剩余数据到缓冲区开始位置
                memcpy(&outbuf[0], &outbuf[len], outfill-len);
                outfill -= len;
            } else {
                // 完全写入：重置缓冲区
                outfill = 0;
            }
            pthread_mutex_unlock(&mxfill);
        }
    }
}

// 强制刷新所有日志：同步接口，确保数据完整写入
// 功能: 刷新标准输出，同步写入缓冲区所有数据
void sys_flushLog (void) {
    fflush(stdout);                                  // 刷新标准输出缓冲区
    fflush(stderr);                                  // 刷新标准错误缓冲区
    
    // 同步写入所有缓冲数据：绕过异步机制
    pthread_mutex_lock(&mxcond);
    pthread_mutex_lock(&mxfill);
    writeLogData(outbuf, outfill);                   // 直接写入当前缓冲区内容
    outfill = 0;                                     // 重置缓冲区
    pthread_mutex_unlock(&mxfill);
    pthread_mutex_unlock(&mxcond);
}

// 系统日志接口：供上层日志模块调用
// 参数: logline - 日志内容，len - 内容长度(0表示强制刷新)
// 功能: 统一的日志添加入口，处理正常日志和刷新请求
void sys_addLog (const char *logline, int len) {
    if( len == 0 ) {
        sys_flushLog();                              // 长度为0：强制刷新所有日志
    } else {
        addLog(logline, len);                        // 正常日志：添加到队列
    }
}

// 初始化日志系统：配置文件输出和标准输出重定向
// 参数: lf - 日志文件配置，captureStdio - 是否捕获标准输出
// 功能: 设置日志文件，配置stdout/stderr重定向，初始化异步IO
void sys_iniLogging (struct logfile* lf, int captureStdio) {
    logfile = lf;                                    // 保存日志文件配置
    
    // 配置标准输出重定向：仅在主进程且有日志文件时执行
    if( logfile->path && captureStdio ) {
        // 标准输出重定向：将stdout/stderr重定向到管道，由日志系统处理
        if( stdout_aio == NULL ) {
            int fds[2] = { -1, -1 };
            
            // 创建非阻塞管道：读端非阻塞，写端阻塞
            if( pipe2(fds, O_NONBLOCK) == -1 ) {
                rt_fatal("Failed to create stdout/stderr pipe: %s", strerror(errno));
            }
            
            // 设置写端为阻塞模式：避免printf等调用失败
            int flags = fcntl(fds[1], F_GETFL, 0);
            if( flags != -1 )
                fcntl(fds[1], F_SETFL, flags & ~O_NONBLOCK);
            
            // 保存原始stderr：用于错误降级输出
            orig_stderr = dup(STDERR_FILENO);
            
            // 重定向标准输出和错误：所有printf输出进入管道
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
            close(fds[1]);                           // 关闭写端原始描述符
            
            // 重定向标准输入：避免阻塞等待输入
            int fd = open("/dev/null", O_RDONLY);
            dup2(fd, STDIN_FILENO);
            close(fd);
            
            // 初始化标准输出处理：设置缓冲区和异步IO
            stdout_idx = MAX_LOGHDR;                 // 预留日志头部空间
            stdout_aio = aio_open(stdout_buf, fds[0], stdout_read, NULL);
            stdout_read(stdout_aio);                 // 立即尝试读取数据
        }
    } else {
        // 关闭标准输出重定向：恢复正常输出模式
        aio_close(stdout_aio);
        stdout_aio = NULL;
    }
    
    // 注册退出处理函数：确保程序退出时刷新所有日志
    atexit(sys_flushLog);
}

// 启动后台日志线程：开启异步日志写入模式
// 功能: 创建后台线程，初始化定时器，提高日志写入性能
void sys_startLogThread () {
    if( !thrUp ) {
        // 创建后台写入线程：专门处理磁盘IO操作
        if( pthread_create(&thr, NULL, (void * (*)(void *))thread_log, NULL) != 0 )
            sys_fatal(FATAL_PTHREAD);               // 线程创建失败，系统致命错误
            
        rt_iniTimer(&delay, on_delay);              // 初始化延迟定时器
        thrUp = 1;                                  // 标记线程已启动
    }
}
