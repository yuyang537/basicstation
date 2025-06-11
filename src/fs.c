/*
 * BasicStation 文件系统模拟模块 - 基于Flash的日志结构化文件系统
 * 
 * 主要功能：
 * 1. Flash存储管理 - 基于双分区的日志结构化存储系统
 * 2. 文件操作接口 - 提供标准POSIX兼容的文件操作API
 * 3. 垃圾回收机制 - 自动空间回收和数据迁移系统
 * 4. 数据加密保护 - 基于密钥的Flash数据加密存储
 * 5. 记录管理系统 - 基于标签的记录格式和完整性检验
 * 
 * 设计特点：
 * - 日志结构化：所有操作追加写入，支持快速恢复
 * - 双分区设计：A/B分区轮换，保证数据安全性
 * - 垃圾回收：Copy-GC算法，只复制活跃数据
 * - 数据加密：位置相关的XOR加密保护
 * - 完整性校验：CRC校验保证数据完整性
 * - POSIX兼容：标准文件操作接口，易于移植
 * 
 * --- Revised 3-Clause BSD License ---
 * Copyright Semtech Corporation 2022. All rights reserved.
 */

#include <stdio.h>                       // 标准输入输出接口
#include <fcntl.h>                       // 文件控制接口

#if defined(CFG_linux) || defined(CFG_flashsim)
#include <sys/stat.h>                    // 文件状态结构
#include <errno.h>                       // 错误码定义
#include <stdarg.h>                      // 可变参数支持
#include <unistd.h>                      // UNIX标准接口
#endif // defined(CFG_linux)
#include "s2conf.h"                      // 系统配置定义
#include "rt.h"                          // 运行时支持
#include "kwcrc.h"                       // CRC校验算法
#include "uj.h"                          // JSON解析支持
#include "fs.h"                          // 文件系统接口定义

// ================================================================================
// Flash文件系统存储布局设计
// ================================================================================
//
// Flash组织结构：
// - 以32位字为基本存储单元
// - 整个Flash分区划分为两个相等的分区(A/B)
// - 当一个分区写满时，垃圾回收将活跃数据复制到另一个分区
// 
// 分区结构：
// - 分区头：魔数(FLASH_MAGIC) + GC序列计数器
// - 每次GC操作递增序列计数器
// - 通过比较两个分区的魔数可确定哪个分区更新
//
// 记录格式：
// [开始标签] ... [结束标签]
//
// 记录可以正向和反向遍历：
// - 开始标签/结束标签都包含长度信息
// - 开始标签携带inode编号
// - inode编号递增分配，GC时重新标记
// - 结束标签包含CRC校验码
//
// 记录类型：
// FILE/DELETE: [开始标签] [文件名CRC] [创建时间] [文件名'\0'{1,4}] [结束标签]
// RENAME:      [开始标签] [旧名CRC] [新名CRC] [旧名'\0'新名'\0'{1,4}] [结束标签]
// DATA:        [开始标签] [数据0] [数据1] ... [数据N'\0'{0,3}] [结束标签]
//
// 填充说明：
// - DATA记录的填充字节数在结束标签中指示
// - 开始标签的填充字节始终为0
// - FILE/DELETE/RENAME的结束标签填充字节为0
//
// GC结束标记：
// - 使用FILE记录标记，文件名为特殊字符串002f2f00
// - fncrc=0, ctime=0
//



// ================================================================================
// 文件系统常量定义
// ================================================================================

// 文件句柄偏移量 - 使文件句柄与系统句柄区分开（安全考虑）
#define OFF_FD 0x10000              // 文件描述符偏移量，避免与系统FD冲突
#define MAX_INO 0x3FFF              // 最大inode编号（14位）
#define CRC_INI 0x1234              // CRC计算初始值

// Flash分区布局定义
#define FLASH_MAGIC 0xA4B5          // Flash分区魔数
#define FLASH_BEG_A (FLASH_ADDR  + FLASH_PAGE_SIZE * FS_PAGE_START)     // A分区起始地址
#define FLASH_BEG_B (FLASH_BEG_A + FLASH_PAGE_SIZE * (FS_PAGE_CNT/2))   // B分区起始地址
#define FLASH_END_A (FLASH_BEG_B)                                       // A分区结束地址
#define FLASH_END_B (FLASH_BEG_B + FLASH_PAGE_SIZE * (FS_PAGE_CNT/2))   // B分区结束地址

// ================================================================================
// 文件系统标签操作宏定义
// ================================================================================

// 标签字段提取函数 - 从32位标签中提取各个字段
static inline u1_t FSTAG_cmd(u4_t v) { return  (v >> 30) & 3; }      // 提取命令类型（2位）
static inline u2_t FSTAG_ino(u4_t v) { return ((v >> 16) & MAX_INO); } // 提取inode号（14位）
static inline u2_t FSTAG_crc(u4_t v) { return ((v >> 16) & 0xFFFF); } // 提取CRC值（16位）
static inline u2_t FSTAG_len(u4_t v) { return   v & 0xFFFC; }         // 提取长度（14位，4字节对齐）
static inline u1_t FSTAG_pad(u4_t v) { return   v & 3; }              // 提取填充字节数（2位）

