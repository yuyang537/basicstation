/**
 * @file fs.c
 * @brief 文件系统实现文件
 * 
 * 该文件实现了基于闪存的简单文件系统。文件系统使用两个分区交替写入，
 * 支持基本的文件操作，并包含垃圾回收机制。
 * 
 * 文件系统组织：
 * 1. 闪存被组织为32位字
 * 2. 整个空间被分为两个分区
 * 3. 当一个分区满时，垃圾回收会将有效数据复制到另一个分区
 * 4. 每个分区包含一个魔数+GC序列计数器
 * 5. 每次GC操作都会增加序列计数器
 * 6. 通过比较两个分区的魔数可以确定哪个分区更新
 * 
 * 记录格式：
 * [begtag] ... [endtag]
 * 
 * 记录类型：
 * 1. FILE/DELETE记录：[begtag][fncrc][ctim][filename\0{1,4}][endtag]
 * 2. RENAME记录：[begtag][fncrc][fncrc2][filename1\0filename2\0{1,4}][endtag]
 * 3. DATA记录：[begtag][data0][data1]...[dataN\0{0,3}][endtag]
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

#include <stdio.h>
#include <fcntl.h>

#if defined(CFG_linux) || defined(CFG_flashsim)
#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#endif // defined(CFG_linux)
#include "s2conf.h"
#include "rt.h"
#include "kwcrc.h"
#include "uj.h"
#include "fs.h"

// 文件描述符偏移量，用于区分系统文件描述符
#define OFF_FD 0x10000
// 最大inode号
#define MAX_INO 0x3FFF
// CRC初始值
#define CRC_INI 0x1234

// 闪存魔数，用于标识文件系统分区
#define FLASH_MAGIC 0xA4B5
// 分区A的起始地址
#define FLASH_BEG_A (FLASH_ADDR  + FLASH_PAGE_SIZE * FS_PAGE_START)
// 分区B的起始地址
#define FLASH_BEG_B (FLASH_BEG_A + FLASH_PAGE_SIZE * (FS_PAGE_CNT/2))
// 分区A的结束地址
#define FLASH_END_A (FLASH_BEG_B)
// 分区B的结束地址
#define FLASH_END_B (FLASH_BEG_B + FLASH_PAGE_SIZE * (FS_PAGE_CNT/2))

/**
 * @brief 从标签中提取命令
 * @param v 标签值
 * @return 命令值(0-3)
 */
static inline u1_t FSTAG_cmd(u4_t v) { return  (v >> 30) & 3; }

/**
 * @brief 从标签中提取inode号
 * @param v 标签值
 * @return inode号
 */
static inline u2_t FSTAG_ino(u4_t v) { return ((v >> 16) & MAX_INO); }

/**
 * @brief 从标签中提取CRC值
 * @param v 标签值
 * @return CRC值
 */
static inline u2_t FSTAG_crc(u4_t v) { return ((v >> 16) & 0xFFFF); }

/**
 * @brief 从标签中提取长度
 * @param v 标签值
 * @return 长度值
 */
static inline u2_t FSTAG_len(u4_t v) { return   v & 0xFFFC; }

/**
 * @brief 从标签中提取填充值
 * @param v 标签值
 * @return 填充值(0-3)
 */
static inline u1_t FSTAG_pad(u4_t v) { return   v & 3; }

/**
 * @brief 创建开始标签
 * @param cmd 命令值
 * @param ino inode号
 * @param len 长度
 * @param pad 填充值
 * @return 标签值
 */
static inline u4_t FSTAG_mkBeg(u1_t cmd, u2_t ino, u2_t len, u1_t pad) {
    return (cmd<<30) | ((ino&MAX_INO)<<16) | (len&0xFFFC) | (pad&3);
}

/**
 * @brief 创建结束标签
 * @param crc CRC值
 * @param len 长度
 * @param pad 填充值
 * @return 标签值
 */
static inline u4_t FSTAG_mkEnd(u2_t crc, u2_t len, u1_t pad) {
    return ((crc&0xFFFF)<<16) | (len&0xFFFC)|(pad&3);
}

// 命令定义
#define FSCMD_FILE   0  ///< 文件命令
#define FSCMD_DATA   1  ///< 数据命令
#define FSCMD_RENAME 2  ///< 重命名命令
#define FSCMD_DELETE 3  ///< 删除命令

/**
 * @brief 文件上下文结构体
 * 
 * 用于跟踪文件操作的位置和状态
 */
typedef struct fctx {
    u4_t faddr;   ///< 闪存地址
    u4_t begtag;  ///< 开始标签
    u4_t endtag;  ///< 结束标签
} fctx_t;

/**
 * @brief 文件句柄结构体
 * 
 * 用于管理打开的文件
 */
typedef struct fh {
    u2_t ino;     ///< inode号
    u2_t droff;   ///< 数据记录内的偏移
    u4_t faddr;   ///< 闪存地址
    u4_t foff;    ///< 文件读取偏移
} fh_t;

/**
 * @brief inode缓存结构体
 * 
 * 用于缓存文件元数据
 */
struct ino_cache {
    u4_t faddrFile;   ///< 创建文件的记录地址
    u4_t faddrRename; ///< 最后一次重命名的地址
    u4_t fncrc;       ///< 文件名CRC
};

// 辅助缓冲区大小定义
#define AUXBUF_SZW (2*((FS_MAX_FNSIZE+3)/4))
#define AUXBUF_SZ4 (4*AUXBUF_SZW)

/**
 * @brief 辅助缓冲区联合体
 * 
 * 用于临时存储和操作数据
 */
static union auxbuf {
    u4_t u4[AUXBUF_SZW];  ///< 32位字数组
    u1_t u1[AUXBUF_SZ4];  ///< 8位字节数组
} auxbuf;

// 全局变量
static fctx_t fctxCache;  ///< 文件上下文缓存
static u4_t   flashKey[4];  ///< 闪存加密密钥
static u4_t   flashWP;  ///< 闪存写入指针
static u2_t   nextIno;  ///< 下一个可用的inode号
static s1_t   fsSection = -1;  ///< 当前活动分区(0或1)
static const char DEFAULT_CWD[] = "/s2/";  ///< 默认工作目录
static str_t  cwd = DEFAULT_CWD;  ///< 当前工作目录
static fh_t   fhTable[FS_MAX_FD];  ///< 文件句柄表

/**
 * @brief 获取当前活动分区的起始地址
 * @return 分区起始地址
 */
static inline u4_t flashFsBeg() {
    return fsSection ? FLASH_BEG_B+4 : FLASH_BEG_A+4;
}

/**
 * @brief 获取当前活动分区的最大地址
 * @return 分区最大地址
 */
static inline u4_t flashFsMax() {
    return fsSection ? FLASH_END_B : FLASH_END_A;
}

/**
 * @brief 加密单个32位字
 * @param faddr 闪存地址
 * @param data 要加密的数据
 * @return 加密后的数据
 */
static u4_t encrypt1 (u4_t faddr, u4_t data) {
    return data ^ flashKey[(faddr>>2) & 3];
}

/**
 * @brief 解密单个32位字
 * @param faddr 闪存地址
 * @param data 要解密的数据
 * @return 解密后的数据
 */
static u4_t decrypt1 (u4_t faddr, u4_t data) {
    return encrypt1(faddr, data);
}

