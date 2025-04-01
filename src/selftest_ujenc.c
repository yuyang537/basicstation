/*
 * 模块概述
 * ========
 * 本文件实现了LoRaWAN基站的JSON编码器(ujenc)的自测试用例。这些测试验证了
 * 系统正确生成各种JSON格式数据的能力，包括基本数据类型、数组、对象等结构。
 * JSON编码是基站与服务器通信的基础，确保生成符合规范的JSON数据对于系统间
 * 通信的可靠性至关重要。
 * 
 * 文件功能
 * ========
 * 本文件主要测试以下功能：
 * - 基本数据类型编码（数字、字符串、布尔值、null）
 * - 复合数据结构编码（数组和对象）
 * - 特殊格式数据编码（时间、日期、MAC地址、EUI等）
 * - 嵌套结构的正确生成
 * - 编码过程中的缓冲区管理
 * - 特殊字符的转义处理
 * 
 * 主要测试组件
 * ========
 * 1. 基本编码测试
 *    - 数值编码（整数、浮点数）
 *    - 布尔值和null编码
 *    - 字符串编码和转义处理
 *    - 嵌套数组和对象
 * 
 * 2. 特殊格式编码测试
 *    - 时间戳编码
 *    - 日期编码
 *    - 十六进制数据编码
 *    - MAC地址编码
 *    - EUI格式编码（用于LoRaWAN设备和应用标识）
 *    - IPv6格式ID编码
 * 
 * 3. 结构化编码测试
 *    - 键值对编码
 *    - 对象嵌套编码
 *    - 数组嵌套编码
 *    - 混合结构编码
 * 
 * 4. 特殊处理测试
 *    - 字符串合并功能
 *    - 空值处理
 *    - 简化的多键值编码（KVn函数）
 *    - 缓冲区溢出处理
 * 
 * 测试策略
 * ========
 * 测试采用构建-验证模式，先构建各种JSON结构，然后与预期的字符串进行比较，
 * 确保输出与JSON规范一致，并且包含正确的数据。测试涵盖了常见使用场景和边界情况。
 * 
 * 注意事项
 * ========
 * 1. 测试依赖
 *    - 测试依赖uj模块中的缓冲区和编码器实现
 *    - 使用selftests框架
 *    - 需要正确的内存管理（避免内存泄漏）
 * 
 * 2. 特殊格式
 *    - 系统定义了多种特殊数据格式的编码方式
 *    - 时间和日期格式需要特别注意
 *    - EUI和MAC地址格式对LoRaWAN通信至关重要
 * 
 * 3. 缓冲区管理
 *    - 测试包括对缓冲区溢出的处理
 *    - 验证编码函数的安全性和健壮性
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
#include "uj.h"

#define BUFSZ (2*1024)


static void test_simple_values() {
    char* jsonbuf = rt_mallocN(char, BUFSZ);

    ujbuf_t B = { .buf = jsonbuf, .bufsize = BUFSZ, .pos = 0 };
    const char* T;

    for( int k=1; k<=3; k++ ) {
        B.pos = 0;
        for( int i=0; i<k; i++ )
            uj_encOpen(&B, '[');
        for( int i=0; i<k; i++ )
            uj_encClose(&B, ']');
        TCHECK(xeos(&B) == 1 );
        T = "[[[]]]";
        TCHECK(strncmp(T+3-k, B.buf, 2*k) == 0 && B.pos == 2*k);
    }

    B.pos = 0;
    uj_encOpen(&B, '[');
    uj_encNull(&B);
    uj_encBool(&B,  0);
    uj_encBool(&B,  1);
    uj_encInt (&B, -1);
    uj_encUint(&B,  1);
    uj_encNum (&B,  1.5);
    uj_encTime(&B, 21.5);
    uj_encDate(&B, 1451649600L*1000000LL);
    uj_encStr (&B,  "-\"\\\b\f\n\r\t\x01\x02\xc2\xbf-");
    uj_encHex (&B,  (const u1_t*)"ABC", 3);
    uj_encMac (&B,  0x1A2B3C4DA1B2C3D4);
    uj_encEui (&B,  0x91A2B3C4D5E6F708);
    uj_encId6 (&B,  0x0000000000000000);
    uj_encId6 (&B,  0x0000000000000001);
    uj_encId6 (&B,  0x0000000000020001);
    uj_encId6 (&B,  0x0004000000000001);
    uj_encId6 (&B,  0x0004000300000000);
    uj_encId6 (&B,  0x0004000000000000);
    uj_encId6 (&B,  0x0000000300020000);
    uj_encClose(&B, ']');
    TCHECK(xeos(&B) == 1 );
    T = "[null,false,true,-1,1,1.5,21.500000,\"2016-01-01 12:00:00\","
        "\"-\\\"\\\\\\b\\f\\n\\r\\t\\u0001\\u0002\xc2\xbf-\","
        "\"414243\","
        "\"3C:4D:A1:B2:C3:D4\","
        "\"91-A2-B3-C4-D5-E6-F7-08\","
        "\"::0\",\"::1\",\"::2:1\",\"4::1\",\"4:3::\",\"4::\",\"0:3:2:0\""
        "]";
    TCHECK(strcmp(T, B.buf) == 0);

    B.pos = 0;
    uj_encOpen (&B, '{');
    uj_encKey  (&B, "msgtype");
    uj_encOpen (&B, '[');
    uj_encStr  (&B, "A" ); uj_mergeStr(&B);
    uj_encStr  (&B, ""  ); uj_mergeStr(&B);
    uj_encStr  (&B, "BC"); uj_mergeStr(&B);
    uj_encStr  (&B, "DE");
    uj_encClose(&B, ']');
    uj_encKey  (&B, "data");
    uj_encOpen (&B, '[');
    uj_encStr  (&B,  NULL);
    uj_encHex  (&B,  NULL, 0);
    uj_encClose(&B, ']');
    uj_encClose(&B, '}');
    TCHECK(xeos(&B) == 1 );
    T = "{\"msgtype\":[\"ABCDE\"],\"data\":[null,null]}";
    TCHECK(strcmp(T, B.buf) == 0);


    B.pos = 0;
    uj_encOpen (&B, '{');
    uj_encKV(&B, "A", 'b', 1);
    uj_encKVn(&B,
              "B", 'i',       -1,
              "C", 'I', (sL_t)-1,
              "D", 'u',        1U,
              "E", 'U', (uL_t) 1,
              "G1",'g',  1.25,
              "G2",'T', 21.25,
              "D", 'D', 1451649600L*1000000LL,
              "F", 's', "abc",
              "G", 'H', 3, (const u1_t*)"ABC",
              "M", 'M', 0x1A2B3C4DA1B2C3D4,
              "H", 'E', 0x91A2B3C4D5E6F708,
              "I", '6', (uL_t)0xB000A,
              NULL);
    uj_encClose(&B, '}');
    TCHECK(xeos(&B) == 1 );
    T = "{\"A\":true,\"B\":-1,\"C\":-1,\"D\":1,\"E\":1,\"G1\":1.25,\"G2\":21.250000,\"D\":\"2016-01-01 12:00:00\","
        "\"F\":\"abc\",\"G\":\"414243\",\"M\":\"3C:4D:A1:B2:C3:D4\",\"H\":\"91-A2-B3-C4-D5-E6-F7-08\",\"I\":\"::b:a\"}";
    TCHECK(strcmp(T, B.buf) == 0);

    B.pos = 0;
    uj_encOpen (&B, '{');
    uj_encKVn(&B,
              "A", '{', "B", 'I', (sL_t)-1,
              /**/      "C", '[', 's', "a1",
              /**/                's', "a2",
              /**/                ']',
              /**/      "D", 'u', 1U,
              /**/      "}",
              "D", 'b', 0,
              NULL);
    uj_encClose(&B, '}');
    TCHECK(xeos(&B) == 1 );
    T = "{\"A\":{\"B\":-1,\"C\":[\"a1\",\"a2\"],\"D\":1},\"D\":false}";
    TCHECK(strcmp(T, B.buf) == 0);

    B.pos = 0;
    uj_encOpen (&B, '{');
    uj_encKVn(&B, "X", 0, NULL, NULL);
    uj_encClose(&B, '}');
    TCHECK(xeos(&B) == 1 );
    TCHECK(strcmp("{\"X\":}", B.buf) == 0);

    B.pos = 0;
    uj_encOpen (&B, '{');
    uj_encKVn(&B, "X", '[', 0, NULL, NULL);
    TCHECK(xeos(&B) == 1 );
    TCHECK(strcmp("{\"X\":[", B.buf) == 0);

    // xprintf
    {
        B.pos = 0;
        uj_encInt(&B, 1234567);
        xprintf(&B, "abc%d", 123);
    }

    // Buffer overflow
    {
        B.pos = 0;
        B.bufsize = 2;
        uj_encInt(&B, 1234567);
        TCHECK(0 == xeos(&B));
        TCHECK(strcmp("1", B.buf) == 0);
        xprintf(&B, "abc"); // buffer overflow
    }

    free(jsonbuf);
}


void selftest_ujenc () {
    test_simple_values();
}
