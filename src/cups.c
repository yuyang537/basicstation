/*
 * BasicStation CUPS协议实现模块 - 配置和更新服务器通信
 * 
 * 主要功能：
 * 1. 配置同步服务 - 与CUPS服务器同步站点配置信息
 * 2. 固件更新管理 - 下载和验证固件更新包  
 * 3. 凭据管理 - 同步和更新CUPS/TC连接凭据
 * 4. 数字签名验证 - 基于ECDSA的固件完整性验证
 * 5. 自动重连机制 - 智能重试和故障转移策略
 * 
 * 设计特点：
 * - 基于HTTP协议的RESTful API通信
 * - 分段式数据传输支持大文件下载
 * - ECDSA数字签名确保固件安全性
 * - 多凭据集支持（REG/BAK/BOOT）故障转移
 * - 与TC模块协调的独占式连接管理
 * - 状态机驱动的可靠协议处理
 * 
 * --- Revised 3-Clause BSD License ---
 * Copyright Semtech Corporation 2022. All rights reserved.
 */

#include "s2conf.h"               // Station配置系统
#include "sys.h"                  // 系统核心功能（URI管理、凭据管理）
#include "uj.h"                   // JSON编码器（请求格式化）
#include "kwcrc.h"                // 关键字CRC（凭据完整性）
#include "cups.h"                 // CUPS模块接口定义
#include "tc.h"                   // TC传输控制器（协调连接）

#include "mbedtls/ecdsa.h"        // ECDSA数字签名验证
#include "mbedtls/error.h"        // mbedTLS错误处理
#include "mbedtls/sha512.h"       // SHA-512哈希计算
#include "mbedtls/bignum.h"       // 大整数运算（椭圆曲线密码学）

// === 常量定义 ===
#define FAIL_CNT_THRES 6          // 失败计数阈值（触发凭据轮换）
#define SIGCRC_LEN 4              // 签名CRC长度（字节）

// === 全局状态变量 ===
static tmr_t   cups_sync_tmr;     // CUPS同步定时器
static cups_t* CUPS;              // 当前CUPS会话指针（单例模式）
static int     cups_credset = SYS_CRED_REG;  // 当前使用的凭据集（REG/BAK/BOOT）
static int     cups_failCnt;      // 连续失败计数器
static s1_t    cstateLast;        // 最后一次CUPS状态（用于状态查询）

// === 数字签名验证系统 ===

// CUPS数字签名结构体 - 封装固件更新的ECDSA签名信息
struct cups_sig {
    mbedtls_sha512_context sha;       // SHA-512哈希上下文（用于计算固件哈希）
    u1_t     signature[128];          // ECDSA签名数据（最大128字节）
    u1_t     hash[64];                // SHA-512哈希值（64字节）
    u1_t     len;                     // 签名实际长度
    union {
        u4_t keycrc;                  // 密钥CRC校验值（32位）
        u1_t keycrcb[4];              // 密钥CRC字节数组形式
    };
};

