/*
 * 文件概述
 * ========
 * 本文件实现了LoRaWAN基站的LoRa帧处理系统，负责解析和构建LoRaWAN协议的数据帧。
 * 它实现了对各种LoRaWAN消息类型的识别和处理，包括加入请求、数据上行和下行帧。
 * 该模块是基站处理LoRaWAN通信的核心组件，确保正确解析和处理终端设备发送的消息。
 * 
 * 文件功能
 * ========
 * 本源文件实现了LoRaWAN帧的基础解析和处理功能：
 * - LoRaWAN消息类型识别
 * - 帧格式解析和验证
 * - 消息内容提取
 * - Join EUI和NetID过滤
 * - 信标(Beacon)帧构建
 * 
 * 主要组件
 * ========
 * 1. 帧解析系统
 *    - LoRaWAN帧格式验证
 *    - 消息类型识别(Join请求、数据帧等)
 *    - 帧头和载荷分离
 *    - 消息内容提取
 * 
 * 2. 过滤机制
 *    - Join EUI过滤表
 *    - NetID过滤规则
 *    - 设备地址验证
 *    - 帧类型筛选
 * 
 * 3. 信标系统
 *    - 信标帧构建
 *    - 时间同步数据生成
 *    - 地理位置信息编码
 *    - CRC校验
 * 
 * 4. 辅助功能
 *    - CRC16计算
 *    - 日志记录
 *    - JSON编码
 * 
 * 关键流程
 * ========
 * 1. 帧解析流程
 *    - 帧格式验证
 *    - 消息类型识别
 *    - 字段提取和解码
 *    - 过滤规则应用
 *    - 结果格式化
 * 
 * 2. 消息处理流程
 *    - Join请求处理
 *    - 数据上行消息处理
 *    - 专有帧处理
 *    - 消息内容提取
 * 
 * 3. 信标生成流程
 *    - 时间戳编码
 *    - 位置信息编码
 *    - 信息描述符设置
 *    - CRC计算和添加
 * 
 * 注意事项
 * ========
 * 1. 协议兼容性
 *    - 符合LoRaWAN 1.0规范
 *    - 支持的帧类型限制
 *    - 消息格式验证严格性
 * 
 * 2. 性能考虑
 *    - 帧解析效率
 *    - 内存使用优化
 *    - 过滤机制效率
 * 
 * 3. 安全性考虑
 *    - 帧格式验证
 *    - MIC完整性检查
 *    - 过滤机制安全性
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

#include "s2conf.h"
#include "uj.h"


#define MHDR_FTYPE  0xE0
#define MHDR_RFU    0x1C
#define MHDR_MAJOR  0x03
#define MHDR_DNFLAG 0x20

#define MAJOR_V1    0x00

#define FRMTYPE_JREQ   0x00
#define FRMTYPE_JACC   0x20
#define FRMTYPE_DAUP   0x40  // data (unconfirmed) up
#define FRMTYPE_DADN   0x60  // data (unconfirmed) dn
#define FRMTYPE_DCUP   0x80  // data confirmed up
#define FRMTYPE_DCDN   0xA0  // data confirmed dn
#define FRMTYPE_REJOIN 0xC0  // rejoin
#define FRMTYPE_PROP   0xE0  // propriatary
#define FTYPE_BIT(t) (1<<(((t) & MHDR_FTYPE)>>5))
#define DNFRAME_TYPE (FTYPE_BIT(FRMTYPE_JACC) | \
                      FTYPE_BIT(FRMTYPE_DADN) | \
                      FTYPE_BIT(FRMTYPE_DCDN) )


// +-----------------------------------------+
// |                JOIN FRAME               |
// +-----+---------+--------+----------+-----+
// |  1  |     8   |    8   |    2     |  4  |  bytes - all fields little endian
// +=====+=========+========+==========+=====+
// | mhdr| joineui | deveui | devnonce | MIC |
// +-----+---------+--------+----------+-----+
#define OFF_mhdr      0
#define OFF_joineui   1
#define OFF_deveui    9
#define OFF_devnonce 17
#define OFF_jreq_mic 19
#define OFF_jreq_len 23

// +------------------------------------------------------------+
// |                           DATA FRAME                       |
// +-----+---------+-----+-------+-------+------+---------+-----+
// |  1  |    4    |  1  |   2   |  0/15 | 0/1  |   0-?   |  4  |   bytes - all fields little endian
// +=====+=========+=====+=======+=======+======+=========+=====+
// | mhdr| devaddr |fctrl|  fcnt | fopts | port | payload | MIC |
// +-----+---------+-----+-------+-------+------+---------+-----+
#define OFF_devaddr     1
#define OFF_fctrl       5
#define OFF_fcnt        6
#define OFF_fopts       8
#define OFF_df_minlen  12


uL_t* s2e_joineuiFilter;
u4_t  s2e_netidFilter[4] = { 0xffFFffFF, 0xffFFffFF, 0xffFFffFF, 0xffFFffFF };


/*
 * 函数：s2e_parse_lora_frame
 * 功能：解析LoRaWAN帧
 * 参数：
 *   - buf: JSON输出缓冲区
 *   - frame: 帧数据
 *   - len: 帧长度
 *   - lbuf: 日志缓冲区
 * 返回值：
 *   成功返回1，失败返回0
 * 说明：
 *   该函数实现了LoRaWAN帧的解析，包括：
 *   1. 帧格式验证
 *      - 检查帧长度
 *      - 验证协议版本
 *      - 确认消息类型
 *   
 *   2. 消息类型处理
 *      - 处理Join请求
 *      - 处理数据帧
 *      - 处理专有帧
 *   
 *   3. 过滤机制
 *      - 应用Join EUI过滤
 *      - 应用NetID过滤
 *      - 验证设备地址
 *   
 *   4. 结果输出
 *      - 生成JSON格式输出
 *      - 记录日志信息
 *      - 返回处理状态
 */
