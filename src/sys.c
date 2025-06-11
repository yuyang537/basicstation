/*
 * BasicStation 系统核心模块 - 系统级功能和配置管理
 * 
 * 主要功能：
 * 1. 文件系统操作和路径管理 - 支持可配置的根目录和临时目录
 * 2. 安全凭证管理 - TLS证书、密钥、信任链的原子性更新机制  
 * 3. 配置文件管理 - TC和CUPS配置的版本化管理和回滚恢复
 * 4. 系统状态管理 - EUI生成、URI缓存、网络连接保活
 * 5. 事务型更新 - 确保配置更新的原子性和一致性
 * 
 * 设计特点：
 * - 支持多进程环境下的文件路径展开（# 和 ? 占位符）
 * - 实现配置文件的备份恢复机制，防止更新失败导致的数据丢失
 * - 采用ASN.1解析实现安全凭证的结构化存储
 * - 提供前向恢复机制，保证系统重启后配置状态的一致性
 * 
 * --- Revised 3-Clause BSD License ---
 * Copyright Semtech Corporation 2022. All rights reserved.
 */

#include <stdio.h>    // 标准输入输出函数
#include <fcntl.h>    // 文件控制操作常量


#if defined(CFG_linux) || defined(CFG_flashsim)
#include <sys/stat.h>      // 文件状态信息结构
#include <errno.h>         // 错误码定义
#include <netinet/ip.h>    // IP协议定义  
#include <netinet/tcp.h>   // TCP协议定义，用于keep-alive设置
#endif

#include "sys.h"      // 系统模块接口定义
#include "uj.h"       // JSON解析器，用于配置文件处理
#include "s2conf.h"   // Station配置管理
#include "fs.h"       // 文件系统抽象层
#include "net.h"      // 网络功能，用于URI校验

// 全局目录路径配置 - 在系统初始化时设置
str_t  homeDir;  // 主配置目录路径，存放所有配置文件
str_t  tempDir;  // 临时文件目录路径，用于原子更新操作
str_t  webDir;   // Web界面静态文件目录路径

// EUI (Extended Unique Identifier) 管理 - 设备唯一标识
uL_t   protoEUI     = 0;  // 原始EUI值，通常从MAC地址获取
uL_t   prefixEUI    = 1;  // EUI前缀标识：1表示使用MAC转EUI方案 (*:ffe:*)
s1_t   sys_slaveIdx = -1; // 从进程索引：-1表示主进程，>=0表示从进程编号

/*
 * 配置文件命名和管理体系
 * 
 * 标准配置文件命名规则：
 *   {tc,cups}{,-bootstrap,-bak}.{uri,key,crt,trust}
 *   例如：tc.uri, cups-bak.crt, tc-boot.trust
 * 
 * 临时文件用于原子更新：
 *   {tc,cups}-temp.{uri,key,crt,trust}
 *   例如：tc-temp.key, cups-temp.uri
 * 
 * 事务恢复标记文件：
 *   {tc,cups}-temp.upd  -- 标记临时文件可以重命名为正式文件
 *   {tc,cups}-temp.cpy  -- 标记正在进行备份复制操作
 *   {tc,cups}-done.bak  -- 标记备份复制已完成且有效
 */

// 文件扩展名类型枚举 - 表示不同类型的配置文件
enum { 
    FN_TRUST,  // .trust - CA信任链文件
    FN_CRT,    // .crt   - 客户端证书文件
    FN_KEY,    // .key   - 私钥文件
    FN_URI,    // .uri   - 服务器URI配置文件
    nFN_EXT    // 扩展名总数
};

// 文件集合类型枚举 - 表示不同版本的配置文件集
enum { 
    FN_REG,   // 正式配置文件集（无后缀）
    FN_BAK,   // 备份配置文件集（-bak后缀）
    FN_BOOT,  // 引导配置文件集（-boot后缀）
    FN_TEMP,  // 临时配置文件集（-temp后缀）
    nFN_SET   // 文件集总数
};