// 标签构造函数 - 将各字段组合成32位标签
// 功能：构造开始标签 - 包含命令类型、inode号、长度、填充信息
static inline u4_t FSTAG_mkBeg(u1_t cmd, u2_t ino, u2_t len, u1_t pad) {
    return (cmd<<30) | ((ino&MAX_INO)<<16) | (len&0xFFFC) | (pad&3);
}

// 功能：构造结束标签 - 包含CRC校验、长度、填充信息
static inline u4_t FSTAG_mkEnd(u2_t crc, u2_t len, u1_t pad) {
    return ((crc&0xFFFF)<<16) | (len&0xFFFC)|(pad&3);
}

// 文件系统命令类型定义
#define FSCMD_FILE   0              // 文件创建命令
#define FSCMD_DATA   1              // 数据写入命令
#define FSCMD_RENAME 2              // 文件重命名命令
#define FSCMD_DELETE 3              // 文件删除命令

// ================================================================================
// 文件系统核心数据结构
// ================================================================================

// 文件上下文结构 - 用于记录遍历和操作
typedef struct fctx {
    u4_t faddr;                     // 当前记录的Flash地址
    u4_t begtag;                    // 开始标签值
    u4_t endtag;                    // 结束标签值
} fctx_t;

// 文件句柄结构 - 描述打开的文件状态
typedef struct fh {
    u2_t ino;                       // 文件inode编号
    u2_t droff;                     // 数据记录内偏移量
    u4_t faddr;                     // 当前数据记录Flash地址
    u4_t foff;                      // 文件读取偏移量
} fh_t;

// inode缓存结构 - 垃圾回收时使用的文件信息缓存
struct ino_cache {
    u4_t faddrFile;                 // 创建FILE记录的地址
    u4_t faddrRename;               // 最后一次重命名记录的地址
    u4_t fncrc;                     // 文件名CRC值
};

// ================================================================================
// 辅助缓冲区定义
// ================================================================================

// 辅助缓冲区大小计算（支持最大文件名长度的2倍，4字节对齐）
#define AUXBUF_SZW (2*((FS_MAX_FNSIZE+3)/4))    // 以32位字为单位的大小
#define AUXBUF_SZ4 (4*AUXBUF_SZW)               // 以字节为单位的大小

// 多用途辅助缓冲区 - 用于临时数据存储和格式转换
static union auxbuf {
    u4_t u4[AUXBUF_SZW];            // 按32位字访问
    u1_t u1[AUXBUF_SZ4];            // 按字节访问
} auxbuf;

// ================================================================================
// 全局状态变量
// ================================================================================

static fctx_t fctxCache;            // 文件上下文缓存
static u4_t   flashKey[4];          // Flash加密密钥（4个32位字）
static u4_t   flashWP;              // Flash写指针（当前写入位置）
static u2_t   nextIno;              // 下一个可用的inode编号
static s1_t   fsSection = -1;       // 当前活跃分区标识（0|1，-1表示未初始化）
static const char DEFAULT_CWD[] = "/s2/";  // 默认工作目录
static str_t  cwd = DEFAULT_CWD;    // 当前工作目录指针
static fh_t   fhTable[FS_MAX_FD];   // 文件句柄表

// ================================================================================
// Flash分区管理函数
// ================================================================================

// 功能：获取当前活跃分区的起始地址
// 返回值：当前分区数据区起始地址（跳过4字节的分区头）
// 调用时机：所有Flash读写操作前确定地址范围
static inline u4_t flashFsBeg() {
    return fsSection ? FLASH_BEG_B+4 : FLASH_BEG_A+4;
}

// 功能：获取当前活跃分区的最大地址
// 返回值：当前分区的结束地址
// 调用时机：检查Flash空间边界时
static inline u4_t flashFsMax() {
    return fsSection ? FLASH_END_B : FLASH_END_A;
}

// ================================================================================
// 数据加密/解密系统
// ================================================================================

// 功能：单字数据加密 - 基于Flash地址的位置相关加密
// 参数：faddr - Flash地址，data - 待加密数据
// 返回值：加密后的数据
// 调用时机：向Flash写入数据前进行加密
// 加密算法：使用Flash地址选择密钥进行XOR加密
static u4_t encrypt1 (u4_t faddr, u4_t data) {
    return data ^ flashKey[(faddr>>2) & 3];  // 根据地址选择4个密钥之一进行XOR
}

