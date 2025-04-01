/*
 * 模块概述
 * ========
 * 本文件实现了LoRaWAN基站的CRC32计算工具。CRC32是一种循环冗余校验
 * 算法，用于检测数据在存储或传输过程中的错误。该工具可以计算文件
 * 或输入流的CRC32校验和，也可以生成CRC表供其他模块使用。
 * 
 * 文件功能
 * ========
 * 本工具主要实现以下功能：
 * - 标准CRC32校验和计算
 * - CRC查找表生成
 * - 单个或多个文件的批量处理
 * - 支持从标准输入读取数据
 * 
 * 主要组件
 * ========
 * 1. CRC表生成系统
 *    - 基于多项式0xEDB88320
 *    - 预计算256个表项用于加速计算
 *    - 表格化输出功能
 * 
 * 2. CRC计算引擎
 *    - 标准CRC32算法实现
 *    - 高效的查表计算方法
 *    - 支持任意大小的输入数据
 * 
 * 3. 文件处理系统
 *    - 多文件批量处理
 *    - 错误处理和报告
 *    - 连续计算和结果累积
 * 
 * 算法细节
 * ========
 * 1. CRC32计算
 *    - 使用标准多项式0xEDB88320
 *    - 初始值为0xFFFFFFFF
 *    - 字节逐个处理，利用查表法加速
 *    - 最终结果与0xFFFFFFFF异或
 * 
 * 2. 表生成
 *    - 对0-255的每个字节值预计算CRC值
 *    - 生成256项的查找表
 *    - 通过位移和异或实现高效计算
 * 
 * 使用方法
 * ========
 * 1. 生成CRC表
 *    - 运行 crc32 TABLE 命令
 *    - 生成的表可复制到其他代码中使用
 * 
 * 2. 计算文件CRC
 *    - 运行 crc32 [文件列表] 命令
 *    - 每个文件对应一个CRC值输出
 *    - 多文件时会输出所有文件的累积CRC
 * 
 * 3. 从标准输入计算
 *    - 运行 crc32 - 命令
 *    - 从标准输入读取数据并计算CRC
 * 
 * 注意事项
 * ========
 * 1. 构建要求
 *    - 本程序仅在CFG_prog_crc32定义时编译
 *    - 编译需要支持C99标准
 * 
 * 2. 性能考虑
 *    - 使用8KB缓冲区进行文件读取
 *    - 查表法大幅提高计算效率
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

#if defined(CFG_prog_crc32)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>


static uint32_t crc_table[256];

static void init_crc_table () {
    const uint32_t POLYNOMIAL = 0xEDB88320;
    uint32_t remainder;
    unsigned char b = 0;
    do {
        remainder = b;
        for( int bit = 8; bit > 0; --bit )
            remainder = (remainder >> 1) ^ (POLYNOMIAL * (remainder & 1));
        crc_table[b] = remainder;
    } while( ++b );
}


uint32_t crc32 (uint32_t crc, const void *buf, size_t size) {
    const uint8_t *p = (uint8_t*)buf;

   crc = crc ^ ~0U;
   while( size-- > 0 )
       crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
   return crc ^ ~0U;
}


int main (int argc, char** argv) {
    if( argc<=1 ) {
        fprintf(stderr,
                "usage: crc32 TABLE\n"
                "usage: crc32 {-|file}..\n");
        return 1;
    }
    init_crc_table();

    if( argc == 2 && strcmp(argv[1], "TABLE") == 0 ) {
        printf("static const uint32_t crc_table[256] = {\n");
        for( int i=0; i<256; i+=8 ) {
            printf("    ");
            for( int j=0; j<8; j++ )
                printf("0x%08X,", crc_table[i+j]);
            printf("\n");
        }
        printf("};\n");
        return 0;
    }

    unsigned all = 0;
    for( int i=1; i<argc; i++ ) {
        char* file = argv[i];
        FILE* f;
        if( strcmp(file, "-") == 0 ) {
            f = stdin;
        } else {
            f = fopen(file,"r");
            if( f == NULL ) {
                fprintf(stderr, "Failed to open '%s': %s\n", file, strerror(errno));
                continue;
            }
        }
        char buf[8*1024];
        unsigned crc = 0;
        while(1) {
            int l = fread(buf, 1, sizeof(buf), f);
            crc = crc32(crc, buf, l);
            all = crc32(all, buf, l);
            if( l < sizeof(buf) )
                break;
        }
        if( ferror(f) ) {
            fclose(f);
            fprintf(stderr, "Failed to read '%s': %s\n", file, strerror(errno));
            continue;
        }
        printf("0x%08X %s\n", crc, file[0] == '-' ? "" : file);
        fclose(f);
    }
    if( argc > 2 )
        printf("0x%08X over all files\n", all);
    return 0;
}

#endif //defined(CFG_prog_crc32)