// 事务文件类型枚举 - 用于配置更新的原子性保障
enum { 
    FN_UPD,   // .upd - 更新标记文件，表示临时文件可以生效
    FN_CPY,   // .cpy - 复制标记文件，表示正在进行备份操作
    FN_DON,   // .done - 完成标记文件，表示备份操作已完成
    nFN_TAF   // 事务文件总数
};

// 配置文件类别枚举 - 区分不同的服务配置
enum { 
    FN_CUPS,  // CUPS服务配置文件
    FN_TC,    // TC (Traffic Controller) 服务配置文件
    nFN_CAT   // 类别总数
};

// 文件名组件字符串常量表 - 用于动态组装文件名
static const char sFN_CAT[] = "cups\0" "tc\0.."                         "?";  // 类别名称：2 x 5字符
static const char sFN_SET[] = "\0....." "-bak\0." "-boot\0" "-temp\0"   "?";  // 集合后缀：4 x 6字符
static const char sFN_EXT[] = "trust\0" "crt\0.." "key\0.." "uri\0.."   "?";  // 扩展名：4 x 6字符
static const char sFN_TAF[] = "-temp.upd\0" "-temp.cpy\0" "-bak.done\0" "?";  // 事务文件：3 x 10字符

// 动态分配的配置文件名数组 - 存储所有可能的文件路径
static char* CFNS[nFN_CAT*(nFN_SET * nFN_EXT + nFN_TAF)];

// 备份完成状态数组 - 记录每个类别的配置是否已备份
static u1_t  bakDone[nFN_CAT];

// URI缓存数组 - 缓存已读取的URI配置，避免重复文件读取
static char  uriCache[nFN_SET-1][MAX_URI_LEN];

// 待处理的凭证数据缓冲区 - 用于分段接收凭证数据
static char* pendData;

// 配置更新状态标志位枚举
enum { 
    UPD_CUPS = 1<<FN_CUPS,  // CUPS配置有更新
    UPD_TC   = 1<<FN_TC,    // TC配置有更新  
    UPD_ERROR = 0xFF        // 更新过程中发生错误
};

// 当前配置更新状态 - 位掩码表示哪些配置类别有更新
static u1_t updateState;

// 文件名构建宏定义 - 根据类别、集合、扩展名快速构建文件路径
#define categoryName(cat) (&sFN_CAT[cat*5])  // 获取配置类别名称字符串
#define configFilename(cat,set,ext) (CFNS[(cat)*(nFN_SET*nFN_EXT + nFN_TAF)+((set)*nFN_EXT)+(ext)])  // 构建配置文件路径
#define transactionFilename(cat,taf) (CFNS[(cat)*(nFN_SET*nFN_EXT + nFN_TAF)+(nFN_SET*nFN_EXT)+(taf)])  // 构建事务文件路径

// 功能：将配置类别枚举转换为字符串名称
// 参数：cred_cat - 配置类别（FN_CUPS或FN_TC）
// 返回值：对应的类别名称字符串（"cups"或"tc"）
// 调用时机：日志输出和错误报告时需要显示类别名称
str_t sys_credcat2str (int cred_cat) { 
    return categoryName(cred_cat);  // 直接返回预定义的类别名称字符串
}

// 功能：将配置集合枚举转换为字符串后缀
// 参数：cred_set - 配置集合（FN_REG、FN_BAK、FN_BOOT、FN_TEMP）
// 返回值：对应的集合后缀字符串（""、"-bak"、"-boot"、"-temp"）
// 调用时机：需要显示配置集合名称时（日志输出、错误报告）
str_t sys_credset2str (int cred_set) { 
    return &sFN_SET[cred_set*6];  // 从字符串表中获取对应的后缀字符串
}

// 功能：获取文件大小（以字节为单位）
// 参数：file - 文件路径字符串
// 返回值：文件大小（字节），失败时返回-1
// 调用时机：读取文件前检查文件是否存在和获取大小信息
static int sizeFile (str_t file) {
    struct stat st;  // 文件状态信息结构
    if( fs_stat(file, &st) == -1 )  // 调用文件系统统计函数获取文件信息
        return -1;  // 文件不存在或无法访问，返回错误标识
    return st.st_size;  // 返回文件大小字段
}

