/*
 * BasicStation TLS安全层模块 - 基于mbedTLS的安全通信封装
 * 
 * 主要功能：
 * 1. TLS配置管理 - SSL/TLS连接配置和证书管理
 * 2. 证书验证系统 - 可信CA、客户端证书、私钥管理
 * 3. 安全会话管理 - TLS会话创建、维护和销毁
 * 4. 随机数生成 - 密码学安全的随机数生成服务
 * 5. 调试支持 - TLS调试日志和错误报告机制
 * 
 * 设计特点：
 * - 基于mbedTLS库的轻量级TLS/SSL实现
 * - 支持双向认证和证书链验证
 * - 统一的错误处理和日志记录机制
 * - 灵活的配置系统支持多种部署场景
 * - 与BasicStation网络模块无缝集成
 * 
 * --- Revised 3-Clause BSD License ---
 * Copyright Semtech Corporation 2022. All rights reserved.
 */

#include "mbedtls/net_sockets.h"  // mbedTLS网络套接字抽象
#include "mbedtls/ssl.h"          // mbedTLS SSL/TLS核心功能
#include "mbedtls/certs.h"        // mbedTLS证书处理
#include "mbedtls/entropy.h"      // mbedTLS熵源管理
#include "mbedtls/ctr_drbg.h"     // mbedTLS CTR-DRBG伪随机数生成器
#include "mbedtls/error.h"        // mbedTLS错误码和错误字符串
#include "mbedtls/debug.h"        // mbedTLS调试支持

#include "s2conf.h"               // Station配置系统
#include "sys.h"                  // 系统核心功能（文件读取、随机数）
#include "rt.h"                   // 运行时系统（内存管理、错误处理）
#include "uj.h"                   // JSON解析器（日志格式化）
#include "tls.h"                  // TLS模块接口定义

// TLS配置结构体 - 封装mbedTLS SSL配置和证书信息
struct tlsconf {
    mbedtls_ssl_config  sslconfig;    // mbedTLS SSL配置对象
    mbedtls_x509_crt*   trust;        // 可信CA证书链指针
    mbedtls_x509_crt*   mycert;       // 客户端证书指针
    mbedtls_pk_context* mykey;        // 客户端私钥上下文指针
};

// TLS调试级别全局变量 - 控制mbedTLS调试输出详细程度
// 0=关闭, 1=错误, 2=状态变化, 3=信息, 4=详细
u1_t tls_dbgLevel;

// === 随机数生成系统 ===
// 提供密码学安全的随机数生成服务，支持系统随机数或mbedTLS内置随机数生成器

#if defined(CFG_sysrandom)
// 功能：系统随机数生成器接口 - 使用操作系统提供的随机数源
// 参数：arg - 未使用的参数（mbedTLS接口兼容性）
//       buf - 随机数输出缓冲区
//       len - 需要生成的随机数字节数
// 返回值：生成的字节数或错误码
// 调用时机：TLS握手过程中需要随机数时由mbedTLS自动调用
int tls_random (void * arg, unsigned char * buf, size_t len) {
    return sys_random(buf, (int)len);    // 直接使用系统随机数服务
}
#else
// mbedTLS内置随机数生成器结构体 - 当系统随机数不可用时使用
typedef struct {
    mbedtls_entropy_context entropy;     // 熵池上下文，收集各种熵源
    mbedtls_ctr_drbg_context ctr_drbg;   // CTR-DRBG伪随机数生成器上下文
} drbg_t;

// 全局随机数生成器实例 - 单例模式，整个TLS模块共享
static drbg_t* DBRG;

// 功能：确保随机数生成器已初始化并返回生成器上下文
// 参数：无
// 返回值：已初始化的CTR-DRBG上下文指针
// 调用时机：首次使用随机数时或随机数生成器未初始化时
static mbedtls_ctr_drbg_context* assertDBRG () {
    if( DBRG != NULL )                   // 随机数生成器已初始化
        return &DBRG->ctr_drbg;          // 直接返回现有实例
    
    DBRG = rt_malloc(drbg_t);            // 分配随机数生成器内存
    mbedtls_entropy_init(&DBRG->entropy);         // 初始化熵池
    mbedtls_ctr_drbg_init(&DBRG->ctr_drbg);       // 初始化CTR-DRBG生成器
    
    u1_t seed[16];                       // 16字节初始种子
    int ret;                             // mbedTLS返回值
    sys_seed(seed, sizeof(seed));        // 从系统获取初始种子
    
    // 使用种子初始化CTR-DRBG生成器
    if( (ret = mbedtls_ctr_drbg_seed(&DBRG->ctr_drbg,
                                     mbedtls_entropy_func, &DBRG->entropy,
                                     seed, sizeof(seed))) != 0 )
        rt_fatal("mbedtls_ctr_drbg_seed failed: %d", ret);  // 初始化失败则致命错误

#ifdef CFG_max_tls_frag_len
    // 启动时报告TLS最大片段长度配置（仅报告一次）
    LOG(MOD_AIO|WARNING, "TLS is using a maximum fragment length of %d bytes", 
        256 << CFG_max_tls_frag_len);
#endif // CFG_max_tls_frag_len

    return &DBRG->ctr_drbg;              // 返回初始化完成的生成器
}
#endif