/**
 * @brief 加密多个32位字
 * @param faddr 闪存地址
 * @param data 要加密的数据数组
 * @param u4cnt 数据字数
 */
static void encryptN (u4_t faddr, u4_t* data, uint u4cnt) {
    for( uint u=0; u<u4cnt; u++ ) {
        data[u] = encrypt1(faddr+u*4, data[u]);
    }
}

static void decryptN (u4_t faddr, u4_t* data, uint u4cnt) {
    for( uint u=0; u<u4cnt; u++ ) {
        data[u] = decrypt1(faddr+u*4, data[u]);
    }
}

void wrFlash1 (u4_t faddr, u4_t data) {
    assert(faddr < (faddr >= FLASH_BEG_B ? FLASH_END_B : FLASH_END_A));
    data = encrypt1(faddr, data);
    sys_writeFlash(faddr, &data, 1);
}

static void wrFlash1wp (u4_t data) {
    u4_t faddr = flashWP;
    wrFlash1(faddr, data);
    flashWP = faddr + 4;
}

u4_t rdFlash1 (u4_t faddr) {
    u4_t data;
    assert(faddr < flashFsMax());
    sys_readFlash(faddr, &data, 1);
    return decrypt1(faddr, data);
}

void wrFlashN (u4_t faddr, u4_t* daddr, uint u4cnt, int keepData) {
    assert(faddr + u4cnt*4 <= (faddr >= FLASH_BEG_B ? FLASH_END_B : FLASH_END_A));
    encryptN(faddr, daddr, u4cnt);
    sys_writeFlash(faddr, daddr, u4cnt);
    if( keepData )
        decryptN(faddr, daddr, u4cnt);
}


static void wrFlashNwp (u4_t* daddr, uint u4cnt, int keepData) {
    u4_t faddr = flashWP;
    wrFlashN (faddr, daddr, u4cnt, keepData);
    flashWP = faddr + u4cnt*4;
}

void rdFlashN(u4_t faddr, u4_t* daddr, uint u4cnt) {
    assert(faddr + u4cnt*4 <= flashFsMax());
    sys_readFlash(faddr, daddr, u4cnt);
    decryptN(faddr, daddr, u4cnt);
}


static u4_t fctx_begtag (fctx_t* fctx) {
    u4_t begtag = fctx->begtag;
    if( begtag == 0 )
        begtag = fctx->begtag = rdFlash1(fctx->faddr);
    return begtag;
}

static u4_t fctx_endtag (fctx_t* fctx) {
    u4_t endtag = fctx->endtag;
    if( endtag == 0 ) {
        u4_t begtag = fctx_begtag(fctx);
        u4_t faddr = fctx->faddr + 4 + FSTAG_len(begtag);
        endtag = fctx->endtag = rdFlash1(faddr);
    }
    return endtag;
}

/**
 * @brief 计算数据的CRC值
 * @param crc 初始CRC值
 * @param data 数据指针
 * @param len 数据长度
 * @return 计算后的CRC值
 */
static u2_t dataCrc (u2_t crc, const u1_t* data, uint len) {
    u1_t a=crc>>8, b=crc&0xFF;
    for( int i=0; i<len; i++ ) {
        b += (a += data[i]);
    }
    while( -len & 3 ) {
        b += a;
        len++;
    }
    return (a<<8)|b;
}

/**
 * @brief 计算文件名的CRC值
 * @param fn 文件名指针
 * @return 计算后的CRC值
 */
static u4_t fnCrc (const char* fn) {
    u4_t crc = 0;
    while( *fn++ )
        crc = UJ_UPDATE_CRC(crc,*fn);
    return UJ_FINISH_CRC(crc);
}

/**
 * @brief 检查闪存是否已满
 * @param reqbytes 请求的字节数
 * @return 0表示有足够空间，-1表示空间不足
 */
static int isFlashFull (u4_t reqbytes) {
    int emergency = 0;
    reqbytes = (reqbytes + 3) & ~3;
    while( flashWP + reqbytes > flashFsMax() || nextIno >= MAX_INO-2 ) {
        if( emergency == 2 ) {
            // 即使在紧急清理后也没有空间
            errno = ENOSPC;
            return -1;
        }
        fs_gc(emergency);
        emergency++;
    }
    return 0;
}

/**
 * @brief 规范化文件路径
 * @param fn 原始文件路径
 * @param wb 输出缓冲区
 * @param maxsz 缓冲区最大大小
 * @return 规范化后的路径长度，0表示错误
 */
int fs_fnNormalize (const char* fn, char* wb, int maxsz) {
    int ri = 0, wi = 0;
    wb[0] = 0;
    if( maxsz <= 2 ) {
        errno = ENAMETOOLONG;
        return 0;
    }
    if( fn[0] != '/' ) {
        wi = strlen(cwd);
        if( wi+2 >= maxsz ) {
            errno = ENAMETOOLONG;
            return 0;
        }
        strcpy(wb, cwd);
    } else {
        ri = wi = 1;
        wb[0] = '/';
    }
    int c;
    while( 1 ) {
        // 路径音节开始 - 前一个字符是/
        c = fn[ri];
        if( c=='/' ) {
            ri++;                 // 忽略双斜杠
            continue;
        }
        if( c=='.' && (fn[ri+1] == '/' || fn[ri+1] == 0) ) {
            ri += 2 - !fn[ri+1];  // 忽略./或.\0
            continue;
        }
        if( c=='.' && fn[ri+1] == '.' && (fn[ri+2] == '/' || fn[ri+2] == 0) ) {
            ri += 3 - !fn[ri+2];  // 跳过../并回退一个音节
            if( wi == 1 )
                continue;  // 根斜杠
            do {
                wi -= 1;
            } while( wb[wi-1] != '/');
            continue;
        }
        if( c == 0 ) {
            if( wi > 1 )
                wi -= 1;  // 移除尾部/
            wb[wi] = 0;
            return wi+1;
        }
        while( 1 ) {
            c = fn[ri];
            if( c==0 ) {
                wb[wi] = 0;
                return wi+1;
            }
            wb[wi++] = c;
            if( wi+2 >= maxsz ) {
                wb[wi] = 0;
                errno = ENAMETOOLONG;
                return 0;
            }
            ri++;
            if( c=='/' )
                break;
        }
    }
}

/**
 * @brief 设置文件上下文到指定地址
 * @param fctx 文件上下文指针
 * @param faddr 闪存地址
 */
static void fctx_setTo (fctx_t* fctx, u4_t faddr) {
    memset(fctx, 0, sizeof(*fctx));
    fctx->faddr = faddr;
}

/**
 * @brief 检查文件名有效性
 * @param fn 文件名指针
 * @return 文件名长度，0表示错误
 */
static int checkFilename (const char* fn) {
    if( fn == NULL ) {
        errno = EFAULT;
        return 0;
    }
    char* wb = (char*)&auxbuf.u4[3];
    int fnlen = auxbuf.u4[0] = fs_fnNormalize(fn, wb, FS_MAX_FNSIZE);
#if defined(CFG_linux)
    if( strncmp(wb, "/s2/", 3) != 0 || (wb[3] != 0 && wb[3] != '/') ) {
        // 分支到linux文件系统
        return -1;
    }
#endif // defined(CFG_linux)
    return fnlen;
}

/**
 * @brief 查找文件
 * @param fctx 文件上下文指针
 * @param fn 文件名
 * @return 0表示成功，-1表示失败
 */