// 功能：构建完整的文件路径，支持目录前缀解析和多进程路径展开
// 参数：prefix - 路径前缀（支持~temp/、~/、绝对路径、相对路径）
//       suffix - 路径后缀（文件名后缀或扩展名）
//       pCachedFile - 用于缓存文件路径的指针（可选）
//       isReadable - 是否检查文件可读性
// 返回值：动态分配的完整文件路径字符串，失败时返回NULL
// 调用时机：需要访问配置文件、日志文件或临时文件时
char* makeFilepath (const char* prefix, const char* suffix, char** pCachedFile, int isReadable) {
    if( pCachedFile )  // 如果提供了缓存指针
        rt_free(*pCachedFile);  // 先释放之前缓存的文件路径
    
    char filepath[MAX_FILEPATH_LEN];  // 本地路径缓冲区
    dbuf_t b = dbuf_ini(filepath);    // 初始化动态缓冲区用于路径构建
    
    // 根据前缀类型选择基础目录
    if( strncmp(prefix, "~temp/", 6) == 0 ) {  // 临时目录前缀
        prefix += 6;                           // 跳过"~temp/"前缀
        xputs(&b, tempDir, -1);               // 使用临时目录作为基础路径
    }
    else if( prefix[0] != '/' && (prefix[0] != '.' || prefix[1] != '/') ) {  // 非绝对路径且非当前目录
        if( prefix[0] == '~' && prefix[1] == '/' )  // 处理~/前缀
            prefix += 2;                            // 跳过"~/"前缀
        xputs(&b, homeDir, -1);                    // 使用主目录作为基础路径
    }
    // 否则使用原始前缀（绝对路径或./相对路径）
    
    // 处理前缀和后缀字符串拼接
    for( int fnx=0; fnx < 2; fnx++ ) {  // 分别处理前缀(fnx=0)和后缀(fnx=1)
        str_t fni = fnx==0 ? prefix : suffix;  // 当前处理的字符串
        char c;
        while( (c=*fni++) != 0 ) {  // 逐字符处理
            if( c == '#' ) {  // '#'占位符：仅在从进程模式下展开
                if( sys_slaveIdx >= 0 )  // 如果是从进程
                    xprintf(&b, "-%d", sys_slaveIdx);  // 添加"-进程号"后缀
            }
            else if( c == '?' ) {  // '?'占位符：总是展开为进程号
                xprintf(&b, "%d", sys_slaveIdx >= 0 ? sys_slaveIdx : 0);  // 主进程使用0
            } else {
                xputs(&b, &c, 1);  // 普通字符直接添加
            }
        }
    }
    
    if( !xeos(&b) )  // 检查路径是否过长
        rt_fatal("File path too big: %s", b.buf);  // 路径过长是致命错误
    
    if( isReadable && fs_access(b.buf, R_OK) != 0 )  // 如果需要检查可读性但文件不可读
        b.buf[0] = b.pos = 0;  // 清空缓冲区表示文件不存在
    
    char* cachedFile = b.buf[0] ? rt_strdup(b.buf) : NULL;  // 复制路径字符串或返回NULL
    if( pCachedFile )  // 如果提供了缓存指针
        *pCachedFile = cachedFile;  // 更新缓存
    return cachedFile;  // 返回构建的文件路径
}

dbuf_t readFile (str_t file, int complain) {
    dbuf_t b = { .buf=NULL, .bufsize=0, .pos=0 };
    int fsize;
    int fd, n;

    if( file == NULL )
        return b;
    // NOTE: we use this to read and /sys/class/net/*/address
    // For the latter stat reports a file size of 4K but read returns a smaller number!
    if( (fd = fs_open(file, O_RDONLY)) == -1 ||
        (fsize = sizeFile(file)) == -1 ||
        fsize > MAX_DOFF ||
        (n = fs_read(fd, b.buf = rt_mallocN(char, fsize+1), fsize)) == -1 ) {
        if( complain )
            LOG(MOD_SYS|ERROR, "Failed to read '%s': %s", file, strerror(errno));
        rt_free(b.buf);
        b.buf = NULL;
    } else {
        b.bufsize = b.pos = n;
        b.buf[b.bufsize] = 0;  // make it zero terminated if used as a string
    }
    if (fd != -1)
        fs_close(fd);
    return b;
}