// 功能：验证ECDSA数字签名 - 确保固件更新的完整性和真实性
// 参数：sig - 包含签名和哈希的签名结构体
// 返回值：1=验证成功，0=验证失败
// 调用时机：收到固件更新包和签名后进行验证
static int cups_verifySig (cups_sig_t* sig) {
    int verified = 0;                 // 验证结果标志
    dbuf_t key;                       // 密钥数据缓冲区
    int keyid = -1;                   // 密钥ID索引
    
    // 遍历所有可用的验证密钥，直到找到能验证签名的密钥
    while ( (key = sys_sigKey(++keyid)).buf != NULL && !verified ) {
        if ( key.bufsize != 64 )      // 验证密钥长度（64字节=32字节X坐标+32字节Y坐标）
            continue;

        // 初始化椭圆曲线密钥对和ECDSA上下文
        mbedtls_ecp_keypair k;
        mbedtls_ecp_keypair_init(&k);
        mbedtls_ecdsa_context ecdsa;
        mbedtls_ecdsa_init(&ecdsa);
        
        int ret;                      // mbedTLS返回值
        
        // 设置椭圆曲线参数和公钥，验证签名
        if ((ret = mbedtls_ecp_group_load        (&k.grp, MBEDTLS_ECP_DP_SECP256R1) ) ||  // 加载SECP256R1曲线
            (ret = mbedtls_mpi_read_binary       (&k.Q.X, (u1_t*)key.buf, 32)       ) ||  // 读取公钥X坐标
            (ret = mbedtls_mpi_read_binary       (&k.Q.Y, (u1_t*)key.buf+32, 32)    ) ||  // 读取公钥Y坐标
            (ret = mbedtls_mpi_lset              (&k.Q.Z, 1)                        ) ||  // 设置Z坐标为1（仿射坐标）
            (ret = mbedtls_ecp_check_pubkey      (&k.grp, &k.Q)                     ) ||  // 验证公钥有效性
            (ret = mbedtls_ecdsa_from_keypair    (&ecdsa, &k)                       ) ||  // 从密钥对创建ECDSA上下文
            (ret = mbedtls_ecdsa_read_signature  (&ecdsa, sig->hash, sizeof(sig->hash), 
                                                  sig->signature, sig->len ))            // 验证签名
         ) {
            verified = 0;             // 任何步骤失败都表示验证失败
        } else {
            verified = 1;             // 所有步骤成功表示验证通过
        }
        
        // 释放椭圆曲线和ECDSA资源
        mbedtls_ecp_keypair_free(&k);
        mbedtls_ecdsa_free(&ecdsa);

        LOG(MOD_CUP|INFO, "ECDSA key#%d -> %s", keyid, verified? "VERIFIED" : "NOT verified");
    }
    
    sys_sigKey(-1);                   // 释放密钥内存
    
    if (!verified) {
        LOG(MOD_CUP|WARNING, "No key could verify signature. Tried %d keys", keyid);
    }
    return verified;                  // 返回验证结果
}

// === CUPS会话完成处理系统 ===

