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
 * BasicStation日志系统核心实现
 * 
 * 功能概述：
 * - 分级日志记录：支持8个日志级别(XDEBUG到CRITICAL)
 * - 模块化日志：支持15个不同模块的独立日志级别控制
 * - 格式化输出：统一的时间戳、模块标识和级别标识格式
 * - 运行时配置：支持动态调整各模块的日志级别
 * - 主从进程支持：支持主从进程架构的日志标识
 * - 高效缓存：内置缓冲区减少系统调用开销
 */

#include "s2conf.h"
#include "sys.h"
#include "rt.h"
#include "uj.h"

// 日志级别字符串映射表：将数字级别转换为4字符显示标识
// 用于日志输出中的级别标识，便于快速识别日志重要性
const char* LVLSTR[] = {
    [XDEBUG  ]= "XDEB",    // 0: 扩展调试信息，最详细的调试输出
    [DEBUG   ]= "DEBU",    // 1: 普通调试信息，开发调试使用
    [VERBOSE ]= "VERB",    // 2: 详细信息，比INFO更详细的运行信息
    [INFO    ]= "INFO",    // 3: 一般信息，正常运行状态信息
    [NOTICE  ]= "NOTI",    // 4: 通知信息，需要关注但非错误的事件
    [WARNING ]= "WARN",    // 5: 警告信息，潜在问题或异常情况
    [ERROR   ]= "ERRO",    // 6: 错误信息，程序错误但可继续运行
    [CRITICAL]= "CRIT",    // 7: 严重错误，可能导致程序终止
};

// 模块名称字符串映射表：将模块标识转换为3字符显示标识
// 用于日志输出中的模块标识，便于定位日志来源模块
const char* MODSTR[] = {
    [MOD_ANY/8]= "any",    // 通用模块，适用于所有模块的配置
    [MOD_RAL/8]= "RAL",    // Radio Abstraction Layer - 无线电抽象层
    [MOD_S2E/8]= "S2E",    // Semtech to Endpoint - 协议栈核心
    [MOD_WSS/8]= "WSS",    // WebSocket Secure - 安全WebSocket通信
    [MOD_JSN/8]= "JSN",    // JSON - JSON解析和处理
    [MOD_AIO/8]= "AIO",    // Asynchronous I/O - 异步IO处理
    [MOD_CUP/8]= "CUP",    // Crypto/Utils/Platform - 加密工具平台
    [MOD_SYS/8]= "SYS",    // System - 系统层功能
    [MOD_TCE/8]= "TCE",    // Transport/Connection Engine - 传输连接引擎
    [MOD_HAL/8]= "HAL",    // Hardware Abstraction Layer - 硬件抽象层
    [MOD_SIO/8]= "___",    // Standard I/O - 标准输入输出(特殊标识)
    [MOD_SYN/8]= "SYN",    // Synchronization - 时间同步
    [MOD_GPS/8]= "GPS",    // GPS - GPS时间同步
    [MOD_SIM/8]= "SIM",    // Simulation - 仿真模式
    [MOD_WEB/8]= "WEB",    // Web Interface - Web界面
};

// 默认日志级别配置：编译时可通过CFG_logini_lvl宏定义覆盖
// 默认使用INFO级别，平衡信息量和性能
#ifndef CFG_logini_lvl
#define CFG_logini_lvl INFO
#endif

// 日志行缓冲区：用于格式化单条日志消息
// 大小由LOGLINE_LEN宏定义，通常为512或1024字节
static char   logline[LOGLINE_LEN];

// 日志缓冲区描述符：管理logline缓冲区的状态
// 包含缓冲区指针、大小和当前写入位置
static dbuf_t logbuf = { .buf=logline, .bufsize=sizeof(logline), .pos=0 };

// 从进程模块标识：用于主从进程架构中标识从进程
// 格式为"Sxx"，其中xx为从进程编号(00-99)
static char   slaveMod[4];

// 各模块日志级别配置数组：每个模块独立配置日志级别
// 数组索引对应模块ID右移3位的值，支持32个模块槽位
// 初始化时所有模块都使用CFG_logini_lvl默认级别
static u1_t   logLevels[32] = {
    CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl,
    CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl,
    CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl,
    CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl
};