int s2e_parse_lora_frame(ujbuf_t* buf, const u1_t* frame, int len, dbuf_t* lbuf) {
    if( len == 0 ) {
    badframe:
        LOG(MOD_S2E|DEBUG, "Not a LoRaWAN frame: %16.4H", len, frame);
        return 0;
    }
    int ftype = frame[OFF_mhdr] & MHDR_FTYPE;
    if( (len < OFF_df_minlen && ftype != FRMTYPE_PROP) ||
        // (FTYPE_BIT(ftype) & DNFRAME_TYPE) != 0 || --- because of device_mode feature we parse everything
        (frame[OFF_mhdr] & (MHDR_RFU|MHDR_MAJOR)) != MAJOR_V1 ) {
	goto badframe;
    }
    if( ftype == FRMTYPE_PROP || ftype == FRMTYPE_JACC ) {
        str_t msgtype = ftype == FRMTYPE_PROP ? "propdf" : "jacc";
        uj_encKVn(buf,
                  "msgtype",   's', msgtype,
                  "FRMPayload",'H', len, &frame[0],
                  NULL);
        xprintf(lbuf, "%s %16.16H", msgtype, len, &frame[0]);
        return 1;
    }
    if( ftype == FRMTYPE_JREQ || ftype == FRMTYPE_REJOIN ) {
        if( len != OFF_jreq_len)
            goto badframe;
        uL_t joineui = rt_rlsbf8(&frame[OFF_joineui]);
        
        if( s2e_joineuiFilter[0] != 0 ) {
            uL_t* f = s2e_joineuiFilter-2;
            while( *(f += 2) ) {
                if( joineui >= f[0] && joineui <= f[1] )
                    goto out1;
            }
            
            xprintf(lbuf, "Join EUI %E filtered", joineui);
            return 0;
          out1:;
        }
        str_t msgtype = (ftype == FRMTYPE_JREQ ? "jreq" : "rejoin");
        u1_t  mhdr = frame[OFF_mhdr];
        uL_t  deveui = rt_rlsbf8(&frame[OFF_deveui]);
        u2_t  devnonce = rt_rlsbf2(&frame[OFF_devnonce]);
        s4_t  mic = (s4_t)rt_rlsbf4(&frame[len-4]);
        uj_encKVn(buf,
                  "msgtype", 's', msgtype,
                  "MHdr",    'i', mhdr,
                  rt_joineui,'E', joineui,
                  rt_deveui, 'E', deveui,
                  "DevNonce",'i', devnonce,
                  "MIC",     'i', mic,
                  NULL);
        xprintf(lbuf, "%s MHdr=%02X %s=%:E %s=%:E DevNonce=%d MIC=%d",
                msgtype, mhdr, rt_joineui, joineui, rt_deveui, deveui, devnonce, mic);
        return 1;
    }
    u1_t foptslen = frame[OFF_fctrl] & 0xF;
    u1_t portoff = foptslen + OFF_fopts;
    if( portoff > len-4  )
        goto badframe;
    u4_t devaddr = rt_rlsbf4(&frame[OFF_devaddr]);
    u1_t netid = devaddr >> (32-7);
    if( ((1 << (netid & 0x1F)) & s2e_netidFilter[netid>>5]) == 0 ) {
        
        xprintf(lbuf, "DevAddr=%X with NetID=%d filtered", devaddr, netid);
        return 0;
    }
    u1_t  mhdr  = frame[OFF_mhdr];
    u1_t  fctrl = frame[OFF_fctrl];
    u2_t  fcnt  = rt_rlsbf2(&frame[OFF_fcnt]);
    s4_t  mic   = (s4_t)rt_rlsbf4(&frame[len-4]);
    str_t dir   = ftype==FRMTYPE_DAUP || ftype==FRMTYPE_DCUP ? "updf" : "dndf";
    uj_encKVn(buf,
              "msgtype",   's', dir,
              "MHdr",      'i', mhdr,
              "DevAddr",   'i', (s4_t)devaddr,
              "FCtrl",     'i', fctrl,
              "FCnt",      'i', fcnt,
              "FOpts",     'H', foptslen, &frame[OFF_fopts],
              "FPort",     'i', portoff == len-4 ? -1 : frame[portoff],
              "FRMPayload",'H', max(0, len-5-portoff), &frame[portoff+1],
              "MIC",       'i', mic,
              NULL);
    xprintf(lbuf, "%s mhdr=%02X DevAddr=%08X FCtrl=%02X FCnt=%d FOpts=[%H] %4.2H mic=%d (%d bytes)",
            dir, mhdr, devaddr, fctrl, fcnt,
            foptslen, &frame[OFF_fopts],
            max(0, len-4-portoff), &frame[portoff], mic, len);
    return 1;
}