// 功能：CUPS会话完成后的处理逻辑 - 处理更新结果并安排下次同步
// 参数：tmr - 定时器指针（未使用）
// 返回值：无
// 调用时机：CUPS会话结束时（成功或失败）由定时器回调
static void cups_ondone (tmr_t* tmr) {
    if( CUPS == NULL ) {                  // 没有活动的CUPS会话
        sys_triggerCUPS(0);               // 立即触发新的CUPS会话
        return;
    }

    str_t msg="", detail="";              // 日志消息和详细信息
    ustime_t ahead = CUPS_RESYNC_INTV;    // 下次同步间隔（默认重试间隔）
    u1_t log = 1;                         // 是否记录日志标志

    if( CUPS->cstate != CUPS_DONE ) {     // CUPS会话失败处理
        // 会话失败 - 准备快速重试
        msg = "Interaction with CUPS failed%s - retrying in %~T";
        
        // 检查是否需要轮换凭据集（连续失败或特定错误）
        if( cups_failCnt > FAIL_CNT_THRES ||           // 连续失败次数超过阈值
            CUPS->cstate == CUPS_ERR_REJECTED ||       // 服务器拒绝连接
            CUPS->cstate == CUPS_ERR_NOURI ) {          // 没有配置URI
            // 轮换凭据集：REG -> BAK -> BOOT -> REG
            cups_credset = (cups_credset+1) % (SYS_CRED_BOOT+1);
        }
        cups_failCnt += 1;                // 增加失败计数
        
        if (CUPS->cstate == CUPS_ERR_NOURI)
            log = 0;                      // URI错误已经记录，避免重复日志
            
    } else {                              // CUPS会话成功处理
        u1_t uflags = CUPS->uflags;       // 获取更新标志
        
        // 处理固件更新
        if( uflags & UPDATE_FLAG(UPDATE) ) {
            LOG(MOD_CUP|INFO, "CUPS provided update.bin");
            u1_t run_update = 0;          // 是否执行更新标志
            
            // 检查是否有数字签名
            if( (uflags & UPDATE_FLAG(SIGNATURE)) ) {
                LOG(MOD_CUP|INFO, "CUPS provided signature len=%d keycrc=%08X", 
                    CUPS->sig->len, CUPS->sig->keycrc);
                assert( CUPS->sig );
                
                // 完成SHA-512哈希计算
                mbedtls_sha512_finish( &CUPS->sig->sha, CUPS->sig->hash );
                mbedtls_sha512_free  ( &CUPS->sig->sha );
                
                // 验证数字签名
                run_update = cups_verifySig(CUPS->sig);
            } else {
                // 没有签名的情况处理
                dbuf_t key = sys_sigKey(0);
                if ( key.buf == NULL ) {
                    LOG(MOD_CUP|INFO, "No Key. No Sig. UPDATE.");
                    run_update = 1;       // 没有密钥文件，允许无签名更新
                } else {
                    LOG(MOD_CUP|ERROR, "Keyfile present, but no signature provided. Aborting update.");
                    sys_sigKey(-1);       // 释放密钥内存
                }
            }
            
            // 执行或取消更新
            if (run_update) {
                LOG(MOD_CUP|INFO, "Running update.bin as background process");
                sys_runUpdate();          // 在后台执行固件更新
            } else {
                LOG(MOD_CUP|INFO, "Aborting update.");
                sys_abortUpdate();        // 取消固件更新
            }
        }
        
        // 处理TC连接更新（URI或凭据变化）
        if( uflags & (UPDATE_FLAG(TC_URI)|UPDATE_FLAG(TC_CRED)) ) {
            str_t s = ((uflags & UPDATE_FLAG(TC_URI )) == UPDATE_FLAG(TC_URI ) ? "uri" :
                       (uflags & UPDATE_FLAG(TC_CRED)) == UPDATE_FLAG(TC_CRED) ? "credentials" :
                       "uri/credentials");
            LOG(MOD_CUP|INFO, "CUPS provided TC updates (%s) %s", s, 
                sys_noTC ? "" : "- restarting TC engine");
            sys_stopTC();                 // 停止TC连接以应用新配置
        }
        
        // 处理CUPS连接更新（URI或凭据变化）
        if( uflags & (UPDATE_FLAG(CUPS_URI)|UPDATE_FLAG(CUPS_CRED)) ) {
            detail = ((uflags & UPDATE_FLAG(CUPS_URI )) == UPDATE_FLAG(CUPS_URI ) ? "uri" :
                      (uflags & UPDATE_FLAG(CUPS_CRED)) == UPDATE_FLAG(CUPS_CRED) ? "credentials" :
                      "uri/credentials");
            msg = "CUPS provided CUPS updates (%s) - reconnecting in %~T";
            // 立即重连以使用新的CUPS设置
        } else {
            // 正常完成，没有CUPS配置变化
            detail = uflags ? "" : " (no updates)";
            msg = "Interaction with CUPS done%s - next regular check in %~T";
            ahead = CUPS_OKSYNC_INTV;     // 使用正常同步间隔
        }
        
        // 重置凭据集和失败计数
        cups_credset = SYS_CRED_REG;      // 回到主凭据集
        cups_failCnt = 0;                 // 清零失败计数
    }
    
    // 如果TC已连接，使用较长的同步间隔
    if( TC && sys_statusTC() == TC_MUXS_CONNECTED )
        ahead = CUPS_OKSYNC_INTV;
    
    // 清理当前CUPS会话
    cups_free(CUPS);
    CUPS = NULL;
    
    // 记录处理结果
    if (log)
        LOG(MOD_CUP|INFO, msg, detail, ahead);
    
    // 重启TC连接并安排下次CUPS同步
    sys_startTC();
    rt_setTimer(&cups_sync_tmr, rt_micros_ahead(ahead));
}


// 功能：完成CUPS会话处理 - 设置会话状态并触发完成回调
// 参数：cups - CUPS会话对象
//       cstate - 最终状态码
// 返回值：无
// 调用时机：CUPS会话结束时（成功或出错）
static void cups_done (cups_t* cups, s1_t cstate) {
    cups->cstate = cstate;                // 设置最终状态
    http_free(&cups->hc);                 // 释放HTTP连接资源
    rt_yieldTo(&cups->timeout, cups_ondone);  // 将定时器转为完成处理回调
    sys_inState(SYSIS_CUPS_DONE);         // 通知系统CUPS会话完成
}

