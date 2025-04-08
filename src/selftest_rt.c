/*
 * 文件概述
 * ========
 * 本文件实现了LoRaWAN基站运行时工具函数(rt)的自测试用例。这些测试验证了
 * 系统基础工具函数的正确性，包括数据转换、字符串解析、时间处理等功能。
 * 运行时工具函数是系统各模块的基础，确保这些函数的正确性对整个系统的
 * 稳定运行至关重要。
 * 
 * 文件功能
 * ========
 * 本文件主要测试以下功能：
 * - 时间单位转换函数（秒、毫秒等）
 * - 字节序转换函数（大小端转换）
 * - 十六进制字符解析
 * - 数字字符串解析
 * - EUI格式字符串解析（设备标识、网络标识等）
 * - 时间跨度字符串解析
 * 
 * 主要测试组件
 * ========
 * 1. 时间单位测试
 *    - 秒到微秒的转换
 *    - 毫秒到微秒的转换
 *    - 时间单位一致性验证
 * 
 * 2. 字节序转换测试
 *    - 2字节、4字节、8字节的小端到主机字节序转换
 *    - 2字节大端到主机字节序转换
 *    - 多字节数组的正确解析
 * 
 * 3. 字符解析测试
 *    - 十六进制字符到数值的转换
 *    - 大小写十六进制字符处理
 *    - 错误输入处理
 * 
 * 4. 数字解析测试
 *    - 十进制字符串转数值
 *    - 处理空格和终止条件
 *    - 错误格式处理
 * 
 * 5. EUI解析测试
 *    - 普通数字格式EUI解析
 *    - 使用连字符分隔的EUI格式解析
 *    - 使用冒号分隔的EUI格式解析
 *    - IPv6格式解析（双冒号缩写）
 *    - 错误格式处理
 * 
 * 6. 时间跨度解析测试
 *    - 复合时间单位解析（天、小时、分钟、秒、毫秒）
 *    - 单一时间单位解析
 *    - 错误格式处理
 * 
 * 测试策略
 * ========
 * 测试采用断言验证方式，对每个工具函数的输出结果与预期值进行比较，确保
 * 函数行为与设计一致。测试涵盖了正常使用场景和错误处理情况。
 * 
 * 注意事项
 * ========
 * 1. 测试依赖
 *    - 测试依赖rt模块的实现
 *    - 使用selftests框架
 * 
 * 2. 重要性
 *    - 这些基础函数被系统中的多个模块使用
 *    - 确保字节序转换的正确性对网络通信至关重要
 *    - EUI解析对LoRaWAN设备识别和通信至关重要
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

#include "selftests.h"
#include "rt.h"

/*
 * 函数：selftest_rt
 * 功能：运行时工具函数自测试主函数
 * 说明：
 *   该函数实现了运行时工具函数的全面测试，包括：
 *   1. 时间单位测试
 *      - 验证秒和毫秒的转换
 *      - 检查时间单位的一致性
 *   
 *   2. 字节序测试
 *      - 测试小端序转换
 *      - 验证大端序转换
 *      - 检查多字节数组解析
 *   
 *   3. 字符解析测试
 *      - 测试十六进制字符转换
 *      - 验证大小写处理
 *      - 检查错误输入处理
 *   
 *   4. 数字解析测试
 *      - 测试十进制字符串解析
 *      - 验证空格处理
 *      - 检查错误格式处理
 *   
 *   5. EUI解析测试
 *      - 测试各种EUI格式解析
 *      - 验证IPv6格式处理
 *      - 检查错误格式处理
 *   
 *   6. 时间跨度测试
 *      - 测试复合时间单位解析
 *      - 验证单一时间单位解析
 *      - 检查错误格式处理
 */
