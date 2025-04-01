/*
 * 模块概述
 * ========
 * 本文件实现了LoRaWAN基站的配置系统，负责管理和解析各种站点配置参数。
 * 该模块提供了一个灵活的配置框架，支持从不同来源（内置默认值、环境变量、
 * 命令行参数、配置文件等）加载和覆盖配置值，确保基站运行参数的正确设置。
 * 
 * 文件功能
 * ========
 * 本文件主要实现以下功能：
 * - 配置参数的定义和初始化
 * - 多种数据类型的配置值解析
 * - 不同来源配置的优先级处理
 * - 配置值验证和错误处理
 * - 环境变量解析和应用
 * 
 * 主要组件
 * ========
 * 1. 参数定义系统
 *    - 通过宏定义生成参数表
 *    - 支持多种数据类型
 *    - 包含参数描述和默认值
 *    - 源跟踪机制
 * 
 * 2. 类型特定解析器
 *    - 布尔值解析
 *    - 无符号整数解析
 *    - 字符串解析
 *    - 时间跨度解析（小时、分钟、秒、毫秒）
 *    - 存储大小解析（KB、MB）
 * 
 * 3. 值处理系统
 *    - JSON解析支持
 *    - 单位转换
 *    - 格式验证
 *    - 内存管理
 * 
 * 4. 配置初始化流程
 *    - 内置默认值应用
 *    - 环境变量覆盖
 *    - 命令行参数处理
 *    - 配置文件集成
 * 
 * 关键算法
 * ========
 * 1. 参数解析
 *    - 使用ujson解析器处理JSON格式值
 *    - 类型特定的转换和验证
 *    - 错误检测和报告
 * 
 * 2. 时间和大小解析
 *    - 支持人类可读格式（如"5m"表示5分钟）
 *    - 不同单位的自动换算
 *    - 数值和单位分离处理
 * 
 * 3. 配置源优先级
 *    - 源优先级排序：命令行 > 配置文件 > 环境变量 > 内置默认值
 *    - 源标记以跟踪配置来源
 *    - 值覆盖逻辑
 * 
 * 注意事项
 * ========
 * 1. 内存管理
 *    - 字符串值需要复制和释放
 *    - 使用rt_strdup和rt_free管理内存
 *    - 避免内存泄漏的处理
 * 
 * 2. 错误处理
 *    - 解析错误的详细日志
 *    - 源跟踪以便于调试
 *    - 失败时的回退机制
 * 
 * 3. 扩展性
 *    - 通过宏系统轻松添加新参数
 *    - 支持自定义解析器
 *    - 与其他配置系统的集成点
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

#include <stdio.h>
#include "s2conf.h"
#include "rt.h"
#include "uj.h"


#undef _s2conf_x_
#undef CONF_PARAM
#define CONF_PARAM(name,type,fn,value,info) type##_t name;

#include "s2conf.h"


static int parse_bool     (struct conf_param* param);
static int parse_u4       (struct conf_param* param);
static int parse_str      (struct conf_param* param);
static int parse_tspan_h  (struct conf_param* param);
static int parse_tspan_m  (struct conf_param* param);
static int parse_tspan_s  (struct conf_param* param);
static int parse_tspan_ms (struct conf_param* param);
static int parse_size_kb  (struct conf_param* param);
static int parse_size_mb  (struct conf_param* param);


#undef _s2conf_x_
#undef CONF_PARAM
#define CONF_PARAM(name,type,fn,value,info) { #name, #type, info, "builtin", value, &name, parse_##fn },

struct conf_param conf_params[] = {
#include "s2conf.h"
    { NULL }
};


static int parse_bool (struct conf_param* param) {
    ujdec_t D;
    char* v = rt_strdup(param->value);
    uj_iniDecoder(&D, v, strlen(v));
    if( uj_decode(&D) ) {
        LOG(ERROR, "Parsing of config param '%s' (%s) as bool failed: %s",
            param->name, param->src, param->value);
        free(v);
        return 0;
    }
    *(bool*)param->pvalue = uj_bool(&D);
    uj_assertEOF(&D);
    rt_free(v);
    return 1;
}


static int parse_u4 (struct conf_param* param) {
    ujdec_t D;
    char* v = rt_strdup(param->value);
    uj_iniDecoder(&D, v, strlen(v));
    if( uj_decode(&D) ) {
        LOG(ERROR, "Parsing of config param '%s' (%s) as u4 failed: %s",
            param->name, param->src, param->value);
        free(v);
        return 0;
    }
    *(u4_t*)param->pvalue = uj_uint(&D);
    uj_assertEOF(&D);
    rt_free(v);
    return 1;
}


static int parse_str (struct conf_param* param) {
    ujdec_t D;
    char* v = rt_strdup(param->value);
    uj_iniDecoder(&D, v, strlen(v));
    if( uj_decode(&D) ) {
        LOG(ERROR, "Parsing of config param '%s' (%s) as 'str' failed: %s",
            param->name, param->src, param->value);
        free(v);
        return 0;
    }
    rt_free(*(void**)param->pvalue);
    *(str_t*)param->pvalue = rt_strdup(uj_str(&D));
    uj_assertEOF(&D);
    rt_free(v);
    return 1;
}


static int parse_tspan (struct conf_param* param, ustime_t defaultUnit) {
    ujdec_t D;
    char* v = rt_strdup(param->value);
    uj_iniDecoder(&D, v, strlen(v));
    if( uj_decode(&D) ) {
        LOG(ERROR, "Parsing of config param '%s' (%s) as 'tspan' failed: %s",
            param->name, param->src, param->value);
        free(v);
        return 0;
    }
    ustime_t tspan;
    if( uj_nextValue(&D) == UJ_STRING ) {
        char *s = uj_str(&D);
        tspan = rt_readSpan((str_t*)&s, defaultUnit);
        if( tspan < 0 || s[0] )
            uj_error(&D, "Syntax error");
    } else {
        tspan = uj_num(&D) * defaultUnit;
    }
    uj_assertEOF(&D);
    *(ustime_t*)param->pvalue = tspan;
    rt_free(v);
    return 1;
}

static int parse_tspan_h (struct conf_param* param) {
    return parse_tspan(param, rt_seconds(3600));
}

static int parse_tspan_m (struct conf_param* param) {
    return parse_tspan(param, rt_seconds(60));
}

static int parse_tspan_s (struct conf_param* param) {
    return parse_tspan(param, rt_seconds(1));
}

static int parse_tspan_ms (struct conf_param* param) {
    return parse_tspan(param, rt_millis(1));
}

static int parse_size (struct conf_param* param, u4_t defaultUnit) {
    ujdec_t D;
    char* v = rt_strdup(param->value);
    uj_iniDecoder(&D, v, strlen(v));
    if( uj_decode(&D) ) {
        LOG(ERROR, "Parsing of config param '%s' (%s) as 'size' failed: %s",
            param->name, param->src, param->value);
        free(v);
        return 0;
    }
    sL_t size;
    if( uj_nextValue(&D) == UJ_STRING ) {
        char *s = uj_str(&D);
        size = rt_readSize((str_t*)&s, defaultUnit);
        if( size < 0 || s[0] )
            uj_error(&D, "Syntax error");
    } else {
        size = uj_num(&D) * defaultUnit;
    }
    uj_assertEOF(&D);
    *(u4_t*)param->pvalue = size;
    rt_free(v);
    return 1;
}

static int parse_size_kb (struct conf_param* param) {
    return parse_size(param, 1024);
}

static int parse_size_mb (struct conf_param* param) {
    return parse_size(param, 1024*1024);
}


void s2conf_ini () {
    for( struct conf_param* p = conf_params; p->name; p++ ) {
        s2conf_set("builtin", p->name, p->value);
        str_t v = getenv(p->name);
        if( v ) {
            if (strcmp(p->type,"str")==0)
                s2conf_set("env", p->name, rt_strdupq(v));
            else
                s2conf_set("env", p->name, rt_strdup(v));
        }
    }
}


void* s2conf_get (str_t name) {
    for( struct conf_param* p = conf_params; p->name; p++ ) {
        if( strcmp(p->name, name) == 0 )
            return p;
    }
    return NULL;
}


int s2conf_set (str_t src, str_t name, str_t value) {
    struct conf_param* p = s2conf_get(name);
    if( p == NULL ) {
        assert(strcmp(src,"builtin") != 0);  // initialization values must be ok
        free((void*)value);
        return -1; // No such param name
    }
    struct conf_param n = *p;
    n.src = src;
    n.value = value;
    if( !n.parseFn(&n) ) {
        assert(p->value != value);  // initialization values must be ok
        free((void*)value);
        return 0;  // parsing failed
    }
    if( strcmp(p->src, "builtin") != 0 )
        free((void*)p->value);
    *p = n;
    return 1;
}


void s2conf_printAll () {
    for( struct conf_param* p = conf_params; p->name; p++ ) {
        fprintf(stderr, "%6s %-20s = %-10s %-12s %s\n",
                p->type, p->name, p->value, p->src, p->info);
    }
}