// 功能：CUPS连接超时处理函数 - 处理网络超时情况
// 参数：tmr - 超时定时器指针
// 返回值：无
// 调用时机：CUPS连接或数据传输超时时由定时器触发
static void cups_timeout (tmr_t* tmr) {
    cups_t* cups = timeout2cups(tmr);     // 从定时器获取CUPS会话对象
    LOG(MOD_CUP|ERROR, "CUPS timed out");
    cups_done(cups, CUPS_ERR_TIMEOUT);   // 以超时错误结束会话
}

// 功能：计算段长度字段的字节数 - 根据CUPS协议状态确定长度字段大小
// 参数：cstate - CUPS协议状态
// 返回值：长度字段的字节数（1、2或4字节）
// 调用时机：解析CUPS响应时确定每个段的长度字段格式
static int sizelen (int cstate) {
    assert(cstate >= CUPS_FEED_CUPS_URI && cstate <= CUPS_FEED_UPDATE);
    // URI段用1字节长度，凭据段用2字节长度，签名/更新段用4字节长度
    return 1<<((cstate - CUPS_FEED_CUPS_URI)>>1);  // 1,2,4 for URI,CRED,SIG/UPDATE
}


static void cups_update_info (conn_t* _conn, int ev) {
    cups_t* cups = conn2cups(_conn);

    if( ev == HTTPEV_CONNECTED ) {
        dbuf_t cupsuri;
        dbuf_str(cupsuri, sys_uri(SYS_CRED_CUPS, cups_credset));
        LOG(MOD_CUP|VERBOSE, "Retrieving update-info from CUPS%s %s...", sys_credset2str(cups_credset), cupsuri.buf);
        struct uri_info cui;
        uri_parse(&cupsuri, &cui, 0); // Does not write
        dbuf_t b = http_getReqbuf(&cups->hc);
        xprintf(&b,
                "POST /update-info HTTP/1.1\r\n"
                "Host: %*s\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 00000\r\n"
                "%s\r\n",
                cui.hostportEnd-cui.hostportBeg, cupsuri.buf+cui.hostportBeg,
                cups->hc.c.authtoken ? cups->hc.c.authtoken : "");
        doff_t bodybeg = b.pos;
        xputs(&b, "{", -1);  // note: uj_encOpen would create a comma since b.pos>0
        str_t version = sys_version();
        uj_encKVn(&b,
                  "router",     '6', sys_eui(),
                  "cupsUri",    's', sys_uri(SYS_CRED_CUPS, SYS_CRED_REG),
                  "tcUri",      's', sys_uri(SYS_CRED_TC, SYS_CRED_REG),
                  "cupsCredCrc",'u', sys_crcCred(SYS_CRED_CUPS, SYS_CRED_REG),
                  "tcCredCrc",  'u', sys_crcCred(SYS_CRED_TC, SYS_CRED_REG),
                  "station",    's', CFG_version " " CFG_bdate,
                  "model",      's', CFG_platform,
                  "package",    's', version,
                  // "os",         's', sys_osversion(), 
                  NULL);
        uj_encKey  (&b, "keys");
        uj_encOpen (&b, '[');
        int keyid = -1;
        u4_t crc = 0;
        while( (crc = sys_crcSigkey(++keyid)) > 0 ) {
            uj_encUint(&b, (uL_t)crc);
        }
        uj_encClose(&b, ']');
        uj_encClose(&b, '}');
        http_setContentLength(b.buf, b.pos-bodybeg);
        LOG(MOD_CUP|DEBUG, "CUPS Request: %.*s", b.pos-bodybeg, b.buf+bodybeg);
        http_request(&cups->hc, &b);
        return;
    }
    if( ev == HTTPEV_RESPONSE ) {
        dbuf_t body = http_getBody(&cups->hc);
        s1_t cstate = cups->cstate;

        if( cups->cstate == CUPS_HTTP_REQ_PEND ) {
            // Hdr is only present in very first chunk
            dbuf_t hdr = http_getHdr(&cups->hc);
            int status = http_getStatus(&cups->hc);
            if( status != 200 ) {
                dbuf_t msg = http_statusText(&hdr);
                LOG(MOD_CUP|VERBOSE, "Failed to retrieve TCURI from CUPS: (%d) %.*s", status, msg.bufsize, msg.buf);
                cups->cstate = CUPS_ERR_REJECTED;
                http_close(&cups->hc);
                return;
            }
            if( cups_credset == SYS_CRED_REG )
                sys_backupConfig(SYS_CRED_CUPS);

            // We expect both CUPS/TC URI segments to fit into HTTP buffer
            u1_t cupsuri_len = body.buf[0];
            u1_t tcuri_len = body.buf[1+cupsuri_len];
            body.pos = 2+cupsuri_len+tcuri_len;  // after both URI segments
            if( body.bufsize < 2 || 1+body.pos > body.bufsize ) { // need one more byte for \0
                LOG(MOD_CUP|ERROR, "Malformed CUPS response: URI segments lengths (%u) exceed available data (%u)", body.pos, body.bufsize);
                goto proto_err;
            }
            sys_resetConfigUpdate();
            if( cupsuri_len ) {
                str_t uri = (str_t)body.buf+1;
                body.buf[1+cupsuri_len] = 0;
                sys_saveUri(SYS_CRED_CUPS, uri);
                LOG(MOD_CUP|INFO, "[Segment] CUPS URI: %s", uri);
                cups->uflags |= UPDATE_FLAG(CUPS_URI);
            }
            if( tcuri_len ) {
                char* uri = (char*)body.buf+2+cupsuri_len;
                char save = uri[tcuri_len];
                uri[tcuri_len] = 0;
                sys_saveUri(SYS_CRED_TC, uri);
                LOG(MOD_CUP|INFO, "[Segment] TC URI: %s", uri);
                uri[tcuri_len] = save;
                cups->uflags |= UPDATE_FLAG(TC_URI);
            }
            cups->cstate = cstate = CUPS_FEED_CUPS_CRED;
            cups->temp_n = 0;
        }
        assert(cstate > CUPS_HTTP_REQ_PEND || cstate < CUPS_DONE);
        // Rewind timeout every time we get some data
        rt_setTimer(&cups->timeout, rt_micros_ahead(CUPS_CONN_TIMEOUT));
        int segm_len = cups->segm_len;

        while( cups->temp_n < 4 ) {
            // Assemble length of a section in temp
            if( body.pos >= body.bufsize ) {
                // Not enough data in body buffer
              get_more:
                if( !http_getMore(&cups->hc) ) {
                    LOG(MOD_CUP|ERROR, "Unexpected end of data");
                  proto_err:
                    LOG(MOD_CUP|ERROR, "CUPS Protocol error. Closing connection.");
                    cups_done(cups, CUPS_ERR_FAILED);
                }
                return; // waiting for next chunk
            }
            cups->temp[cups->temp_n++] = body.buf[body.pos++];
            if( cups->temp_n == sizelen(cstate) ) {
                if( (segm_len = rt_rlsbf4(cups->temp)) == 0 ) {
                  next_cstate:
                    if( (cups->cstate = cstate += 1) == CUPS_DONE ) {
                        sys_commitConfigUpdate();
                        http_close(&cups->hc);
                        return;
                    }
                    cups->temp_n = 0;
                    continue;
                }
                if( segm_len < 0 ) {
                    LOG(MOD_CUP|ERROR, "Segment %d length not allowed (must be <2GB): 0x%08x bytes", cstate-CUPS_FEED_CUPS_URI, segm_len);
                    goto proto_err;
                }
                cups->segm_off = 0;
                cups->segm_len = segm_len;
                cups->temp_n = 4;
                memset(&cups->temp, 0, sizeof(cups->temp));
                if( cstate == CUPS_FEED_CUPS_CRED ) {
                    sys_credStart(SYS_CRED_CUPS, segm_len);
                    cups->uflags |= UPDATE_FLAG(CUPS_CRED);
                    LOG(MOD_CUP|INFO, "[Segment] CUPS Credentials (%d bytes)", segm_len);
                }
                else if( cstate == CUPS_FEED_TC_CRED ) {
                    sys_credStart(SYS_CRED_TC, segm_len);
                    cups->uflags |= UPDATE_FLAG(TC_CRED);
                    LOG(MOD_CUP|INFO, "[Segment] TC Credentials (%d bytes)", segm_len);
                } else if( cstate == CUPS_FEED_SIGNATURE ) {
                    LOG(MOD_CUP|INFO, "[Segment] FW Signature (%d bytes)", segm_len);
                    rt_free(cups->sig);
                    if( segm_len < 8 || segm_len > sizeof(cups->sig->signature) + SIGCRC_LEN ) {
                        LOG(MOD_CUP|ERROR, "Illegal signature segment length (must be 8-%d bytes): %d", sizeof(cups->sig->signature) + SIGCRC_LEN, segm_len);
                        goto proto_err;
                    }
                    cups->sig = rt_malloc(cups_sig_t);
                } else { // cstate == CUPS_FEED_UPDATE
                    assert(cstate == CUPS_FEED_UPDATE);
                    sys_commitConfigUpdate(); 
                    sys_updateStart(segm_len);
                    LOG(MOD_CUP|INFO, "[Segment] FW Update (%d bytes)", segm_len);
                }
            }
        }
      check_segm:
        if( cups->segm_off >= segm_len ) {
            // Segment finished
            if( cstate == CUPS_FEED_CUPS_CRED ) {
                sys_credComplete(SYS_CRED_CUPS, cups->segm_len);
                LOG(MOD_CUP|INFO, "[Segment] CUPS Credentials update completed (%d bytes)", cups->segm_len);
            }
            else if( cstate == CUPS_FEED_TC_CRED ) {
                sys_credComplete(SYS_CRED_TC, cups->segm_len);
                LOG(MOD_CUP|INFO, "[Segment] TC Credentials update completed (%d bytes)", cups->segm_len);
            } else if( cstate == CUPS_FEED_SIGNATURE ) {
                cups->uflags |= UPDATE_FLAG(SIGNATURE);
                cups->sig->len = cups->segm_len - SIGCRC_LEN;
                mbedtls_sha512_init(&cups->sig->sha);
                mbedtls_sha512_starts(&cups->sig->sha, 0);
            }
            else { // cstate == CUPS_FEED_UPDATE
                if( sys_updateCommit(cups->segm_len) ) {
                    cups->uflags |= UPDATE_FLAG(UPDATE);
                    LOG(MOD_CUP|INFO, "[Segment] Update committed (%d bytes)", cups->segm_len);
                } else {
                    LOG(MOD_CUP|ERROR, "[Segment] Update received (%d bytes) but failed to write (ignored)", cups->segm_len);
                }
            }
            goto next_cstate;
        }
        if( body.pos >= body.bufsize )
            goto get_more;

        int segm_off = cups->segm_off;
        int dlen = min(segm_len - segm_off, body.bufsize - body.pos);
        u1_t* data = (u1_t*)&body.buf[body.pos];
        if( cstate == CUPS_FEED_CUPS_CRED ) {
            sys_credWrite(SYS_CRED_CUPS, data, segm_off, dlen);
        }
        else if( cstate == CUPS_FEED_TC_CRED ) {
            sys_credWrite(SYS_CRED_TC, data, segm_off, dlen);
        }
        else if( cstate == CUPS_FEED_SIGNATURE ) {
            // CRC of signature comes just after length
            int siglen = dlen;
            if( segm_off < SIGCRC_LEN ) { // CRC before signature
                int d = min(SIGCRC_LEN-segm_off, siglen);
                memcpy(cups->sig->keycrcb + segm_off, data, d);
                segm_off += d;
                data     += d;
                siglen   -= d;
            }
            if (siglen > 0 && segm_off + siglen - SIGCRC_LEN <= sizeof(cups->sig->signature)) {
                memcpy(cups->sig->signature+segm_off-SIGCRC_LEN, data, siglen);
            }
        } else {
            assert(cstate == CUPS_FEED_UPDATE);
            if( cups->sig != NULL ) {
                mbedtls_sha512_update(&cups->sig->sha, data, dlen);
            }
            sys_updateWrite(data, segm_off, dlen);
        }
        body.pos += dlen;
        cups->segm_off += dlen;
        goto check_segm;
    }
    if( ev == HTTPEV_CLOSED ) {
        if( cups->cstate >= CUPS_INI && cups->cstate < CUPS_DONE )
            cups->cstate = CUPS_ERR_CLOSED;  // unexpected close
        cups_done(cups, cups->cstate);
        return;
    }
    LOG(MOD_CUP|INFO, "cups_update_info - Unknown event: %d", ev);
}


