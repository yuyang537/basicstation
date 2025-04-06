#include <klee/klee.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>

// 首先包含基础类型定义
#include "../../../src/rt.h"
// 然后包含其他依赖
#include "../../../src/s2conf.h"
#include "../../../src/kwcrc.h"
#include "../../../src/uj.h"
// 最后包含fs.h
#include "../../../src/fs.h"

// 符号化输入缓冲区的最大大小
#define MAX_BUF_SIZE 1024
#define MAX_PATH_LEN 256

// 测试文件系统初始化和安全属性
void test_fs_initialization() {
    u4_t key[4];
    // 使用符号化的密钥
    klee_make_symbolic(key, sizeof(key), "fs_key");
    
    // 测试文件系统初始化
    int result = fs_ini(key);
    
    // 验证初始化结果
    assert(result >= 0 && "文件系统初始化失败");
    
    // 获取文件系统信息并验证基本属性
    fsinfo_t info;
    fs_info(&info);
    assert(info.pagecnt > 0 && "页面数量无效");
    assert(info.pagesize > 0 && "页面大小无效");
    assert(info.fbase != 0 && "基地址无效");
}

// 测试文件操作的安全性
void test_file_operations() {
    char filename[MAX_PATH_LEN];
    char buffer[MAX_BUF_SIZE];
    int size;
    
    // 创建符号化的文件名和数据
    klee_make_symbolic(filename, sizeof(filename), "filename");
    klee_make_symbolic(buffer, sizeof(buffer), "buffer");
    klee_make_symbolic(&size, sizeof(size), "size");
    
    // 添加约束条件
    klee_assume(size >= 0 && size <= MAX_BUF_SIZE);
    klee_assume(filename[MAX_PATH_LEN-1] == '\0');
    
    // 测试文件打开操作
    int fd = fs_open(filename, 1); // 1 表示写入模式
    if (fd >= 0) {
        // 验证文件描述符的有效性
        assert(fd >= 0 && fd < 256 && "文件描述符超出有效范围");
        
        // 测试写入操作
        int written = fs_write(fd, buffer, size);
        if (written >= 0) {
            assert(written <= size && "写入数据量超过请求大小");
        }
        
        // 测试读取操作
        char read_buffer[MAX_BUF_SIZE];
        int read = fs_read(fd, read_buffer, size);
        if (read >= 0) {
            assert(read <= size && "读取数据量超过请求大小");
        }
        
        // 关闭文件
        int close_result = fs_close(fd);
        assert(close_result == 0 && "文件关闭失败");
    }
}

// 测试路径规范化和访问控制
void test_path_security() {
    char path[MAX_PATH_LEN];
    char normalized_path[MAX_PATH_LEN];
    
    // 创建符号化的路径
    klee_make_symbolic(path, sizeof(path), "path");
    klee_assume(path[MAX_PATH_LEN-1] == '\0');
    
    // 测试路径规范化
    int norm_result = fs_fnNormalize(path, normalized_path, MAX_PATH_LEN);
    if (norm_result == 0) {
        // 验证规范化路径不包含特殊序列
        assert(strstr(normalized_path, "..") == NULL && "规范化路径包含父目录引用");
        assert(normalized_path[0] != '/' && "规范化路径为绝对路径");
    }
    
    // 测试访问控制
    int access_result = fs_access(normalized_path, 0); // 0表示检查存在性
    // 不需要断言具体的返回值，因为文件可能存在也可能不存在
}

// 测试资源管理和错误恢复
void test_resource_management() {
    // 测试垃圾回收
    fs_gc(0); // 非紧急模式
    
    // 验证文件系统状态
    int check_result = fs_ck();
    assert(check_result >= 0 && "文件系统一致性检查失败");
    
    // 获取并验证资源使用情况
    fsinfo_t info;
    fs_info(&info);
    assert(info.used <= (info.pagecnt * info.pagesize) && "使用空间超过总空间");
    assert(info.free <= (info.pagecnt * info.pagesize) && "可用空间超过总空间");
}

int main() {
    // 运行所有测试用例
    test_fs_initialization();
    test_file_operations();
    test_path_security();
    test_resource_management();
    return 0;
} 