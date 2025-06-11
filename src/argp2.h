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
 * ARGP2 命令行参数解析模块头文件
 * ================================
 * 功能描述：提供轻量级的命令行参数解析功能，用于BasicStation启动时的参数处理
 * 设计理念：简化版的GNU argp库实现，支持长短选项解析和参数验证
 * 适用场景：嵌入式环境下的命令行参数解析，避免对GNU argp库的依赖
 */

#if defined(CFG_argp)  // 如果定义了CFG_argp宏，使用自定义的argp实现
#ifndef _argp2_h_
#define _argp2_h_

// ==============================================
// 核心常量定义 - 参数解析控制码
// ==============================================

#define ARGP_KEY_ARG 1      // 表示解析到的是位置参数（非选项参数）
#define ARGP_KEY_END 2      // 表示参数解析结束，用于清理和验证操作
#define OPTION_HIDDEN 0x10  // 选项标志：该选项在帮助信息中隐藏显示
#define ARGP_ERR_UNKNOWN -1 // 错误码：遇到未知选项或解析失败

// ==============================================
// 核心数据结构定义
// ==============================================

/**
 * struct argp_state - 参数解析状态结构
 * 功能：记录当前参数解析的状态和上下文信息
 * 生命周期：在整个参数解析过程中保持，用于跟踪解析进度
 */
struct argp_state {
    int    argc;    // 命令行参数总数，包括程序名称
    char** argv;    // 命令行参数数组，argv[0]为程序名称
    int    aidx;    // 当前处理的参数索引（argument index）
    int    cidx;    // 当前处理的字符索引（character index，用于短选项组合）
};

/**
 * struct argp_option - 命令行选项定义结构
 * 功能：定义单个命令行选项的属性和行为
 * 使用：创建选项数组，以NULL结构体结尾
 */
struct argp_option {
    const char* long_opt;   // 长选项名称（如"help"）
    int         short_opt;  // 短选项字符（如'h'），0表示无短选项
    const char* arg_spec;   // 参数规格说明（如"FILE"），NULL表示无参数
    int         flag;       // 选项标志（如OPTION_HIDDEN），控制选项行为
    const char* doc;        // 选项描述文档，用于生成帮助信息
};

/**
 * struct argp - 参数解析器主结构
 * 功能：定义完整的参数解析配置和回调机制
 * 核心：包含选项表、解析函数和文档信息
 */
struct argp {
    struct argp_option* options;  // 选项定义数组，最后一个元素必须为全0
    int (*parsefn) (int key, char* arg, struct argp_state* state);  // 参数解析回调函数
    const char* args_spec;        // 位置参数规格说明（如"[FILE...]"）
    void* dummy1;                 // 保留字段，兼容GNU argp结构
    void* dummy2;                 // 保留字段，用于后续扩展
    void* dummy3;                 // 保留字段，保持结构对齐
    void* dummy4;                 // 保留字段，预留接口
};

// ==============================================
// 核心API函数声明
// ==============================================

/**
 * argp_parse - 执行命令行参数解析
 * 参数：
 *   argp   - 参数解析器配置结构
 *   argc   - 命令行参数计数
 *   argv   - 命令行参数数组
 *   flag   - 解析控制标志（当前未使用）
 *   dummy1 - 保留参数，兼容GNU argp
 *   dummy2 - 保留参数，保持接口一致
 * 返回值：
 *   0  - 解析成功
 *   非0 - 解析失败或遇到错误
 * 功能：
 *   1. 遍历命令行参数数组
 *   2. 识别长选项（--option）和短选项（-o）
 *   3. 调用解析回调函数处理每个选项和参数
 *   4. 处理选项参数的提取和验证
 *   5. 生成错误信息和帮助文档
 */
int argp_parse (struct argp* argp, int argc, char** argv, int flag, void* dummy1, void* dummy2);

#endif // _argp2_h_
#else // defined(CFG_argp)
#include <argp.h>  // 如果未定义CFG_argp，使用系统标准的GNU argp库
#endif // defined(CFG_argp)