static int trimEnd (char* s) {
    int n = strlen(s);
    while( n>0 && strchr(" \t\r\n", s[n-1]) ) --n;
    s[n] = 0;
    return n;
}


str_t readFileAsString (str_t basename, str_t suffix, str_t* pCachedValue) {
    // Read file every time - should not happen that often anyway
    // Cached value is only used to free memory - so caller has not to deal with it
    str_t value = *pCachedValue;
    if( value ) {
        rt_free((void*)value);
        *pCachedValue = NULL;
    }
    str_t file = makeFilepath(basename, suffix, NULL, 0);
    dbuf_t b = readFile(file, 0);
    rt_free((void*)file);
    if( !b.buf )
        return NULL;
    trimEnd(b.buf);
    value = rt_strdup(b.buf); // copy over - might be smaller
    *pCachedValue = value;
    rt_free(b.buf);
    return value;
}

int writeFile (str_t file, const char* data, int datalen) {
    int fd, err= 1;
    if( (fd = fs_open(file, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR|S_IRGRP)) == -1 ||
        fs_write(fd, data, datalen) == -1 ) {
        LOG(MOD_SYS|CRITICAL, "Failed to write file '%s': %s", file, strerror(errno));
        err = 0;
    }
    fs_close(fd);
    return err;
}

dbuf_t sys_webFile (str_t filename) {
    if( !webDir )
        goto err;
    char filepath[MAX_FILEPATH_LEN];
    dbuf_t b = dbuf_ini(filepath);
    xputs(&b, webDir, -1);
    
    xputs(&b, filename[0]=='/' ? filename+1 : filename, -1);
    if( !xeos(&b) ) {
      err:
        b.buf = NULL;
        b.pos = b.bufsize = 0;
        return b;
    }
    return readFile(b.buf, 0);
}

dbuf_t sys_readFile (str_t filename) {
    str_t fpath = makeFilepath(filename,"",NULL,1);
    dbuf_t b = readFile(fpath, 1);
    rt_free((void*)fpath);
    return b;
}

dbuf_t sys_checkFile (str_t filename) {
    str_t fpath = makeFilepath(filename,"",NULL,1);
    dbuf_t b = readFile(fpath, 0);
    rt_free((void*)fpath);
    return b;
}


str_t sys_makeFilepath (str_t filename, int complain) {
    return makeFilepath(filename,"",NULL,complain);
}

void sys_writeFile (str_t filename, dbuf_t* b) {
    str_t fn = makeFilepath(filename,"",NULL,0);
    writeFile(fn, b->buf, b->pos);
    rt_free((void*)fn);
}

uL_t sys_eui () {
    if( (protoEUI >> 48) != 0 )
        return protoEUI;
    if( (prefixEUI & 0xFFFFffffFFFF) != 0 ) {
        // Expand MAC to EUI
        return ((protoEUI & 0xFFFFFF000000) << 16) | 0xFFFE000000 | (protoEUI & 0xFFFFFF);
    }
    return prefixEUI | protoEUI;
}

str_t sys_uri (int cred_cat, int cred_set) {
    str_t uri_fn = configFilename(cred_cat, cred_set, FN_URI);
    dbuf_t dbuf = readFile(uri_fn, 0);
    if( dbuf.buf == NULL )
        return NULL;
    dbuf.bufsize = trimEnd(dbuf.buf);
    if( dbuf.bufsize+1 > MAX_URI_LEN ) {
        LOG(MOD_SYS|ERROR, "URI in '%s' too long (max %d): %s", uri_fn, MAX_URI_LEN, dbuf.buf);
        rt_free(dbuf.buf);
        return NULL;
    }
    char* p = &uriCache[cred_cat][cred_set];
    strcpy(p, dbuf.buf);
    rt_free(dbuf.buf);
    return p;
}

void sys_saveUri (int cred_cat, str_t uri) {
    str_t uri_fn = configFilename(cred_cat, FN_TEMP, FN_URI);
    if( !writeFile(uri_fn, uri, strlen(uri)) )
        updateState |= UPD_ERROR;
    updateState |= (1<<cred_cat);
}


