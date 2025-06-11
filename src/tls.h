/*
 * TLS - 传输层安全模块头文件
 * ===========================
 * 功能：BasicStation的TLS/SSL安全通信实现
 * 核心：证书管理、安全会话、加密连接、双向认证
 */

#ifndef _tls_h_
#define _tls_h_

#include "rt.h"  // 运行时基础类型
#include "mbedtls/ssl.h"
#include "mbedtls/net.h"

typedef struct tlsconf tlsconf_t;
typedef struct mbedtls_ssl_context* tlsctx_p;

// TLS证书类型枚举
enum {
    CERT_UNDEF = 0,      // 未定义：无证书类型
    CERT_DER   = 1,      // DER格式：二进制编码的证书
    CERT_PEM   = 2,      // PEM格式：Base64编码的证书
};

/**
 * struct tlscreds - TLS凭证结构
 * 功能：存储TLS连接所需的证书和密钥信息
 */
typedef struct tlscreds {
    u1_t  certtype;     // 证书类型：DER或PEM格式
    str_t cacert;       // CA证书：根证书或证书链
    str_t clientcert;   // 客户端证书：用于双向认证的客户端证书
    str_t clientkey;    // 客户端私钥：与客户端证书配对的私钥
} tlscreds_t;

extern u1_t tls_dbgLevel;
void log_mbedError (u1_t mod_level, int ret, const char* fmt, ...);

tlsconf_t* tls_makeConf      ();
void       tls_freeConf      (tlsconf_t* conf);
int        tls_setMyCert     (tlsconf_t* conf, const char* cert, int certlen, const char* key, int keylen, const char* pwd);
int        tls_setTrustedCAs (tlsconf_t* conf, const char* file_or_data, int len);
tlsctx_p   tls_makeSession   (tlsconf_t* conf, const char* servername);
void       tls_freeSession   (tlsctx_p tlsctx);

int tls_read  (mbedtls_net_context* netctx, tlsctx_p tlsctx,       u1_t* p, size_t sz);
int tls_write (mbedtls_net_context* netctx, tlsctx_p tlsctx, const u1_t* p, size_t sz);

// TLS系统管理API
void tls_ini ();        // 初始化TLS系统：配置加密库和随机数生成器
void tls_free ();       // 释放TLS资源：清理加密上下文和内存
void tls_debug (int debug); // 设置调试级别：控制TLS调试信息输出

// 全局TLS配置
extern u1_t TLS_NOCHECKCERT;  // 证书验证开关：1=跳过证书验证，0=严格验证

#endif // _tls_h_
