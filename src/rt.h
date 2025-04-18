/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的核心运行时支持系统，提供基础设施和通用功能支持。
 * 作为整个系统的基础层，它为上层模块提供统一的运行时环境和工具集。
 * 该模块处理系统级别的功能，包括定时器管理、日志记录、内存管理和异步IO操作等。
 * 
 * 文件功能
 * ========
 * 本头文件定义了运行时系统的核心接口和数据结构：
 * - 提供基本数据类型定义和常用宏
 * - 实现定时器管理系统
 * - 提供日志记录功能
 * - 封装内存管理操作
 * - 定义异步IO框架
 * - 提供时间管理和转换功能
 * 
 * 主要组件
 * ========
 * 1. 基础数据类型
 *    - 定义了统一的整数类型(u1_t, s1_t等)
 *    - 提供时间相关类型(ustime_t, struct datetime)
 *    - 实现缓冲区管理结构(dbuf_t)
 * 
 * 2. 定时器系统
 *    - tmr_t：定时器结构，支持回调机制
 *    - 定时器队列管理接口
 *    - 精确的微秒级定时功能
 * 
 * 3. 日志系统
 *    - 多级别日志控制(DEBUG到CRITICAL)
 *    - 模块化日志标识
 *    - 格式化输出支持
 * 
 * 4. 异步IO框架
 *    - aio_t：异步IO处理结构
 *    - 事件驱动的IO操作接口
 *    - 非阻塞IO支持
 * 
 * 关键流程
 * ========
 * 1. 定时器处理
 *    - 定时器注册与取消
 *    - 定时器回调触发
 *    - 定时器队列维护
 * 
 * 2. 内存管理
 *    - 安全的内存分配和释放
 *    - 调试模式下的内存跟踪
 *    - 内存泄漏检测支持
 * 
 * 3. 异步IO处理
 *    - IO事件循环
 *    - 读写事件处理
 *    - 文件描述符管理
 * 
 * 注意事项
 * ========
 * 1. 线程安全
 *    - 大多数函数不是线程安全的
 *    - 需要在单线程环境中使用
 *    - 异步回调需要注意上下文切换
 * 
 * 2. 资源管理
 *    - 注意及时释放已分配的资源
 *    - 避免定时器泄漏
 *    - 正确关闭IO句柄
 * 
 * 3. 性能考虑
 *    - 日志级别影响系统性能
 *    - 定时器数量会影响系统响应时间
 *    - 大量IO操作时需要注意系统负载
 */

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

#ifndef _rt_h_
#define _rt_h_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define ATTR_FASTCODE

typedef uint8_t         bit_t;
typedef uint8_t         u1_t;
typedef int8_t          s1_t;
typedef uint16_t        u2_t;
typedef int16_t         s2_t;
typedef uint32_t        u4_t;
typedef int32_t         s4_t;
typedef uint64_t        uL_t;   // u8_t blocked by lwip (minihub)
typedef int64_t         sL_t;   //  -ditto-
typedef unsigned int    uint;
typedef const char*     str_t;

typedef sL_t ustime_t;

struct datetime {
    u2_t year;
    u1_t month;
    u1_t day;
    u1_t hour;
    u1_t minute;
    u1_t second;
    u4_t usec;
};

#define USTIME_MIN ((ustime_t)0x8000000000000000)
#define USTIME_MAX ((ustime_t)0x7FFFffffFFFFffff)


#if defined(CFG_dbuf_small) // Max dbuf size 64 KB
typedef u2_t doff_t;
enum { MAX_DOFF = 0xFFFF };
#else                       // Max dbuf size 4 GB
typedef u4_t doff_t;
enum { MAX_DOFF = 0xFFFFFFFF };
#endif

typedef struct dbuf {
    char*  buf;
    doff_t bufsize;
    doff_t pos;
} dbuf_t;

#define dbuf_ini(array) { .buf=(array), .bufsize=sizeof(array), .pos=0 }
#define dbuf_str(b, str) ((b).bufsize=strlen(str), (b).pos=0, (b).buf=(char*)(str))
dbuf_t dbuf_dup (dbuf_t b);
void   dbuf_free (dbuf_t* b);


// Do not evaluate a/b twice - GCC specific
#define max(a,b)                        \
    ({ __typeof__ (a) _a = (a);         \
       __typeof__ (b) _b = (b);         \
       _a > _b ? _a : _b; })

#define min(a,b)                        \
    ({ __typeof__ (a) _a = (a);         \
       __typeof__ (b) _b = (b);         \
       _a < _b ? _a : _b; })

#define abs(a)                          \
    ({ __typeof__ (a) _a = (a);         \
        _a < 0 ? -_a : _a; })

#define SIZE_ARRAY(a) (sizeof(a)/sizeof((a)[0]))

// Convert a pointer to a member to pointer to struct
#define memberof(type, memberp, member) ((type*)((u1_t*)(memberp) - offsetof(type, member)))


// --------------------------------------------------------------------------------
//
// Logging
//
// --------------------------------------------------------------------------------

enum { XDEBUG=0, DEBUG, VERBOSE, INFO, NOTICE, WARNING, ERROR, CRITICAL };  // must be 8!
enum { MOD_ANY= 0*8, MOD_RAL= 1*8, MOD_S2E= 2*8, MOD_WSS= 3*8,
       MOD_JSN= 4*8, MOD_AIO= 5*8, MOD_CUP= 6*8, MOD_SYS= 7*8,
       MOD_TCE= 8*8, MOD_HAL= 9*8, MOD_SIO=10*8, MOD_SYN=11*8,
       MOD_GPS=12*8, MOD_SIM=13*8, MOD_WEB=14*8, MOD_ALL=0xF8 };