int checkUris () {
    int errs=0, nuris=0;

    for( int cat=0; cat < nFN_CAT; cat++ ) {
        str_t scheme = cat == FN_CUPS ? "http" : "ws";
        if( cat == FN_TC && sys_noTC ) continue;
        int nuriscat = nuris;
        for( int set=FN_REG; set <= FN_BOOT; set++ ) {
            char host[MAX_HOSTNAME_LEN];
            char port[MAX_PORT_LEN];
            str_t uri = sys_uri(cat, set);
            if( !uri ) continue;
            if( !uri_checkHostPortUri(uri, scheme, host, sizeof(host), port, sizeof(port)) ) {
                printf("%s: Misconfigured URI - expecting scheme %s: %s\n", configFilename(cat, set, FN_URI), scheme, uri);
                errs++;
            } else {
                nuris++;
            }
        }
        if( nuriscat == nuris && cat == FN_CUPS ) {
            sys_noCUPS = 1;
        }
    }
    if( nuris == 0 ) {
        printf("No server URIs configured - expecting at least one of the following files to exist:\n");
        for( int cat=0; cat < nFN_CAT; cat++ ) {
            if( cat == FN_TC && sys_noTC ) continue;
            for( int set=FN_REG; set <= FN_BOOT; set++ ) {
                printf("   %s\n", configFilename(cat, set, FN_URI));
            }
        }
    }
    return errs==0 && nuris > 0;
}


static int updateConfigFiles (int cat, int rollFwd) {
    // Rename temp setup files to regular files.
    str_t taf_upd = transactionFilename(cat, FN_UPD);
    if( !rollFwd && !writeFile(taf_upd, "", 0) ) {
        fs_unlink(taf_upd);
        LOG(MOD_SYS|CRITICAL, "Failed to create '%s': %s", taf_upd);
        return 0;
    }
    fs_sync();
    for( int ext=0; ext < nFN_EXT; ext++ ) {
        str_t fn_temp = configFilename(cat, FN_TEMP, ext);
        str_t fn_reg  = configFilename(cat, FN_REG,  ext);
        if( fs_access(fn_temp, F_OK) == 0 ) {
            if( fs_rename(fn_temp, fn_reg) == -1 )
                rt_fatal("Failed to rename '%s' -> '%s': %s", fn_temp, fn_reg, strerror(errno));
        }
    }
    fs_sync();
    fs_unlink(taf_upd);
    return 1;
}


static int backupConfigFiles (int cat, int rollFwd) {
    // Copy a set of config files to a backup set.
    if( bakDone[cat] )
        return 1;   // already did a copy

    str_t taf_cpy = transactionFilename(cat, FN_CPY);
    if( !rollFwd && !writeFile(taf_cpy, "", 0) ) {
        fs_unlink(taf_cpy);
        LOG(MOD_SYS|CRITICAL, "Failed to create '%s': %s", taf_cpy);
        return 0;
    }
    fs_sync();
    str_t unlink_fn = transactionFilename(cat, FN_DON);
    if( fs_unlink(unlink_fn) == -1 && errno != ENOENT ) {
      unlink_fail:
        LOG(MOD_SYS|CRITICAL, "Failed to unlink '%s': %s", unlink_fn, strerror(errno));
        return 0;  // just walk away and keep ta file - copying will be continued with next station restart
    }
    for( int ext=0; ext < nFN_EXT; ext++ ) {
        unlink_fn = configFilename(cat, FN_BAK, ext);
        if( fs_unlink(unlink_fn) == -1 && errno != ENOENT )
            goto unlink_fail;
    }
    for( int ext=0; ext < nFN_EXT; ext++ ) {
        str_t fn_bak = configFilename(cat, FN_BAK, ext);
        str_t fn_reg = configFilename(cat, FN_REG, ext);
        dbuf_t dbuf = readFile(fn_reg, /*no complaints*/0);
        if( dbuf.buf ) {
            if( !writeFile(fn_bak, dbuf.buf, dbuf.bufsize) ) {
                LOG(MOD_SYS|CRITICAL, "Failed to write '%s': %s", fn_bak, strerror(errno));
                return 0;  // just walk away and keep taf file - copying will be continued with next station restart
            }
            rt_free(dbuf.buf);
        }
    }
    str_t taf_don = transactionFilename(cat, FN_DON);
    if( !writeFile(taf_don, "", 0) ) {
        LOG(MOD_SYS|CRITICAL, "Failed to write '%s': %s", taf_don, strerror(errno));
        return 0;  // just walk away and keep taf file - copying will be continued with next station restart
    }
    fs_sync();
    fs_unlink(taf_cpy);
    fs_sync();
    bakDone[cat] = 1;
    return 1;
}

