/*
 * 模块概述
 * ========
 * 本文件实现了LoRaWAN基站的自测试系统，用于验证各个模块的功能正确性。
 * 自测试系统提供了一个框架，允许在开发和部署过程中执行单元测试，确保
 * 代码质量和系统稳定性。该模块是开发过程中的关键工具，可以自动化地检测
 * 各个组件的问题。
 * 
 * 文件功能
 * ========
 * 本文件主要实现以下功能：
 * - 自测试框架的核心实现
 * - 测试失败处理机制
 * - 测试结果报告
 * - 测试函数注册和调用
 * - 测试执行控制流程
 * 
 * 主要组件
 * ========
 * 1. 测试执行引擎
 *    - 测试函数表管理
 *    - 顺序执行测试
 *    - 失败处理和恢复
 *    - 结果统计和报告
 * 
 * 2. 测试失败处理
 *    - 错误信息格式化
 *    - 测试回溯机制
 *    - 异常处理和恢复
 *    - 测试续行控制
 * 
 * 3. 测试结果报告
 *    - 成功/失败统计
 *    - 测试输出格式化
 *    - 错误位置信息
 *    - 退出状态控制
 * 
 * 关键流程
 * ========
 * 1. 测试初始化流程
 *    - 设置测试环境
 *    - 准备失败处理机制
 *    - 统计初始化
 * 
 * 2. 测试执行流程
 *    - 顺序调用测试函数
 *    - 捕获测试失败
 *    - 恢复并继续下一测试
 *    - 累计失败计数
 * 
 * 3. 结果报告流程
 *    - 统计测试结果
 *    - 输出成功/失败信息
 *    - 设置进程退出状态
 *    - 结束测试会话
 * 
 * 注意事项
 * ========
 * 1. 执行控制
 *    - 通过编译时配置控制测试启用
 *    - 测试失败使用longjmp恢复
 *    - 测试独立性保证
 * 
 * 2. 扩展方法
 *    - 新测试函数需添加到测试表中
 *    - 测试应避免持久性副作用
 *    - 测试应有明确的通过/失败条件
 * 
 * 3. 使用场景
 *    - 开发阶段验证功能
 *    - 调试期间重现问题
 *    - 发布前回归测试
 *    - 持续集成流程
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
#include <stdlib.h>
#include <setjmp.h>
#include "selftests.h"

#if defined(CFG_selftests)

static int fails;
static jmp_buf onfail;

static void (*const selftest_fns[])() = {
    selftest_txq,
    selftest_rxq,
    selftest_lora,
    selftest_rt,
    selftest_ujdec,
    selftest_ujenc,
    selftest_xprintf,
    selftest_fs,
    NULL
};


// LCOV_EXCL_START
void selftest_fail (const char* expr, const char* file, int line) {
    fprintf(stderr, "TEST FAILED: %s at %s:%d\n", expr, file, line);
    longjmp(onfail, 1);
}
// LCOV_EXCL_STOP


void selftests () {
    int i=-1;
    while( selftest_fns[++i] ) {
        if( setjmp(onfail) ) {
            fails += 1;                  // LCOV_EXCL_LINE
        } else {
            selftest_fns[i]();
        }
    }
    if( fails == 0 ) {
        fprintf(stderr,"ALL %d SELFTESTS PASSED\n", i);
        exit(0);
    }
    fprintf(stderr,"TESTS FAILED: %d of %d\n", fails, i); // LCOV_EXCL_LINE
    exit(70);                                             // LCOV_EXCL_LINE
}

#else // !defined(CFG_selftests)

void selftests () {}

#endif // !defined(CFG_selftests)
