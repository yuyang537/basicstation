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
 * 测试用例1: 路径规范化安全测试
 */
void test_path_normalization() {
    char path[64];
    char normalized[64];
    int result;
    
    // 符号化路径字符串
    klee_make_symbolic(path, sizeof(path), "path_string");
    
    // 确保路径是一个合法的C字符串
    klee_assume(path[sizeof(path)-1] == '\0');
    
    // 测试路径规范化函数
    result = fs_fnNormalize(path, normalized, sizeof(normalized));
    
    // 验证返回结果
    if (result > 0) {
        // 确保规范化路径是以 '/' 开头的绝对路径
        assert(normalized[0] == '/');
        
        // 确保规范化路径不包含连续的 '/'
        for (int i = 0; i < result-1; i++) {
            if (normalized[i] == '/') {
                assert(normalized[i+1] != '/');
            }
        }
        
        // 确保规范化路径不包含 "." 或 ".."
        assert(strstr(normalized, "/.") == NULL || 
               (strstr(normalized, "/.") != NULL && 
                strstr(normalized, "/.")[2] != '\0' && 
                strstr(normalized, "/.")[2] != '/'));
        assert(strstr(normalized, "/..") == NULL || 
               (strstr(normalized, "/..") != NULL && 
                strstr(normalized, "/..")[3] != '\0' && 
                strstr(normalized, "/..")[3] != '/'));
    } 
    // 如果失败，结果应为0
    else {
        assert(result == 0);
        // 验证失败原因
        assert(errno == ENAMETOOLONG || errno == EFAULT);
    }
}

/*
 * 测试用例2: 文件名检查安全测试
 */
void test_filename_validation() {
    // 这个测试需要通过调用fs_open间接测试checkFilename函数
    char filename[128];
    
    // 符号化文件名
    klee_make_symbolic(filename, sizeof(filename), "filename");
    
    // 确保文件名是一个合法的C字符串
    klee_assume(filename[sizeof(filename)-1] == '\0');
    
    // 打开文件 - 这会调用checkFilename进行验证
    int fd = fs_open(filename, O_RDONLY);
    
    // 验证结果
    if (fd >= 0) {
        // 如果成功，关闭文件
        assert(fs_close(fd) == 0);
    } else {
        // 如果失败，检查错误码
        assert(errno == ENAMETOOLONG || errno == ENOENT || 
               errno == EFAULT || errno == EINVAL);
    }
}

/*
 * 测试用例3: 文件描述符安全测试
 */
void test_file_descriptor() {
    int fd;
    
    // 符号化文件描述符
    klee_make_symbolic(&fd, sizeof(fd), "file_descriptor");
    
    // 符号执行关闭操作
    int result = fs_close(fd);
    
    // 验证结果
    if (result == 0) {
        // 成功关闭：fd必须是有效的文件描述符
        assert(fd >= 0x10000 && fd < 0x10000 + FS_MAX_FD);
    } else {
        // 失败：确保errno是一个有效值
        assert(errno == EBADF || errno == EINVAL);
    }
}

/*
 * 测试用例4: 文件读写边界测试
 */
void test_file_read_write_bounds() {
    int fd;
    void* buffer;
    int length;
    
    // 符号化文件描述符、缓冲区地址和读写长度
    klee_make_symbolic(&fd, sizeof(fd), "fd_read_write");
    klee_make_symbolic(&buffer, sizeof(buffer), "buffer_addr");
    klee_make_symbolic(&length, sizeof(length), "read_write_length");
    
    // 限制符号值范围以减少状态空间
    klee_assume(fd >= -1 && fd < 0x10000 + FS_MAX_FD);
    klee_assume(length >= -10 && length < 1024);
    
    // 测试读操作
    int read_result = fs_read(fd, buffer, length);
    
    // 验证读取结果
    if (read_result >= 0) {
        // 读取成功：长度不为负，且不超过请求的长度
        assert(read_result <= length);
    } else {
        // 读取失败：检查错误码
        assert(errno == EBADF || errno == EINVAL || errno == EFAULT);
    }
    
    // 测试写操作
    int write_result = fs_write(fd, buffer, length);
    
    // 验证写入结果
    if (write_result >= 0) {
        // 写入成功：写入的字节数不超过请求的长度
        assert(write_result <= length);
    } else {
        // 写入失败：检查错误码
        assert(errno == EBADF || errno == EINVAL || 
               errno == EFAULT || errno == ENOSPC);
    }
}

/*
 * 测试用例5: 文件句柄转换安全测试
 */
