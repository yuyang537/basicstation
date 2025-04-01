/*
 * 模块概述
 * ========
 * 本模块是 LoRaWAN 基站的闪存模拟器，提供了对物理闪存芯片的软件模拟功能。
 * 它通过使用普通文件来模拟闪存存储空间，使得系统在没有实际闪存硬件的环境中也能运行。
 * 该模块主要用于开发和测试阶段，替代真实的闪存硬件，简化开发过程和提高调试效率。
 * 
 * 文件功能
 * ========
 * 本文件实现了闪存模拟器的核心功能：
 * - 文件映射到内存实现闪存模拟
 * - 闪存读写和擦除操作
 * - 持久化存储机制
 * - 闪存地址空间管理
 * 
 * 主要组件
 * ========
 * 1. 存储模拟系统
 *    - 基于文件的存储模拟
 *    - 内存映射（mmap）接口
 *    - 持久化写入控制
 *    - 页面管理和边界检查
 * 
 * 2. 闪存操作接口
 *    - 闪存初始化函数
 *    - 页面擦除操作
 *    - 数据读取函数
 *    - 数据写入函数
 * 
 * 3. 存储管理系统
 *    - 闪存地址转换
 *    - 存储空间分配
 *    - 文件大小控制
 *    - 同步机制
 * 
 * 关键流程
 * ========
 * 1. 初始化流程
 *    - 创建或打开模拟闪存文件
 *    - 确保文件大小符合要求
 *    - 将文件映射到内存
 *    - 初始化内部状态
 * 
 * 2. 读写操作流程
 *    - 地址范围验证
 *    - 内存映射访问
 *    - 数据复制操作
 *    - 内存同步（msync）
 * 
 * 3. 擦除操作流程
 *    - 页面边界检查
 *    - 填充擦除值（通常为0xFF）
 *    - 持久化更新
 * 
 * 注意事项
 * ========
 * 1. 性能考虑
 *    - 文件操作开销
 *    - 同步操作延迟
 *    - 内存映射效率
 * 
 * 2. 使用限制
 *    - 仅适用于开发和测试环境
 *    - 地址对齐要求
 *    - 需要足够的文件系统空间
 * 
 * 3. 平台兼容性
 *    - 依赖于POSIX mmap接口
 *    - 仅在Linux或特定配置下可用
 *    - 文件权限和访问控制
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

#if defined(CFG_linux) || defined(CFG_flashsim)

#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "s2conf.h"
#include "sys.h"
#include "fs.h"


static int   fd;
static u1_t* mem;


u4_t* sys_ptrFlash () {
    return (u4_t*)mem;
}

void sys_eraseFlash (u4_t faddr, uint pagecnt) {
    assert((faddr&(FLASH_PAGE_SIZE-1)) == 0);
    memset(&mem[faddr-FLASH_ADDR], FLASH_ERASED&0xFF, pagecnt*FLASH_PAGE_SIZE);
    if( msync(mem, FLASH_SIZE, MS_SYNC) == -1 )
        LOG(MOD_SYS|ERROR, "Flash simulation - msync failed: %s", strerror(errno));
}

void sys_writeFlash (u4_t faddr, u4_t* data, uint u4cnt) {
    assert((faddr&3) == 0 && faddr >= FLASH_ADDR && faddr+u4cnt*4 <= FLASH_ADDR+FLASH_SIZE);
    memcpy(&mem[faddr-FLASH_ADDR], data, u4cnt*4);
    if( msync(mem, FLASH_SIZE, MS_SYNC) == -1 )
        LOG(MOD_SYS|ERROR, "Flash simulation - msync failed: %s", strerror(errno));
}

void sys_readFlash  (u4_t faddr, u4_t* data, uint u4cnt) {
    assert((faddr&3) == 0 && faddr >= FLASH_ADDR && faddr+u4cnt*4 <= FLASH_ADDR+FLASH_SIZE);
    memcpy(data, &mem[faddr-FLASH_ADDR], u4cnt*4);
}

void sys_iniFlash () {
    if( mem ) return;
    str_t fsimFn = sys_makeFilepath("./station.flash", 0);
    str_t op = "";
    fd = open(fsimFn, O_CREAT|O_APPEND|O_RDWR, S_IRUSR|S_IWUSR);
    if( fd == -1 ) {
      err:
        op = "open";
        rt_fatal("Cannot %s flash file '%s': %s", op, fsimFn, strerror(errno));
    }
    off_t flen = lseek(fd, 0, SEEK_END);
    if( flen == -1 ) {
        op = "lseek";
        goto err;
    }
    if( flen < FLASH_SIZE ) {
        u1_t pg[FLASH_PAGE_SIZE];
        memset(pg, FLASH_ERASED&0xFF, sizeof(pg));
        while( flen < FLASH_SIZE ) {
            if( write(fd, pg, FLASH_PAGE_SIZE) == -1 ) {
                op = "write";
                goto err;
            }
            flen += FLASH_PAGE_SIZE;
        }
        fsync(fd);
    }
    mem = mmap(NULL, FLASH_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if( mem==MAP_FAILED ) {
        op = "mmap";
        goto err;
    }
    free((void*)fsimFn);
}

#endif // defined(CFG_flashsim)
