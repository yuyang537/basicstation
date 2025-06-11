/*
 * RT - 运行时基础库头文件
 * =======================
 * 功能：BasicStation的核心运行时支持库
 * 核心：基础类型定义、时间管理、内存管理、日志系统、AIO异步IO
 */

#ifndef _rt_h_
#define _rt_h_

#include <stddef.h>   // 标准定义：size_t、NULL等
#include <stdbool.h>  // 布尔类型：bool、true、false
#include <stdint.h>   // 整数类型：uint8_t、int32_t等
#include <stdarg.h>   // 可变参数：va_list、va_start等
#include <stdlib.h>   // 标准库：malloc、free等
#include <string.h>   // 字符串：strlen、memcpy等
#include <assert.h>   // 断言：assert宏

#define ATTR_FASTCODE  // 快速代码属性：标记性能关键代码

// 基础数据类型定义
typedef uint8_t         bit_t;   // 位类型：单个位或小整数
typedef uint8_t         u1_t;    // 无符号8位：字节类型
typedef int8_t          s1_t;    // 有符号8位：字节类型
typedef uint16_t        u2_t;    // 无符号16位：字类型
typedef int16_t         s2_t;    // 有符号16位：字类型
typedef uint32_t        u4_t;    // 无符号32位：双字类型
typedef int32_t         s4_t;    // 有符号32位：双字类型
typedef uint64_t        uL_t;    // 无符号64位：长整型（u8_t被lwip阻塞）
typedef int64_t         sL_t;    // 有符号64位：长整型
typedef unsigned int    uint;    // 无符号整数：平台相关
typedef const char*     str_t;   // 字符串类型：常量字符指针

typedef sL_t ustime_t;  // 微秒时间类型：以微秒为单位的时间戳

/**
 * struct datetime - 日期时间结构
 * 功能：表示人类可读的日期和时间
 */
struct datetime {
    u2_t year;     // 年份：公元年份
    u1_t month;    // 月份：1-12
    u1_t day;      // 日期：1-31
    u1_t hour;     // 小时：0-23
    u1_t minute;   // 分钟：0-59
    u1_t second;   // 秒钟：0-59
    u4_t usec;     // 微秒：0-999999
};

// 时间常量定义
#define USTIME_MIN ((ustime_t)0x8000000000000000)  // 最小时间：时间戳最小值
#define USTIME_MAX ((ustime_t)0x7FFFffffFFFFffff)  // 最大时间：时间戳最大值

// 数据缓冲区大小配置
#if defined(CFG_dbuf_small)  // 小缓冲区配置：最大64KB
typedef u2_t doff_t;         // 偏移类型：16位偏移
enum { MAX_DOFF = 0xFFFF };  // 最大偏移：64KB-1
#else                        // 大缓冲区配置：最大4GB
typedef u4_t doff_t;         // 偏移类型：32位偏移
enum { MAX_DOFF = 0xFFFFFFFF }; // 最大偏移：4GB-1
#endif

/**
 * struct dbuf - 动态缓冲区结构
 * 功能：管理动态分配的内存缓冲区，支持读写指针
 */
typedef struct dbuf {
    char*  buf;      // 缓冲区指针：指向分配的内存
    doff_t bufsize;  // 缓冲区大小：总分配字节数
    doff_t pos;      // 当前位置：读写位置指针
} dbuf_t;

// 缓冲区初始化宏
#define dbuf_ini(array) { .buf=(array), .bufsize=sizeof(array), .pos=0 }  // 数组初始化
#define dbuf_str(b, str) ((b).bufsize=strlen(str), (b).pos=0, (b).buf=(char*)(str))  // 字符串初始化

// 缓冲区管理API
dbuf_t dbuf_dup (dbuf_t b);   // 复制缓冲区：创建缓冲区副本
void   dbuf_free (dbuf_t* b); // 释放缓冲区：释放内存并重置

// 安全的min/max宏（GCC特定，避免重复求值）
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

#define SIZE_ARRAY(a) (sizeof(a)/sizeof((a)[0]))  // 数组大小：计算数组元素个数

// 结构体成员指针转换宏
#define memberof(type, memberp, member) ((type*)((u1_t*)(memberp) - offsetof(type, member)))

// 日志级别枚举
enum { XDEBUG=0, DEBUG, VERBOSE, INFO, NOTICE, WARNING, ERROR, CRITICAL };  // 必须是8级！

// 日志模块枚举
enum { MOD_ANY= 0*8, MOD_RAL= 1*8, MOD_S2E= 2*8, MOD_WSS= 3*8,
       MOD_JSN= 4*8, MOD_AIO= 5*8, MOD_CUP= 6*8, MOD_SYS= 7*8,
       MOD_TCE= 8*8, MOD_HAL= 9*8, MOD_SIO=10*8, MOD_SYN=11*8,
       MOD_GPS=12*8, MOD_SIM=13*8, MOD_WEB=14*8, MOD_ALL=0xF8 };

