/*
 * 模块概述
 * ========
 * 本模块实现了 LoRaWAN 基站的日志系统，提供了灵活且可配置的日志记录功能。
 * 它支持多个日志级别和模块标识，使得系统运行状态和调试信息可以被有效地跟踪和管理。
 * 该模块与系统的其他部分紧密集成，为问题诊断和系统监控提供重要支持。
 * 
 * 文件功能
 * ========
 * 本文件实现了日志系统的核心功能：
 * - 日志级别控制和过滤
 * - 模块化的日志标识
 * - 格式化日志输出
 * - 日志缓冲区管理
 * - 多进程日志支持
 * 
 * 主要组件
 * ========
 * 1. 日志级别系统
 *    - 8个日志级别(XDEBUG到CRITICAL)
 *    - 每个模块独立的日志级别控制
 *    - 日志级别解析和设置接口
 * 
 * 2. 模块标识系统
 *    - 15个系统模块标识
 *    - 从机进程特殊标识
 *    - 模块名称映射表
 * 
 * 3. 日志格式化
 *    - 时间戳生成
 *    - 模块和级别标识
 *    - 可变参数处理
 *    - 特殊格式支持
 * 
 * 4. 缓冲区管理
 *    - 固定大小的日志行缓冲
 *    - 缓冲区溢出保护
 *    - 刷新机制
 * 
 * 关键流程
 * ========
 * 1. 日志记录流程
 *    - 日志级别检查
 *    - 头部信息生成
 *    - 消息格式化
 *    - 缓冲区写入
 *    - 日志刷新
 * 
 * 2. 配置管理流程
 *    - 日志级别解析
 *    - 模块配置更新
 *    - 从机标识设置
 * 
 * 3. 输出控制流程
 *    - 标准输出处理
 *    - 特殊格式处理
 *    - IO刷新控制
 * 
 * 注意事项
 * ========
 * 1. 性能考虑
 *    - 日志级别过滤的效率
 *    - 缓冲区大小限制(LOGLINE_LEN)
 *    - 格式化开销
 * 
 * 2. 资源管理
 *    - 及时刷新缓冲区
 *    - 避免缓冲区溢出
 *    - 合理使用特殊日志功能
 * 
 * 3. 使用建议
 *    - 适当设置日志级别
 *    - 正确使用模块标识
 *    - 注意日志信息的完整性
 */

#include "s2conf.h"
#include "sys.h"
#include "rt.h"
#include "uj.h"

const char* LVLSTR[] = {
    [XDEBUG  ]= "XDEB",
    [DEBUG   ]= "DEBU",
    [VERBOSE ]= "VERB",
    [INFO    ]= "INFO",
    [NOTICE  ]= "NOTI",
    [WARNING ]= "WARN",
    [ERROR   ]= "ERRO",
    [CRITICAL]= "CRIT",
};
const char* MODSTR[] = {
    [MOD_ANY/8]= "any",
    [MOD_RAL/8]= "RAL",
    [MOD_S2E/8]= "S2E",
    [MOD_WSS/8]= "WSS",
    [MOD_JSN/8]= "JSN",
    [MOD_AIO/8]= "AIO",
    [MOD_CUP/8]= "CUP",
    [MOD_SYS/8]= "SYS",
    [MOD_TCE/8]= "TCE",
    [MOD_HAL/8]= "HAL",
    [MOD_SIO/8]= "___",
    [MOD_SYN/8]= "SYN",
    [MOD_GPS/8]= "GPS",
    [MOD_SIM/8]= "SIM",
    [MOD_WEB/8]= "WEB",
};

#ifndef CFG_logini_lvl
#define CFG_logini_lvl INFO
#endif

static char   logline[LOGLINE_LEN];
static dbuf_t logbuf = { .buf=logline, .bufsize=sizeof(logline), .pos=0 };
static char   slaveMod[4];
static u1_t   logLevels[32] = {
    CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl,
    CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl,
    CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl,
    CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl, CFG_logini_lvl
};


static int log_header (u1_t mod_level) {
    int mod = (mod_level & MOD_ALL) >> 3;
    logbuf.pos = 0;
    str_t mod_s = slaveMod[0] ? slaveMod : mod >= SIZE_ARRAY(MODSTR) ? "???":MODSTR[mod];
    xprintf(&logbuf, "%.3T [%s:%s] ", rt_getUTC(), mod_s, LVLSTR[mod_level & 7]);
    return logbuf.pos;
}

int log_str2level (const char* level) {
    if( level[0] >= '0' && level[0] <='7' ) {
        return (level[0]-'0') | MOD_ALL;
    }
    int mod = MOD_ALL;
    if( level[0] && level[1] && level[2] && level[3] == ':' ) {
        for( int m=0; m < SIZE_ARRAY(MODSTR); m++ ) {
            if( strncasecmp(level, MODSTR[m], 3) == 0 ) {
                mod = m << 3;
                level += 4;
                goto cklevel;
            }
        }
        return -1;
    }
 cklevel:
    for( int i=0; i < SIZE_ARRAY(LVLSTR); i++ ) {
        if( strncasecmp(level, LVLSTR[i], 4) == 0 )
            return i | mod;
    }
    return -1;
}

str_t log_parseLevels (const char* levels) {
    do {
        int l = log_str2level(levels);
        if( l < 0 )
            return levels;
        log_setLevel(l);
        str_t s = strchr(levels, ',');
        if( s == NULL )
            return NULL;
        levels = s+1;
    } while(1);
}

void log_setSlaveIdx (s1_t idx) {
    slaveMod[0] = 'S';
    slaveMod[1] = idx/10 + '0';
    slaveMod[2] = idx%10 + '0';
}

int log_setLevel (int level) {
    if( level < 0 ) return -1;
    int mod = level & MOD_ALL;
    level &= 7;
    if( mod == MOD_ALL ) {
        for( int m=0; m<32; m++ ) {
            logLevels[m] = level;
        }
        return -1;
    }
    int old = logLevels[mod>>3];
    logLevels[mod>>3] = level;
    return old;
}

int log_shallLog (u1_t mod_level) {
    return (mod_level&7) >= logLevels[(mod_level & MOD_ALL) >> 3];
}

void log_vmsg (u1_t mod_level, const char* fmt, va_list args) {
    if( !log_shallLog(mod_level) )
        return;
    int n = log_header(mod_level);
    logbuf.pos = n;
    vxprintf(&logbuf, fmt, args);
    log_flush();
}

void log_msg (u1_t mod_level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(mod_level, fmt, ap);
    va_end(ap);
}

void log_hal (u1_t level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(MOD_HAL|level, fmt, ap);
    va_end(ap);
}

int log_special (u1_t mod_level, dbuf_t* b) {
    if( !log_shallLog(mod_level) )
        return 0;
    b->buf = logbuf.buf;
    b->bufsize = logbuf.bufsize;
    b->pos = log_header(mod_level);
    return 1;
}

void log_specialFlush (int len) {
    assert(len < logbuf.bufsize);
    logbuf.pos = len;
    log_flush();
}

void log_flush () {
    xeol(&logbuf);
    xeos(&logbuf);
    sys_addLog(logbuf.buf, logbuf.pos);
    logbuf.pos = 0;
}

void log_flushIO () {
    log_flush();
    sys_addLog(logbuf.buf, 0);
}

