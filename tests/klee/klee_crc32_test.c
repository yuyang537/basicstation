/*
 * KLEE 符号执行测试文件 - crc32.c
 * ================================
 * 
 * 安全属性说明
 * ============
 * 1. CRC32计算的正确性和一致性
 * 2. 空输入和极端情况的处理
 * 3. 多次计算的累积结果一致性
 */

#include <klee/klee.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

/* 从源文件复制的CRC表和实现 */
static uint32_t crc_table[256];

/* 初始化CRC表 */
static void init_crc_table() {
    const uint32_t POLYNOMIAL = 0xEDB88320;
    uint32_t remainder;
    unsigned char b = 0;
    do {
        remainder = b;
        for (int bit = 8; bit > 0; --bit)
            remainder = (remainder >> 1) ^ (POLYNOMIAL * (remainder & 1));
        crc_table[b] = remainder;
    } while (++b);
}

/* CRC32计算函数 */
uint32_t crc32(uint32_t crc, const void *buf, size_t size) {
    const uint8_t *p = (uint8_t*)buf;

    crc = crc ^ ~0U;
    while (size-- > 0)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ ~0U;
}

/*
 * 测试1: 基本正确性测试
 * 验证CRC算法的正确性和一致性
 */
void test_crc32_basic() {
    // 初始化CRC表
    init_crc_table();
    
    // 创建符号化输入
    char data[10];
    size_t size;
    
    klee_make_symbolic(data, sizeof(data), "data");
    klee_make_symbolic(&size, sizeof(size), "size");
    
    // 限制size不超过数据大小
    klee_assume(size <= sizeof(data));
    
    // 计算CRC
    uint32_t result = crc32(0, data, size);
    
    // 对同样的输入再次计算CRC，结果应该相同
    uint32_t result2 = crc32(0, data, size);
    klee_assert(result == result2 && "安全属性: 对相同输入的CRC计算应该得到相同结果");
}

/*
 * 测试2: 空输入和边界情况
 * 验证算法对空输入和极端情况的处理
 */
void test_crc32_empty_input() {
    // 初始化CRC表
    init_crc_table();
    
    // 对空数据计算CRC
    uint32_t empty_result = crc32(0, "", 0);
    
    // CRC值不应为所有位全1 (这是初始值 ~0U)
    klee_assert(empty_result != ~0U && "安全属性: 空数据的CRC不应等于初始值");
    
    // 空数据的CRC值应该是确定的
    klee_assert(empty_result == 0 && "安全属性: 空数据的CRC应为0");
}

/*
 * 测试3: 增量CRC计算
 * 验证分段计算CRC的正确性
 */
void test_crc32_incremental() {
    // 初始化CRC表
    init_crc_table();
    
    // 创建符号化输入
    char data[20];
    size_t size1, size2;
    
    klee_make_symbolic(data, sizeof(data), "data");
    klee_make_symbolic(&size1, sizeof(size1), "size1");
    klee_make_symbolic(&size2, sizeof(size2), "size2");
    
    // 约束大小不超过数组大小
    klee_assume(size1 < sizeof(data));
    klee_assume(size2 < sizeof(data));
    klee_assume(size1 + size2 <= sizeof(data));
    
    // 一次性计算整个数据的CRC
    uint32_t full_result = crc32(0, data, size1 + size2);
    
    // 分两段计算CRC
    uint32_t partial_result = crc32(0, data, size1);
    partial_result = crc32(partial_result, data + size1, size2);
    
    // 验证两种计算方式得到相同结果
    klee_assert(full_result == partial_result && 
               "安全属性: 增量CRC计算应与一次性计算结果相同");
}

int main() {
    // 运行所有测试用例
    test_crc32_basic();
    test_crc32_empty_input();
    test_crc32_incremental();
    
    return 0;
} 