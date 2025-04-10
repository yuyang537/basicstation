#include <klee/klee.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// 项目头文件
#include "../../../src/rt.h"  // 基础类型定义
#include "../../../src/sys.h"  // 系统接口


// 测试用例1: 系统初始化和控制测试
void test_sys_functions() {
    // sys_init 测试
    str_t config = "{}";
    klee_make_symbolic(config, 1024, "config");
    sys_init(config);
    
    // sys_runloop 测试
    int timeout;
    klee_make_symbolic(&timeout, sizeof(timeout), "timeout");
    klee_assume(timeout >= 0);
    sys_runloop(timeout);
    
    // sys_enableWatchdog 测试
    int enable;
    klee_make_symbolic(&enable, sizeof(enable), "enable");
    klee_assume(enable == 0 || enable == 1);
    sys_enableWatchdog(enable);
}

// 测试用例2: 时间单位转换测试
void test_time_conversion() {
    // 秒到微秒转换
    uint32_t seconds;
    klee_make_symbolic(&seconds, sizeof(seconds), "seconds");
    klee_assume(seconds < 1000000); // 防止溢出
    uint64_t microseconds = rt_seconds(seconds);
    assert(microseconds == seconds * 1000000);
    
    // 毫秒到微秒转换
    uint32_t milliseconds;
    klee_make_symbolic(&milliseconds, sizeof(milliseconds), "milliseconds");
    klee_assume(milliseconds < 1000000); // 防止溢出
    uint64_t ms_to_us = rt_millis(milliseconds);
    assert(ms_to_us == milliseconds * 1000);
}

// 测试用例3: 字节序转换测试
void test_endian_conversion() {
    // 准备测试数据
    u1_t bytes[8];
    klee_make_symbolic(bytes, sizeof(bytes), "bytes");
    
    // 2字节小端到主机字节序
    uint16_t le16 = rt_rlsbf2(bytes);
    assert(le16 == ((uint16_t)bytes[1] << 8) | bytes[0]);
    
    // 2字节大端到主机字节序
    uint16_t be16 = rt_rmsbf2(bytes);
    assert(be16 == ((uint16_t)bytes[0] << 8) | bytes[1]);
    
    // 4字节小端到主机字节序
    uint32_t le32 = rt_rlsbf4(bytes);
    assert(le32 == ((uint32_t)bytes[3] << 24) | ((uint32_t)bytes[2] << 16) | 
                   ((uint32_t)bytes[1] << 8) | bytes[0]);
    
    // 8字节小端到主机字节序
    uint64_t le64 = rt_rlsbf8(bytes);
    assert(le64 == ((uint64_t)bytes[7] << 56) | ((uint64_t)bytes[6] << 48) | 
                   ((uint64_t)bytes[5] << 40) | ((uint64_t)bytes[4] << 32) |
                   ((uint64_t)bytes[3] << 24) | ((uint64_t)bytes[2] << 16) | 
                   ((uint64_t)bytes[1] << 8) | bytes[0]);
}

// 测试用例4: 字符解析测试
void test_char_parsing() {
    // 十六进制字符到数值转换
    char hex_char;
    klee_make_symbolic(&hex_char, sizeof(hex_char), "hex_char");
    int value = rt_hexDigit(hex_char);
    
    // 测试大小写十六进制字符
    if (hex_char >= 'A' && hex_char <= 'F') {
        assert(value == (hex_char - 'A' + 10));
    } else if (hex_char >= 'a' && hex_char <= 'f') {
        assert(value == (hex_char - 'a' + 10));
    } else if (hex_char >= '0' && hex_char <= '9') {
        assert(value == (hex_char - '0'));
    } else {
        assert(value == -1);
    }
}

// 测试用例5: 数字解析测试
void test_number_parsing() {
    char number_str[32];
    klee_make_symbolic(number_str, sizeof(number_str), "number_str");
    number_str[31] = '\0'; // 确保字符串终止
    
    str_t p = number_str;
    int value = rt_readDec(&p);
    
    // 验证解析结果
    if (value != -1) {
        assert(p >= number_str && p <= number_str + 31);
    }
}

// 测试用例6: EUI解析测试
void test_eui_parsing() {
    char eui_str[32];
    klee_make_symbolic(eui_str, sizeof(eui_str), "eui_str");
    eui_str[31] = '\0'; // 确保字符串终止
    
    str_t p = eui_str;
    uint64_t eui = rt_readEui(&p, 0);
    
    // 验证解析结果
    if (eui != 0) {
        assert(p > eui_str && p <= eui_str + 31);
    }
}

// 测试用例7: 时间跨度解析测试
void test_timespan_parsing() {
    char timespan_str[32];
    klee_make_symbolic(timespan_str, sizeof(timespan_str), "timespan_str");
    timespan_str[31] = '\0'; // 确保字符串终止
    
    str_t p = timespan_str;
    int64_t microseconds = rt_readSpan(&p, 1); // 允许无单位
    
    // 验证解析结果
    if (microseconds >= 0) {
        assert(p > timespan_str && p <= timespan_str + 31);
    }
}

// 主测试函数
int main() {
    // 运行所有测试用例
    test_sys_functions();
    test_time_conversion();
    test_endian_conversion();
    test_char_parsing();
    test_number_parsing();
    test_eui_parsing();
    test_timespan_parsing();
    
    return 0;
} 