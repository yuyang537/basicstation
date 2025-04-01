/*
 * 模块概述
 * ========
 * 本模块实现了LoRaWAN基站的TLS安全层，提供基于mbedTLS库的安全通信能力。
 * 它为基站与网络服务器之间的通信提供加密、认证和完整性保护，确保数据传输
 * 的安全性。该模块是基站安全通信的基础，支持各种TLS相关操作，包括证书管理、
 * 安全会话建立和加密数据交换。
 * 
 * 文件功能
 * ========
 * 本文件主要实现以下功能：
 * - TLS配置和会话管理
 * - 证书加载和解析
 * - 安全套接字通信
 * - 密钥管理
 * - 错误处理和日志记录
 * - 随机数生成
 * 
 * 主要组件
 * ========
 * 1. TLS配置管理
 *    - SSL配置初始化和定制
 *    - 认证模式设置
 *    - 最大分片长度控制
 *    - 调试级别控制
 * 
 * 2. 证书管理系统
 *    - 信任根证书加载
 *    - 客户端证书加载
 *    - 私钥管理
 *    - 证书解析和验证
 * 
 * 3. 安全会话管理
 *    - 会话创建和销毁
 *    - 服务器名称指示(SNI)
 *    - TLS握手处理
 *    - 会话状态管理
 * 
 * 4. 安全通信接口
 *    - 加密数据读取
 *    - 加密数据写入
 *    - 错误处理和恢复
 *    - 会话重用
 * 
 * 5. 随机数生成
 *    - 熵源管理
 *    - 随机数生成器初始化
 *    - 系统随机数接口
 * 
 * 关键流程
 * ========
 * 1. TLS初始化流程
 *    - 配置创建和默认值设置
 *    - 随机数生成器初始化
 *    - 证书和私钥加载
 *    - 验证设置
 * 
 * 2. 安全会话建立流程
 *    - 会话对象创建
 *    - 服务器连接
 *    - TLS握手
 *    - 证书验证
 * 
 * 3. 安全通信流程
 *    - 数据加密
 *    - 安全传输
 *    - 数据解密
 *    - 错误处理
 * 
 * 注意事项
 * ========
 * 1. 安全考虑
 *    - 证书管理的重要性
 *    - 私钥保护
 *    - 随机数质量要求
 *    - 证书验证策略
 * 
 * 2. 资源使用
 *    - 内存占用管理
 *    - 会话资源释放
 *    - 处理性能影响
 * 
 * 3. 错误处理
 *    - TLS错误代码解析
 *    - 连接失败恢复
 *    - 证书问题诊断
 *    - 调试信息级别
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

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/certs.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"

#include "s2conf.h"
#include "sys.h"
#include "rt.h"
#include "uj.h"
#include "tls.h"

struct tlsconf {
    mbedtls_ssl_config  sslconfig;
    mbedtls_x509_crt*   trust;
    mbedtls_x509_crt*   mycert;
    mbedtls_pk_context* mykey;
};

u1_t tls_dbgLevel;

#if defined(CFG_sysrandom)
int tls_random (void * arg, unsigned char * buf, size_t len) {
    return sys_random(buf, (int)len);
}
#else
typedef struct {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
} drbg_t;

static drbg_t* DBRG;

static mbedtls_ctr_drbg_context* assertDBRG () {
    if( DBRG != NULL )
        return &DBRG->ctr_drbg;
    DBRG = rt_malloc(drbg_t);
    mbedtls_entropy_init(&DBRG->entropy);
    mbedtls_ctr_drbg_init(&DBRG->ctr_drbg);
    u1_t seed[16];
    int ret;
    sys_seed(seed, sizeof(seed));
    if( (ret = mbedtls_ctr_drbg_seed(&DBRG->ctr_drbg,
                                     mbedtls_entropy_func, &DBRG->entropy,
                                     seed, sizeof(seed))) != 0 )
        rt_fatal("mbedtls_ctr_drbg_seed failed: %d", ret);

#ifdef CFG_max_tls_frag_len
    // Report this only once - at startup
    LOG(MOD_AIO|WARNING, "TLS is using a maximum fragment length of %d bytes", 256 << CFG_max_tls_frag_len);
#endif // CFG_max_tls_frag_len

    return &DBRG->ctr_drbg;
}
#endif


#if defined(CFG_tlsdebug)
static void log_mbedDebug (void *ctx, int level, const char *file, int line, const char *str) {
    LOG(MOD_AIO|DEBUG, "%s:%d MBEDTLS[%d]: %s", file,line, level, str);
}
#endif // CFG_tlsdebug


void log_mbedError (u1_t mod_level, int ret, const char* fmt, ...) {
    dbuf_t b;
    if( !log_special(mod_level, &b) )
        return;
    va_list ap;
    va_start(ap, fmt);
    vxprintf(&b, fmt, ap);
    xputs(&b, ": ", -1);
    char* p = b.buf + b.pos;
    mbedtls_strerror(ret, p, b.bufsize - b.pos);
    b.pos += strlen(p);
    va_end(ap);
    log_specialFlush(b.pos);
}


tlsconf_t* tls_makeConf (mbedtls_x509_crt* trust, mbedtls_x509_crt* mycert, mbedtls_pk_context* mykey) {
    tlsconf_t* conf = rt_malloc(tlsconf_t);
    mbedtls_ssl_config_init(&conf->sslconfig);
    conf->trust  = NULL;
    conf->mycert = NULL;
    conf->mykey  = NULL;
    int ret;
    if( (ret = mbedtls_ssl_config_defaults(&conf->sslconfig,
                                           MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT ) ) != 0 )
        rt_fatal("mbedtls_ssl_config_defaults", ret);
#if defined(CFG_sysrandom)
    mbedtls_ssl_conf_rng     (&conf->sslconfig, tls_random, NULL);
#else
    mbedtls_ssl_conf_rng     (&conf->sslconfig, mbedtls_ctr_drbg_random, assertDBRG());
#endif
    mbedtls_ssl_conf_authmode(&conf->sslconfig, MBEDTLS_SSL_VERIFY_REQUIRED);
#if defined(CFG_max_tls_frag_len)
    if( (ret = mbedtls_ssl_conf_max_frag_len(&conf->sslconfig, CFG_max_tls_frag_len)) != 0)
        rt_fatal("mbedtls_ssl_conf_max_frag_len", ret);
#endif // CFG_max_tls_frag_len
#if defined(CFG_tlsdebug)
    mbedtls_ssl_conf_dbg(&conf->sslconfig, log_mbedDebug, NULL );
    mbedtls_debug_set_threshold(tls_dbgLevel);  // 0=off, 1=error, 2=state change, 3=info, 4=verbose
#endif // CFG_tlsdebug
    return conf;
}


// NOTE: this must only be done if no tlsctx_p is referencing this conf.
void tls_freeConf (tlsconf_t* conf) {
    if( conf == NULL )
        return;
    if( conf->trust ) {
        mbedtls_x509_crt_free(conf->trust);
        rt_free(conf->trust);
        conf->trust = NULL;
    }
    if( conf->mycert ) {
        mbedtls_x509_crt_free(conf->mycert);
        rt_free(conf->mycert);
        conf->mycert = NULL;
    }
    if( conf->mykey ) {
        mbedtls_pk_free(conf->mykey);
        rt_free(conf->mykey);
        conf->mykey = NULL;
    }
    mbedtls_ssl_config_free(&conf->sslconfig);
    rt_free(conf);
}


static int _readCAs (mbedtls_x509_crt** pcas, const char* cafile, int len, const char* what) {
    mbedtls_x509_crt* cas;
    if( *pcas ) {
        mbedtls_x509_crt_free(*pcas);
        cas = *pcas;
        *pcas = NULL;
    } else {
        cas = rt_malloc(mbedtls_x509_crt);
        mbedtls_x509_crt_init(cas);
    }
    int ret;
    u1_t * certb = (u1_t*)cafile;
    int certl = len;
    if( len <= 0 ) {
        dbuf_t dbuf = sys_readFile(cafile);
        if( dbuf.buf == NULL ) {
            LOG(MOD_AIO|ERROR, "Reading %s from '%s' failed", what, cafile);
            goto errexit;
        }
        certb = (u1_t*)dbuf.buf;
        certl = dbuf.bufsize;
        // '\0' at the end expected by mbedTLS if PEM
        certl += certb[0] == '-' ? 1 : 0;
    }
    if( (ret = mbedtls_x509_crt_parse(cas, certb, certl)) != 0 ) {
        log_mbedError(ERROR, ret, "Parsing %s certificate", what);
        goto errexit;
    }
    if( len <= 0 ) {
        // Reuse buffer to print cert info before freeing
        mbedtls_x509_crt_info( (char*)certb, certl, "", cas );
        LOG(MOD_AIO|INFO,"%s: \n%s", cafile, certb);
        rt_free(certb);
    }

    *pcas = cas;
    return 1;
errexit:
    if( len <= 0 )
        rt_free(certb);
    mbedtls_x509_crt_free(cas);
    rt_free(cas);
    return 0;
}


int tls_setMyCert (tlsconf_t* conf, const char* cert, int certlen, const char* key, int keylen, const char* pwd) {
    mbedtls_pk_context* mykey;
    if( conf->mykey ) {
        mbedtls_pk_free(conf->mykey);
        mykey = conf->mykey;
        conf->mykey = NULL;
    } else {
        mykey = rt_malloc(mbedtls_pk_context);
        mbedtls_pk_init(mykey);
    }
    int ret;
    u1_t* keyb = (u1_t*)key;
    int keyl = keylen;
    if( keylen <= 0 ) {
        dbuf_t dbuf = sys_readFile(key);
        if( dbuf.buf == NULL ) {
            LOG(MOD_AIO|ERROR, "Failed Reading keyfile '%s", key);
            goto errexit;
        }
        keyb = (u1_t*)dbuf.buf;
        keyl = dbuf.bufsize+1;
    }
    if( (ret = mbedtls_pk_parse_key(mykey, keyb, keyl, (const u1_t*)pwd, pwd?strlen(pwd):0)) != 0 ) {
        log_mbedError(ERROR, ret, "Parsing key");
        goto errexit;
    }
    if( keylen <= 0 ) {
        rt_free(keyb);
        keyb = NULL;
    }
    if( !_readCAs(&conf->mycert, cert, certlen, "my cert") )
        goto errexit;
    conf->mykey = mykey;
    if( (ret = mbedtls_ssl_conf_own_cert(&conf->sslconfig, conf->mycert, conf->mykey)) != 0 ) {
        log_mbedError(ERROR, ret, "Using my key/cert");
        
        return 0;
    }
    return 1;
errexit:
    if( keylen <= 0 )
        rt_free(keyb);
    mbedtls_pk_free(mykey);
    rt_free(mykey);
    return 0;
}


int tls_setTrustedCAs (tlsconf_t* conf, const char* ca, int calen) {
    if( !_readCAs(&conf->trust, ca, calen, "trust") )
        return 0;
    mbedtls_ssl_conf_ca_chain(&conf->sslconfig, conf->trust, NULL);
    return 1;
}


tlsctx_p tls_makeSession (tlsconf_t* conf, const char* servername) {
    mbedtls_ssl_context* sslctx = rt_malloc(mbedtls_ssl_context);
    mbedtls_ssl_init(sslctx);
    int ret;
    if( (ret = mbedtls_ssl_setup(sslctx, &conf->sslconfig)) != 0 ) {
        log_mbedError(ERROR, ret, "mbedtls_ssl_setup failed");
      fail:
        mbedtls_ssl_free(sslctx);
        rt_free(sslctx);
        return NULL;
    }
    if( servername && TLS_SNI ) {
        if( (ret = mbedtls_ssl_set_hostname(sslctx, servername)) != 0 ) {
            log_mbedError(ERROR, ret, "mbedtls_ssl_set_hostname");
            goto fail;
        }
    }
    // To be done in ws_connect/http_connect
    //mbedtls_ssl_set_bio(sslctx, netctx, mbedtls_net_send, mbedtls_net_recv, NULL);
    return sslctx;
}

// NOTE: this does not free the TLS config (since it could be shared among multiple sessions)
void tls_freeSession (tlsctx_p tlsctx) {
    if( tlsctx != NULL ) {
        mbedtls_ssl_free(tlsctx);
        rt_free(tlsctx);
    }
}

int tls_write(mbedtls_net_context* netctx, tlsctx_p tlsctx, const u1_t* p, size_t sz) {
    if( tlsctx )
        return mbedtls_ssl_write(tlsctx, p, sz);
    return mbedtls_net_send(netctx, p, sz);
}

int tls_read(mbedtls_net_context* netctx, tlsctx_p tlsctx, u1_t* p, size_t sz) {
    if( tlsctx )
        return mbedtls_ssl_read(tlsctx, p, sz);
    return mbedtls_net_recv(netctx, p, sz);
}