void setupConfigFilenames () {
    assert((int)SYS_CRED_CUPS  == (int)FN_CUPS  && (int)SYS_CRED_TC     == (int)FN_TC);
    assert((int)SYS_CRED_REG   == (int)FN_REG   && (int)SYS_CRED_BAK    == (int)FN_BAK && (int)SYS_CRED_BOOT  == (int)FN_BOOT);
    assert((int)SYS_CRED_TRUST == (int)FN_TRUST && (int)SYS_CRED_MYCERT == (int)FN_CRT && (int)SYS_CRED_MYKEY == (int)FN_KEY);
    char filepath[MAX_FILEPATH_LEN];
    dbuf_t b = dbuf_ini(filepath);

    for( int cat=0; cat < nFN_CAT; cat++ ) {
        b.pos = 0;
        xputs(&b, homeDir, -1);
        xputs(&b, categoryName(cat), -1);
        int p0 = b.pos;
        for( int set=0; set < nFN_SET; set++ ) {
            xputs(&b, &sFN_SET[set*6], -1);
            int p1 = b.pos;
            for( int ext=0; ext < nFN_EXT; ext++ ) {
                xputs(&b, ".", 1);
                xputs(&b, &sFN_EXT[ext*6], -1);
                if( !xeos(&b) )
                    rt_fatal("File path too big: %s", b.buf);
                configFilename(cat,set,ext) = rt_strdup(b.buf);
                b.pos = p1;
            }
            b.pos = p0;
        }
        for( int taf=0; taf < nFN_TAF; taf++ ) {
            xputs(&b, &sFN_TAF[taf*10], -1);
            xeos(&b);
            transactionFilename(cat,taf) = rt_strdup(b.buf);
            b.pos = p0;
        }
    }
}


void checkRollForward () {
    int ok = 1;
    str_t taf_file;
    for( int cat=0; cat < nFN_CAT; cat++ ) {
        taf_file = transactionFilename(cat, FN_UPD);
        if( fs_access(taf_file, F_OK) == 0 ) {
            // A new set of config files got created and replacing the regular ones was interrupted.
            // Pick up replacing and run to completion.
            ok &= updateConfigFiles(cat, 1);
        }
        taf_file = transactionFilename(cat, FN_CPY);
        if( fs_access(taf_file, F_OK) == 0 ) {
            // Making a backup copy of a set of config files was interrupted.
            // Rerun the copy process and clear transaction marker.
            ok &= backupConfigFiles(cat, 1);
        }
        taf_file = transactionFilename(cat, FN_DON);
        if( fs_access(taf_file, F_OK) == 0 ) {
            bakDone[cat] = 1;
        }
    }
    if( !ok )
        rt_fatal("Forward recovery of some station config files failed");
}


int sys_cred (int cred_cat, int cred_set, str_t* elems, int* elemslen) {
    memset(elems,    0, sizeof(elems[0]   ) * SYS_CRED_NELEMS);
    memset(elemslen, 0, sizeof(elemslen[0]) * SYS_CRED_NELEMS);
    for( int ext=FN_TRUST; ext < FN_URI; ext++ ) {
        str_t fn = configFilename(cred_cat, cred_set, ext);
        int sz = sizeFile(fn);
        if( sz > 0 ) { // Empty file (sz==0) is treated as absent
            elems[ext] = fn;
        }
    }
    if( elems[SYS_CRED_TRUST] == NULL ) {
        return SYS_AUTH_NONE;
    }
    if( elems[SYS_CRED_MYCERT] == NULL && elems[SYS_CRED_MYKEY] != NULL ) {
        return SYS_AUTH_TOKEN;
    }
    if( elems[SYS_CRED_MYCERT] == NULL || elems[SYS_CRED_MYKEY] == NULL ) {
        return SYS_AUTH_SERVER;
    }
    return SYS_AUTH_BOTH;
}