// 功能：单字数据解密 - 解密函数与加密函数相同（XOR特性）
// 参数：faddr - Flash地址，data - 待解密数据
// 返回值：解密后的数据
// 调用时机：从Flash读取数据后进行解密
static u4_t decrypt1 (u4_t faddr, u4_t data) {
    return encrypt1(faddr, data);            // XOR加密的逆运算就是自身
}

// 功能：多字数据加密 - 批量加密连续的数据块
// 参数：faddr - 起始Flash地址，data - 数据缓冲区，u4cnt - 32位字数量
// 返回值：无（就地加密）
// 调用时机：批量写入Flash数据前
static void encryptN (u4_t faddr, u4_t* data, uint u4cnt) {
    for( uint u=0; u<u4cnt; u++ ) {
        data[u] = encrypt1(faddr+u*4, data[u]);  // 逐字加密，地址递增
    }
}

// 功能：多字数据解密 - 批量解密连续的数据块
// 参数：faddr - 起始Flash地址，data - 数据缓冲区，u4cnt - 32位字数量
// 返回值：无（就地解密）
// 调用时机：批量读取Flash数据后
static void decryptN (u4_t faddr, u4_t* data, uint u4cnt) {
    for( uint u=0; u<u4cnt; u++ ) {
        data[u] = decrypt1(faddr+u*4, data[u]);  // 逐字解密，地址递增
    }
}

// ================================================================================
// Flash读写接口函数
// ================================================================================

// 功能：写入单个32位字到Flash - 带地址检查和加密
// 参数：faddr - 目标Flash地址，data - 待写入数据
// 返回值：无
// 调用时机：写入单个数据字时
void wrFlash1 (u4_t faddr, u4_t data) {
    assert(faddr < (faddr >= FLASH_BEG_B ? FLASH_END_B : FLASH_END_A));  // 检查地址边界
    data = encrypt1(faddr, data);        // 加密数据
    sys_writeFlash(faddr, &data, 1);     // 调用系统接口写入
}

// 功能：按写指针位置写入数据 - 自动更新写指针
// 参数：data - 待写入数据
// 返回值：无
// 调用时机：顺序写入文件系统记录时
static void wrFlash1wp (u4_t data) {
    u4_t faddr = flashWP;                // 获取当前写指针
    wrFlash1(faddr, data);               // 写入数据
    flashWP = faddr + 4;                 // 更新写指针到下一个位置
}

// 功能：从Flash读取单个32位字 - 带解密
// 参数：faddr - 源Flash地址
// 返回值：解密后的数据
// 调用时机：读取单个数据字时
u4_t rdFlash1 (u4_t faddr) {
    u4_t data;
    assert(faddr < flashFsMax());        // 检查地址边界
    sys_readFlash(faddr, &data, 1);      // 调用系统接口读取
    return decrypt1(faddr, data);        // 解密并返回数据
}

// 功能：批量写入多个32位字到Flash - 带加密和可选数据保持
// 参数：faddr - 目标Flash地址，daddr - 数据缓冲区，u4cnt - 字数量，keepData - 是否保持原数据
// 返回值：无
// 调用时机：批量写入大量数据时
void wrFlashN (u4_t faddr, u4_t* daddr, uint u4cnt, int keepData) {
    assert(faddr + u4cnt*4 <= (faddr >= FLASH_BEG_B ? FLASH_END_B : FLASH_END_A));  // 检查边界
    encryptN(faddr, daddr, u4cnt);       // 批量加密数据
    sys_writeFlash(faddr, daddr, u4cnt); // 调用系统接口批量写入
    if( keepData )                       // 如果需要保持原数据
        decryptN(faddr, daddr, u4cnt);   // 恢复原数据（解密回来）
}

// 功能：按写指针批量写入数据 - 自动更新写指针
// 参数：daddr - 数据缓冲区，u4cnt - 字数量，keepData - 是否保持原数据
// 返回值：无
// 调用时机：顺序批量写入文件系统记录时
static void wrFlashNwp (u4_t* daddr, uint u4cnt, int keepData) {
    u4_t faddr = flashWP;                // 获取当前写指针
    wrFlashN(faddr, daddr, u4cnt, keepData);  // 批量写入
    flashWP = faddr + u4cnt*4;           // 更新写指针
}

// 功能：从Flash批量读取数据 - 带解密
// 参数：faddr - 源Flash地址，daddr - 数据缓冲区，u4cnt - 字数量
// 返回值：无
// 调用时机：批量读取大量数据时
void rdFlashN(u4_t faddr, u4_t* daddr, uint u4cnt) {
    assert(faddr + u4cnt*4 <= flashFsMax());  // 检查地址边界
    sys_readFlash(faddr, daddr, u4cnt);  // 调用系统接口批量读取
    decryptN(faddr, daddr, u4cnt);       // 批量解密数据
}


// ================================================================================
// 文件上下文操作函数
// ================================================================================

