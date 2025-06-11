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

// 文件功能：S2 配置参数管理系统 - LoRaWAN基站Station-to-Server协议的配置解析和管理
// 作用：提供统一的配置参数定义、解析、设置和获取接口，支持多种数据类型和来源

#include <stdio.h>        // 标准输入输出函数
#include "s2conf.h"       // S2配置系统头文件，包含参数定义和类型声明
#include "rt.h"           // 运行时基础库，提供内存管理和工具函数
#include "uj.h"           // JSON编解码器，用于解析配置值


// 宏定义重置 - 使用CONF_PARAM宏来定义实际的全局变量
#undef _s2conf_x_
#undef CONF_PARAM
#define CONF_PARAM(name,type,fn,value,info) type##_t name;  // 定义配置参数的全局变量

#include "s2conf.h"       // 重新包含头文件，此时CONF_PARAM宏会展开为变量定义


// 静态函数声明 - 各种类型的配置参数解析函数
static int parse_bool     (struct conf_param* param);  // 解析布尔值参数
static int parse_u4       (struct conf_param* param);  // 解析32位无符号整数参数
static int parse_str      (struct conf_param* param);  // 解析字符串参数
static int parse_tspan_h  (struct conf_param* param);  // 解析小时为单位的时间跨度
static int parse_tspan_m  (struct conf_param* param);  // 解析分钟为单位的时间跨度
static int parse_tspan_s  (struct conf_param* param);  // 解析秒为单位的时间跨度
static int parse_tspan_ms (struct conf_param* param);  // 解析毫秒为单位的时间跨度
static int parse_size_kb  (struct conf_param* param);  // 解析KB为单位的大小参数
static int parse_size_mb  (struct conf_param* param);  // 解析MB为单位的大小参数


// 宏定义重置 - 使用CONF_PARAM宏来构建配置参数数组
#undef _s2conf_x_
#undef CONF_PARAM
#define CONF_PARAM(name,type,fn,value,info) { #name, #type, info, "builtin", value, &name, parse_##fn },

// 全局配置参数数组 - 包含所有系统配置参数的定义和元数据
struct conf_param conf_params[] = {
#include "s2conf.h"      // 包含所有配置参数定义，通过宏展开为数组元素
    { NULL }             // 数组结束标记，name为NULL表示数组结束
};


// 函数功能：解析布尔类型的配置参数
// 参数说明：param - 配置参数结构体指针，包含参数名、值和目标变量地址
// 返回值：1表示解析成功，0表示解析失败
// 调用时机：当配置系统需要设置布尔类型参数时调用
static int parse_bool (struct conf_param* param) {
    ujdec_t D;                                    // JSON解码器结构体
    char* v = rt_strdup(param->value);           // 复制参数值字符串到临时变量
    uj_iniDecoder(&D, v, strlen(v));             // 初始化JSON解码器，准备解析字符串
    if( uj_decode(&D) ) {                        // 尝试解码JSON值
        LOG(ERROR, "Parsing of config param '%s' (%s) as bool failed: %s",
            param->name, param->src, param->value);  // 记录解析错误日志
        free(v);                                 // 释放临时字符串内存
        return 0;                                // 返回解析失败
    }
    *(bool*)param->pvalue = uj_bool(&D);         // 将解析后的布尔值写入目标变量
    uj_assertEOF(&D);                            // 断言JSON流已到达结尾
    rt_free(v);                                  // 释放临时字符串内存
    return 1;                                    // 返回解析成功
}


// 函数功能：解析32位无符号整数类型的配置参数
// 参数说明：param - 配置参数结构体指针
// 返回值：1表示解析成功，0表示解析失败
// 调用时机：当配置系统需要设置u4类型参数时调用
static int parse_u4 (struct conf_param* param) {
    ujdec_t D;                                    // JSON解码器结构体
    char* v = rt_strdup(param->value);           // 复制参数值字符串
    uj_iniDecoder(&D, v, strlen(v));             // 初始化JSON解码器
    if( uj_decode(&D) ) {                        // 尝试解码JSON值
        LOG(ERROR, "Parsing of config param '%s' (%s) as u4 failed: %s",
            param->name, param->src, param->value);  // 记录解析错误
        free(v);                                 // 释放内存
        return 0;                                // 返回失败
    }
    *(u4_t*)param->pvalue = uj_uint(&D);         // 将解析后的无符号整数写入目标变量
    uj_assertEOF(&D);                            // 确认JSON流结束
    rt_free(v);                                  // 释放内存
    return 1;                                    // 返回成功
}