// === 调试和错误处理系统 ===
// 提供TLS调试输出和错误信息格式化功能

#if defined(CFG_tlsdebug)
// 功能：mbedTLS调试信息回调函数 - 将mbedTLS内部调试信息输出到BasicStation日志系统
// 参数：ctx - 上下文指针（未使用）
//       level - mbedTLS调试级别
//       file - 源文件名
//       line - 源文件行号
//       str - 调试信息字符串
// 返回值：无
// 调用时机：mbedTLS内部需要输出调试信息时自动调用
static void log_mbedDebug (void *ctx, int level, const char *file, int line, const char *str) {
    LOG(MOD_AIO|DEBUG, "%s:%d MBEDTLS[%d]: %s", file,line, level, str);
}
#endif // CFG_tlsdebug

// 功能：mbedTLS错误信息格式化和记录函数 - 将mbedTLS错误码转换为可读错误信息
// 参数：mod_level - 日志模块和级别
//       ret - mbedTLS错误码
//       fmt - 格式化字符串
//       ... - 可变参数列表
// 返回值：无
// 调用时机：TLS操作失败时调用以记录详细错误信息
void log_mbedError (u1_t mod_level, int ret, const char* fmt, ...) {
    dbuf_t b;                            // 日志缓冲区
    if( !log_special(mod_level, &b) )    // 检查日志级别并获取缓冲区
        return;                          // 日志级别不满足要求，直接返回
    
    va_list ap;                          // 可变参数列表
    va_start(ap, fmt);                   // 初始化可变参数
    vxprintf(&b, fmt, ap);               // 格式化用户提供的错误描述
    xputs(&b, ": ", -1);                 // 添加分隔符
    
    char* p = b.buf + b.pos;             // 获取错误字符串写入位置
    mbedtls_strerror(ret, p, b.bufsize - b.pos);  // 将mbedTLS错误码转换为字符串
    b.pos += strlen(p);                  // 更新缓冲区位置
    va_end(ap);                          // 清理可变参数
    log_specialFlush(b.pos);             // 刷新日志输出
}


// === TLS配置管理系统 ===

// 功能：创建TLS配置对象 - 初始化SSL/TLS连接的基本配置参数
// 参数：trust - 可信CA证书链（可选，当前参数未使用）
//       mycert - 客户端证书（可选，当前参数未使用）  
//       mykey - 客户端私钥（可选，当前参数未使用）
// 返回值：已初始化的TLS配置对象指针，失败时程序终止
// 调用时机：需要建立TLS连接前调用以创建配置
tlsconf_t* tls_makeConf (mbedtls_x509_crt* trust, mbedtls_x509_crt* mycert, mbedtls_pk_context* mykey) {
    tlsconf_t* conf = rt_malloc(tlsconf_t);      // 分配TLS配置结构体内存
    mbedtls_ssl_config_init(&conf->sslconfig);  // 初始化mbedTLS SSL配置对象
    
    // 初始化证书和密钥指针为空（后续通过专门函数设置）
    conf->trust  = NULL;                 // 可信CA证书链
    conf->mycert = NULL;                 // 客户端证书
    conf->mykey  = NULL;                 // 客户端私钥
    
    int ret;                             // mbedTLS返回值
    
    // 设置SSL配置默认值：客户端模式，流传输，默认安全配置
    if( (ret = mbedtls_ssl_config_defaults(&conf->sslconfig,
                                           MBEDTLS_SSL_IS_CLIENT,      // 客户端模式
                                           MBEDTLS_SSL_TRANSPORT_STREAM,  // TCP流传输
                                           MBEDTLS_SSL_PRESET_DEFAULT ) ) != 0 )   // 默认安全预设
        rt_fatal("mbedtls_ssl_config_defaults", ret);  // 配置失败则致命错误

#if defined(CFG_sysrandom)
    // 使用系统随机数生成器
    mbedtls_ssl_conf_rng     (&conf->sslconfig, tls_random, NULL);
#else
    // 使用mbedTLS内置CTR-DRBG随机数生成器
    mbedtls_ssl_conf_rng     (&conf->sslconfig, mbedtls_ctr_drbg_random, assertDBRG());
#endif

    // 设置认证模式为必须验证（要求服务器证书验证）
    mbedtls_ssl_conf_authmode(&conf->sslconfig, MBEDTLS_SSL_VERIFY_REQUIRED);

#if defined(CFG_max_tls_frag_len)
    // 配置TLS最大片段长度以优化内存使用
    if( (ret = mbedtls_ssl_conf_max_frag_len(&conf->sslconfig, CFG_max_tls_frag_len)) != 0)
        rt_fatal("mbedtls_ssl_conf_max_frag_len", ret);
#endif // CFG_max_tls_frag_len

#if defined(CFG_tlsdebug)
    // 启用TLS调试支持
    mbedtls_ssl_conf_dbg(&conf->sslconfig, log_mbedDebug, NULL );
    mbedtls_debug_set_threshold(tls_dbgLevel);  // 设置调试级别：0=关闭, 1=错误, 2=状态变化, 3=信息, 4=详细
#endif // CFG_tlsdebug

    return conf;                         // 返回已配置的TLS配置对象
}


