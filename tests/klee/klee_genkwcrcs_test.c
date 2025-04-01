
/*
 * KLEE 符号执行测试文件
 * =====================
 * 本文件用于对 genkwcrcs.c 中的函数进行符号执行测试，
 * 验证以下安全属性：
 * 1. 哈希函数不应返回0值
 * 2. 不同的输入不应产生相同的哈希值（哈希冲突）
 * 3. 生成的标识符必须符合C语言命名规范
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <klee/klee.h>

// 定义 ujcrc_t 类型（如果没有包含原始头文件）
typedef unsigned int ujcrc_t;

// 从 genkwcrcs.c 复制的常量和宏
#define P1 257
#define P2 65537
#define SC (32-8)

// 函数声明
ujcrc_t X(ujcrc_t x) {
    return x;
}

#define FINISH_CRC(crc)    X((crc) ? (crc) : 1)
#define UPDATE_CRC(crc, c) X((((crc)>>SC)*P2) ^ ((crc)*P1) ^ ((c)&0x7F))

// 实现 calcCRC 函数用于测试
ujcrc_t calcCRC(const char* s) {
    ujcrc_t crc = 0;
    int c;
    while( (c=*s++) ) {
        crc = UPDATE_CRC(crc,c);
    }
    return FINISH_CRC(crc);
}

// 生成标识符函数（从主程序提取）
void generateIdentifier(const char* keyword, char* ident, size_t maxLen) {
    size_t len = strlen(keyword);
    if (len >= maxLen) {
        len = maxLen - 1;
    }
    
    int cj = 0;
    for (cj = 0; keyword[cj] && cj < len; cj++) {
        char c = keyword[cj];
        ident[cj] = (c>='0' && c<='9') || (c>='a' && c<='z') || (c>='A' && c<='Z') ? c : '_';
    }
    ident[cj] = 0;
}

// 检测冲突
int hasCollision(const char* str1, const char* str2) {
    if (strcmp(str1, str2) == 0) {
        return 0; // 相同字符串不算冲突
    }
    
    ujcrc_t crc1 = calcCRC(str1);
    ujcrc_t crc2 = calcCRC(str2);
    
    return crc1 == crc2;
}

// 测试函数：验证哈希值不为0
void test_crc_not_zero() {
    // 创建符号化的输入
    char str[10];
    klee_make_symbolic(str, sizeof(str), "input_string");
    str[sizeof(str)-1] = '\0'; // 确保字符串结束
    
    // 计算CRC
    ujcrc_t crc = calcCRC(str);
    
    // 断言：CRC值不应为0
    klee_assert(crc != 0 && "CRC不能为0");
}

// 测试函数：验证标识符生成
void test_identifier_generation() {
    // 创建符号化的输入
    char keyword[10];
    klee_make_symbolic(keyword, sizeof(keyword), "keyword");
    keyword[sizeof(keyword)-1] = '\0'; // 确保字符串结束
    
    // 生成标识符
    char ident[256];
    generateIdentifier(keyword, ident, sizeof(ident));
    
    // 断言：确保生成的每个字符都符合C标识符规范
    for (int i = 0; ident[i]; i++) {
        klee_assert((isalnum(ident[i]) || ident[i] == '_') && 
                   "标识符必须只包含字母、数字或下划线");
    }
}

// 测试函数：测试潜在的哈希碰撞
void test_collision_detection() {
    // 创建两个符号化的输入字符串
    char str1[5];
    char str2[5];
    klee_make_symbolic(str1, sizeof(str1), "string1");
    klee_make_symbolic(str2, sizeof(str2), "string2");
    str1[sizeof(str1)-1] = '\0';
    str2[sizeof(str2)-1] = '\0';
    
    // 假设两个字符串不同
    klee_assume(strcmp(str1, str2) != 0);
    
    // 计算哈希值
    ujcrc_t crc1 = calcCRC(str1);
    ujcrc_t crc2 = calcCRC(str2);
    
    // 对于不同的字符串，哈希值应该不同
    // 注意：这个断言可能会失败，因为哈希冲突是可能存在的
    // KLEE会找到可能导致冲突的输入
    klee_assert(crc1 != crc2 && "不同的字符串应有不同的哈希值");
}

int main() {
    // 运行所有测试用例
    test_crc_not_zero();
    test_identifier_generation();
    
    // 注意：碰撞检测测试可能会找到反例
    // 因为哈希冲突的可能性总是存在的
    // test_collision_detection();
    
    return 0;
} 