void  log_setSlaveIdx (s1_t idx);
int   log_setLevel (int level);
str_t log_parseLevels (const char* levels);
int   log_str2level (const char* level);
int   log_shallLog (u1_t mod_level);
void  log_msg (u1_t mod_level, const char* fmt, ...);
void  log_vmsg (u1_t mod_level, const char* fmt, va_list args);
int   log_special (u1_t mod_level, dbuf_t* buf);
void  log_specialFlush (int len);
void  log_flush ();
void  log_flushIO ();


#if defined(CFG_log_file_line)
#define LOG(level, fmt, ...) {                                  \
        if( log_shallLog(level) ) {                             \
        log_msg((level), "-- %s[%d]", __FILE__, __LINE__);      \
        log_msg((level), fmt, ## __VA_ARGS__);                  \
        }                                                       \
}
#else // !defined(CFG_log_file_line)
#ifndef CFG_logmod_exclude
#define CFG_logmod_exclude 0
#endif
#ifndef CFG_loglvl_exclude
#define CFG_loglvl_exclude -1
#endif
#define LOG(level, fmt, ...) {                                  \
        if( !((1<<((level)>>3)) & (CFG_logmod_exclude)) &&      \
            ((level) & 7) > (CFG_loglvl_exclude) &&             \
            log_shallLog(level) ) {                             \
            log_msg((level), fmt, ## __VA_ARGS__);              \
        }                                                       \
}
#endif // !defined(CFG_log_file_line)


// --------------------------------------------------------------------------------
//
// General runtime support
//
// --------------------------------------------------------------------------------

struct tmr;
typedef void (*tmrcb_t)(struct tmr* tmr);
typedef struct tmr {
    struct tmr* next;
    ustime_t    deadline;
    tmrcb_t     callback;
    void*       ctx;
} tmr_t;


#define TMR_NIL ((tmr_t*)0) // Timer not queued for timeout
#define TMR_END ((tmr_t*)1) // End of timer queue

void rt_iniTimer  (tmr_t* tmr, tmrcb_t callback);
void rt_setTimer  (tmr_t* tmr, ustime_t deadline);
void rt_setTimerCb(tmr_t* tmr, ustime_t deadline, tmrcb_t callback);
void rt_clrTimer  (tmr_t* tmr);
void rt_yieldTo   (tmr_t* tmr, tmrcb_t callback);

void     rt_ini();
void     rt_fatal(const char* fmt, ...);
ustime_t rt_processTimerQ();
uL_t     rt_eui();

void rt_usleep (ustime_t us);

extern ustime_t rt_utcOffset;
extern ustime_t rt_utcOffset_ts;

ustime_t rt_getTime ();
ustime_t rt_getUTC ();
ustime_t rt_ustime2utc (ustime_t ustime);
struct datetime rt_datetime (ustime_t ustime);

#define rt_seconds(n) ((ustime_t)((n)*(ustime_t)1000000))
#define rt_millis(n)  ((ustime_t)((n)*(ustime_t)1000))
#define rt_micros_ahead(n)  (rt_getTime()+(n))
#define rt_seconds_ahead(n) (rt_getTime()+rt_seconds(n))
#define rt_millis_ahead(n)  (rt_getTime()+rt_millis(n))

u2_t rt_rlsbf2 (const u1_t* buf);
u2_t rt_rmsbf2 (const u1_t* buf);
u4_t rt_rlsbf4 (const u1_t* buf);
uL_t rt_rlsbf8 (const u1_t* buf);

char*   rt_strdup   (str_t s);
char*   rt_strdupn  (str_t s, int n);
char*   rt_strdupq  (str_t s);

void*  _rt_malloc   (int size, int zero);
void*  _rt_malloc_d (int size, int zero, const char* f, int l);
void   _rt_free_d   (void* p, const char* f, int l);

#if defined(CFG_variant_debug)
#define rt_malloc(type)      ((type*)_rt_malloc_d(sizeof(type), 1, __FILE__, __LINE__))
#define rt_mallocN(type,num) ((type*)_rt_malloc_d(sizeof(type)*(num), 1, __FILE__, __LINE__))
#define rt_free(p)           _rt_free_d(p, __FILE__, __LINE__)
#else
#define rt_malloc(type)      ((type*)_rt_malloc(sizeof(type), 1))
#define rt_mallocN(type,num) ((type*)_rt_malloc(sizeof(type)*(num), 1))
#define rt_free              free
#endif

int  rt_hexDigit (int c);  // expose this - it's useful elsewhere
sL_t rt_readDec  (str_t* pp);
uL_t rt_readEui  (str_t* pp, int len);
sL_t rt_readSpan (str_t* pp, ustime_t defaultUnit);
sL_t rt_readSize (str_t* pp, ustime_t defaultUnit);

u4_t rt_crc32 (u4_t crc, const void* buf, int size);

void  rt_addFeature (str_t s);
str_t rt_features ();

extern str_t rt_deveui, rt_joineui;

// --------------------------------------------------------------------------------
//
// Asyncio support
//
// --------------------------------------------------------------------------------

struct aio;
typedef void (*aiofn_t)(struct aio* ah);

typedef struct aio {
    int     fd;
    aiofn_t wrfn;
    aiofn_t rdfn;
    void*   ctx;
} aio_t;

void   aio_ini    ();
void   aio_loop   (); // this call does not return!
aio_t* aio_open   (void* ctx, int fd, aiofn_t rdfn, aiofn_t wrfn);
aio_t* aio_fromCtx(void* ctx);
void   aio_close  (aio_t* aio);
void   aio_set_rdfn(aio_t* aio, aiofn_t rdfn);
void   aio_set_wrfn(aio_t* aio, aiofn_t wrfn);


#endif // _rt_h_