// 功能：释放TLS配置对象 - 清理SSL配置和相关证书资源
// 参数：conf - 要释放的TLS配置对象指针
// 返回值：无
// 调用时机：不再需要TLS配置时调用（必须确保没有TLS会话正在使用该配置）
// 重要提示：只有当没有tlsctx_p引用此配置时才能调用此函数
void tls_freeConf (tlsconf_t* conf) {
    if( conf == NULL )                   // 空指针检查
        return;
    
    // 释放可信CA证书链
    if( conf->trust ) {
        mbedtls_x509_crt_free(conf->trust);      // 释放mbedTLS证书结构
        rt_free(conf->trust);                    // 释放证书内存
        conf->trust = NULL;                      // 避免悬空指针
    }
    
    // 释放客户端证书
    if( conf->mycert ) {
        mbedtls_x509_crt_free(conf->mycert);     // 释放mbedTLS证书结构
        rt_free(conf->mycert);                   // 释放证书内存
        conf->mycert = NULL;                     // 避免悬空指针
    }
    
    // 释放客户端私钥
    if( conf->mykey ) {
        mbedtls_pk_free(conf->mykey);            // 释放mbedTLS私钥结构
        rt_free(conf->mykey);                    // 释放私钥内存
        conf->mykey = NULL;                      // 避免悬空指针
    }
    
    mbedtls_ssl_config_free(&conf->sslconfig);  // 释放SSL配置结构
    rt_free(conf);                               // 释放配置对象本身
}


// === 证书管理系统 ===

// 功能：读取和解析X.509证书 - 从文件或内存数据加载证书
// 参数：pcas - 证书对象指针的指针（输入/输出参数）
//       cafile - 证书文件路径或证书数据指针
//       len - 证书数据长度（<=0表示从文件读取）
//       what - 证书类型描述（用于日志输出）
// 返回值：1=成功，0=失败
// 调用时机：需要设置可信CA或客户端证书时调用
static int _readCAs (mbedtls_x509_crt** pcas, const char* cafile, int len, const char* what) {
    mbedtls_x509_crt* cas;               // 证书对象指针
    
    // 检查是否已有证书对象，如有则复用（先释放旧内容）
    if( *pcas ) {
        mbedtls_x509_crt_free(*pcas);    // 释放旧证书内容
        cas = *pcas;                     // 复用证书对象
        *pcas = NULL;                    // 临时清空指针
    } else {
        cas = rt_malloc(mbedtls_x509_crt);   // 分配新证书对象
        mbedtls_x509_crt_init(cas);          // 初始化证书对象
    }
    
    int ret;                             // mbedTLS返回值
    u1_t * certb = (u1_t*)cafile;        // 证书数据指针
    int certl = len;                     // 证书数据长度
    
    // 如果长度<=0，则从文件读取证书
    if( len <= 0 ) {
        dbuf_t dbuf = sys_readFile(cafile);      // 读取证书文件
        if( dbuf.buf == NULL ) {
            LOG(MOD_AIO|ERROR, "Reading %s from '%s' failed", what, cafile);
            goto errexit;               // 文件读取失败
        }
        certb = (u1_t*)dbuf.buf;         // 获取文件数据
        certl = dbuf.bufsize;            // 获取文件大小
        
        // 如果是PEM格式（以'-'开头），mbedTLS需要字符串结尾'\0'
        certl += certb[0] == '-' ? 1 : 0;
    }
    
    // 解析证书数据
    if( (ret = mbedtls_x509_crt_parse(cas, certb, certl)) != 0 ) {
        log_mbedError(ERROR, ret, "Parsing %s certificate", what);
        goto errexit;                   // 证书解析失败
    }
    
    // 如果是从文件读取的证书，输出证书信息到日志
    if( len <= 0 ) {
        // 复用缓冲区来打印证书信息
        mbedtls_x509_crt_info( (char*)certb, certl, "", cas );
        LOG(MOD_AIO|INFO,"%s: \n%s", cafile, certb);
        rt_free(certb);                 // 释放文件读取的缓冲区
    }

    *pcas = cas;                        // 设置输出参数
    return 1;                           // 成功返回

errexit:
    if( len <= 0 )                      // 从文件读取时需要释放缓冲区
        rt_free(certb);
    mbedtls_x509_crt_free(cas);         // 释放证书对象内容
    rt_free(cas);                       // 释放证书对象
    return 0;                           // 失败返回
}


