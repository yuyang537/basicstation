/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的命令行参数解析系统，提供简化版的 GNU argp 兼容接口。
 * 它为不支持标准 argp 库的平台提供替代实现，确保命令行解析功能的跨平台一致性。
 * 该模块允许基站程序以统一的方式处理命令行选项和参数，无需关心底层平台差异。
 * 
 * 文件功能
 * ========
 * 本头文件定义了简化版 argp 库的核心接口：
 * - argp 相关结构体（argp, argp_state, argp_option）
 * - 参数解析函数 argp_parse
 * - 选项标志和键值常量
 * - 条件编译逻辑，在有标准 argp 时使用系统库
 * 
 * 主要组件
 * ========
 * 1. 选项描述系统
 *    - argp_option 结构体：定义命令行选项
 *    - 长短选项名支持
 *    - 选项文档和参数规格
 *    - 隐藏选项标志
 * 
 * 2. 解析状态管理
 *    - argp_state 结构体：跟踪解析状态
 *    - 参数计数和索引管理
 *    - 命令行数组访问
 *    - 当前处理位置
 * 
 * 3. 解析控制系统
 *    - argp 结构体：整合选项和解析函数
 *    - 参数规格描述
 *    - 解析回调函数
 *    - 错误处理支持
 * 
 * 4. 平台兼容层
 *    - 条件编译指令
 *    - 标准 argp 接口仿真
 *    - 关键常量定义
 * 
 * 关键接口
 * ========
 * 1. 选项定义
 *    - 通过 argp_option 数组定义选项
 *    - 设置长短选项名
 *    - 指定文档字符串
 *    - 配置选项标志
 * 
 * 2. 解析函数
 *    - 实现回调函数处理识别的选项
 *    - 接收选项键、参数和状态
 *    - 返回处理结果代码
 * 
 * 3. 参数解析
 *    - 调用 argp_parse 函数启动解析
 *    - 传入选项定义和回调
 *    - 处理命令行参数
 * 
 * 注意事项
 * ========
 * 1. 兼容性限制
 *    - 简化版实现，不支持完整 GNU argp 功能
 *    - 基本选项和参数处理足够大多数需求
 *    - 复杂情况可能需要额外处理
 * 
 * 2. 用法差异
 *    - 部分标志和行为可能与标准 argp 有差异
 *    - 错误处理机制简化
 *    - 文档显示格式可能不同
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

#if defined(CFG_argp)
#ifndef _argp2_h_
#define _argp2_h_

#define ARGP_KEY_ARG 1
#define ARGP_KEY_END 2
#define OPTION_HIDDEN 0x10
#define ARGP_ERR_UNKNOWN -1

struct argp_state {
    int    argc;
    char** argv;
    int    aidx;
    int    cidx;
};

struct argp_option {
    const char* long_opt;
    int         short_opt;
    const char* arg_spec;
    int         flag;
    const char* doc;
};

struct argp {
    struct argp_option* options;
    int (*parsefn) (int key, char* arg, struct argp_state* state);
    const char* args_spec;
    void* dummy1;
    void* dummy2;
    void* dummy3;
    void* dummy4;
};

int argp_parse (struct argp* argp, int argc, char** argv, int flag, void* dummy1, void* dummy2);

#endif // _argp2_h_
#else // defined(CFG_argp)
#include <argp.h>
#endif // defined(CFG_argp)