// 功能：获取文件上下文的开始标签
// 参数：fctx - 文件上下文指针
// 返回值：开始标签值
// 调用时机：需要解析记录信息时
static u4_t fctx_begtag (fctx_t* fctx) {
    if( fctx->begtag == 0 )              // 如果缓存为空
        fctx->begtag = rdFlash1(fctx->faddr);  // 从Flash读取开始标签
    return fctx->begtag;                 // 返回缓存的标签值
}

// 功能：获取文件上下文的结束标签
// 参数：fctx - 文件上下文指针
// 返回值：结束标签值
// 调用时机：需要获取记录长度或CRC信息时
static u4_t fctx_endtag (fctx_t* fctx) {
    if( fctx->endtag == 0 ) {            // 如果缓存为空
        u4_t begtag = fctx_begtag(fctx); // 获取开始标签
        u2_t len = FSTAG_len(begtag);    // 提取记录长度
        // 从记录末尾读取结束标签
        fctx->endtag = rdFlash1(fctx->faddr + len + 4);
    }
    return fctx->endtag;                 // 返回缓存的标签值
}

// ================================================================================
// CRC校验和哈希计算
// ================================================================================

// 功能：计算数据CRC校验值 - 使用kwcrc算法
// 参数：crc - 初始CRC值，data - 数据缓冲区，len - 数据长度
// 返回值：计算后的CRC值
// 调用时机：写入记录前计算校验值，验证记录完整性时
static u2_t dataCrc (u2_t crc, const u1_t* data, uint len) {
    for( uint i=0; i<len; i++ ) {
        crc = kwcrc(crc, data[i]);       // 逐字节计算CRC
    }
    return crc;                          // 返回最终CRC值
}

// 功能：计算文件名CRC哈希值 - 用于快速文件查找
// 参数：fn - 文件名字符串
// 返回值：文件名的CRC哈希值
// 调用时机：文件查找、创建、重命名操作时
static u4_t fnCrc (const char* fn) {
    return dataCrc(CRC_INI, (const u1_t*)fn, strlen(fn));  // 计算文件名CRC
}

// ================================================================================
// Flash空间管理
// ================================================================================

// 功能：检查Flash空间是否充足
// 参数：reqbytes - 请求的字节数
// 返回值：1表示空间不足，0表示空间充足
// 调用时机：写入数据前检查可用空间
static int isFlashFull (u4_t reqbytes) {
    u4_t flashFsSize = flashFsMax() - flashFsBeg();     // 计算分区总大小
    u4_t flashFsUsed = flashWP - flashFsBeg();          // 计算已使用空间
    u4_t flashFsFree = flashFsSize - flashFsUsed;       // 计算剩余空间
    
    // 保留一定的安全边界，避免空间完全耗尽
    if( flashFsFree < reqbytes + 512 ) {                 // 预留512字节缓冲
        if( flashFsUsed*2 < flashFsSize && !emergency )  // 如果使用率<50%且非紧急情况
            return 1;                                    // 建议进行垃圾回收
        if( flashFsFree < reqbytes + 64 )                // 如果真的空间不足
            return 1;                                    // 必须进行垃圾回收
    }
    return 0;                                            // 空间充足
}

// ================================================================================
// 文件名处理和验证
// ================================================================================

// 功能：设置文件上下文到指定地址
// 参数：fctx - 文件上下文，faddr - Flash地址
// 返回值：无
// 调用时机：开始处理新记录时
static void fctx_setTo (fctx_t* fctx, u4_t faddr) {
    fctx->faddr = faddr;                 // 设置地址
    fctx->begtag = 0;                    // 清空标签缓存
    fctx->endtag = 0;                    // 强制重新读取
}

// 功能：检查和规范化文件名
// 参数：fn - 输入文件名
// 返回值：规范化后的文件名长度，0表示失败
// 调用时机：所有文件操作前的文件名验证
static int checkFilename (const char* fn) {
    if( fn == NULL ) {                   // 空指针检查
        errno = EFAULT;                  // 设置错误码
        return 0;
    }
    char* wb = (char*)&auxbuf.u4[3];     // 使用辅助缓冲区
    int fnlen = auxbuf.u4[0] = fs_fnNormalize(fn, wb, FS_MAX_FNSIZE);  // 规范化文件名
#if defined(CFG_linux)
    // Linux环境下的特殊处理：检查是否为s2路径
    if( strncmp(wb, "/s2/", 3) != 0 || (wb[3] != 0 && wb[3] != '/') ) {
        return -1;                       // 分支到Linux文件系统
    }
#endif // defined(CFG_linux)
    return fnlen;                        // 返回规范化后的长度
}

// ================================================================================
// 文件查找系统
// ================================================================================

