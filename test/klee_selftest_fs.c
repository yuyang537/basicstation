/*
 * 文件概述
 * ========
 * 本文件实现了使用KLEE符号执行引擎对LoRaWAN基站文件系统(fs)的全面测试。
 * KLEE是一个符号执行工具，能够自动探索程序的执行路径，发现潜在bug。
 * 通过符号执行，可以验证文件系统在各种边界条件下的行为。
 * 
 * 测试策略
 * ========
 * 1. 使用符号输入：通过KLEE生成具有符号属性的输入数据
 * 2. 边界条件测试：探索文件系统操作的边界情况
 * 3. 错误路径验证：验证错误处理机制的正确性
 * 4. 完整性检查：验证文件系统操作的完整性和一致性
 */

#include <klee/klee.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#include <assert.h>
#include "fs.h"
#include "rt.h"

/*
 * 函数: test_path_normalization
 * 功能: 测试路径规范化功能
 * 说明: 
 *   使用KLEE符号执行测试路径规范化功能在各种输入下的行为。
 *   符号路径可以覆盖各种边界情况，如超长路径、特殊字符等。
 */
void test_path_normalization() {
    char path[64];       // 输入路径
    char normalized[64]; // 规范化后的路径
    int size;           // 规范化结果大小
    
    // 使用KLEE创建符号路径
    klee_make_symbolic(path, sizeof(path), "symbolic_path");
    
    // 添加约束，确保路径是有效字符串
    klee_assume(path[sizeof(path)-1] == '\0');
    
    // 测试路径规范化函数
    size = fs_fnNormalize(path, normalized, sizeof(normalized));
    
    // 验证结果
    if (size > 0) {
        // 验证规范化路径以根目录开始
        if (normalized[0] != '/') {
            klee_assert(0 && "规范化路径必须以'/'开始");
        }
        
        // 验证返回值与字符串长度匹配
        if (size != strlen(normalized) + 1) {
            klee_assert(0 && "返回大小应为字符串长度+1");
        }
    } else {
        // 如果规范化失败，原因可能是路径太长或格式错误
        // 对于这些情况，size应为0
        klee_assert(size == 0);
    }
}

/*
 * 函数: test_file_operations
 * 功能: 测试基本文件操作
 * 说明:
 *   测试文件创建、打开、写入、读取和关闭操作。
 *   使用符号数据作为文件内容，覆盖各种数据情况。
 */
void test_file_operations() {
    int fd;              // 文件描述符
    char data[128];      // 测试数据
    char readback[128];  // 读回数据
    int data_size;       // 数据大小
    
    // 创建符号数据和大小
    klee_make_symbolic(data, sizeof(data), "file_data");
    klee_make_symbolic(&data_size, sizeof(data_size), "data_size");
    
    // 添加约束：数据大小在合理范围内
    klee_assume(data_size > 0 && data_size <= sizeof(data));
    
    // 测试文件创建和写入
    fd = fs_open("test.dat", O_CREAT|O_TRUNC|O_WRONLY, 0644);
    if (fd < 0) {
        klee_assert(0 && "文件创建应该成功");
    }
    
    // 写入数据
    int written = fs_write(fd, data, data_size);
    klee_assert(written == data_size && "写入应该完成");
    
    // 关闭文件
    int close_result = fs_close(fd);
    klee_assert(close_result == 0 && "关闭文件应该成功");
    
    // 重新打开文件读取数据
    fd = fs_open("test.dat", O_RDONLY);
    klee_assert(fd >= 0 && "打开文件应该成功");
    
    // 读取数据
    int read_size = fs_read(fd, readback, sizeof(readback));
    klee_assert(read_size == data_size && "应该读取相同大小的数据");
    
    // 验证数据完整性
    for (int i = 0; i < data_size; i++) {
        klee_assert(data[i] == readback[i] && "读取数据应与写入数据一致");
    }
    
    // 关闭文件
    close_result = fs_close(fd);
    klee_assert(close_result == 0 && "关闭文件应该成功");
}

/*
 * 函数: test_file_system_limits
 * 功能: 测试文件系统边界条件
 * 说明:
 *   测试文件系统的各种边界情况，如：
 *   - 最大文件数
 *   - 文件大小限制
 *   - 路径长度限制
 *   - 错误处理
 */