// 生成日志消息头部：包含时间戳、模块标识和级别标识
// 参数: mod_level - 模块和级别的组合值(高5位模块，低3位级别)
// 返回: 头部字符串的长度
// 格式: "HH:MM:SS.mmm [MOD:LVL] "
static int log_header (u1_t mod_level) {
    int mod = (mod_level & MOD_ALL) >> 3;    // 提取模块ID(右移3位)
    logbuf.pos = 0;                          // 重置缓冲区写入位置
    
    // 确定模块标识字符串：优先使用从进程标识，否则使用标准模块名
    str_t mod_s = slaveMod[0] ? slaveMod : mod >= SIZE_ARRAY(MODSTR) ? "???":MODSTR[mod];
    
    // 格式化日志头部：时间戳(精确到毫秒) + [模块:级别]
    // %.3T 格式化UTC时间为HH:MM:SS.mmm格式
    xprintf(&logbuf, "%.3T [%s:%s] ", rt_getUTC(), mod_s, LVLSTR[mod_level & 7]);
    
    return logbuf.pos;                       // 返回头部长度，用于后续内容追加
}

// 解析日志级别字符串：将文本级别转换为数字级别
// 参数: level - 级别字符串，支持数字(0-7)或文本格式
// 返回: 成功返回级别值，失败返回-1
// 支持格式: "5", "INFO", "MOD:INFO"
int log_str2level (const char* level) {
    // 处理数字格式级别(0-7)：直接转换为数字并应用到所有模块
    if( level[0] >= '0' && level[0] <='7' ) {
        return (level[0]-'0') | MOD_ALL;     // 数字级别 + 所有模块标识
    }
    
    int mod = MOD_ALL;                       // 默认应用到所有模块
    
    // 检查是否为模块特定格式："MOD:LEVEL"
    if( level[0] && level[1] && level[2] && level[3] == ':' ) {
        // 查找匹配的模块名称
        for( int m=0; m < SIZE_ARRAY(MODSTR); m++ ) {
            if( strncasecmp(level, MODSTR[m], 3) == 0 ) {
                mod = m << 3;                // 模块ID左移3位
                level += 4;                  // 跳过模块名和冒号
                goto cklevel;
            }
        }
        return -1;                           // 未找到匹配的模块名
    }
    
 cklevel:
    // 查找匹配的级别名称
    for( int i=0; i < SIZE_ARRAY(LVLSTR); i++ ) {
        if( strncasecmp(level, LVLSTR[i], 4) == 0 )
            return i | mod;                  // 级别值 + 模块标识
    }
    return -1;                               // 未找到匹配的级别名
}

// 解析多个日志级别配置：处理逗号分隔的级别列表
// 参数: levels - 级别配置字符串，如"INFO,RAL:DEBUG,SYS:WARN"
// 返回: 成功返回NULL，失败返回错误位置的指针
// 功能: 逐个解析并应用每个级别配置
str_t log_parseLevels (const char* levels) {
    do {
        int l = log_str2level(levels);       // 解析当前级别配置
        if( l < 0 )
            return levels;                   // 解析失败，返回错误位置
            
        log_setLevel(l);                     // 应用级别配置
        
        str_t s = strchr(levels, ',');       // 查找下一个配置项
        if( s == NULL )
            return NULL;                     // 所有配置项处理完成
        levels = s+1;                        // 移动到下一个配置项
    } while(1);
}

// 设置从进程标识：用于主从进程架构中区分不同从进程的日志
// 参数: idx - 从进程索引(0-99)
// 功能: 生成格式为"Sxx"的从进程标识，用于替换标准模块名
void log_setSlaveIdx (s1_t idx) {
    slaveMod[0] = 'S';                       // 从进程标识前缀
    slaveMod[1] = idx/10 + '0';              // 十位数字
    slaveMod[2] = idx%10 + '0';              // 个位数字
    slaveMod[3] = '\0';                      // 字符串结束符
}

// 设置模块日志级别：动态调整特定模块或所有模块的日志级别
// 参数: level - 级别配置(低3位为级别，高位为模块标识)
// 返回: 成功返回旧级别，失败返回-1
// 功能: 支持全局设置或单模块设置
int log_setLevel (int level) {
    if( level < 0 ) return -1;               // 参数检查
    
    int mod = level & MOD_ALL;               // 提取模块标识
    level &= 7;                              // 提取级别值(0-7)
    
    if( mod == MOD_ALL ) {
        // 全局设置：应用到所有模块
        for( int m=0; m<32; m++ ) {
            logLevels[m] = level;
        }
        return -1;                           // 全局设置无旧值概念
    }
    
    // 单模块设置：更新指定模块的级别
    int old = logLevels[mod>>3];             // 保存旧级别
    logLevels[mod>>3] = level;               // 设置新级别
    return old;                              // 返回旧级别
}

