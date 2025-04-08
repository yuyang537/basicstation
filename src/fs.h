/**
 * @file fs.h
 * @brief 文件系统接口头文件
 * 
 * 该文件定义了基于闪存的简单文件系统接口。文件系统主要用于存储配置数据、
 * 日志文件和其他需要在断电后保持的数据。系统使用两个闪存分区进行交替写入，
 * 支持基本的文件操作如创建、读取、写入、删除和重命名。
 * 
 * 文件系统特点：
 * 1. 使用两个分区交替写入，延长闪存寿命
 * 2. 支持文件加密存储
 * 3. 自动垃圾回收机制
 * 4. 支持基本的文件操作
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

#ifndef _fs_h_
#define _fs_h_

/**
 * @brief 闪存底层操作函数
 * 
 * 这些函数提供了对闪存的基本操作，包括擦除、写入和读取。
 * 这些操作是文件系统的基础，由硬件抽象层实现。
 */
void  sys_eraseFlash (u4_t faddr, uint pagecnt);  ///< 擦除指定地址开始的多个闪存页
void  sys_writeFlash (u4_t faddr, u4_t* data, uint u4cnt);  ///< 向闪存写入数据
void  sys_readFlash  (u4_t faddr, u4_t* data, uint u4cnt);  ///< 从闪存读取数据
u4_t* sys_ptrFlash   ();  ///< 获取闪存基地址指针
void  sys_iniFlash   ();  ///< 初始化闪存控制器

/**
 * @brief 闪存读写辅助函数
 * 
 * 这些函数提供了对闪存的单字和多字读写操作。
 */
u4_t rdFlash1 (u4_t faddr);  ///< 从闪存读取单个32位字
void rdFlashN (u4_t faddr, u4_t* daddr, uint u4cnt);  ///< 从闪存读取多个32位字
void wrFlash1 (u4_t faddr, u4_t data);  ///< 向闪存写入单个32位字
void wrFlashN (u4_t faddr, u4_t* daddr, uint u4cnt, int keepData);  ///< 向闪存写入多个32位字

/**
 * @brief 标准文件操作接口
 * 
 * 这些函数提供了类似POSIX的文件操作接口。
 */
int  fs_open   (str_t filename, int mode, ...);  ///< 打开文件
int  fs_read   (int fd,       void* buf, int size);  ///< 读取文件内容
int  fs_write  (int fd, const void* buf, int size);  ///< 写入文件内容
int  fs_close  (int fd);  ///< 关闭文件
int  fs_rename (str_t from, str_t to);  ///< 重命名文件
int  fs_unlink (str_t from);  ///< 删除文件
int  fs_chdir  (str_t dir);  ///< 改变当前工作目录
int  fs_access (str_t fn, int mode);  ///< 检查文件访问权限
int  fs_stat   (str_t fn, struct stat* st);  ///< 获取文件状态
int  fs_lseek  (int fd, int offset, int whence);  ///< 移动文件指针
void fs_sync   ();  ///< 同步文件系统缓存

/**
 * @brief 文件系统工具函数
 */
int  fs_fnNormalize (const char* fn, char* wb, int maxsz);  ///< 规范化文件路径

/**
 * @brief 文件系统管理函数
 */
int  fs_ini   (u4_t key[4]);  ///< 初始化文件系统
int  fs_ck    ();  ///< 检查文件系统完整性
void fs_erase ();  ///< 擦除整个文件系统
void fs_gc    (int emergency);  ///< 执行垃圾回收
int  fs_dump  (void (*logfn)(u1_t mod_level, const char* fmt, ...));  ///< 转储文件系统信息
int  fs_shell (char* cmdline);  ///< 文件系统命令行接口

/**
 * @brief 文件系统信息结构体
 * 
 * 用于存储和查询文件系统的状态信息。
 */
typedef struct fsinfo {
    void* fbasep;      ///< 闪存基地址指针
    u4_t  fbase;       ///< 闪存基地址
    u2_t  pagecnt;     ///< 总页数
    u2_t  pagesize;    ///< 页大小
    u1_t  activeSection;  ///< 当前活动分区(0或1)
    u2_t  gcCycles;    ///< 垃圾回收次数
    u4_t  records;     ///< 记录总数
    u4_t  used;        ///< 已用空间(字节)
    u4_t  free;        ///< 可用空间(字节)
    u4_t  key[4];      ///< 加密密钥
} fsinfo_t;

/**
 * @brief 获取文件系统信息
 * @param infop 指向存储文件系统信息的结构体指针
 */
void fs_info(fsinfo_t* infop);

#endif // _fs_h_