static int fs_findFile (fctx_t* fctx, const char* fn) {
    int fnlen;
    if( fn != NULL ) {
        fnlen = checkFilename(fn);
        if( fnlen == 0 )
            return -1;
    } else {
        // 调用者已经执行了checkFilename
        fnlen = auxbuf.u4[0];
    }
    char* wb = (char*)&auxbuf.u1[12];
    u4_t seekcrc = auxbuf.u4[1] = fnCrc(wb);
    u4_t faddr = flashWP;  // 最后一条记录的末尾
    while( faddr > flashFsBeg() ) {
        u4_t endtag = rdFlash1(faddr-4);
        u4_t len = FSTAG_len(endtag);
        faddr -= len+8;
        u4_t begtag = rdFlash1(faddr);
        u1_t cmd = FSTAG_cmd(begtag);
        if( cmd == FSCMD_DATA )
            continue;
        u4_t fncrc = rdFlash1(faddr+4);
        if( seekcrc == fncrc ) {
            if( cmd == FSCMD_RENAME || cmd == FSCMD_DELETE )
                break;
            assert(cmd == FSCMD_FILE);
            fctx_setTo(fctx, faddr);
            fctx->begtag = begtag;
            fctx->endtag = endtag;
            return 0;
        }
        if( cmd == FSCMD_RENAME && seekcrc == rdFlash1(faddr+8) )
            seekcrc = fncrc;
    }
    errno = ENOENT;
    return -1;
}

/**
 * @brief 处理文件操作
 * @param fn 文件名
 * @param fn2 第二个文件名（用于重命名）
 * @param cmd 命令类型
 * @param ino inode号
 * @return 0表示成功，-1表示失败
 */
static int fs_handleFile (const char* fn, const char* fn2, u1_t cmd, u2_t ino) {
    char* wb = (char*)&auxbuf.u1[12];
    int fnlen;
    if( fn == NULL ) {
        // 之前的操作已经将规范化的文件名放入auxbuf
        fnlen = auxbuf.u4[0];
    } else {
        fnlen = fs_fnNormalize(fn, wb, FS_MAX_FNSIZE);
        if( fnlen == 0 )
            return -1;
    }
    auxbuf.u4[1] = fnCrc(wb);
    if( fn2 != NULL ) {
        int fnlen2 = fs_fnNormalize(fn2, wb+fnlen, FS_MAX_FNSIZE);
        if( fnlen2 == 0 )
            return -1;
        auxbuf.u4[2] = fnCrc(wb+fnlen);
        fnlen += fnlen2;
    } else {
        auxbuf.u4[2] = rt_getUTC()/rt_seconds(1);
    }
    while( (fnlen & 3) != 0 )
        wb[fnlen++] = 0;
    fnlen += 8;
    auxbuf.u4[0] = FSTAG_mkBeg(cmd, ino, fnlen, 0);
    u4_t dlen4 = fnlen/4+2;
    auxbuf.u4[dlen4-1] = FSTAG_mkEnd(dataCrc(CRC_INI, &auxbuf.u1[4], fnlen), fnlen, 0);
    wrFlashNwp(auxbuf.u4, dlen4, 1);
    return 0;
}

/**
 * @brief 创建文件
 * @param fh 文件句柄指针
 * @param fn 文件名
 * @return 0表示成功，-1表示失败
 */
static int fs_createFile (fh_t* fh, const char* fn) {
    u4_t faddr = flashWP;
    if( fs_handleFile(fn, NULL, FSCMD_FILE, nextIno++) == -1 )
        return -1;
    u4_t begtag = auxbuf.u4[0];
    fh->faddr = faddr;
    fh->ino   = FSTAG_ino(begtag);
    fh->droff = FSTAG_len(begtag);  // 完整 - 读取移动到下一个
    fh->foff  = 0;
    return 0;
}

/**
 * @brief 将文件描述符转换为文件句柄
 * @param fd 文件描述符
 * @return 文件句柄指针，NULL表示错误
 */
static fh_t* fd2fh (int fd) {
    if( fd < OFF_FD || fd >= OFF_FD+FS_MAX_FD ) {
        errno = EINVAL;
        return NULL;
    }
    if( fhTable[fd-OFF_FD].ino == 0 || fhTable[fd-OFF_FD].ino > MAX_INO ) {
        errno = EBADF;
        return NULL;
    }
    return &fhTable[fd-OFF_FD];
}

/**
 * @brief 查找下一个数据记录
 * @param fctx 文件上下文指针
 * @param ino inode号
 * @return 1表示找到，0表示未找到
 */
static int fs_findNextDataRecord (fctx_t* fctx, u2_t ino) {
    u4_t faddr = fctx->faddr;
    if( faddr >= flashWP )
        return 0;
    u4_t begtag = fctx_begtag(fctx);
    if( ino == 0 )
        ino = FSTAG_ino(begtag);
    while(1) {
        faddr += FSTAG_len(begtag) + 8;
        if( faddr >= flashWP )
            return 0;
        begtag = rdFlash1(faddr);
        if( FSTAG_ino(begtag) == ino && FSTAG_cmd(begtag) == FSCMD_DATA )
            break;
    }
    fctx_setTo(fctx, faddr);
    fctx->begtag = begtag;
    return 1;
}

/**
 * @brief 读取文件内容
 * @param fd 文件描述符
 * @param dp 数据缓冲区
 * @param dlen 要读取的长度
 * @return 实际读取的字节数，-1表示错误
 */
int fs_read (int fd, void* dp, int dlen) {
    u1_t* data = (u1_t*)dp;
    fh_t* fh = fd2fh(fd);
    if( fh == NULL ) {
#if defined(CFG_linux)
        if( errno == EINVAL ) {
            return read(fd, dp, dlen);
        }
#endif
        return -1;
    }
    if( dlen == 0 )
        return 0;
    if( fh->faddr == 0 ) {  // opened for writing?
        errno = EBADF;
        return -1;
    }
    fctx_t* fctx = &fctxCache;
    fctx_setTo(fctx, fh->faddr);
    int rlen = 0;
    int droff = fh->droff;
    while(1) {
        int begtag = fctx_begtag(fctx);
        int drend = FSTAG_len(begtag) - FSTAG_pad(fctx_endtag(fctx));
        while( droff < drend ) {
            u4_t cpylen = drend-droff;
            if( cpylen > dlen )
                cpylen = dlen;
            u4_t fb = fctx->faddr + 4 + droff;
            u4_t fb4 = fb & ~3;
            u4_t fl4 = ((fb+cpylen+3) & ~3) - fb4;
            if( fl4 > AUXBUF_SZ4 ) {
                fl4 = AUXBUF_SZ4;
                cpylen = AUXBUF_SZ4 - (fb-fb4);
            }
            rdFlashN(fb4, auxbuf.u4, fl4/4);
            memcpy(data, &auxbuf.u1[fb-fb4], cpylen);
            droff += cpylen;
            rlen += cpylen;
            dlen -= cpylen;
            if( dlen == 0 ) {
                goto done;
            }
            data += cpylen;
        }
        if( !fs_findNextDataRecord(fctx, 0) ) {
            // Keep data record - droff indicates no more
            // data in this one. Next read wil check again if
            // data blocks have been appended
            goto done;
        }
        droff = 0;
    }
  done:
    fh->faddr = fctx->faddr;
    fh->droff = droff;
    fh->foff += rlen;
    return rlen;
}