// 检查是否应该记录日志：根据模块和级别配置判断
// 参数: mod_level - 模块和级别的组合值
// 返回: 1表示应该记录，0表示跳过
// 逻辑: 当前级别 >= 配置的最低级别时记录
int log_shallLog (u1_t mod_level) {
    return (mod_level&7) >= logLevels[(mod_level & MOD_ALL) >> 3];
}

// 格式化日志消息(可变参数版本)：使用vprintf风格的参数列表
// 参数: mod_level - 模块级别组合，fmt - 格式字符串，args - 参数列表
// 功能: 完整的日志处理流程，从级别检查到最终输出
void log_vmsg (u1_t mod_level, const char* fmt, va_list args) {
    if( !log_shallLog(mod_level) )           // 级别检查：跳过不需要记录的日志
        return;
        
    int n = log_header(mod_level);           // 生成并写入日志头部
    logbuf.pos = n;                          // 设置内容起始位置
    vxprintf(&logbuf, fmt, args);            // 格式化用户消息内容
    log_flush();                             // 刷新日志到输出系统
}

// 格式化日志消息(标准版本)：使用printf风格的可变参数
// 参数: mod_level - 模块级别组合，fmt - 格式字符串，... - 可变参数
// 功能: 标准的日志记录接口，内部调用log_vmsg处理
void log_msg (u1_t mod_level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);                       // 初始化参数列表
    log_vmsg(mod_level, fmt, ap);            // 调用可变参数版本
    va_end(ap);                              // 清理参数列表
}

// HAL模块专用日志接口：为硬件抽象层提供便捷的日志记录
// 参数: level - 日志级别，fmt - 格式字符串，... - 可变参数
// 功能: 自动添加MOD_HAL模块标识，简化HAL层日志调用
void log_hal (u1_t level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);                       // 初始化参数列表
    log_vmsg(MOD_HAL|level, fmt, ap);        // 自动添加HAL模块标识
    va_end(ap);                              // 清理参数列表
}

// 特殊日志缓冲区获取：为高级日志操作提供直接缓冲区访问
// 参数: mod_level - 模块级别组合，b - 输出缓冲区描述符
// 返回: 1表示可以使用缓冲区，0表示级别不满足要求
// 用途: 允许调用者直接操作日志缓冲区，用于复杂格式化或性能优化
int log_special (u1_t mod_level, dbuf_t* b) {
    if( !log_shallLog(mod_level) )           // 级别检查
        return 0;
        
    b->buf = logbuf.buf;                     // 设置缓冲区指针
    b->bufsize = logbuf.bufsize;             // 设置缓冲区大小
    b->pos = log_header(mod_level);          // 生成头部并设置内容起始位置
    return 1;                                // 返回成功标志
}

// 特殊日志刷新：配合log_special使用，刷新直接操作的缓冲区
// 参数: len - 最终消息长度(包含头部)
// 功能: 设置缓冲区长度并刷新到输出系统
void log_specialFlush (int len) {
    assert(len < logbuf.bufsize);            // 长度检查，防止缓冲区溢出
    logbuf.pos = len;                        // 设置实际内容长度
    log_flush();                             // 刷新到输出系统
}

// 刷新日志缓冲区：将当前缓冲区内容输出到系统日志
// 功能: 添加行结束符，字符串结束符，并调用系统接口输出
void log_flush () {
    xeol(&logbuf);                           // 添加换行符
    xeos(&logbuf);                           // 添加字符串结束符
    sys_addLog(logbuf.buf, logbuf.pos);      // 调用系统接口输出日志
    logbuf.pos = 0;                          // 重置缓冲区位置
}

// 强制刷新所有日志：确保所有待输出日志立即写入
// 功能: 刷新当前缓冲区并触发系统层强制刷新
void log_flushIO () {
    log_flush();                             // 刷新当前缓冲区
    sys_addLog(logbuf.buf, 0);               // 长度为0触发系统强制刷新
}

