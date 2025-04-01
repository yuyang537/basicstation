/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的文件系统实现，提供基于闪存的持久化存储功能。
 * 它实现了一个轻量级的嵌入式文件系统，用于存储配置、证书和其他持久化数据。
 * 该模块是基站数据持久化的关键组件，确保系统配置和关键数据在断电后能够恢复。
 * 
 * 文件功能
 * ========
 * 本头文件定义了文件系统的核心接口：
 * - 闪存底层操作
 * - 文件操作（开、读、写、关闭等）
 * - 目录和路径管理
 * - 文件系统管理（初始化、检查、垃圾回收等）
 * - 文件系统信息查询
 * 
 * 主要组件
 * ========
 * 1. 闪存抽象层
 *    - 闪存读写操作
 *    - 页擦除功能
 *    - 地址管理
 *    - 底层初始化
 * 
 * 2. 文件操作系统
 *    - 文件打开和关闭
 *    - 数据读写操作
 *    - 文件指针控制
 *    - 文件属性管理
 * 
 * 3. 路径管理系统
 *    - 路径规范化
 *    - 目录操作
 *    - 文件重命名和删除
 *    - 路径访问控制
 * 
 * 4. 文件系统管理
 *    - 初始化和一致性检查
 *    - 垃圾回收机制
 *    - 文件系统诊断
 *    - 加密支持
 * 
 * 关键流程
 * ========
 * 1. 文件系统初始化流程
 *    - 闪存驱动初始化
 *    - 文件系统结构检查
 *    - 元数据加载
 *    - 一致性验证
 * 
 * 2. 文件操作流程
 *    - 文件打开和创建
 *    - 数据读写操作
 *    - 文件同步
 *    - 资源释放
 * 
 * 3. 文件系统维护流程
 *    - 垃圾回收触发
 *    - 空间整理
 *    - 损坏修复
 *    - 数据同步
 * 
 * 注意事项
 * ========
 * 1. 资源限制
 *    - 闪存容量限制
 *    - 文件数量限制
 *    - 目录深度限制
 *    - 文件名长度限制
 * 
 * 2. 可靠性考虑
 *    - 断电保护机制
 *    - 写入验证
 *    - 元数据备份
 *    - 一致性检查
 * 
 * 3. 性能因素
 *    - 闪存擦写次数优化
 *    - 读写速度
 *    - 垃圾回收效率
 *    - 内存使用优化
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

void  sys_eraseFlash (u4_t faddr, uint pagecnt);
void  sys_writeFlash (u4_t faddr, u4_t* data, uint u4cnt);
void  sys_readFlash  (u4_t faddr, u4_t* data, uint u4cnt);
u4_t* sys_ptrFlash   ();
void  sys_iniFlash   ();

u4_t rdFlash1 (u4_t faddr);
void rdFlashN (u4_t faddr, u4_t* daddr, uint u4cnt);

void wrFlash1 (u4_t faddr, u4_t data);
void wrFlashN (u4_t faddr, u4_t* daddr, uint u4cnt, int keepData);

int  fs_open   (str_t filename, int mode, ...);
int  fs_read   (int fd,       void* buf, int size);
int  fs_write  (int fd, const void* buf, int size);
int  fs_close  (int fd);
int  fs_rename (str_t from, str_t to);
int  fs_unlink (str_t from);
int  fs_chdir  (str_t dir);
int  fs_access (str_t fn, int mode);
int  fs_stat   (str_t fn, struct stat* st);
int  fs_lseek  (int fd, int offset, int whence);
void fs_sync   ();

int  fs_fnNormalize (const char* fn, char* wb, int maxsz);

int  fs_ini   (u4_t key[4]);
int  fs_ck    ();
void fs_erase ();
void fs_gc    (int emergency);
int  fs_dump  (void (*logfn)(u1_t mod_level, const char* fmt, ...));
int  fs_shell (char* cmdline);

typedef struct fsinfo {
    void* fbasep;
    u4_t  fbase;
    u2_t  pagecnt;
    u2_t  pagesize;
    u1_t  activeSection;
    u2_t  gcCycles;
    u4_t  records;
    u4_t  used;
    u4_t  free;
    u4_t  key[4];
} fsinfo_t;

void fs_info(fsinfo_t* infop);

#endif // _fs_h_