u4_t sys_crcCred (int cred_cat, int cred_set) {
    u4_t crc = 0;
    for( int ext=FN_TRUST; ext < FN_URI; ext++ ) {
        dbuf_t data = readFile(configFilename(cred_cat, cred_set, ext), 0);
        if( data.buf && data.bufsize != 0 )
            crc = rt_crc32(crc, data.buf, data.bufsize);
        else
            crc = rt_crc32(crc, &(u1_t[]){0,0,0,0}, 4);
        rt_free(data.buf);
    }
    return crc;
}


void sys_resetConfigUpdate () {
    updateState = 0;
    for( int cat=0; cat < nFN_CAT; cat++ ) {
        str_t fn = transactionFilename(cat, FN_UPD);
        if( fn ) fs_unlink(fn);
        for( int ext=0; ext < nFN_EXT; ext++ ) {
            fn = configFilename(cat, FN_TEMP, ext);
            if( fn ) fs_unlink(fn);
        }
    }
    fs_sync();
}

void sys_commitConfigUpdate () {
    if( updateState == UPD_ERROR )
        return;
    for( int cat=0; cat < nFN_CAT; cat++ ) {
        if( updateState & (1<<cat) ) {
            updateConfigFiles(cat, 0);
        }
    }
    updateState = 0;
}

void sys_backupConfig (int cred_cat) {
    backupConfigFiles(cred_cat, 0);
}


void sys_credStart (int cred_cat, int len) {
    rt_free(pendData);
    pendData = rt_mallocN(char, len+1);
}

void sys_credWrite (int cred_cat, u1_t* data, int off, int len) {
    memcpy(pendData+off, data, len);
    updateState |= 1<<cred_cat;
}

#define ASN1_ISSEQ(PTR) ( (PTR)[0] == 0x30 )
//#define ALIGN(V,A) (((V)+(A-1)) & ~(A-1))

static inline int asn1_seqlen(char * ptr) {
    if( ptr[1] & 0x80 ) {
        return (( ((u2_t) ptr[2] & 0xff) << 8) | ( (u2_t) ptr[3] & 0xff)) + 4;
    } else {
        return ptr[1] + 2;
    }
}