/**
 * @brief 写入文件内容
 * @param fd 文件描述符
 * @param dp 数据缓冲区
 * @param dlen 要写入的长度
 * @return 实际写入的字节数，-1表示错误
 */
int fs_write (int fd, const void* dp, int dlen) {
    const u1_t* data = (const u1_t*)dp;
    fh_t* fh = fd2fh(fd);
    if( fh == NULL ) {
#if defined(CFG_linux)
        if( errno == EINVAL ) {
            return write(fd, dp, dlen);
        }
#endif
        return -1;
    }
    if( fh->faddr != 0 ) {  // opened for reading?
        errno = EBADF;
        return -1;
    }
    if( dlen == 0 )
        return 0;

    if( isFlashFull(dlen+8) == -1 )
        return -1;

    auxbuf.u4[0] = 0;
    u2_t  dlenCeil = (dlen+3) & ~3;
    u2_t  dcrc = dataCrc(CRC_INI, data, dlen);
    int   doff = 0;
    u1_t  tbeg=0, tend=0;
    u1_t* tb = &auxbuf.u1[4];
    int   tblen = sizeof(auxbuf.u1)-8;
    auxbuf.u4[0] = FSTAG_mkBeg(FSCMD_DATA, fh->ino, dlenCeil, 0);
    while( !tend ) {
        int cpylen = dlen-doff;
        if( cpylen > tblen )
            cpylen = tblen;
        doff += cpylen;
        int cpylen4 = (cpylen+3)/4;
        if( doff == dlen ) {
            auxbuf.u4[0+cpylen4] = 0;  // 主动填充
            auxbuf.u4[1+cpylen4] = FSTAG_mkEnd(dcrc, dlenCeil, dlenCeil-dlen);
            tend = 1;
        }
        memcpy(tb, data+doff-cpylen, cpylen);
        wrFlashNwp(&auxbuf.u4[tbeg], (1-tbeg)+cpylen4+tend, 0);
        tbeg = 1;
    }
    return dlen;
}

/**
 * @brief 改变当前工作目录
 * @param dir 目标目录路径
 * @return 0表示成功，-1表示失败
 */
int fs_chdir (str_t dir) {
    // 规范化目录路径
    str_t ndir = dir;
    if( dir != NULL ) {
        ndir = (str_t)auxbuf.u1;
        int sz = fs_fnNormalize(dir, (char*)auxbuf.u1, FS_MAX_FNSIZE);
        if( sz == 0 )
            return -1;
        auxbuf.u1[sz-1] = '/';
        auxbuf.u1[sz] = 0;
    }
    if( cwd != DEFAULT_CWD )
        free((void*)cwd);
    if( ndir == NULL || strcmp(ndir, DEFAULT_CWD) == 0 ) {
        cwd = DEFAULT_CWD;
    } else {
        cwd = rt_strdup(ndir);
    }
    return 0;
}

/**
 * @brief 删除文件
 * @param fn 文件名
 * @return 0表示成功，-1表示失败
 */
int fs_unlink (const char* fn) {
    int fnlen = checkFilename(fn);
#if defined(CFG_linux)
    if( fnlen == -1 ) {
        return unlink(fn);
    }
#endif // defined(CFG_linux)
    if( fnlen <= 0 )
        return -1;
    if( fs_findFile(&fctxCache, NULL) == -1 )
        return -1;
    return fs_handleFile(NULL, NULL, FSCMD_DELETE, FSTAG_ino(fctx_begtag(&fctxCache)));
}

/**
 * @brief 重命名文件
 * @param from 原文件名
 * @param to 新文件名
 * @return 0表示成功，-1表示失败
 */
int fs_rename (const char* from, const char* to) {
    int fnlen2 = checkFilename(to);
    int fnlen = checkFilename(from);
    if( fnlen == 0 || fnlen2 == 0 )
        return -1;
#if defined(CFG_linux)
    if( fnlen == -1 && fnlen2 == -1 ) {
        return rename(from, to);
    }
#endif // defined(CFG_linux)
    if( fnlen == -1 || fnlen2 == -1 ) {
        errno = EXDEV;
        return -1;
    }
    if( isFlashFull(fnlen+fnlen2+16) == -1 )
        return -1;
    if( fs_findFile(&fctxCache, NULL) == -1 )
        return -1;
    return fs_handleFile(NULL, to, FSCMD_RENAME, FSTAG_ino(fctx_begtag(&fctxCache)));
}

/**
 * @brief 检查文件访问权限
 * @param fn 文件名
 * @param mode 访问模式
 * @return 0表示有权限，-1表示无权限
 */
int fs_access (str_t fn, int mode) {
    int fnlen = checkFilename(fn);
#if defined(CFG_linux)
    if( fnlen == -1 ) {
        return access(fn, mode);
    }
#endif // defined(CFG_linux)
    if( fnlen <= 0 )
        return -1;
    return fs_findFile(&fctxCache, NULL);
}

/**
 * @brief 打开文件
 * @param fn 文件名
 * @param mode 打开模式
 * @param ... 可变参数（用于Linux系统）
 * @return 文件描述符，-1表示错误
 */
int fs_open (str_t fn, int mode, ...) {
    int fnlen = checkFilename(fn);
#if defined(CFG_linux)
    if( fnlen == -1 ) {
        va_list ap;
        va_start(ap, mode);
        int flags = va_arg(ap, int);
        va_end(ap);
        return open(fn, mode, flags);
    }
#endif // defined(CFG_linux)
    if( fnlen <= 0 )
        return -1;

    if( isFlashFull(fnlen+16) == -1 )
        return -1;

    fh_t* fh = NULL;
    for( int i=0; i< FS_MAX_FD; i++ ) {
        if( fhTable[i].ino == 0 ) {
            fh = &fhTable[i];
            break;
        }
    }
    if( fh == NULL ) {
        errno = ENFILE;
        return -1;
    }

    if( mode == (O_CREAT|O_WRONLY|O_TRUNC) ) {
        if( fs_createFile(fh, NULL) == -1 )
            return -1;
        fh->faddr = 0;  // 只写模式
        fh->droff = 0;  // 写入时不使用
        fh->foff  = 0;  // 写入时不使用
    }
    else if( mode == (O_CREAT|O_APPEND|O_WRONLY) ) {
        fctx_t* fctx = &fctxCache;
        if( fs_findFile(fctx, NULL) == -1 ) {
            if( fs_createFile(fh, NULL) == -1 )
                return -1;
            fh->faddr = 0;  // 只写模式
            fh->droff = 0;  // 写入时不使用
            fh->foff  = 0;  // 写入时不使用
        } else {
            u4_t begtag = fctx_begtag(fctx);
            fh->ino   = FSTAG_ino(begtag);
            fh->droff = 0;  // 写入时不使用
            fh->foff  = 0;  // 写入时不使用
            fh->faddr = 0;  // 只写模式
        }
    }
    else if( mode == O_RDONLY ) {
        fctx_t* fctx = &fctxCache;
        if( fs_findFile(fctx, NULL) == -1 )
            return -1;
        u4_t begtag = fctx_begtag(fctx);
        fh->ino   = FSTAG_ino(begtag);
        fh->droff = FSTAG_len(begtag);  // 完整 - 读取移动到下一个
        fh->foff  = 0;
        fh->faddr = fctx->faddr;
    }
    else {
        errno = EINVAL;
        return -1;
    }
    return fh - fhTable + OFF_FD;
}