// 功能：设置客户端证书和私钥 - 配置TLS客户端认证所需的证书和私钥
// 参数：conf - TLS配置对象
//       cert - 证书文件路径或证书数据
//       certlen - 证书数据长度（<=0表示从文件读取）
//       key - 私钥文件路径或私钥数据
//       keylen - 私钥数据长度（<=0表示从文件读取）
//       pwd - 私钥密码（可选）
// 返回值：1=成功，0=失败
// 调用时机：需要双向认证时设置客户端证书和私钥
int tls_setMyCert (tlsconf_t* conf, const char* cert, int certlen, const char* key, int keylen, const char* pwd) {
    mbedtls_pk_context* mykey;           // 私钥上下文指针
    
    // 检查是否已有私钥对象，如有则复用（先释放旧内容）
    if( conf->mykey ) {
        mbedtls_pk_free(conf->mykey);    // 释放旧私钥内容
        mykey = conf->mykey;             // 复用私钥对象
        conf->mykey = NULL;              // 临时清空指针
    } else {
        mykey = rt_malloc(mbedtls_pk_context);   // 分配新私钥对象
        mbedtls_pk_init(mykey);                  // 初始化私钥对象
    }
    
    int ret;                             // mbedTLS返回值
    u1_t* keyb = (u1_t*)key;             // 私钥数据指针
    int keyl = keylen;                   // 私钥数据长度
    
    // 如果长度<=0，则从文件读取私钥
    if( keylen <= 0 ) {
        dbuf_t dbuf = sys_readFile(key);         // 读取私钥文件
        if( dbuf.buf == NULL ) {
            LOG(MOD_AIO|ERROR, "Failed Reading keyfile '%s", key);
            goto errexit;               // 文件读取失败
        }
        keyb = (u1_t*)dbuf.buf;          // 获取文件数据
        keyl = dbuf.bufsize+1;           // 文件大小+1（确保字符串结尾）
    }
    
    // 解析私钥数据（支持密码保护的私钥）
    if( (ret = mbedtls_pk_parse_key(mykey, keyb, keyl, 
                                    (const u1_t*)pwd, 
                                    pwd?strlen(pwd):0)) != 0 ) {
        log_mbedError(ERROR, ret, "Parsing key");
        goto errexit;                   // 私钥解析失败
    }
    
    // 如果是从文件读取，释放文件缓冲区
    if( keylen <= 0 ) {
        rt_free(keyb);                   // 释放私钥文件缓冲区
        keyb = NULL;                     // 避免悬空指针
    }
    
    // 读取并设置客户端证书
    if( !_readCAs(&conf->mycert, cert, certlen, "my cert") )
        goto errexit;                   // 证书读取失败
    
    conf->mykey = mykey;                 // 设置私钥到配置中
    
    // 将证书和私钥绑定到SSL配置
    if( (ret = mbedtls_ssl_conf_own_cert(&conf->sslconfig, conf->mycert, conf->mykey)) != 0 ) {
        log_mbedError(ERROR, ret, "Using my key/cert");
        return 0;                       // 绑定失败
    }
    
    return 1;                           // 成功返回

errexit:
    if( keylen <= 0 )                   // 从文件读取时需要释放缓冲区
        rt_free(keyb);
    mbedtls_pk_free(mykey);             // 释放私钥对象内容
    rt_free(mykey);                     // 释放私钥对象
    return 0;                           // 失败返回
}