// 函数功能：解析字符串类型的配置参数
// 参数说明：param - 配置参数结构体指针
// 返回值：1表示解析成功，0表示解析失败
// 调用时机：当配置系统需要设置字符串类型参数时调用
static int parse_str (struct conf_param* param) {
    ujdec_t D;                                    // JSON解码器结构体
    char* v = rt_strdup(param->value);           // 复制参数值字符串
    uj_iniDecoder(&D, v, strlen(v));             // 初始化JSON解码器
    if( uj_decode(&D) ) {                        // 尝试解码JSON值
        LOG(ERROR, "Parsing of config param '%s' (%s) as 'str' failed: %s",
            param->name, param->src, param->value);  // 记录解析错误
        free(v);                                 // 释放临时内存
        return 0;                                // 返回失败
    }
    rt_free(*(void**)param->pvalue);             // 释放旧的字符串内存(如果存在)
    *(str_t*)param->pvalue = rt_strdup(uj_str(&D));  // 分配新内存并复制解析后的字符串
    uj_assertEOF(&D);                            // 确认JSON流结束
    rt_free(v);                                  // 释放临时内存
    return 1;                                    // 返回成功
}


// 函数功能：解析时间跨度参数的通用函数
// 参数说明：param - 配置参数结构体指针
//          defaultUnit - 默认时间单位(微秒)，用于纯数字值的转换
// 返回值：1表示解析成功，0表示解析失败
// 调用时机：被具体的时间跨度解析函数调用
static int parse_tspan (struct conf_param* param, ustime_t defaultUnit) {
    ujdec_t D;                                    // JSON解码器结构体
    char* v = rt_strdup(param->value);           // 复制参数值字符串
    uj_iniDecoder(&D, v, strlen(v));             // 初始化JSON解码器
    if( uj_decode(&D) ) {                        // 尝试解码JSON值
        LOG(ERROR, "Parsing of config param '%s' (%s) as 'tspan' failed: %s",
            param->name, param->src, param->value);  // 记录解析错误
        free(v);                                 // 释放内存
        return 0;                                // 返回失败
    }
    ustime_t tspan;                              // 时间跨度变量
    if( uj_nextValue(&D) == UJ_STRING ) {        // 如果是字符串格式(如"30s", "5m")
        char *s = uj_str(&D);                    // 获取字符串值
        tspan = rt_readSpan((str_t*)&s, defaultUnit);  // 解析时间跨度字符串
        if( tspan < 0 || s[0] )                  // 检查解析是否成功
            uj_error(&D, "Syntax error");        // 设置语法错误
    } else {
        tspan = uj_num(&D) * defaultUnit;        // 如果是数字格式，乘以默认单位
    }
    uj_assertEOF(&D);                            // 确认JSON流结束
    *(ustime_t*)param->pvalue = tspan;           // 将解析后的时间跨度写入目标变量
    rt_free(v);                                  // 释放内存
    return 1;                                    // 返回成功
}

// 函数功能：解析以小时为默认单位的时间跨度参数
// 参数说明：param - 配置参数结构体指针
// 返回值：1表示解析成功，0表示解析失败
// 调用时机：解析小时类型的时间配置参数时调用
static int parse_tspan_h (struct conf_param* param) {
    return parse_tspan(param, rt_seconds(3600)); // 调用通用解析函数，默认单位为3600秒(1小时)
}

// 函数功能：解析以分钟为默认单位的时间跨度参数
static int parse_tspan_m (struct conf_param* param) {
    return parse_tspan(param, rt_seconds(60));   // 默认单位为60秒(1分钟)
}

// 函数功能：解析以秒为默认单位的时间跨度参数
static int parse_tspan_s (struct conf_param* param) {
    return parse_tspan(param, rt_seconds(1));    // 默认单位为1秒
}

// 函数功能：解析以毫秒为默认单位的时间跨度参数
static int parse_tspan_ms (struct conf_param* param) {
    return parse_tspan(param, rt_millis(1));     // 默认单位为1毫秒
}