// 日志系统API
void  log_setSlaveIdx (s1_t idx);                        // 设置子进程索引：标识日志来源
int   log_setLevel (int level);                          // 设置日志级别：控制日志输出详细程度
str_t log_parseLevels (const char* levels);              // 解析日志级别：从字符串解析日志配置
int   log_str2level (const char* level);                 // 字符串转级别：将级别名转换为数值
int   log_shallLog (u1_t mod_level);                     // 是否记录：检查是否应该输出此日志
void  log_msg (u1_t mod_level, const char* fmt, ...);    // 记录消息：输出格式化日志消息
void  log_vmsg (u1_t mod_level, const char* fmt, va_list args); // 记录消息（va_list版本）
int   log_special (u1_t mod_level, dbuf_t* buf);         // 特殊日志：输出缓冲区内容
void  log_specialFlush (int len);                        // 刷新特殊日志：强制输出指定长度
void  log_flush ();                                      // 刷新日志：强制输出所有缓存日志
void  log_flushIO ();                                    // 刷新IO：强制输出所有IO缓存

// 日志宏定义（根据配置选择不同实现）
#if defined(CFG_log_file_line)
#define LOG(level, fmt, ...) {                                  \
        if( log_shallLog(level) ) {                             \
        log_msg((level), "-- %s[%d]", __FILE__, __LINE__);      \
        log_msg((level), fmt, ## __VA_ARGS__);                  \
        }                                                       \
}
#else // !defined(CFG_log_file_line)
#ifndef CFG_logmod_exclude
#define CFG_logmod_exclude 0        // 排除模块：默认不排除任何模块
#endif
#ifndef CFG_loglvl_exclude
#define CFG_loglvl_exclude -1       // 排除级别：默认不排除任何级别
#endif
#define LOG(level, fmt, ...) {                                  \
        if( !((1<<((level)>>3)) & (CFG_logmod_exclude)) &&      \
            ((level) & 7) > (CFG_loglvl_exclude) &&             \
            log_shallLog(level) ) {                             \
            log_msg((level), fmt, ## __VA_ARGS__);              \
        }                                                       \
}
#endif // !defined(CFG_log_file_line)

// 定时器系统
struct tmr;                              // 定时器结构前置声明
typedef void (*tmrcb_t)(struct tmr* tmr); // 定时器回调函数类型

/**
 * struct tmr - 定时器结构
 * 功能：管理单个定时器的状态和回调
 */
typedef struct tmr {
    struct tmr* next;      // 下一个定时器：定时器链表指针
    ustime_t    deadline;  // 到期时间：定时器触发的绝对时间
    tmrcb_t     callback;  // 回调函数：定时器到期时调用的函数
    void*       ctx;       // 上下文：回调函数的上下文参数
} tmr_t;

// 定时器常量
#define TMR_NIL ((tmr_t*)0)  // 定时器未排队：表示定时器未在队列中
#define TMR_END ((tmr_t*)1)  // 定时器队列结束：表示队列末尾

// 定时器管理API
void rt_iniTimer  (tmr_t* tmr, tmrcb_t callback);          // 初始化定时器：设置回调函数
void rt_setTimer  (tmr_t* tmr, ustime_t deadline);         // 设置定时器：设置到期时间
void rt_setTimerCb(tmr_t* tmr, ustime_t deadline, tmrcb_t callback); // 设置定时器和回调
void rt_clrTimer  (tmr_t* tmr);                            // 清除定时器：从队列中移除
void rt_yieldTo   (tmr_t* tmr, tmrcb_t callback);          // 让出控制：立即执行回调

// 运行时核心API
void     rt_ini();                // 初始化运行时：设置基础运行环境
void     rt_fatal(const char* fmt, ...); // 致命错误：输出错误信息并退出
ustime_t rt_processTimerQ();      // 处理定时器队列：处理到期的定时器
uL_t     rt_eui();                // 获取EUI：获取设备唯一标识符

void rt_usleep (ustime_t us);     // 微秒睡眠：精确的微秒级延迟

// 时间管理
extern ustime_t rt_utcOffset;     // UTC偏移：本地时间与UTC的偏移
extern ustime_t rt_utcOffset_ts;  // UTC偏移时间戳：偏移值的时间戳

ustime_t rt_getTime ();                    // 获取当前时间：本地单调时间
ustime_t rt_getUTC ();                     // 获取UTC时间：协调世界时
ustime_t rt_ustime2utc (ustime_t ustime);  // 转换为UTC：本地时间转UTC
struct datetime rt_datetime (ustime_t ustime); // 转换为日期时间：时间戳转可读格式