void selftest_rt() {
    TCHECK(rt_seconds(2) == rt_millis(2000));
    u1_t b[] = { 1,2,3,4,5,6,7,8 };
    TCHECK(rt_rlsbf2(b) == 0x0201);
    TCHECK(rt_rmsbf2(b) == 0x0102);
    TCHECK(rt_rlsbf4(b) == 0x04030201);
    TCHECK(rt_rlsbf8(b) == (uL_t)0x0807060504030201);
    TCHECK(rt_hexDigit('1') == 1);
    TCHECK(rt_hexDigit('a') == 10);
    TCHECK(rt_hexDigit('f') == 15);
    TCHECK(rt_hexDigit('A') == 10);
    TCHECK(rt_hexDigit('F') == 15);
    TCHECK(rt_hexDigit('g') == -1);
    TCHECK(rt_hexDigit(  0) == -1);

    str_t p;
    str_t s1 = "12345";
    p = s1;
    TCHECK(rt_readDec(&p) == 12345);
    TCHECK(p==s1+5);

    str_t s2 = "12345  ";
    p = s2;
    TCHECK(rt_readDec(&p) == 12345);
    TCHECK(p==s2+5);

    str_t s3 = "x12345  ";
    p = s3;
    TCHECK(rt_readDec(&p) == -1);
    TCHECK(p==s3);


    str_t eui1 = "123456  ";
    p = eui1;
    TCHECK(rt_readEui(&p, 0) == 0x123456);
    TCHECK(p==eui1+6);

    str_t eui2 = "12-34-56-78-9a-bc-de-f0  ";
    p = eui2;
    TCHECK(rt_readEui(&p, 0) == (uL_t)0x123456789abcdef0);
    TCHECK(p==eui2+23);

    str_t eui3 = "12:34:56:78:9a";
    p = eui3;
    TCHECK(rt_readEui(&p, 0) == (uL_t)0x123456789a);
    TCHECK(p==eui3+14);

    str_t eui4 = "12::34  ";
    p = eui4;
    uL_t e4 = rt_readEui(&p, 0);
    TCHECK(e4 == (uL_t)0x0012000000000034);
    TCHECK(p==eui4+6);

    str_t eui5 = "::12:34";
    p = eui5;
    uL_t e5 = rt_readEui(&p, 0);
    TCHECK(e5 == (uL_t)0x0000000000120034);
    TCHECK(p==eui5+7);

    str_t eui6 = "12:34::";
    p = eui6;
    uL_t e6 = rt_readEui(&p, 0);
    TCHECK(e6 == (uL_t)0x0012003400000000);
    TCHECK(p==eui6+7);

    str_t eui7 = "1:2:3:4";
    p = eui7;
    uL_t e7 = rt_readEui(&p, 0);
    TCHECK(e7 == (uL_t)0x0001000200030004);
    TCHECK(p==eui7+7);



    str_t eui20 = "12:::34";
    p = eui20;
    TCHECK(rt_readEui(&p, 0) == 0);
    TCHECK(p==eui20);

    str_t eui21 = "1:2:3:4:5:6:7:8:9:0";
    p = eui21;
    TCHECK(rt_readEui(&p, 0) == 0);
    TCHECK(p==eui21);

    str_t eui22 = ":12:34";
    p = eui22;
    TCHECK(rt_readEui(&p, 0) == 0);
    TCHECK(p==eui22);


    str_t sp1 = "1d2h3m4s5ms---";
    p = sp1;
    TCHECK(rt_readSpan(&p, 1) == ((((((ustime_t)1*24)+2)*60+3)*60+4)*1000+5)*1000);
    TCHECK(p[0]=='-');

    str_t sp2 = "123ms400---";
    p = sp2;
    TCHECK(rt_readSpan(&p, 1) == 123400);
    TCHECK(p[0]=='-');

    str_t sp3 = "123ms400---";
    p = sp3;
    TCHECK(rt_readSpan(&p, 0) == -1);

    str_t sp4 = "ms400---";
    p = sp4;
    TCHECK(rt_readSpan(&p, 0) == -1);
}