// 功能：设置可信CA证书链 - 配置用于验证服务器证书的可信CA证书
// 参数：conf - TLS配置对象
//       ca - CA证书文件路径或证书数据
//       calen - CA证书数据长度（<=0表示从文件读取）
// 返回值：1=成功，0=失败
// 调用时机：需要验证服务器证书时设置可信CA证书链
int tls_setTrustedCAs (tlsconf_t* conf, const char* ca, int calen) {
    if( !_readCAs(&conf->trust, ca, calen, "trust") )   // 读取CA证书
        return 0;                                       // 读取失败
    
    // 将CA证书链设置到SSL配置中
    mbedtls_ssl_conf_ca_chain(&conf->sslconfig, conf->trust, NULL);
    return 1;                                           // 设置成功
}

// === TLS会话管理系统 ===

// 功能：创建TLS会话上下文 - 基于配置创建具体的TLS连接会话
// 参数：conf - TLS配置对象
//       servername - 服务器名称（用于SNI扩展）
// 返回值：TLS会话上下文指针，失败时返回NULL
// 调用时机：建立TLS连接前创建会话上下文
tlsctx_p tls_makeSession (tlsconf_t* conf, const char* servername) {
    mbedtls_ssl_context* sslctx = rt_malloc(mbedtls_ssl_context);  // 分配SSL上下文
    mbedtls_ssl_init(sslctx);                           // 初始化SSL上下文
    
    int ret;                                            // mbedTLS返回值
    
    // 使用配置设置SSL会话
    if( (ret = mbedtls_ssl_setup(sslctx, &conf->sslconfig)) != 0 ) {
        log_mbedError(ERROR, ret, "mbedtls_ssl_setup failed");
      fail:
        mbedtls_ssl_free(sslctx);                       // 释放SSL上下文内容
        rt_free(sslctx);                                // 释放SSL上下文对象
        return NULL;                                    // 返回失败
    }
    
    // 如果提供了服务器名称且启用SNI，设置服务器名称指示
    if( servername && TLS_SNI ) {
        if( (ret = mbedtls_ssl_set_hostname(sslctx, servername)) != 0 ) {
            log_mbedError(ERROR, ret, "mbedtls_ssl_set_hostname");
            goto fail;                                  // 设置服务器名称失败
        }
    }
    
    // 注意：BIO设置将在ws_connect/http_connect中完成
    //mbedtls_ssl_set_bio(sslctx, netctx, mbedtls_net_send, mbedtls_net_recv, NULL);
    return sslctx;                                      // 返回成功创建的会话
}

// 功能：释放TLS会话上下文 - 清理SSL会话占用的资源
// 参数：tlsctx - TLS会话上下文指针
// 返回值：无
// 调用时机：TLS连接关闭后释放会话资源
// 注意：此函数不会释放TLS配置（因为配置可能被多个会话共享）
void tls_freeSession (tlsctx_p tlsctx) {
    if( tlsctx != NULL ) {                              // 空指针检查
        mbedtls_ssl_free(tlsctx);                       // 释放SSL上下文内容
        rt_free(tlsctx);                                // 释放SSL上下文对象
    }
}

// === TLS数据传输接口 ===

// 功能：TLS写入数据 - 通过TLS连接发送数据（带加密）或直接发送数据
// 参数：netctx - 网络上下文
//       tlsctx - TLS会话上下文（可选）
//       p - 要发送的数据指针
//       sz - 要发送的数据大小
// 返回值：实际发送的字节数或错误码
// 调用时机：需要通过网络连接发送数据时调用
int tls_write(mbedtls_net_context* netctx, tlsctx_p tlsctx, const u1_t* p, size_t sz) {
    if( tlsctx )                                        // 如果有TLS上下文
        return mbedtls_ssl_write(tlsctx, p, sz);        // 使用TLS加密传输
    return mbedtls_net_send(netctx, p, sz);             // 否则直接发送明文数据
}

// 功能：TLS读取数据 - 通过TLS连接接收数据（带解密）或直接接收数据
// 参数：netctx - 网络上下文
//       tlsctx - TLS会话上下文（可选）
//       p - 接收数据的缓冲区指针
//       sz - 缓冲区大小
// 返回值：实际接收的字节数或错误码
// 调用时机：需要从网络连接接收数据时调用
int tls_read(mbedtls_net_context* netctx, tlsctx_p tlsctx, u1_t* p, size_t sz) {
    if( tlsctx )                                        // 如果有TLS上下文
        return mbedtls_ssl_read(tlsctx, p, sz);         // 使用TLS解密接收
    return mbedtls_net_recv(netctx, p, sz);             // 否则直接接收明文数据
}