// === CUPS会话生命周期管理 ===

// 功能：初始化CUPS会话对象 - 创建新的CUPS通信会话
// 参数：无
// 返回值：已初始化的CUPS会话对象指针
// 调用时机：需要与CUPS服务器通信时创建新会话
cups_t* cups_ini () {
    assert(CUPS_BUFSZ > MAX_HOSTNAME_LEN + MAX_PORT_LEN + 2);  // 确保缓冲区足够大
    
    cups_t* cups = rt_malloc(cups_t);     // 分配CUPS会话对象内存
    http_ini(&cups->hc, CUPS_BUFSZ);      // 初始化HTTP连接（指定缓冲区大小）
    rt_iniTimer(&cups->timeout, cups_timeout);  // 初始化超时定时器
    cups->cstate = CUPS_INI;              // 设置初始状态
    return cups;                          // 返回初始化完成的会话对象
}

// 功能：释放CUPS会话对象 - 清理会话占用的所有资源
// 参数：cups - 要释放的CUPS会话对象
// 返回值：无
// 调用时机：CUPS会话结束后清理资源
void cups_free (cups_t* cups) {
    if( cups == NULL )                    // 空指针检查
        return;
    
    http_free(&cups->hc);                 // 释放HTTP连接资源
    rt_clrTimer(&cups->timeout);          // 清除超时定时器
    cstateLast = cups->cstate;            // 保存最后状态供查询使用
    cups->cstate = CUPS_ERR_DEAD;         // 标记为已释放状态
    
    // 释放数字签名相关资源
    if (cups->sig) {
        mbedtls_sha512_free(&cups->sig->sha);  // 释放SHA-512上下文
        rt_free(cups->sig);               // 释放签名结构体
    }
    
    rt_free(cups);                        // 释放会话对象本身
}


