/*
 * KLEE符号执行测试 - 运行时系统安全属性验证
 * ======================================
 * 
 * 本测试文件用于验证运行时系统模块的关键安全属性：
 * 1. 内存管理安全
 * 2. 定时器操作安全
 * 3. 时间处理安全
 * 4. 字符串操作安全
 * 5. 数据解析安全
 */

#include <klee/klee.h>
#include <stdio.h>
#include <string.h>
#include "rt.h"

// 测试内存分配和释放
void test_rt_memory() {
    int size;
    char* str;
    
    // 符号化输入
    klee_make_symbolic(&size, sizeof(size), "alloc_size");
    klee_assume(size > 0 && size <= 1024); // 限制分配大小
    
    // 测试内存分配
    void* ptr = _rt_malloc(size, 0);
    klee_assert(ptr != NULL); // 分配应该成功
    
    // 测试字符串复制
    str = rt_strdup("test");
    klee_assert(str != NULL);
    klee_assert(strcmp(str, "test") == 0);
    
    // 清理
    _rt_free_d(ptr, __FILE__, __LINE__);
    _rt_free_d(str, __FILE__, __LINE__);
}

// 测试定时器操作
void test_rt_timer() {
    tmr_t timer;
    ustime_t deadline;
    
    // 符号化输入
    klee_make_symbolic(&deadline, sizeof(deadline), "timer_deadline");
    klee_assume(deadline > rt_getTime()); // 确保截止时间在未来
    
    // 初始化定时器
    rt_iniTimer(&timer, NULL);
    klee_assert(timer.next == TMR_NIL);
    klee_assert(timer.callback == NULL);
    
    // 设置定时器
    rt_setTimer(&timer, deadline);
    klee_assert(timer.deadline == deadline);
    
    // 清除定时器
    rt_clrTimer(&timer);
    klee_assert(timer.next == TMR_NIL);
}

// 测试时间处理
void test_rt_time() {
    ustime_t time;
    struct datetime dt;
    
    // 符号化输入
    klee_make_symbolic(&time, sizeof(time), "time_value");
    klee_assume(time >= 0); // 确保时间非负
    
    // 测试时间转换
    dt = rt_datetime(time);
    klee_assert(dt.year >= 1970); // 年份应该在1970年后
    klee_assert(dt.month >= 1 && dt.month <= 12);
    klee_assert(dt.day >= 1 && dt.day <= 31);
    klee_assert(dt.hour >= 0 && dt.hour <= 23);
    klee_assert(dt.minute >= 0 && dt.minute <= 59);
    klee_assert(dt.second >= 0 && dt.second <= 59);
    klee_assert(dt.usec >= 0 && dt.usec <= 999999);
}

// 测试字符串操作
void test_rt_string() {
    char input[256];
    char* output;
    int n;
    
    // 符号化输入
    klee_make_symbolic(input, sizeof(input), "input_string");
    klee_make_symbolic(&n, sizeof(n), "copy_length");
    klee_assume(input[255] == '\0');
    klee_assume(n > 0 && n <= 255);
    
    // 测试字符串复制
    output = rt_strdupn(input, n);
    klee_assert(output != NULL);
    klee_assert(strncmp(input, output, n) == 0);
    
    // 清理
    _rt_free_d(output, __FILE__, __LINE__);
}

// 测试数据解析
void test_rt_parse() {
    char input[256];
    sL_t value;
    uL_t eui;
    
    // 符号化输入
    klee_make_symbolic(input, sizeof(input), "parse_input");
    klee_assume(input[255] == '\0');
    
    // 测试十进制解析
    value = rt_readDec(&input);
    klee_assert(value >= 0); // 确保解析结果有效
    
    // 测试EUI解析
    eui = rt_readEui(&input, 8);
    klee_assert(eui >= 0); // 确保EUI值有效
}

// 主测试函数
int main() {
    // 运行各个测试用例
    test_rt_memory();
    test_rt_timer();
    test_rt_time();
    test_rt_string();
    test_rt_parse();
    
    return 0;
} 