/*
 * 函数：crc16_no_table
 * 功能：计算CRC16校验值
 * 参数：
 *   - pdu: 数据缓冲区
 *   - len: 数据长度
 * 返回值：
 *   返回16位CRC校验值
 * 说明：
 *   该函数实现了CRC16校验计算，使用多项式0x1021。
 *   采用无查表法实现，适用于资源受限环境。
 *   主要用于验证信标帧的完整性。
 */
static int crc16_no_table(uint8_t* pdu, int len) {
    uint32_t remainder = 0;
    uint32_t polynomial = 0x1021;
    for( int i=0; i<len; i++ ) {
        remainder ^= pdu[i] << 8;
        for( int bit=0; bit < 8; bit++ ) {
            if( remainder & 0x8000 ) {
                remainder = (remainder << 1) ^ polynomial;
            } else {
                remainder <<= 1;
            }
        }
    }
    return remainder & 0xFFFF;
}



// Pack parameters into a BEACON pdu with the following layout:
//    | 0-n |       4    |  2  |     1    |  3  |  3  | 0-n |  2  |   bytes - all fields little endian
//    | RFU | epoch_secs | CRC | infoDesc | lat | lon | RFU | CRC |
//
/*
 * 函数：s2e_make_beacon
 * 功能：构建信标帧
 * 参数：
 *   - layout: 帧布局信息
 *   - epoch_secs: 时间戳
 *   - infodesc: 信息描述符
 *   - lat: 纬度
 *   - lon: 经度
 *   - pdu: 输出缓冲区
 * 说明：
 *   该函数实现了LoRaWAN信标帧的构建，包括：
 *   1. 帧结构
 *      - 预留字段(RFU)
 *      - 时间戳
 *      - CRC校验
 *      - 信息描述符
 *      - 位置信息
 *   
 *   2. 数据处理
 *      - 时间戳编码
 *      - 位置信息编码
 *      - CRC计算
 *   
 *   3. 帧组装
 *      - 字段填充
 *      - 校验和计算
 *      - 结果输出
 */
void s2e_make_beacon(uint8_t* layout, sL_t epoch_secs, int infodesc, double lat, double lon, uint8_t* pdu) {
    int time_off     = layout[0];
    int infodesc_off = layout[1];
    int bcn_len      = layout[2];
    memset(pdu, 0, bcn_len);
    for( int i=0; i<4; i++ ) 
        pdu[time_off+i] = epoch_secs>>(8*i);
    uint32_t ulon = (uint32_t)(lon / 180 * (1U<<31));
    uint32_t ulat = (uint32_t)(lat /  90 * (1U<<31));
    for( int i=0; i<3; i++ ) {
        pdu[infodesc_off+1+i] = ulon>>(8*i);
        pdu[infodesc_off+4+i] = ulat>>(8*i);
    } 
    pdu[infodesc_off] = infodesc;
    int crc1 = crc16_no_table(&pdu[0],infodesc_off-2);
    int crc2 = crc16_no_table(&pdu[infodesc_off], bcn_len-2-infodesc_off);
    for( int i=0; i<2; i++ ) {
        pdu[infodesc_off-2+i] = crc1>>(8*i);
        pdu[bcn_len-2+i]      = crc2>>(8*i);
    }
}