void cups_start (cups_t* cups) {
    assert(cups->cstate == CUPS_INI);

    str_t cupsuri = sys_uri(SYS_CRED_CUPS, cups_credset);
    if( cupsuri == NULL ) {
        LOG(MOD_CUP|ERROR, "No CUPS%s URI configured", sys_credset2str(cups_credset));
        cups_done(cups, CUPS_ERR_NOURI);
        return;
    }
    LOG(MOD_CUP|INFO, "Connecting to CUPS%s ... %s (try #%d)",
        sys_credset2str(cups_credset), cupsuri, cups_failCnt+1);
    log_flushIO();
    // Use HTTP buffer as temp place for host/port strings
    // Gets destroyed when reading response.
    char* hostname = (char*)cups->hc.c.rbuf;
    char* port     = (char*)cups->hc.c.rbuf + MAX_HOSTNAME_LEN + 1;
    int   ok;
    if( (ok = uri_checkHostPortUri(cupsuri, "http", hostname, MAX_HOSTNAME_LEN, port, MAX_PORT_LEN)) == URI_BAD ) {
        LOG(MOD_CUP|ERROR,"Bad CUPS URI: %s", cupsuri);
        goto errexit;
    }
    if( ok == URI_TLS && !conn_setup_tls(&cups->hc.c, SYS_CRED_CUPS, cups_credset, hostname) ) {
        goto errexit;
    }
    if( !http_connect(&cups->hc, hostname, port) ) {
        LOG(MOD_CUP|ERROR, "CUPS connect failed - URI: %s", cupsuri);
        goto errexit;
    }
    rt_setTimerCb(&cups->timeout, rt_micros_ahead(CUPS_CONN_TIMEOUT), cups_timeout);
    cups->hc.c.evcb = (evcb_t)cups_update_info;
    cups->cstate = CUPS_HTTP_REQ_PEND;
    return;

 errexit:
    cups_done(cups, CUPS_ERR_FAILED);
    return;
}