void test_file_system_limits() {
    int fds[100];  // 文件描述符数组
    int i = 0;
    
    // 测试文件描述符限制
    for (i = 0; i < 100; i++) {
        char filename[32];
        sprintf(filename, "test_file_%d.dat", i);
        
        fds[i] = fs_open(filename, O_CREAT|O_TRUNC|O_WRONLY, 0644);
        if (fds[i] < 0) {
            // 达到文件描述符限制
            klee_assert(errno == ENFILE && "应该是描述符耗尽错误");
            break;
        }
    }
    
    // 关闭所有打开的文件
    for (int j = 0; j < i; j++) {
        fs_close(fds[j]);
    }
    
    // 测试超长文件名
    char long_filename[1024];
    memset(long_filename, 'a', sizeof(long_filename)-1);
    long_filename[sizeof(long_filename)-1] = '\0';
    
    int fd = fs_open(long_filename, O_CREAT|O_TRUNC|O_WRONLY, 0644);
    if (fd >= 0) {
        // 如果成功打开，确保能正常关闭
        fs_close(fd);
    } else {
        // 如果打开失败，应该是由于文件名太长
        klee_assert(errno == ENAMETOOLONG || errno == EINVAL);
    }
}

/*
 * 函数: test_file_system_consistency
 * 功能: 测试文件系统一致性
 * 说明:
 *   测试文件系统操作后的一致性，包括：
 *   - 文件内容一致性
 *   - 文件系统状态一致性
 *   - 文件元数据一致性
 */
void test_file_system_consistency() {
    // 创建文件
    int fd = fs_open("consistency_test.dat", O_CREAT|O_TRUNC|O_WRONLY, 0644);
    klee_assert(fd >= 0);
    
    // 写入数据
    char test_data[] = "KLEE测试数据";
    int data_len = strlen(test_data);
    int written = fs_write(fd, test_data, data_len);
    klee_assert(written == data_len);
    
    // 关闭文件
    fs_close(fd);
    
    // 获取文件状态
    struct stat st;
    int stat_result = fs_stat("consistency_test.dat", &st);
    klee_assert(stat_result == 0);
    klee_assert(st.st_size == data_len);
    
    // 检查文件系统一致性
    int check_result = fs_ck();
    klee_assert(check_result == 1);
}

/*
 * 函数: test_garbage_collection
 * 功能: 测试垃圾回收机制
 * 说明:
 *   测试文件系统的垃圾回收功能，包括：
 *   - 普通垃圾回收
 *   - 紧急垃圾回收
 *   - 删除文件后的空间回收
 */
void test_garbage_collection() {
    // 记录初始状态
    fsinfo_t info_before;
    fs_info(&info_before);
    
    // 创建和删除多个文件
    for (int i = 0; i < 5; i++) {
        char filename[32];
        sprintf(filename, "gc_test_%d.dat", i);
        
        // 创建文件
        int fd = fs_open(filename, O_CREAT|O_TRUNC|O_WRONLY, 0644);
        klee_assert(fd >= 0);
        
        // 写入一些数据
        char data[100];
        memset(data, 'A' + i, sizeof(data));
        fs_write(fd, data, sizeof(data));
        fs_close(fd);
        
        // 删除一些文件
        if (i % 2 == 0) {
            int unlink_result = fs_unlink(filename);
            klee_assert(unlink_result == 0);
        }
    }
    
    // 执行垃圾回收
    fs_gc(0);  // 普通垃圾回收
    
    // 检查垃圾回收后的状态
    fsinfo_t info_after;
    fs_info(&info_after);
    
    // 验证垃圾回收有效
    klee_assert(info_after.gcCycles > info_before.gcCycles);
    
    // 检查文件系统一致性
    int check_result = fs_ck();
    klee_assert(check_result == 1);
}

/*
 * 函数: main
 * 功能: 主测试入口
 * 说明:
 *   初始化文件系统并执行各项测试
 */
int main() {
    // 初始化文件系统
    u4_t key[4] = {0x71593cbf, 0x81db1a48, 0x22fc47fe, 0xe8cf23ea};
    fs_erase();
    fs_ini(key);
    
    // 执行各项测试
    test_path_normalization();
    test_file_operations();
    test_file_system_limits();
    test_file_system_consistency();
    test_garbage_collection();
    
    return 0;
} 