/**
 * @brief 关闭文件
 * @param fd 文件描述符
 * @return 0表示成功，-1表示失败
 */
int fs_close(int fd) {
    fh_t* fh = fd2fh(fd);
    if( fh == NULL ) {
#if defined(CFG_linux)
        if( errno == EINVAL ) {
            return close(fd);
        }
#endif
        return -1;
    }
    memset(fh, 0, sizeof(*fh));
    return 0;
}

/**
 * @brief 获取文件状态
 * @param fn 文件名
 * @param st 状态结构体指针
 * @return 0表示成功，-1表示失败
 */
int fs_stat (str_t fn, struct stat* st) {
    int fnlen = checkFilename(fn);
#if defined(CFG_linux)
    if( fnlen == -1 ) {
        return stat(fn, st);
    }
#endif // defined(CFG_linux)
    if( fnlen <= 0 )
        return -1;
    if( fs_findFile(&fctxCache, NULL) == -1 )
        return -1;
    u2_t ino = FSTAG_ino(fctx_begtag(&fctxCache));
    u4_t ctim = rdFlash1(fctxCache.faddr+8);
    uint sz = 0;
    while( fs_findNextDataRecord(&fctxCache, ino) ) {
        u4_t endtag = fctx_endtag(&fctxCache);
        sz += FSTAG_len(endtag) - FSTAG_pad(endtag);
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = 0006;
    st->st_ino = ino;
    st->st_size = sz;
    st->st_ctim.tv_sec = ctim;
    return 0;
}

/**
 * @brief 移动文件指针
 * @param fd 文件描述符
 * @param offset 偏移量
 * @param whence 起始位置
 * @return 0表示成功，-1表示失败
 */
int fs_lseek (int fd, int offset, int whence) {
    fh_t* fh = fd2fh(fd);
    if( fh == NULL )
        return -1;
    if( fh->faddr == 0 ) {
        // 可写文件不支持seek - 文件系统只支持追加
        errno = EINVAL;
        return -1;
    }
    if( whence != SEEK_SET || offset < 0 ) {
        // 目前不支持 - 因为未使用
        errno = EINVAL;
        return -1;
    }
    u2_t ino = fh->ino;
    fctx_setTo(&fctxCache, flashFsBeg());
    int droff=0, foff=0;
    while( fs_findNextDataRecord(&fctxCache, ino) ) {
        u4_t endtag = fctx_endtag(&fctxCache);
        droff = FSTAG_len(endtag) - FSTAG_pad(endtag);
        foff += droff;
        if( foff >= offset ) {
            fh->faddr = fctxCache.faddr;
            fh->droff = droff - (foff-offset);
            fh->foff  = offset;
            return 0;
        }
    }
    fh->faddr = fctxCache.faddr;
    fh->droff = droff;
    fh->foff  = foff;
    return 0;
}

/**
 * @brief 同步文件系统缓存
 */
void fs_sync () {
#if defined(CFG_linux)
    sync();
#endif // defined(CFG_linux)
}

/**
 * @brief 验证记录完整性
 * @param fctx 文件上下文指针
 * @return inode号，-1表示验证失败
 */
static int fs_validateRecord (fctx_t* fctx) {
    u4_t begtag = fctx_begtag(fctx);
    u2_t ino    = FSTAG_ino(begtag);
    u4_t len    = FSTAG_len(begtag);
    u4_t pad    = FSTAG_pad(begtag);
    u4_t faddr  = fctx->faddr;
    if( faddr + 8 + len > flashFsMax() ||
        // 目前没有任何需要初始填充的情况
        len == 0 || pad )
        return -1;
    u4_t endtag = fctx_endtag(fctx);
    u4_t endpad = FSTAG_pad(endtag);
    u4_t endlen = FSTAG_len(endtag);
    u2_t dcrc   = FSTAG_crc(endtag);
    if( len != endlen || pad+endpad > len )
        return -1;
    u4_t off=0, cpycnt=0;
    u2_t xcrc = CRC_INI;

    while( off < len ) {
        cpycnt = len - off;
        if( cpycnt > AUXBUF_SZ4 )
            cpycnt = AUXBUF_SZ4;
        rdFlashN(faddr + off + 4, auxbuf.u4, cpycnt/4);
        xcrc = dataCrc(xcrc, auxbuf.u1, cpycnt);
        off += cpycnt;
    }
    if( xcrc != dcrc )
        return -1;
    fctx_setTo(fctx, faddr + len + 8);
    return ino;
}

/**
 * @brief 智能擦除闪存页
 * @param pgaddr 页起始地址
 * @param pagecnt 页数
 */
static void fs_smartErase (u4_t pgaddr, u4_t pagecnt) {
    while( pagecnt > 0 ) {
        u4_t off=0, len=AUXBUF_SZ4;
        while( off < FLASH_PAGE_SIZE ) {
            if( off + AUXBUF_SZ4 > FLASH_PAGE_SIZE )
                len = FLASH_PAGE_SIZE - off;
            u4_t lenw = len/4;
            sys_readFlash(pgaddr+off, auxbuf.u4, lenw);
            for( int wi=0; wi<lenw; wi++ ) {
                if( auxbuf.u4[wi] != FLASH_ERASED ) {
                    sys_eraseFlash(pgaddr, 1);
                    goto nextpage;
                }
            }
            off += len;
        }
      nextpage:
        pagecnt--;
        pgaddr += FLASH_PAGE_SIZE;
    }
}

/**
 * @brief 检查文件系统完整性
 * @return 0-原始闪存，1-分区恢复，2-需要GC
 */
int fs_ck () {
    u4_t magic[2];

    fsSection = 1;
    magic[1] = rdFlash1(FLASH_BEG_B);
    fsSection = 0;
    magic[0] = rdFlash1(FLASH_BEG_A);

    if( (magic[0] >> 16) != FLASH_MAGIC && (magic[1] >> 16) != FLASH_MAGIC ) {
        // 看起来是原始闪存 - 从未见过任何事务
        fs_smartErase(FLASH_BEG_A, FS_PAGE_CNT);
        fsSection = 0;
        flashWP = flashFsBeg()-4;
        wrFlash1wp(FLASH_MAGIC<<16);
        nextIno = 1;
        LOG(MOD_SYS|INFO, "FSCK 初始化原始闪存");
        return 0;
    }
    if( (magic[0] >> 16) == FLASH_MAGIC && (magic[1] >> 16) == FLASH_MAGIC ) {
        // 两个分区都有魔数 - 可能是中断的GC
        // 在较旧的分区上重新运行GC
        int d = magic[0] - magic[1];
        if( d != 1 && d != -1 ) {
            LOG(MOD_SYS|ERROR, "FSCK 发现奇怪的魔数: A=%08X B=%08X", magic[0], magic[1]);
        }
        fsSection = d < 0 ? 0 : 1;
        LOG(MOD_SYS|INFO, "FSCK 找到两个分区标记: %c%d -> %c",
            fsSection+'A', magic[fsSection] & 0xFFFF, (1^fsSection)+'A');
    } else {
        // 只有一个分区有魔数标记 - 使其成为当前分区
        assert( ((magic[0] >> 16) == FLASH_MAGIC) == !((magic[1] >> 16) == FLASH_MAGIC) );
        fsSection = (magic[0] >> 16) == FLASH_MAGIC ? 0 : 1;
        LOG(MOD_SYS|INFO, "FSCK 找到分区标记 %c%d",
            fsSection+'A', magic[fsSection] & 0xFFFF);
    }

    // 验证当前分区
    uint rcnt=0, maxino=0; int ino;
    fctx_setTo(&fctxCache, flashFsBeg());
    while( (ino = fs_validateRecord(&fctxCache)) >= 0 ) {
        if( ino > maxino ) maxino = ino;
        rcnt++;
    }
    nextIno = maxino+1;           // 不太可能的inode回滚！-> 紧急gc
    flashWP = fctxCache.faddr;
    LOG(MOD_SYS|INFO, "FSCK 分区 %c: %d 记录, %d 字节已用, %d 字节空闲",
        fsSection+'A', rcnt, flashWP - (flashFsBeg()-4), flashFsMax()-flashWP);

    u4_t fend = flashFsMax();
    u4_t faddr = fctxCache.faddr;
    while( faddr < fend ) {
        u4_t len = fend - faddr;
        if( len > AUXBUF_SZ4 )
            len = AUXBUF_SZ4;
        u4_t lenw = len/4;
        sys_readFlash(faddr, auxbuf.u4, lenw);
        for( int wi=0; wi<lenw; wi++ ) {
            if( auxbuf.u4[wi] != FLASH_ERASED ) {
                LOG(MOD_SYS|INFO, "FSCK 分区 %c 后面有脏闪存 - 需要GC", fsSection+'A');
                fs_gc(0);
                return 2;
            }
        }
        faddr += len;
    }
    // 我们发现了一组正常的记录，后面是
    // 直到分区结束的已擦除闪存
    // 智能擦除另一个分区
    fs_smartErase(fsSection ? FLASH_BEG_A : FLASH_BEG_B, FS_PAGE_CNT/2);
    LOG(MOD_SYS|INFO, "FSCK 分区 %c 后面是已擦除闪存 - 全部清除", fsSection+'A');
    return 1;
}

/**
 * @brief 获取文件系统信息
 * @param infop 信息结构体指针
 */
void fs_info(fsinfo_t* infop) {
    infop->fbasep   = sys_ptrFlash();
    infop->fbase    = FLASH_BEG_A;
    infop->pagecnt  = FS_PAGE_CNT & ~1;
    infop->pagesize = FLASH_PAGE_SIZE;
    infop->activeSection = fsSection;
    infop->gcCycles = rdFlash1(flashFsBeg()-4) & 0xFFFF;
    infop->used = flashWP - flashFsBeg() + 4;
    infop->free = flashFsMax() - flashWP;
    u4_t rcnt = 0;
    u4_t faddr = flashFsBeg();
    while( faddr < flashWP ) {
        faddr += FSTAG_len(rdFlash1(faddr)) + 8;
        rcnt++;
    }
    infop->records = rcnt;
    memcpy(infop->key, flashKey, sizeof(infop->key));
}

/**
 * @brief 执行垃圾回收
 * @param emergency 是否紧急GC
 */
void fs_gc (int emergency) {
    // Invalidate all open files
    // If any one of them survises GC it'll be reinstated
    for( int fdi=0; fdi < FS_MAX_FD; fdi++ ) {
        if( fhTable[fdi].ino != 0 )
            fhTable[fdi].ino |= MAX_INO+1;  // invalidate
    }

    u4_t faddrCont = flashFsBeg();
    u4_t faddrEnd = flashWP;

    fsSection ^= 1;
    flashWP = flashFsBeg() - 4;
    fsSection ^= 1;
    wrFlash1wp(rdFlash1(flashFsBeg()-4) + 1);
    nextIno = 1;

    while( faddrCont < faddrEnd ) {
        // Start a new collect phase - gather a set inodes
        // and follow them til the end of the FS log.
        // Cache is all zeros
        struct ino_cache inocache[16] = {{ 0 }};
        
        u1_t ucache = 0;
        u1_t overflow = 0;
        u4_t faddr = faddrCont;
        u4_t begtag;
        faddrCont = faddrEnd;
        for(; faddr < faddrEnd; faddr += 8 + FSTAG_len(begtag) ) {
            begtag = rdFlash1(faddr);
            u1_t cmd = FSTAG_cmd(begtag);
            if( cmd == FSCMD_DATA )
                continue;
            u4_t fncrc = rdFlash1(faddr+4);
            s1_t match = -1;
            // Is file in cache?
            for( u1_t ui=0; ui<ucache; ui++ ) {
                if( fncrc == inocache[ui].fncrc ) {
                    match = ui;
                    break;
                }
            }
            if( match >= 0 ) {
                // File is tracked in cache - update cache with effects of found command
                if( cmd == FSCMD_FILE ) {
                    // Override previous definition
                    inocache[match].faddrFile = faddr;
                    inocache[match].faddrRename = 0;
                    inocache[match].fncrc = fncrc;
                }
                else if( cmd == FSCMD_DELETE ) {
                    // Remove info from cache - keep list compact
                    ucache -= 1;
                    if( match != ucache )                   // last element remove?
                        inocache[match] = inocache[ucache]; // copy last down to fill the gap
                    memset(&inocache[ucache], 0, sizeof(inocache[ucache]));
                    // If all tracked files are gone and we had an overflow then
                    // we can stop here and restart at continuation faddr
                    if( ucache == 0 && overflow )
                        break;
                }
                else if( cmd == FSCMD_RENAME ) {
                    // Update to new name and remember the record containing the latest name
                    inocache[match].faddrRename = faddr;
                    inocache[match].fncrc = rdFlash1(faddr+8);
                }
            } else if( cmd == FSCMD_FILE && !overflow ) {
                // Found a new file not yet tracked and cache is likely not yet full
                if( ucache < SIZE_ARRAY(inocache) ) {
                    // Append this file to cache
                    inocache[ucache].faddrFile = faddr;
                    inocache[ucache].fncrc = fncrc;
                    ucache += 1;
                } else {
                    // Cache is full - this means we have to restart GC scan
                    // at this point again. For the set of files in the cache
                    // we keep following them until faddrEnd and see if they survive
                    // and if so in what state.
                    overflow = 1;
                    faddrCont = faddr;
                }
            }
        }
        // cache contains surviving files - copy them over to other section.
        for( u1_t ui=0; ui<ucache; ui++ ) {
            // Create a new FILE record
            struct ino_cache* c = &inocache[ui];
            u4_t a = c->faddrRename ? c->faddrRename : c->faddrFile;
            u4_t begtag = rdFlash1(a);
            u2_t len = FSTAG_len(begtag);
            rdFlashN(a, auxbuf.u4, len/4+2);
            if( c->faddrRename ) {
                // Extract new filename from last RENAME record
                // and copy to start of a new FILE record
                char* fn = (char*)&auxbuf.u4[3];
                char* fn2 = fn + strlen(fn)+1;
                len = strlen(fn2)+1;
                auxbuf.u4[1] = auxbuf.u4[2];             // fncrc
                auxbuf.u4[2] = rdFlash1(c->faddrFile+8); // ctim
                memmove(fn, fn2, len);
                while( (len&3) != 0 )
                    fn[len++] = 0;
                len = len+8;
                u2_t dcrc = dataCrc(CRC_INI, &auxbuf.u1[4], len);
                auxbuf.u4[len/4+1] = FSTAG_mkEnd(dcrc, len, 0);
            }
            if( emergency ) {
                char* fn = (char*)&auxbuf.u4[3];
                if( strstr(fn, ".log") != NULL )
                    continue; // do not copy over any log files
            }
            auxbuf.u4[0] = FSTAG_mkBeg(FSCMD_FILE, nextIno+ui, len, 0);
            wrFlashNwp(auxbuf.u4, len/4+2, 0);

            // Fixup open file table
            u2_t ino = FSTAG_ino(begtag);
            for( int fdi=0; fdi < FS_MAX_FD; fdi++ ) {
                if( fhTable[fdi].ino == ino+MAX_INO+1 )
                    fhTable[fdi].ino = nextIno+ui;
            }

            // Copy all DATA records for this ino
            a = c->faddrFile;
            do {
                begtag = rdFlash1(a);
                len = 8 + FSTAG_len(begtag);
                if( FSTAG_cmd(begtag) == FSCMD_DATA && FSTAG_ino(begtag) == ino ) {
                    u4_t off = 0;
                    while( off < len ) {
                        u4_t n = len-off;
                        if( n > AUXBUF_SZ4 )
                            n = AUXBUF_SZ4;
                        rdFlashN(a+off, auxbuf.u4, n/4);
                        if( off == 0 )
                            auxbuf.u4[0] = FSTAG_mkBeg(FSCMD_DATA, nextIno+ui, len-8, 0);
                        wrFlashNwp(auxbuf.u4, n/4, 0);
                        off += n;
                    }
                }
                a += len;
            } while( a < faddrEnd );
        }
        nextIno += ucache;
    }
    sys_eraseFlash(flashFsBeg()-4, FS_PAGE_CNT/2);
    fsSection ^= 1;

    for( int fdi=0; fdi < FS_MAX_FD; fdi++ ) {
        if( fhTable[fdi].ino != 0 &&
            fhTable[fdi].ino <= MAX_INO &&
            fhTable[fdi].faddr != 0 ) {
            if( fs_lseek(OFF_FD+fdi, fhTable[fdi].foff, SEEK_SET) == -1 ) {
                fhTable[fdi].ino |= MAX_INO+1;  // disable
            }
        }
    }
}

/**
 * @brief 擦除整个文件系统
 */
void fs_erase () {
    sys_iniFlash ();
    fs_smartErase (FLASH_BEG_A, FS_PAGE_CNT);
    fsSection = -1;  // 解锁fs_ini
}

/**
 * @brief 初始化文件系统
 * @param key 加密密钥
 * @return 0-原始闪存，1-分区恢复，2-需要GC
 */
int fs_ini (u4_t key[4]) {
    if( fsSection != -1 )
        return -1;
    sys_iniFlash();
    if( key ) {
        memcpy(flashKey, key, sizeof(flashKey));
    }
    return fs_ck();
}

// 命令名称数组
static const char* const CMD_NAMES[] = {
    "FILE", "DATA", "RENAME", "DELETE"
};

// 格式化字符串定义
#define FSDMP_ADDR_FMT "[%08X] "
#define FSDMP_PFX_FMT FSDMP_ADDR_FMT "%-6s ino=%-5d "
#define FSDMP_DFLT_FMT FSDMP_PFX_FMT "[%08X] %10d %s"
#define FSDMP_RENAME_FMT FSDMP_PFX_FMT "[%08X] [%08X] %s => %s"
#define FSDMP_DATA_SRT_FMT FSDMP_PFX_FMT "%04X|%-5d %02X %02X %02X %02X / %d"
#define FSDMP_DATA_LNG_FMT FSDMP_PFX_FMT "%04X|%-5d %02X %02X %02X %02X .. %02X %02X %02X %02X/%d"

int fs_dump (void (*_LOG)(u1_t mod_level, const char* fmt, ...)) {
    if( _LOG == NULL )
        _LOG = log_msg;

    fctx_t* fctx = &fctxCache;
    u4_t faddr = flashFsBeg();
    u4_t fend  = flashFsMax();
    u4_t magic = rdFlash1(faddr-4);

    _LOG(MOD_SYS|INFO, "Dump of flash section %c%d", fsSection+'A', magic & 0xFFFF);

    while( faddr < fend ) {
        fctx_setTo(fctx, faddr);
        u4_t begtag = fctx_begtag(fctx);
        u1_t cmd = FSTAG_cmd(begtag);
        u2_t ino = FSTAG_ino(begtag);
        u2_t len = FSTAG_len(begtag);
        u2_t pad = FSTAG_pad(begtag);

        if( begtag == decrypt1(faddr, FLASH_ERASED) )
            break;

        if( faddr + len + 8 >= flashFsMax() ) {
            _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "len=%d+8 reaches beyond end of flash section", faddr, len);
            break;
        }
        u4_t endtag = fctx_endtag(fctx);
        u2_t endlen = FSTAG_len(endtag);
        u2_t endpad = FSTAG_pad(endtag);
        u2_t dcrc   = FSTAG_crc(endtag);

        if( len != FSTAG_len(endtag) || pad + endpad > len || pad != 0 || len == 0 ) {
            _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "Mismatching len/beg/end/pad lengths: %d/%d pad=%d/%d len=%d",
                faddr, len, endlen, pad, endpad, len);
            break;
        }
        if( cmd != FSCMD_DATA ) {
            if( len > sizeof(auxbuf) ) {
                _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "Too large for auxbuf: len=%d > %d", faddr, len, (int)sizeof(auxbuf));
                break;
            }
            rdFlashN(faddr, auxbuf.u4, len/4+16);
            u2_t xcrc = dataCrc(CRC_INI, &auxbuf.u1[4], len);
            if( dcrc != xcrc ) {
                _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "Mismatching data CRC: found=0x%04X - expecting=0x%04X", faddr, dcrc, xcrc);
                break;
            }
            char* fn = (char*)&auxbuf.u1[12];
            if( cmd == FSCMD_RENAME ) {
                char* fn2 = fn + strlen(fn) + 1;
                _LOG(MOD_SYS|INFO, FSDMP_RENAME_FMT,
                    faddr, CMD_NAMES[cmd], ino, auxbuf.u4[1], auxbuf.u4[2], fn, fn2);
            } else {
                _LOG(MOD_SYS|INFO, FSDMP_DFLT_FMT,
                    faddr, CMD_NAMES[cmd], ino, auxbuf.u4[1], auxbuf.u4[2], fn);
            }
        } else {
            u4_t off = 0;
            u1_t d0[4] = {0}, dn[4] = {0};
            u4_t cpycnt = 0;
            u2_t xcrc = CRC_INI;
            while( off < len ) {
                cpycnt = len - off;
                if( cpycnt > AUXBUF_SZ4 )
                    cpycnt = AUXBUF_SZ4;
                rdFlashN(faddr + off + 4, auxbuf.u4, cpycnt/4);
                if( off == 0 )
                    memcpy(d0, &auxbuf.u1[0], 4);
                if( off+cpycnt >= len )
                    memcpy(dn, &auxbuf.u1[cpycnt-4], 4);
                xcrc = dataCrc(xcrc, auxbuf.u1, cpycnt);
                off += cpycnt;
            }
            if( xcrc != dcrc ) {
                _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "Mismatching data CRC: found=0x%04X - expecting=0x%04X", faddr, dcrc, xcrc);
                break;
            }
            if( len == 4 ) {
                _LOG(MOD_SYS|INFO, FSDMP_DATA_SRT_FMT,
                    faddr, CMD_NAMES[cmd], ino, dcrc, len, d0[0], d0[1], d0[2], d0[3], endpad);
            } else {
                _LOG(MOD_SYS|INFO, FSDMP_DATA_LNG_FMT,
                    faddr, CMD_NAMES[cmd], ino, dcrc, len,
                    d0[0], d0[1], d0[2], d0[3], dn[0], dn[1], dn[2], dn[3], endpad);
            }
        }
        faddr += len + 8;
    }
    int clean = 1;
    u4_t fsend = faddr;
    int dirtcnt = 0;
    while( faddr < fend ) {
        u4_t len = fend - faddr;
        if( len > AUXBUF_SZ4 )
            len = AUXBUF_SZ4;
        len /= 4;
        sys_readFlash(faddr, auxbuf.u4, len);
        int dirtbeg = -1, dirtend = -1;
        for( int i=0; i<len; i++ ) {
            if( auxbuf.u4[i] != FLASH_ERASED ) {
                if( dirtbeg == -1 )
                    dirtbeg = i;
                dirtend = i;
                clean = 0;
            }
        }
        if( dirtcnt < 200 && dirtbeg != -1 && (_LOG != log_msg || log_shallLog(MOD_SYS|ERROR)) ) {
            int di = dirtbeg;
            while( di < dirtend ) {
                dbuf_t dbuf;
                log_special(MOD_SYS|ERROR, &dbuf);
                int off = dbuf.pos;
                xprintf(&dbuf, "[%08X] DIRT: ", faddr);
                for( int i=0; i<8 && di<=dirtend; i++,di++ )
                    xprintf(&dbuf, "%08X ", auxbuf.u4[di]);
                xeos(&dbuf);
                if( _LOG == log_msg ) {
                    log_specialFlush(dbuf.pos);
                } else {
                    _LOG(MOD_SYS|ERROR, "%s", &dbuf.buf[off]);
                }
                dirtcnt++;
            }
        }
        faddr += len*4;
    }
    if( clean ) {
        _LOG(MOD_SYS|INFO, FSDMP_ADDR_FMT "End of file system - start of cleared flash", fsend);
    } else {
        _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "End of file system - rest of flash not clean", fsend);
    }
    return clean;
}