// === CUPS系统接口函数 ===

// 功能：延迟启动CUPS会话的回调函数 - 定时器到期后启动CUPS会话
// 参数：tmr - 定时器指针（未使用）
// 返回值：无
// 调用时机：延迟启动定时器到期时自动调用
static void delayedCUPSstart(tmr_t* tmr) {
    LOG(MOD_CUP|INFO, "Starting a CUPS session now.");
    cups_start(CUPS);                     // 启动CUPS会话
}

// 功能：触发CUPS会话 - 启动与CUPS服务器的通信会话
// 参数：delay - 延迟启动时间（秒），负数表示使用默认延迟
// 返回值：无
// 调用时机：需要与CUPS服务器同步时由系统或定时器调用
void sys_triggerCUPS (int delay) {
    if( CUPS != NULL || sys_noCUPS )      // 已有会话进行中或禁用CUPS
        return;                           // 直接返回，避免重复会话

#if defined(CFG_cups_exclusive)
    // CUPS独占模式：停止TC连接为CUPS让路
    if( !sys_noTC ) {
        LOG(MOD_CUP|INFO, "Stopping TC in favor of CUPS");
        sys_stopTC();                     // 停止TC连接
    }
#endif // defined(CFG_cups_exclusive)

    if( delay < 0 ) {                     // 使用默认延迟时间
        delay = CUPS_RESYNC_INTV/1000000; // 将微秒转换为秒
    }
    
    LOG(MOD_CUP|INFO, "Starting a CUPS session in %d seconds.", delay);
    sys_inState(SYSIS_CUPS_INTERACT);     // 通知系统进入CUPS交互状态
    CUPS = cups_ini();                    // 初始化新的CUPS会话
    rt_clrTimer(&cups_sync_tmr);          // 清除同步定时器
    rt_setTimerCb(&CUPS->timeout, rt_seconds_ahead(delay), delayedCUPSstart);  // 设置延迟启动定时器
}