// 函数功能：解析大小参数的通用函数
// 参数说明：param - 配置参数结构体指针
//          defaultUnit - 默认大小单位(字节)，用于纯数字值的转换
// 返回值：1表示解析成功，0表示解析失败
// 调用时机：被具体的大小解析函数调用
static int parse_size (struct conf_param* param, u4_t defaultUnit) {
    ujdec_t D;                                    // JSON解码器结构体
    char* v = rt_strdup(param->value);           // 复制参数值字符串
    uj_iniDecoder(&D, v, strlen(v));             // 初始化JSON解码器
    if( uj_decode(&D) ) {                        // 尝试解码JSON值
        LOG(ERROR, "Parsing of config param '%s' (%s) as 'size' failed: %s",
            param->name, param->src, param->value);  // 记录解析错误
        free(v);                                 // 释放内存
        return 0;                                // 返回失败
    }
    sL_t size;                                   // 大小变量(有符号长整型)
    if( uj_nextValue(&D) == UJ_STRING ) {        // 如果是字符串格式(如"10MB", "512KB")
        char *s = uj_str(&D);                    // 获取字符串值
        size = rt_readSize((str_t*)&s, defaultUnit);  // 解析大小字符串
        if( size < 0 || s[0] )                   // 检查解析是否成功
            uj_error(&D, "Syntax error");        // 设置语法错误
    } else {
        size = uj_num(&D) * defaultUnit;         // 如果是数字格式，乘以默认单位
    }
    uj_assertEOF(&D);                            // 确认JSON流结束
    *(u4_t*)param->pvalue = size;                // 将解析后的大小写入目标变量
    rt_free(v);                                  // 释放内存
    return 1;                                    // 返回成功
}

// 函数功能：解析以KB为默认单位的大小参数
static int parse_size_kb (struct conf_param* param) {
    return parse_size(param, 1024);              // 默认单位为1024字节(1KB)
}

// 函数功能：解析以MB为默认单位的大小参数
static int parse_size_mb (struct conf_param* param) {
    return parse_size(param, 1024*1024);         // 默认单位为1024*1024字节(1MB)
}


// 函数功能：初始化S2配置系统，设置所有参数的默认值和环境变量值
// 参数说明：无
// 返回值：无
// 调用时机：系统启动时调用，在处理命令行参数和配置文件之前
void s2conf_ini () {
    for( struct conf_param* p = conf_params; p->name; p++ ) {  // 遍历所有配置参数
        s2conf_set("builtin", p->name, p->value);  // 设置内置默认值，来源标记为"builtin"
        str_t v = getenv(p->name);               // 尝试从环境变量获取参数值
        if( v ) {                                // 如果环境变量存在
            if (strcmp(p->type,"str")==0)        // 如果参数类型是字符串
                s2conf_set("env", p->name, rt_strdupq(v));  // 复制带引号的字符串值
            else
                s2conf_set("env", p->name, rt_strdup(v));   // 复制普通值
        }
    }
}

// 函数功能：获取指定名称的配置参数结构体
// 参数说明：name - 配置参数名称
// 返回值：配置参数结构体指针，如果参数不存在则返回NULL
// 调用时机：查询特定配置参数信息时调用
void* s2conf_get (str_t name) {
    for( struct conf_param* p = conf_params; p->name; p++ ) {  // 遍历配置参数数组
        if( strcmp(p->name, name) == 0 )         // 比较参数名称
            return p;                            // 找到匹配项，返回参数结构体指针
    }
    return NULL;                                 // 未找到匹配项，返回NULL
}

// 函数功能：设置指定配置参数的值
// 参数说明：src - 值来源标识("builtin", "env", "config", "cmdline"等)
//          name - 配置参数名称
//          value - 新的参数值字符串
// 返回值：1表示设置成功，0表示设置失败(参数不存在或解析失败)
// 调用时机：从配置文件、命令行参数或环境变量设置配置值时调用
int s2conf_set (str_t src, str_t name, str_t value) {
    struct conf_param* p = s2conf_get(name);     // 查找指定名称的配置参数
    if( p == NULL )                              // 如果参数不存在
        return 0;                                // 返回设置失败
    struct conf_param n = *p;                    // 复制参数结构体
    n.src = src;                                 // 设置新的来源标识
    n.value = value;                             // 设置新的值
    if( !n.parseFn(&n) ) {                       // 调用对应的解析函数
        if( strcmp(p->src,"builtin") != 0 )      // 如果解析失败且旧值不是内置值
            rt_free((void*)p->value);            // 释放旧值内存
        p->src = "builtin";                      // 恢复为内置来源
        p->value = "null";                       // 设置为null值
        return 0;                                // 返回设置失败
    }
    // 解析成功，更新参数信息
    if( p->value && strcmp(p->src,"builtin") != 0 )  // 如果旧值不是内置值
        rt_free((void*)p->value);                // 释放旧值内存
    p->src = src;                                // 更新来源标识
    p->value = value;                            // 更新值
    return 1;                                    // 返回设置成功
}

// 函数功能：打印所有配置参数的当前值和信息
// 参数说明：无
// 返回值：无
// 调用时机：调试或诊断时调用，显示系统当前的配置状态
void s2conf_printAll () {
    for( struct conf_param* p = conf_params; p->name; p++ ) {  // 遍历所有配置参数
        printf("%-25s %-10s %-10s %s\n", p->name, p->type, p->src, p->value);  // 格式化输出参数信息
    }
}