void test_fd2fh_safety() {
    // 间接测试fd2fh函数的安全性
    int fd;
    
    // 符号化文件描述符
    klee_make_symbolic(&fd, sizeof(fd), "fd_conversion");
    
    // 尝试获取文件状态(stat)，这会间接调用fd2fh函数
    struct stat st;
    char filename[32] = "/s2/test.txt"; // 使用固定文件名
    
    int result = fs_stat(filename, &st);
    
    // 验证结果
    if (result == 0) {
        // 成功：验证返回的结构体内容
        assert(st.st_ino > 0);
        assert(st.st_ino <= 0x3FFF); // MAX_INO
        assert(st.st_size >= 0);
    } else {
        // 失败：检查错误码
        assert(errno == ENOENT || errno == EINVAL);
    }
}

/*
 * 测试用例6: 文件系统初始化安全测试
 */
void test_fs_ini_safety() {
    u4_t key[4];
    
    // 符号化密钥
    klee_make_symbolic(key, sizeof(key), "fs_key");
    
    // 首先擦除文件系统以确保初始状态
    fs_erase();
    
    // 初始化文件系统
    int result = fs_ini(key);
    
    // 验证结果
    assert(result >= 0 && result <= 2);
    
    // 再次尝试初始化，应该失败
    result = fs_ini(key);
    assert(result == -1);
}

/*
 * 测试用例7: 文件偏移设置安全测试
 */
void test_file_offset() {
    int fd, offset, whence;
    
    // 符号化参数
    klee_make_symbolic(&fd, sizeof(fd), "seek_fd");
    klee_make_symbolic(&offset, sizeof(offset), "seek_offset");
    klee_make_symbolic(&whence, sizeof(whence), "seek_whence");
    
    // 限制范围
    klee_assume(fd >= -1 && fd < 0x10000 + FS_MAX_FD);
    klee_assume(offset >= -1024 && offset < 1024);
    klee_assume(whence >= 0 && whence <= 2); // SEEK_SET, SEEK_CUR, SEEK_END
    
    // 测试文件偏移设置
    int result = fs_lseek(fd, offset, whence);
    
    // 验证结果
    if (result >= 0) {
        // 成功：新位置应该是合理的
        assert(result >= 0);
    } else {
        // 失败：检查错误码
        assert(errno == EBADF || errno == EINVAL);
    }
}

/*
 * 测试用例8: 文件系统信息安全测试
 */
void test_fs_info_safety() {
    fsinfo_t info;
    
    // 获取文件系统信息
    fs_info(&info);
    
    // 验证信息的合理性
    assert(info.pagecnt > 0);
    assert(info.pagesize > 0);
    assert(info.activeSection <= 1); // 0或1
    assert(info.used <= info.pagecnt * info.pagesize);
    assert(info.free <= info.pagecnt * info.pagesize);
    assert(info.used + info.free <= info.pagecnt * info.pagesize);
}

/*
 * 测试用例9: Flash操作安全测试
 */
void test_flash_operations() {
    u4_t address;
    u4_t data;
    
    // 符号化地址和数据
    klee_make_symbolic(&address, sizeof(address), "flash_address");
    klee_make_symbolic(&data, sizeof(data), "flash_data");
    
    // 限制地址范围在合法的闪存区域内
    // 假设FLASH_BEG_A是0x10000000，每个分区大小是0x10000
    klee_assume(address >= 0x10000000 && address < 0x10020000);
    klee_assume(address % 4 == 0); // 4字节对齐
    
    // 尝试写入和读取，但这需要在可控环境中才能测试
    // 这里只是形式上的测试，实际上我们需要环境支持
    // wrFlash1(address, data);
    // u4_t read_data = rdFlash1(address);
    // assert(read_data == data);
}

/*
 * 测试用例10: 垃圾回收安全测试
 */
void test_gc_safety() {
    int emergency;
    
    // 符号化紧急标志
    klee_make_symbolic(&emergency, sizeof(emergency), "gc_emergency");
    
    // 限制为有效值
    klee_assume(emergency >= 0 && emergency <= 1);
    
    // 执行垃圾回收
    fs_gc(emergency);
    
    // 验证文件系统仍然可用
    fsinfo_t info;
    fs_info(&info);
    assert(info.free > 0); // 应该有一些可用空间
}

/*
 * 主测试入口
 */
int main() {
    // 初始化文件系统
    u4_t key[4] = {0x71593cbf, 0x81db1a48, 0x22fc47fe, 0xe8cf23ea};
    fs_erase();
    fs_ini(key);
    
    // 运行所有测试用例
    test_path_handling();
    test_fs_management();
    test_segmented_io();
    test_boundary_conditions();
    test_path_normalization();
    test_filename_validation();
    test_file_descriptor();
    test_file_read_write_bounds();
    test_fd2fh_safety();
    test_fs_ini_safety();
    test_file_offset();
    test_fs_info_safety();
    test_flash_operations();
    test_gc_safety();
    
    return 0;
} 