// 功能：初始化CUPS子系统 - 设置CUPS同步定时器
// 参数：无
// 返回值：无
// 调用时机：系统启动时初始化CUPS子系统
void sys_iniCUPS () {
    rt_iniTimer(&cups_sync_tmr, cups_ondone);  // 初始化同步定时器，绑定完成处理函数
}

// 功能：清理CUPS子系统 - 停止所有CUPS定时器
// 参数：无
// 返回值：无
// 调用时机：系统关闭或重置CUPS时调用
void sys_clearCUPS () {
    rt_clrTimer(&cups_sync_tmr);          // 清除同步定时器
}

// 功能：延迟下次CUPS同步 - 在适当时机延迟CUPS同步操作
// 参数：无
// 返回值：无
// 调用时机：当前没有活动CUPS会话且状态异常时延迟同步
void sys_delayCUPS () {
    if( sys_statusCUPS() < 0 ) {          // 当前CUPS状态为错误状态
        LOG(MOD_CUP|INFO, "Next CUPS interaction delayed by %~T.", CUPS_OKSYNC_INTV);
        rt_setTimer(&cups_sync_tmr, rt_micros_ahead(CUPS_OKSYNC_INTV));  // 设置延迟同步定时器
    }
}

// 功能：获取CUPS当前状态 - 查询CUPS子系统的当前运行状态
// 参数：无
// 返回值：CUPS状态码（正数表示运行中，负数表示错误状态）
// 调用时机：需要检查CUPS状态时随时调用
s1_t sys_statusCUPS () {
    return CUPS ? CUPS->cstate : cstateLast;  // 返回当前状态或最后记录的状态
}