#if defined(CFG_linux) || defined(CFG_flashsim)
/**
 * @brief 文件系统命令行接口
 * @param cmdline 命令行字符串
 * @return 0表示成功，非0表示错误
 */
int fs_shell (char* cmdline) {
    char* argv[6];
    int argc=0, c;
    while(1) {
        while( (c = *cmdline) == ' ' )
            cmdline++;
        if( c == 0 )
            break;
        argv[argc++] = cmdline;
        if( argc == SIZE_ARRAY(argv)-1 )
            break;
        while( (c = *cmdline) != ' ' && c != 0 )
            cmdline++;
        if( c == 0 )
            break;
        *cmdline++ = 0;
    }
    argv[argc] = NULL;
    int err = 0;

    if( strcmp(argv[0], "?") == 0 || strcmp(argv[0], "h") == 0 || strcmp(argv[0], "help") == 0 ) {
        printf("fscmd command list:\n"
               " dump fsck ersase gc info (no arguments)\n"
               " unlink access stat read write (args: FILE)\n"
               " rename (args: OLDFILE NEWFILE)\n"
               );
        return 0;
    }
    if( strcmp(argv[0], "dump") == 0 ) {
        return fs_dump(NULL) == 1 ? 0 : 1;
    }
    if( strcmp(argv[0], "fsck") == 0 ) {
        return fs_ck();
    }
    if( strcmp(argv[0], "erase") == 0 ) {
        fs_erase();
        return 0;
    }
    if( strcmp(argv[0], "gc") == 0 ) {
        fs_gc(argv[1]==NULL?0:1);
        return 0;
    }
    if( strcmp(argv[0], "info") == 0 ) {
        fsinfo_t i;
        fs_info(&i);
        printf("fbase=0x%08X pagecnt=%d pagesize=0x%X\n"
               "active: section %c\n"
               "gc cycle: %d\n"
               "records=%d\n"
               "used=%d bytes\n"
               "free=%d bytes\n"
               "key=%08X-%08X-%08X-%08X\n",
               i.fbase, i.pagecnt, i.pagesize,
               i.activeSection+'A',
               i.gcCycles,
               i.records, i.used, i.free,
               i.key[0], i.key[1], i.key[2], i.key[3]);
        return 0;
    }
    if( strcmp(argv[0], "rename") == 0 ) {
        if( argc != 3 ) {
            printf("usage: rename OLDFILE NEWFILE\n");
            return 2;
        }
        err = fs_rename(argv[1], argv[2]);
        goto check_err;
    }
    if( strcmp(argv[0], "unlink") == 0 ) {
        if( argc != 2 ) {
            printf("usage: unlink FILE\n");
            return 2;
        }
        err = fs_unlink(argv[1]);
        goto check_err;
    }
    if( strcmp(argv[0], "access") == 0 ) {
        if( argc != 2 ) {
            printf("usage: access FILE\n");
            return 2;
        }
        err = fs_access(argv[1], F_OK);
        printf("File %s %s\n", argv[1], err==0?"exists":"does not exist");
        return err == -1 ? 1 : 0;
    }
    if( strcmp(argv[0], "stat") == 0 ) {
        if( argc != 2 ) {
            printf("usage: stat FILE\n");
            return 2;
        }
        struct stat st;
        if( (err = fs_stat(argv[1], &st)) == -1 )
            goto check_err;
        printf("ino=%d\n"      "ctim=%d\n"             "size=%d\n",
               (int)st.st_ino, (int)st.st_ctim.tv_sec, (int)st.st_size);
        return 0;
    }
    if( strcmp(argv[0], "read") == 0 ) {
        if( argc != 2 ) {
            printf("usage: read FILE\n");
            return 2;
        }
        u1_t buf[128];
        int n, fd = fs_open(argv[1], O_RDONLY);
        while( (n = fs_read(fd, buf, sizeof(buf))) > 0 )
            fwrite(buf, 1, n, stdout);
        fs_close(fd);
        return fd>=0 && n == 0 ? 0 : 1;
    }
    if( strcmp(argv[0], "write") == 0 ) {
        if( argc != 2 ) {
            printf("usage: write FILE\n");
            return 2;
        }
        u1_t buf[4*1024];
        int n, fd = err = fs_open(argv[1], O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR|S_IWUSR|S_IRGRP);
        while( err >= 0 && (n=fread(buf, 1, sizeof(buf), stdin)) > 0 )
            err = fs_write(fd, buf, n);
        goto check_err;
        //fs_close(fd);
        //return fd>=0 && n == 0 && err > 0 ? 0 : 1;
    }

    printf("Unknown command: %s\n", argv[0]);
    return 1;

  check_err:
    if( err >= 0 )
        return 0;
    printf("Failed: (%d) %s\n", errno, strerror(errno));
    return 1;
}

#else // defined(CFG_linux)

/**
 * @brief 空实现（非Linux平台）
 * @param cmdline 命令行字符串
 * @return 0
 */
int fs_shell (char* cmdline) {
    return 0;
}

#endif // defined(CFG_linux)
