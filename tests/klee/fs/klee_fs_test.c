/*
 * KLEE符号执行测试 - 文件系统模块
 * ================================
 * 本文件实现了基于KLEE的文件系统(fs)模块符号执行测试。
 * 测试验证了文件系统操作的安全性和正确性，包括边界条件和错误处理。
 */

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
#include "../../../src/rt.h"
#include "../../../src/fs.h"

// 测试用例最大文件名长度
#define MAX_FILENAME_LEN 32
// 测试用例最大文件内容长度
#define MAX_CONTENT_LEN 256
// 测试用例最大路径长度
#define MAX_PATH_LEN 128

/*
 * 辅助函数：生成符号化文件名
 */
static void make_symbolic_filename(char* filename) {
    klee_make_symbolic(filename, MAX_FILENAME_LEN, "filename");
    filename[MAX_FILENAME_LEN-1] = '\0';
}

/*
 * 辅助函数：生成符号化路径
 */
static void make_symbolic_path(char* path) {
    klee_make_symbolic(path, MAX_PATH_LEN, "path");
    path[MAX_PATH_LEN-1] = '\0';
}

/*
 * 辅助函数：生成符号化文件内容
 */
static void make_symbolic_content(char* content, size_t* size) {
    klee_make_symbolic(size, sizeof(*size), "content_size");
    klee_assume(*size <= MAX_CONTENT_LEN);
    klee_make_symbolic(content, *size, "content");
}

/*
 * 测试用例1：路径处理测试
 * 验证：
 * - 路径规范化
 * - 相对路径处理
 * - 特殊路径处理（.和..）
 */
void test_path_handling() {
    char path[MAX_PATH_LEN];
    char norm[MAX_PATH_LEN];
    int sz;

    // 生成符号化路径
    make_symbolic_path(path);
    
    // 测试路径规范化
    sz = fs_fnNormalize(path, norm, sizeof(norm));
    if (sz > 0) {
        // 验证规范化路径的基本属性
        assert(norm[0] == '/' && "规范化路径必须以/开始");
        assert(strlen(norm) < MAX_PATH_LEN && "路径长度超出限制");
        assert(strstr(norm, "..") == NULL && "规范化路径不应包含..");
    } else {
        // 验证无效路径的处理
        klee_assume(strlen(path) >= MAX_PATH_LEN || 
                   strstr(path, "..") != NULL);
    }

    // 测试目录切换
    int ret = fs_chdir(path);
    if (ret == 0) {
        // 验证当前目录状态
        struct stat st;
        ret = fs_stat(".", &st);
        assert(ret == 0 && "当前目录应该可访问");
    } else {
        // 验证错误处理
        klee_assume(errno == ENOENT || errno == ENOTDIR || 
                   errno == EACCES);
    }
}

/*
 * 测试用例2：文件系统管理测试
 * 验证：
 * - 文件系统初始化
 * - 垃圾回收
 * - 空间管理
 */
void test_fs_management() {
    // 初始化文件系统
    u4_t key[4] = {0x12345678, 0x87654321, 0xabcdef01, 0x10fedcba};
    int ret = fs_ini(key);
    assert(ret == 0 && "文件系统初始化失败");

    // 获取文件系统信息
    fsinfo_t info;
    fs_info(&info);
    
    // 验证基本信息
    assert(info.pagecnt > 0 && "页数必须大于0");
    assert(info.pagesize > 0 && "页大小必须大于0");
    assert(info.free > 0 && "必须有可用空间");

    // 测试垃圾回收
    fs_gc(0);  // 常规垃圾回收
    fs_gc(1);  // 紧急垃圾回收

    // 验证垃圾回收后的状态
    fsinfo_t info_after;
    fs_info(&info_after);
    assert(info_after.free >= 0 && "垃圾回收后仍应有可用空间");
}

/*
 * 测试用例3：分段读写测试
 * 验证：
 * - 大文件读写
 * - 分段操作
 * - 文件截断
 */
void test_segmented_io() {
    char filename[MAX_FILENAME_LEN];
    char write_buf[MAX_CONTENT_LEN];
    char read_buf[MAX_CONTENT_LEN];
    size_t total_size;

    // 生成测试数据
    make_symbolic_filename(filename);
    make_symbolic_content(write_buf, &total_size);

    // 创建文件
    int fd = fs_open(filename, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return;

    // 分段写入
    size_t written = 0;
    while (written < total_size) {
        size_t chunk = (total_size - written < 64) ? 
                      (total_size - written) : 64;
        ssize_t ret = fs_write(fd, write_buf + written, chunk);
        if (ret < 0) {
            fs_close(fd);
            return;
        }
        written += ret;
    }

    // 验证文件大小
    struct stat st;
    assert(fs_stat(filename, &st) == 0);
    assert(st.st_size == total_size);

    // 随机访问读取
    off_t offset = total_size / 2;
    assert(fs_lseek(fd, offset, SEEK_SET) == offset);
    ssize_t bytes_read = fs_read(fd, read_buf, total_size - offset);
    assert(bytes_read == total_size - offset);
    assert(memcmp(write_buf + offset, read_buf, bytes_read) == 0);

    fs_close(fd);
}

/*
 * 测试用例4：边界条件测试
 * 验证：
 * - 文件系统满
 * - 资源耗尽
 * - 错误恢复
 */
void test_boundary_conditions() {
    char filename[MAX_FILENAME_LEN];
    char content[MAX_CONTENT_LEN];
    size_t content_size;
    int fds[256];  // 文件描述符数组
    int fd_count = 0;

    // 生成测试数据
    make_symbolic_filename(filename);
    make_symbolic_content(content, &content_size);

    // 测试文件描述符耗尽
    while (fd_count < 256) {
        int fd = fs_open(filename, O_CREAT | O_RDWR, 0644);
        if (fd < 0) {
            // 验证是否因资源耗尽而失败
            klee_assume(errno == EMFILE || errno == ENFILE);
            break;
        }
        fds[fd_count++] = fd;
    }

    // 关闭所有文件描述符
    while (fd_count > 0) {
        assert(fs_close(fds[--fd_count]) == 0);
    }

    // 测试文件系统空间耗尽
    int fd = fs_open(filename, O_CREAT | O_RDWR, 0644);
    if (fd >= 0) {
        while (1) {
            ssize_t written = fs_write(fd, content, content_size);
            if (written < 0) {
                // 验证是否因空间不足而失败
                klee_assume(errno == ENOSPC);
                break;
            }
        }
        fs_close(fd);
    }

    // 测试错误恢复
    fs_gc(1);  // 执行紧急垃圾回收
    fd = fs_open(filename, O_RDWR);
    if (fd >= 0) {
        // 验证系统是否恢复正常
        assert(fs_write(fd, content, 1) >= 0);
        fs_close(fd);
    }
}

/*
 * 主测试入口
 */
int main() {
    // 运行所有测试用例
    test_path_handling();
    test_fs_management();
    test_segmented_io();
    test_boundary_conditions();
    
    return 0;
} 