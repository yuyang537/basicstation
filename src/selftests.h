/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的自测试系统，提供了一套完整的测试框架来验证系统各个组件的功能正确性。
 * 它定义了测试宏和测试函数，使开发者能够对关键模块进行单元测试，确保代码质量和系统稳定性。
 * 该模块是开发和维护过程中的重要工具，帮助识别和修复潜在的问题。
 * 
 * 文件功能
 * ========
 * 本头文件定义了自测试系统的核心接口：
 * - 测试断言宏（TCHECK, TFAIL）
 * - 各个子系统的测试函数声明
 * - 测试失败处理函数
 * - 主测试入口函数
 * 
 * 主要组件
 * ========
 * 1. 测试断言系统
 *    - TCHECK 宏：条件检查断言
 *    - TFAIL 宏：直接失败断言
 *    - 失败信息收集和处理
 * 
 * 2. 单元测试函数
 *    - 发送队列测试（txq）
 *    - 接收队列测试（rxq）
 *    - LoRa 编解码测试
 *    - 运行时系统测试（rt）
 *    - JSON 编解码测试（ujdec、ujenc）
 *    - 格式化输出测试（xprintf）
 *    - 文件系统测试（fs）
 * 
 * 3. 测试控制系统
 *    - 测试失败处理
 *    - 测试结果报告
 *    - 全局测试入口
 * 
 * 测试范围
 * ========
 * 1. 队列系统测试
 *    - 发送队列的分配和释放
 *    - 接收队列的入队和出队
 *    - 资源管理和边界条件
 * 
 * 2. 数据处理测试
 *    - LoRa 数据编解码
 *    - JSON 解析和生成
 *    - 格式化字符串处理
 * 
 * 3. 系统功能测试
 *    - 运行时环境
 *    - 文件系统操作
 *    - 内存管理
 * 
 * 使用方法
 * ========
 * 1. 单元测试
 *    - 调用特定的测试函数（如 selftest_txq()）来测试单个模块
 *    - 使用 TCHECK 宏验证条件
 *    - 检查测试输出和日志
 * 
 * 2. 全系统测试
 *    - 调用 selftests() 函数运行所有测试
 *    - 分析测试结果和报告
 *    - 解决发现的问题
 * 
 * 注意事项
 * ========
 * 1. 测试环境
 *    - 测试可能需要特定的硬件或模拟环境
 *    - 某些测试可能修改系统状态
 *    - 建议在隔离环境中运行完整测试
 * 
 * 2. 扩展测试
 *    - 添加新测试时需遵循现有模式
 *    - 确保测试覆盖边界条件和错误情况
 *    - 保持测试代码的可维护性
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

#ifndef _selftests_h_
#define _selftests_h_


#define TCHECK(cond) do {                                               \
        if(!(cond)) {                                                   \
            selftest_fail(#cond, __FILE__, __LINE__);                   \
        }                                                               \
    } while (0)

#define TFAIL(expr) do {                                                \
        selftest_fail(#expr, __FILE__, __LINE__);                       \
    } while (0)


extern void selftest_txq ();
extern void selftest_rxq ();
extern void selftest_lora ();
extern void selftest_rt ();
extern void selftest_ujdec ();
extern void selftest_ujenc ();
extern void selftest_xprintf ();
extern void selftest_fs ();

void selftest_fail (const char* expr, const char* file, int line);
void selftests ();


#endif // _selftests_h_