#define rt_seconds(n) ((ustime_t)((n)*(ustime_t)1000000))  // 秒转微秒：将秒转换为微秒时间戳
#define rt_millis(n)  ((ustime_t)((n)*(ustime_t)1000))
#define rt_micros_ahead(n)  (rt_getTime()+(n))
#define rt_seconds_ahead(n) (rt_getTime()+rt_seconds(n))
#define rt_millis_ahead(n)  (rt_getTime()+rt_millis(n))

// 字节序转换API（小端格式读取）
u2_t rt_rlsbf2 (const u1_t* buf);  // 读取小端16位：从缓冲区读取小端格式16位值
u2_t rt_rmsbf2 (const u1_t* buf);  // 读取大端16位：从缓冲区读取大端格式16位值  
u4_t rt_rlsbf4 (const u1_t* buf);  // 读取小端32位：从缓冲区读取小端格式32位值
uL_t rt_rlsbf8 (const u1_t* buf);  // 读取小端64位：从缓冲区读取小端格式64位值

// 字符串处理API
char*   rt_strdup   (str_t s);      // 复制字符串：分配内存并复制字符串
char*   rt_strdupn  (str_t s, int n); // 复制字符串（指定长度）：复制指定长度的字符串
char*   rt_strdupq  (str_t s);      // 复制字符串（引用计数）：带引用计数的字符串复制

// 内存管理API
void*  _rt_malloc   (int size, int zero);                    // 分配内存：分配指定大小内存
void*  _rt_malloc_d (int size, int zero, const char* f, int l); // 调试版内存分配：带调试信息
void   _rt_free_d   (void* p, const char* f, int l);         // 调试版内存释放：带调试信息

#if defined(CFG_variant_debug)
#define rt_malloc(type)      ((type*)_rt_malloc_d(sizeof(type), 1, __FILE__, __LINE__))
#define rt_mallocN(type,num) ((type*)_rt_malloc_d(sizeof(type)*(num), 1, __FILE__, __LINE__))
#define rt_free(p)           _rt_free_d(p, __FILE__, __LINE__)
#else
#define rt_malloc(type)      ((type*)_rt_malloc(sizeof(type), 1))
#define rt_mallocN(type,num) ((type*)_rt_malloc(sizeof(type)*(num), 1))
#define rt_free              free
#endif

// 解析工具API
int  rt_hexDigit (int c);                                    // 十六进制数字：将字符转换为十六进制值
sL_t rt_readDec  (str_t* pp);                               // 读取十进制：从字符串解析十进制数
uL_t rt_readEui  (str_t* pp, int len);                      // 读取EUI：从字符串解析EUI
sL_t rt_readSpan (str_t* pp, ustime_t defaultUnit);         // 读取时间跨度：解析时间间隔
sL_t rt_readSize (str_t* pp, ustime_t defaultUnit);         // 读取大小：解析字节大小

// 工具函数
u4_t rt_crc32 (u4_t crc, const void* buf, int size);        // CRC32校验：计算CRC32校验值

// 功能管理
void  rt_addFeature (str_t s);  // 添加功能：注册系统功能
str_t rt_features ();          // 获取功能：获取已注册的功能列表

extern str_t rt_deveui, rt_joineui;

// AIO异步IO系统
struct aio;                              // AIO结构前置声明
typedef void (*aiofn_t)(struct aio* ah); // AIO回调函数类型

/**
 * struct aio - 异步IO结构
 * 功能：管理单个文件描述符的异步IO操作
 */
typedef struct aio {
    int     fd;    // 文件描述符：要监控的文件描述符
    aiofn_t wrfn;  // 写回调：可写时调用的函数
    aiofn_t rdfn;  // 读回调：可读时调用的函数
    void*   ctx;   // 上下文：回调函数的上下文参数
} aio_t;

// AIO管理API
void   aio_ini    ();                                        // 初始化AIO：设置异步IO系统
void   aio_loop   ();                                        // AIO事件循环：此调用永不返回！
aio_t* aio_open   (void* ctx, int fd, aiofn_t rdfn, aiofn_t wrfn); // 打开AIO：注册文件描述符
aio_t* aio_fromCtx(void* ctx);
void   aio_close  (aio_t* aio);                              // 关闭AIO：注销文件描述符
void   aio_set_rdfn(aio_t* aio, aiofn_t rdfn);               // 设置读回调：更新读事件回调
void   aio_set_wrfn(aio_t* aio, aiofn_t wrfn);               // 设置写回调：更新写事件回调

#endif // _rt_h_