void sys_credComplete (int cred_cat, int len) {
    pendData[len] = 0;
    char* data[SYS_CRED_NELEMS];
    int datalen[SYS_CRED_NELEMS];
    u4_t to, co, ko, tl, cl, kl;

    // Trust
    if( !ASN1_ISSEQ(pendData) ) {
        LOG(MOD_SYS|ERROR, "Failed to parse %s credentials: ASN.1 SEQ expected for trust (0x%02x)", categoryName(cred_cat), pendData[0]);
        goto parsing_failed;
    }
    to = 0;
    tl = asn1_seqlen(pendData + to);

    // Client Certificate
    co = to + tl;
    if( pendData[co] == 00 ) {
        // No certificate
        cl = 0;
        ko = co + 4;
    } else {
        if( !ASN1_ISSEQ(pendData+co) ) {
            LOG(MOD_SYS|ERROR, "Failed to parse %s credentials: ASN.1 SEQ expected for cert (0x%02x)", categoryName(cred_cat), pendData[co]);
            goto parsing_failed;
        }
        cl = asn1_seqlen(pendData + co);
        ko = co + cl;
    }

    if (ko > len) {
        LOG(MOD_SYS|ERROR, "Failed to parse %s credentials: expecting more data (key_offset=%d, total_len=%d)", categoryName(cred_cat), ko, len);
        goto parsing_failed;
    }

    if( ASN1_ISSEQ(&pendData[ko]) ) { // Key
        kl = asn1_seqlen(pendData + ko);
    } else if( pendData[ko] == 0 ) {
        kl = 0;
    } else { // Token
        kl = len - ko;
    }

    data[SYS_CRED_TRUST]  = pendData+to; datalen[SYS_CRED_TRUST]  = tl;
    data[SYS_CRED_MYCERT] = pendData+co; datalen[SYS_CRED_MYCERT] = cl;
    data[SYS_CRED_MYKEY]  = pendData+ko; datalen[SYS_CRED_MYKEY]  = kl;

    u1_t * p = (u1_t*)pendData;
    LOG(MOD_SYS|INFO, " credComplete - trust_off=%4u, trust_len=%4u               %02x %02x %02x %02x  %02x %02x %02x %02x",
        to, tl,                                     p[to+0], p[to+1], p[to+2], p[to+3],p[to+4], p[to+5], p[to+6], p[to+7]
    );
    LOG(MOD_SYS|INFO, " credComplete - cert_off =%4u, cert_len =%4u  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x",
        co, cl, p[co-4], p[co-3], p[co-2], p[co-1], p[co+0], p[co+1], p[co+2], p[co+3], p[co+4], p[co+5], p[co+6], p[co+7]
    );
    LOG(MOD_SYS|INFO, " credComplete - key_off  =%4u, key_len  =%4u  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x",
        ko, kl, p[ko-4], p[ko-3], p[ko-2], p[ko-1], p[ko+0], p[ko+1], p[ko+2], p[ko+3], p[ko+4], p[ko+5], p[ko+6], p[ko+7]
    );
    if( datalen[SYS_CRED_TRUST] + datalen[SYS_CRED_MYCERT] + datalen[SYS_CRED_MYKEY] > len ) {
        LOG(MOD_SYS|ERROR, "Failed to parse %s credentials! Lengths do not align segment_len=%d parsed_len=%d. Ignoring.",
            categoryName(cred_cat),
            len, datalen[SYS_CRED_TRUST] + datalen[SYS_CRED_MYCERT] + datalen[SYS_CRED_MYKEY]);
        goto parsing_failed;
    }
    for( int ext=FN_TRUST; ext < FN_URI; ext++ ) {
        str_t fn = configFilename(cred_cat, FN_TEMP, ext);
        // Note: unset credential files are create as empty files.
        // This makes creating bak set of files easier as if files would be absent.
        if( !writeFile(fn, data[ext], datalen[ext]) )
            goto parsing_failed;
    }
    goto parsing_done;
parsing_failed:
    // updateState |= UPD_ERROR;
    (void) updateState;

parsing_done:
    rt_free(pendData);
    pendData = NULL;
}

u4_t sys_crcSigkey (int key_id) {
    u4_t crc = 0;
    dbuf_t data = sys_sigKey(key_id);
    if( data.buf )
        crc = rt_crc32(crc, data.buf, data.bufsize);
    sys_sigKey(-1); // Clear buffer
    return crc;
}


dbuf_t sys_sigKey (int key_id) {
    static dbuf_t b;
    if( key_id < 0 && b.buf ) {
        rt_free(b.buf);
        b.buf = NULL;
    }
    if( b.buf )
        rt_free(b.buf);
    char path[20];
    snprintf(path, sizeof(path), "~/sig-%d.key", key_id);
    b = sys_readFile(path);
    return b;
}

void sys_keepAlive (int fd) {
    str_t tag = "SO_KEEPALIVE";
    int v = TCP_KEEPALIVE_EN;
    if( setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,  &v, sizeof(v)) == -1 ) goto err;
    if( v == 0 ) return;
    tag = "TCP_KEEPCNT";
    v = TCP_KEEPALIVE_CNT;
    if( setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &v, sizeof(v)) == -1 ) goto err;
    tag = "TCP_KEEPIDLE";
    v = TCP_KEEPALIVE_IDLE;
    if( setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof(v)) == -1 ) goto err;
    tag = "TCP_KEEPINTVL";
    v = TCP_KEEPALIVE_INTVL;
    if( setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof(v)) == -1 ) goto err;
    return;
 err:
    LOG(MOD_AIO|ERROR, "Failed to set %s=%d: %s", tag, v, strerror(errno));
}