// 功能：在Flash中查找指定文件
// 参数：fctx - 文件上下文，fn - 文件名（NULL表示使用缓存的文件名）
// 返回值：0表示找到，-1表示未找到
// 调用时机：文件打开、状态查询等操作时
// 查找策略：从Flash末尾向前扫描，处理重命名和删除操作
static int fs_findFile (fctx_t* fctx, const char* fn) {
    int fnlen;
    if( fn != NULL ) {                   // 如果提供了文件名
        fnlen = checkFilename(fn);       // 检查和规范化文件名
        if( fnlen == 0 )                 // 文件名无效
            return -1;
    } else {
        fnlen = auxbuf.u4[0];            // 调用者已经进行了checkFilename
    }
    
    char* wb = (char*)&auxbuf.u1[12];    // 获取规范化后的文件名
    u4_t seekcrc = auxbuf.u4[1] = fnCrc(wb);  // 计算文件名CRC用于快速匹配
    u4_t faddr = flashWP;                // 从Flash写指针开始（最新记录）
    
    // 从后向前遍历所有记录
    while( faddr > flashFsBeg() ) {
        u4_t endtag = rdFlash1(faddr-4); // 读取前一个记录的结束标签
        u4_t len = FSTAG_len(endtag);    // 获取记录长度
        faddr -= len+8;                  // 计算记录起始地址
        u4_t begtag = rdFlash1(faddr);   // 读取开始标签
        u1_t cmd = FSTAG_cmd(begtag);    // 获取命令类型
        
        if( cmd == FSCMD_DATA )          // 跳过数据记录
            continue;
            
        u4_t fncrc = rdFlash1(faddr+4);  // 读取记录中的文件名CRC
        if( seekcrc == fncrc ) {         // CRC匹配，可能找到目标文件
            if( cmd == FSCMD_RENAME || cmd == FSCMD_DELETE )
                break;                   // 文件已被重命名或删除
            assert(cmd == FSCMD_FILE);   // 应该是文件创建记录
            // 找到文件，设置上下文
            fctx_setTo(fctx, faddr);
            fctx->begtag = begtag;
            fctx->endtag = endtag;
            return 0;                    // 成功找到
        }
        
        // 处理重命名操作：如果当前记录是重命名且新名匹配，则继续查找旧名
        if( cmd == FSCMD_RENAME && seekcrc == rdFlash1(faddr+8) )
            seekcrc = fncrc;             // 更新查找目标为旧文件名CRC
    }
    
    errno = ENOENT;                      // 文件不存在
    return -1;
}

static int fs_handleFile (const char* fn, const char* fn2, u1_t cmd, u2_t ino) {
    char* wb = (char*)&auxbuf.u1[12];
    int fnlen;
    if( fn == NULL ) {
        // Some previous operation already put normalized filename into auxbuf
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

static int fs_createFile (fh_t* fh, const char* fn) {
    u4_t faddr = flashWP;
    if( fs_handleFile(fn, NULL, FSCMD_FILE, nextIno++) == -1 )
        return -1;
    u4_t begtag = auxbuf.u4[0];
    fh->faddr = faddr;
    fh->ino   = FSTAG_ino(begtag);
    fh->droff = FSTAG_len(begtag);  // full - read moves on to next
    fh->foff  = 0;
    return 0;
}


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
    //u2_t  dcrc = dataCrc(dataCrc(CRC_INI, data, dlen), auxbuf.u1, dlenCeil-dlen);
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
            auxbuf.u4[0+cpylen4] = 0;  // proactively padding
            auxbuf.u4[1+cpylen4] = FSTAG_mkEnd(dcrc, dlenCeil, dlenCeil-dlen);
            tend = 1;
        }
        memcpy(tb, data+doff-cpylen, cpylen);
        wrFlashNwp(&auxbuf.u4[tbeg], (1-tbeg)+cpylen4+tend, 0);
        tbeg = 1;
    }
    return dlen;
}


int fs_chdir (str_t dir) {
    // Normalize dir
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
        fh->faddr = 0;  // WRONLY
        fh->droff = 0;  // not used during write
        fh->foff  = 0;  // not used during write
    }
    else if( mode == (O_CREAT|O_APPEND|O_WRONLY) ) {
        fctx_t* fctx = &fctxCache;
        if( fs_findFile(fctx, NULL) == -1 ) {
            if( fs_createFile(fh, NULL) == -1 )
                return -1;
            fh->faddr = 0;  // WRONLY
            fh->droff = 0;  // not used during write
            fh->foff  = 0;  // not used during write
        } else {
            u4_t begtag = fctx_begtag(fctx);
            fh->ino   = FSTAG_ino(begtag);
            fh->droff = 0;  // not used during write
            fh->foff  = 0;  // not used during write
            fh->faddr = 0;  // WRONLY
        }
    }
    else if( mode == O_RDONLY ) {
        fctx_t* fctx = &fctxCache;
        if( fs_findFile(fctx, NULL) == -1 )
            return -1;
        u4_t begtag = fctx_begtag(fctx);
        fh->ino   = FSTAG_ino(begtag);
        fh->droff = FSTAG_len(begtag);  // full - read moves on to next
        fh->foff  = 0;
        fh->faddr = fctx->faddr;
    }
    else {
        errno = EINVAL;
        return -1;
    }
    return fh - fhTable + OFF_FD;
}


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


