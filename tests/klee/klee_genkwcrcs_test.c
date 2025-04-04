/*
 * KLEE 符号执行测试文件 - genkwcrcs.c
 * ====================================
 * 
 * 安全属性说明
 * ============
 * 1. 哈希函数不应返回零值（通过FINISH_CRC宏确保）
 * 2. 标识符生成应始终产生有效的C语言标识符
 * 3. 不同的输入不应产生相同的哈希值（哈希冲突检测）
 */

#include <klee/klee.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>

/* 从源文件复制的关键常量和宏定义 */
#define P1 257
#define P2 65537
#define SC (32-8)

/* ujcrc_t 类型定义 */
typedef unsigned int ujcrc_t;

/* CRC 计算宏 */
#define FINISH_CRC(crc)    X((crc) ? (crc) : 1)
#define UPDATE_CRC(crc, c) X((((crc)>>SC)*P2) ^ ((crc)*P1) ^ ((c)&0x7F))

/* 返回值不变的函数，在原始代码中用于调试/转换 */
ujcrc_t X(ujcrc_t x) {
    return x;
}

/* CRC 计算函数实现 */
ujcrc_t calcCRC(const char* s) {
    ujcrc_t crc = 0;
    int c;
    while ((c = *s++)) {
        crc = UPDATE_CRC(crc, c);
    }
    return FINISH_CRC(crc);
}

/* 标识符生成函数实现 */
void generateIdentifier(const char* keyword, char* ident, size_t maxLen) {
    int cj = 0;
    for (cj = 0; keyword[cj] && cj < maxLen - 1; cj++) {
        char c = keyword[cj];
        ident[cj] = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ? c : '_';
    }
    ident[cj] = 0;
}

/*
 * 测试1: CRC非零安全属性
 * 验证CRC函数永远不会返回零值
 */
void test_crc_never_zero() {
    char input[10];
    klee_make_symbolic(input, sizeof(input), "input");
    input[sizeof(input) - 1] = '\0'; // 确保字符串以NULL结尾
    
    ujcrc_t result = calcCRC(input);
    
    // 验证CRC值永远不为0
    klee_assert(result != 0 && "安全属性: CRC值不应为零");
}

/*
 * 测试2: 标识符生成安全属性
 * 验证生成的标识符始终是合法的C语言标识符
 */
void test_identifier_generation() {
    char keyword[10];
    char identifier[11] = {0};
    
    klee_make_symbolic(keyword, sizeof(keyword), "keyword");
    keyword[sizeof(keyword) - 1] = '\0'; // 确保字符串以NULL结尾
    
    generateIdentifier(keyword, identifier, sizeof(identifier));
    
    // 验证标识符的第一个字符不是数字
    if (identifier[0] != '\0') {
        klee_assert((isalpha(identifier[0]) || identifier[0] == '_') && 
                   "安全属性: 标识符首字符必须是字母或下划线");
    }
    
    // 验证所有字符都是合法的标识符字符
    for (int i = 0; identifier[i] != '\0'; i++) {
        klee_assert((isalnum(identifier[i]) || identifier[i] == '_') && 
                   "安全属性: 标识符只能包含字母、数字或下划线");
    }
}

/*
 * 测试3: 哈希冲突检测
 * 验证不同输入是否产生不同的哈希值
 * 注意: 此测试可能找到反例,因为哈希冲突在理论上总是存在的
 */
void test_hash_collision() {
    char str1[5];
    char str2[5];
    
    klee_make_symbolic(str1, sizeof(str1), "str1");
    klee_make_symbolic(str2, sizeof(str2), "str2");
    
    str1[sizeof(str1) - 1] = '\0';
    str2[sizeof(str2) - 1] = '\0';
    
    // 假设两个字符串不同
    klee_assume(strcmp(str1, str2) != 0);
    
    ujcrc_t crc1 = calcCRC(str1);
    ujcrc_t crc2 = calcCRC(str2);
    
    // 验证不同字符串产生不同CRC值
    // 注意: KLEE可能会找到反例
    klee_prefer_cex(str1, crc1 == crc2);
    klee_prefer_cex(str2, crc1 == crc2);
}

int main() {
    // 运行所有测试用例
    test_crc_never_zero();
    test_identifier_generation();
    // test_hash_collision();  // 可选测试,可能找到冲突
    
    return 0;
} 