int fs_lseek (int fd, int offset, int whence) {
    fh_t* fh = fd2fh(fd);
    if( fh == NULL )
        return -1;
    if( fh->faddr == 0 ) {
        // no seek on writable files - fs can do only append
        errno = EINVAL;
        return -1;
    }
    if( whence != SEEK_SET || offset < 0 ) {
        // not supported right now - because not used
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


void fs_sync () {
#if defined(CFG_linux)
    sync();
#endif // defined(CFG_linux)
}


static int fs_validateRecord (fctx_t* fctx) {
    u4_t begtag = fctx_begtag(fctx);
    u2_t ino    = FSTAG_ino(begtag);
    u4_t len    = FSTAG_len(begtag);
    u4_t pad    = FSTAG_pad(begtag);
    u4_t faddr  = fctx->faddr;
    if( faddr + 8 + len > flashFsMax() ||
        // right we don't have anything that requires initial padding
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


// return:
//   0 - pristine flash
//   1 - section recovered as is
//   2 - GC was required
//
int fs_ck () {
    u4_t magic[2];

    fsSection = 1;
    magic[1] = rdFlash1(FLASH_BEG_B);
    fsSection = 0;
    magic[0] = rdFlash1(FLASH_BEG_A);

    if( (magic[0] >> 16) != FLASH_MAGIC && (magic[1] >> 16) != FLASH_MAGIC ) {
        // Looks pristine - never seen any transactions
        fs_smartErase(FLASH_BEG_A, FS_PAGE_CNT);
        fsSection = 0;
        flashWP = flashFsBeg()-4;
        wrFlash1wp(FLASH_MAGIC<<16);
        nextIno = 1;
        LOG(MOD_SYS|INFO, "FSCK initializing pristine flash");
        return 0;
    }
    if( (magic[0] >> 16) == FLASH_MAGIC && (magic[1] >> 16) == FLASH_MAGIC ) {
        // Both sections with magics - probably aborted GC
        // Rerun GC on older section.
        int d = magic[0] - magic[1];
        if( d != 1 && d != -1 ) {
            LOG(MOD_SYS|ERROR, "FSCK discovered strange magics: A=%08X B=%08X", magic[0], magic[1]);
        }
        fsSection = d < 0 ? 0 : 1;
        LOG(MOD_SYS|INFO, "FSCK found two section markers: %c%d -> %c",
            fsSection+'A', magic[fsSection] & 0xFFFF, (1^fsSection)+'A');
    } else {
        // Only one section has a magic marker - make it current.
        assert( ((magic[0] >> 16) == FLASH_MAGIC) == !((magic[1] >> 16) == FLASH_MAGIC) );
        fsSection = (magic[0] >> 16) == FLASH_MAGIC ? 0 : 1;
        LOG(MOD_SYS|INFO, "FSCK found section marker %c%d",
            fsSection+'A', magic[fsSection] & 0xFFFF);
    }

    // Validate current section
    uint rcnt=0, maxino=0; int ino;
    fctx_setTo(&fctxCache, flashFsBeg());
    while( (ino = fs_validateRecord(&fctxCache)) >= 0 ) {
        if( ino > maxino ) maxino = ino;
        rcnt++;
    }
    nextIno = maxino+1;           // unlikely ino rollover! -> emergency gc
    flashWP = fctxCache.faddr;
    LOG(MOD_SYS|INFO, "FSCK section %c: %d records, %d bytes used, %d bytes free",
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
                LOG(MOD_SYS|INFO, "FSCK section %c followed by dirty flash - GC required.", fsSection+'A');
                fs_gc(0);
                return 2;
            }
        }
        faddr += len;
    }
    // We found a set of sane records followed by
    // erased flash until section end.
    // Do a smart erase of the other section
    fs_smartErase(fsSection ? FLASH_BEG_A : FLASH_BEG_B, FS_PAGE_CNT/2);
    LOG(MOD_SYS|INFO, "FSCK section %c followed by erased flash - all clear.", fsSection+'A');
    return 1;
}


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


// ================================================================================
// 文件系统初始化和管理函数
// ================================================================================

// 功能：文件系统擦除操作 - 完全重置文件系统
// 参数：无
// 返回值：无
// 调用时机：系统格式化或完全重置时
void fs_erase () {
    sys_iniFlash ();                     // 初始化Flash硬件
    // sys_eraseFlash(FLASH_BEG_A, FS_PAGE_CNT);  // 原始的整块擦除
    fs_smartErase (FLASH_BEG_A, FS_PAGE_CNT);    // 智能擦除，跳过已擦除页
    fsSection = -1;                      // 解锁fs_ini，允许重新初始化
}

// 功能：文件系统初始化 - 设置加密密钥并检查文件系统状态
// 参数：key - 4个32位加密密钥数组
// 返回值：0表示成功，-1表示失败
// 调用时机：系统启动时进行文件系统初始化
int fs_ini (u4_t key[4]) {
    if( fsSection != -1 )                // 检查是否已经初始化
        return -1;                       // 已初始化，拒绝重复初始化
    sys_iniFlash();                      // 初始化Flash硬件接口
    if( key ) {                          // 如果提供了加密密钥
        memcpy(flashKey, key, sizeof(flashKey));  // 复制密钥到全局变量
        // LOG(MOD_SYS|INFO, "FS_KEY = %08X-%08X-%08X-%08X", key[0], key[1], key[2], key[3])
    }
    return fs_ck();                      // 检查并恢复文件系统状态
}

// ================================================================================
// 文件系统调试和诊断
// ================================================================================

// 调试输出格式定义 - 用于fs_dump函数的格式化输出
static const char* const CMD_NAMES[] = {
    "FILE", "DATA", "RENAME", "DELETE"  // 记录类型名称数组
};
#define FSDMP_ADDR_FMT "[%08X] "         // 地址格式
#define FSDMP_PFX_FMT FSDMP_ADDR_FMT "%-6s ino=%-5d "  // 前缀格式
#define FSDMP_DFLT_FMT FSDMP_PFX_FMT "[%08X] %10d %s"  // 默认记录格式
#define FSDMP_RENAME_FMT FSDMP_PFX_FMT "[%08X] [%08X] %s => %s"  // 重命名记录格式
#define FSDMP_DATA_SRT_FMT FSDMP_PFX_FMT "%04X|%-5d %02X %02X %02X %02X / %d"     // 短数据记录格式
#define FSDMP_DATA_LNG_FMT FSDMP_PFX_FMT "%04X|%-5d %02X %02X %02X %02X .. %02X %02X %02X %02X/%d"  // 长数据记录格式

// 功能：转储文件系统内容 - 详细输出所有记录用于调试
// 参数：_LOG - 日志输出函数指针，NULL时使用默认log_msg
// 返回值：1表示Flash区域干净，0表示存在脏数据
// 调用时机：系统调试、故障诊断时
int fs_dump (void (*_LOG)(u1_t mod_level, const char* fmt, ...)) {
    if( _LOG == NULL )                   // 如果未提供日志函数
        _LOG = log_msg;                  // 使用默认日志函数

    fctx_t* fctx = &fctxCache;           // 使用全局文件上下文缓存
    u4_t faddr = flashFsBeg();           // 从分区开始地址
    u4_t fend  = flashFsMax();           // 到分区结束地址
    u4_t magic = rdFlash1(faddr-4);      // 读取分区魔数

    // 输出分区头信息
    _LOG(MOD_SYS|INFO, "Dump of flash section %c%d", fsSection+'A', magic & 0xFFFF);

    // 遍历所有记录
    while( faddr < fend ) {
        fctx_setTo(fctx, faddr);         // 设置文件上下文到当前地址
        u4_t begtag = fctx_begtag(fctx); // 读取开始标签
        u1_t cmd = FSTAG_cmd(begtag);    // 提取命令类型
        u2_t ino = FSTAG_ino(begtag);    // 提取inode编号
        u2_t len = FSTAG_len(begtag);    // 提取记录长度
        u2_t pad = FSTAG_pad(begtag);    // 提取填充字节数

        // 检查是否到达已擦除区域
        if( begtag == decrypt1(faddr, FLASH_ERASED) )
            break;

        // 检查记录长度是否超出边界
        if( faddr + len + 8 >= flashFsMax() ) {
            _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "len=%d+8 reaches beyond end of flash section", faddr, len);
            break;
        }
        
        u4_t endtag = fctx_endtag(fctx);  // 读取结束标签
        u2_t endlen = FSTAG_len(endtag);  // 提取结束标签长度
        u2_t endpad = FSTAG_pad(endtag);  // 提取结束标签填充
        u2_t dcrc   = FSTAG_crc(endtag);  // 提取CRC校验值

        // 验证记录格式完整性
        if( len != FSTAG_len(endtag) || pad + endpad > len || pad != 0 || len == 0 ) {
            _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "Mismatching len/beg/end/pad lengths: %d/%d pad=%d/%d len=%d",
                faddr, len, endlen, pad, endpad, len);
            break;
        }
        
        // 处理非数据记录（FILE/RENAME/DELETE）
        if( cmd != FSCMD_DATA ) {
            if( len > sizeof(auxbuf) ) {  // 检查缓冲区大小
                _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "Too large for auxbuf: len=%d > %d", faddr, len, (int)sizeof(auxbuf));
                break;
            }
            rdFlashN(faddr, auxbuf.u4, len/4+16);  // 读取记录数据
            u2_t xcrc = dataCrc(CRC_INI, &auxbuf.u1[4], len);  // 计算CRC
            if( dcrc != xcrc ) {          // 验证CRC
                _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "Mismatching data CRC: found=0x%04X - expecting=0x%04X", faddr, dcrc, xcrc);
                break;
            }
            char* fn = (char*)&auxbuf.u1[12];  // 获取文件名指针
            if( cmd == FSCMD_RENAME ) {   // 重命名记录特殊处理
                char* fn2 = fn + strlen(fn) + 1;  // 获取新文件名
                _LOG(MOD_SYS|INFO, FSDMP_RENAME_FMT,
                    faddr, CMD_NAMES[cmd], ino, auxbuf.u4[1], auxbuf.u4[2], fn, fn2);
            } else {                      // 其他记录标准输出
                _LOG(MOD_SYS|INFO, FSDMP_DFLT_FMT,
                    faddr, CMD_NAMES[cmd], ino, auxbuf.u4[1], auxbuf.u4[2], fn);
            }
        } else {
            // 处理数据记录 - 分块读取并计算CRC
            u4_t off = 0;
            u1_t d0[4] = {0}, dn[4] = {0};  // 存储首尾4字节用于输出
            u4_t cpycnt = 0;
            u2_t xcrc = CRC_INI;
            while( off < len ) {
                cpycnt = len - off;
                if( cpycnt > AUXBUF_SZ4 )
                    cpycnt = AUXBUF_SZ4;
                rdFlashN(faddr + off + 4, auxbuf.u4, cpycnt/4);  // 读取数据块
                if( off == 0 )            // 保存首4字节
                    memcpy(d0, &auxbuf.u1[0], 4);
                if( off+cpycnt >= len )   // 保存尾4字节
                    memcpy(dn, &auxbuf.u1[cpycnt-4], 4);
                xcrc = dataCrc(xcrc, auxbuf.u1, cpycnt);  // 累积CRC计算
                off += cpycnt;
            }
            if( xcrc != dcrc ) {          // 验证数据完整性
                _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "Mismatching data CRC: found=0x%04X - expecting=0x%04X", faddr, dcrc, xcrc);
                break;
            }
            // 根据数据长度选择输出格式
            if( len == 4 ) {
                _LOG(MOD_SYS|INFO, FSDMP_DATA_SRT_FMT,
                    faddr, CMD_NAMES[cmd], ino, dcrc, len, d0[0], d0[1], d0[2], d0[3], endpad);
            } else {
                _LOG(MOD_SYS|INFO, FSDMP_DATA_LNG_FMT,
                    faddr, CMD_NAMES[cmd], ino, dcrc, len,
                    d0[0], d0[1], d0[2], d0[3], dn[0], dn[1], dn[2], dn[3], endpad);
            }
        }
        faddr += len + 8;                 // 移动到下一记录
    }
    
    // 检查剩余Flash区域是否干净
    int clean = 1;
    u4_t fsend = faddr;
    int dirtcnt = 0;
    while( faddr < fend ) {
        u4_t len = fend - faddr;
        if( len > AUXBUF_SZ4 )
            len = AUXBUF_SZ4;
        len /= 4;
        sys_readFlash(faddr, auxbuf.u4, len);  // 直接读取（无解密）
        int dirtbeg = -1, dirtend = -1;
        for( int i=0; i<len; i++ ) {
            if( auxbuf.u4[i] != FLASH_ERASED ) {  // 检查是否为擦除状态
                if( dirtbeg == -1 )
                    dirtbeg = i;
                dirtend = i;
                clean = 0;                // 发现脏数据
            }
        }
        // 输出脏数据详情（限制数量避免日志过多）
        if( dirtcnt < 200 && dirtbeg != -1 && (_LOG != log_msg || log_shallLog(MOD_SYS|ERROR)) ) {
            int di = dirtbeg;
            while( di < dirtend ) {
                dbuf_t dbuf;
                log_special(MOD_SYS|ERROR, &dbuf);
                int off = dbuf.pos;
                xprintf(&dbuf, "[%08X] DIRT: ", faddr);  // 标记脏数据
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
    
    // 输出最终检查结果
    if( clean ) {
        _LOG(MOD_SYS|INFO, FSDMP_ADDR_FMT "End of file system - start of cleared flash", fsend);
    } else {
        _LOG(MOD_SYS|ERROR, FSDMP_ADDR_FMT "End of file system - rest of flash not clean", fsend);
    }
    return clean;                        // 返回Flash清洁状态
}


#if defined(CFG_linux) || defined(CFG_flashsim)
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

int fs_shell (char* cmdline) {
    return 0;
}

#endif // defined(CFG_linux)
