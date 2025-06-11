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

//=============================================================================
// 文件功能：LoRaWAN物理层协议解析和信标生成
// 作用说明：本模块实现LoRaWAN协议栈的物理层功能，主要包括：
//          1. LoRaWAN帧格式解析：解析上行和下行数据帧，提取关键字段
//          2. 设备过滤机制：基于JoinEUI和NetID过滤不需要处理的设备
//          3. 信标帧生成：为Class B设备生成定时信标帧
//          4. 协议完整性校验：验证帧格式、长度、版本等合规性
//          5. JSON格式转换：将二进制帧数据转换为JSON格式供上层处理
// 技术特点：严格按照LoRaWAN 1.0.x规范实现，支持多种帧类型和过滤策略
// 应用场景：作为BasicStation网关的协议解析核心，处理所有LoRaWAN帧
//=============================================================================

#include "s2conf.h"       // BasicStation配置系统，提供全局配置参数和常量定义
#include "uj.h"           // 轻量级JSON编码器，用于将解析结果编码为JSON格式

//=============================================================================
// LoRaWAN MHDR (MAC Header) 字段位掩码定义
// 功能：定义MHDR字段中各个子字段的位模式，用于帧类型识别和协议解析
//=============================================================================
#define MHDR_FTYPE  0xE0  // 帧类型字段掩码：bits 7-5，标识消息类型(Join/Data/Proprietary)
#define MHDR_RFU    0x1C  // 预留字段掩码：bits 4-2，必须为0以保证向前兼容性
#define MHDR_MAJOR  0x03  // 主版本号掩码：bits 1-0，标识LoRaWAN协议主版本

#define MHDR_DNFLAG 0x20  // 下行标志位：bit 5，用于快速判断是否为下行帧
#define MAJOR_V1    0x00  // LoRaWAN v1.x 主版本号：当前支持的协议版本

//=============================================================================
// LoRaWAN 帧类型常量定义
// 功能：定义各种LoRaWAN消息类型的MHDR值，符合LoRaWAN协议规范
//=============================================================================
#define FRMTYPE_JREQ   0x00  // Join Request：入网请求，设备发起入网流程
#define FRMTYPE_JACC   0x20  // Join Accept：入网接受，服务器响应入网请求
#define FRMTYPE_DAUP   0x40  // Data (unconfirmed) up：上行数据，无需确认
#define FRMTYPE_DADN   0x60  // Data (unconfirmed) dn：下行数据，无需确认
#define FRMTYPE_DCUP   0x80  // Data confirmed up：上行数据，需要确认
#define FRMTYPE_DCDN   0xA0  // Data confirmed dn：下行数据，需要确认
#define FRMTYPE_REJOIN 0xC0  // Rejoin：重新入网，用于恢复会话密钥
#define FRMTYPE_PROP   0xE0  // Proprietary：厂商自定义消息类型

// 将帧类型转换为位掩码的宏，用于快速比较和分类
#define FTYPE_BIT(t) (1<<(((t) & MHDR_FTYPE)>>5))

// 下行帧类型集合：包含所有从服务器发送到设备的帧类型
#define DNFRAME_TYPE (FTYPE_BIT(FRMTYPE_JACC) | \
                      FTYPE_BIT(FRMTYPE_DADN) | \
                      FTYPE_BIT(FRMTYPE_DCDN) )

//=============================================================================
// LoRaWAN Join Request/Rejoin 帧结构定义
// 功能：定义入网请求帧的字段偏移量，用于解析和构造入网消息
// 帧格式：固定长度23字节，所有多字节字段采用小端序
//=============================================================================
// +-----------------------------------------+
// |                JOIN FRAME               |
// +-----+---------+--------+----------+-----+
// |  1  |     8   |    8   |    2     |  4  |  bytes - all fields little endian
// +=====+=========+========+==========+=====+
// | mhdr| joineui | deveui | devnonce | MIC |
// +-----+---------+--------+----------+-----+
#define OFF_mhdr      0   // MHDR字段偏移：MAC头部，包含帧类型和协议版本
#define OFF_joineui   1   // JoinEUI字段偏移：8字节，标识Join Server
#define OFF_deveui    9   // DevEUI字段偏移：8字节，设备全局唯一标识符
#define OFF_devnonce 17   // DevNonce字段偏移：2字节，设备生成的随机数
#define OFF_jreq_mic 19   // MIC字段偏移：4字节，消息完整性校验码
#define OFF_jreq_len 23   // Join帧总长度：固定23字节

//=============================================================================
// LoRaWAN Data Frame 帧结构定义
// 功能：定义数据帧的字段偏移量，支持可变长度的帧选项和载荷
// 帧格式：最小12字节，支持0-15字节FOpts和可变长度载荷
//=============================================================================
// +------------------------------------------------------------+
// |                           DATA FRAME                       |
// +-----+---------+-----+-------+-------+------+---------+-----+
// |  1  |    4    |  1  |   2   |  0/15 | 0/1  |   0-?   |  4  |   bytes - all fields little endian
// +=====+=========+=====+=======+=======+======+=========+=====+
// | mhdr| devaddr |fctrl|  fcnt | fopts | port | payload | MIC |
// +-----+---------+-----+-------+-------+------+---------+-----+
#define OFF_devaddr     1   // DevAddr字段偏移：4字节，设备网络地址
#define OFF_fctrl       5   // FCtrl字段偏移：1字节，帧控制，包含FOpts长度
#define OFF_fcnt        6   // FCnt字段偏移：2字节，帧计数器，防重放攻击
#define OFF_fopts       8   // FOpts字段偏移：0-15字节，MAC命令选项
#define OFF_df_minlen  12   // 数据帧最小长度：不包含FOpts、FPort和载荷的基本长度

//=============================================================================
// 全局过滤器变量定义
// 功能：配置设备过滤规则，减少不必要的协议处理和网络流量
//=============================================================================
uL_t* s2e_joineuiFilter;  // JoinEUI过滤器：指向范围数组，过滤不需要处理的入网请求
                          // 格式：[min1, max1, min2, max2, ..., 0] 范围对，0表示结束

// NetID过滤器：32个网络ID的位掩码数组，每位表示是否接受对应NetID的设备
// 数组索引：netid>>5 (除以32)，位索引：netid&0x1F (模32)
// 默认值：0xFFFFFFFF 表示接受所有NetID
u4_t  s2e_netidFilter[4] = { 0xffFFffFF, 0xffFFffFF, 0xffFFffFF, 0xffFFffFF };

//=============================================================================
// 函数功能：解析LoRaWAN帧并转换为JSON格式
// 参数说明：buf - JSON输出缓冲区，用于构建JSON对象
//          frame - 待解析的LoRaWAN帧数据
//          len - 帧数据长度(字节)
//          lbuf - 日志输出缓冲区，用于记录解析结果的可读格式
// 返回值：1表示成功解析，0表示帧格式错误或被过滤
// 调用时机：收到上行LoRaWAN帧时调用，进行协议解析和格式转换
// 执行逻辑：1.基本格式校验 2.帧类型识别 3.字段提取 4.过滤检查 5.JSON编码
//=============================================================================
int s2e_parse_lora_frame (ujbuf_t* buf, const u1_t* frame , int len, dbuf_t* lbuf) {
    if( len == 0 ) {                           // 检查帧长度，空帧直接拒绝
    badframe:                                  // 错误处理标签：统一处理格式错误
        LOG(MOD_S2E|DEBUG, "Not a LoRaWAN frame: %16.4H", len, frame);  // 记录非LoRaWAN帧
        return 0;                              // 返回解析失败
    }
    int ftype = frame[OFF_mhdr] & MHDR_FTYPE;  // 提取帧类型字段(bits 7-5)
    
    // 基本格式校验：检查长度、下行帧类型、协议版本
    if( (len < OFF_df_minlen && ftype != FRMTYPE_PROP) ||     // 数据帧长度不足(除非是自定义帧)
        // (FTYPE_BIT(ftype) & DNFRAME_TYPE) != 0 || --- 注释：因设备模式特性，解析所有帧
        (frame[OFF_mhdr] & (MHDR_RFU|MHDR_MAJOR)) != MAJOR_V1 ) {  // 检查预留位和协议版本
	goto badframe;                         // 格式错误，跳转到错误处理
    }
    
    // 处理自定义帧和Join Accept帧：直接编码为JSON，不做详细解析
    if( ftype == FRMTYPE_PROP || ftype == FRMTYPE_JACC ) {
        str_t msgtype = ftype == FRMTYPE_PROP ? "propdf" : "jacc";  // 确定消息类型字符串
        uj_encKVn(buf,                         // 编码为JSON键值对
                  "msgtype",   's', msgtype,   // 消息类型
                  "FRMPayload",'H', len, &frame[0],  // 原始载荷(十六进制)
                  NULL);                       // 结束标记
        xprintf(lbuf, "%s %16.16H", msgtype, len, &frame[0]);  // 生成日志输出
        return 1;                              // 返回解析成功
    }
    
    // 处理Join Request和Rejoin帧：包含设备标识和入网参数
    if( ftype == FRMTYPE_JREQ || ftype == FRMTYPE_REJOIN ) {
        if( len != OFF_jreq_len)               // 检查Join帧固定长度(23字节)
            goto badframe;                     // 长度错误，格式无效
        
        uL_t joineui = rt_rlsbf8(&frame[OFF_joineui]);  // 提取JoinEUI(小端序8字节)
        
        // JoinEUI过滤检查：如果配置了过滤器，检查是否在允许范围内
        if( s2e_joineuiFilter[0] != 0 ) {      // 检查过滤器是否启用(首元素非0)
            uL_t* f = s2e_joineuiFilter-2;     // 指向范围数组起始位置
            while( *(f += 2) ) {               // 遍历所有范围对，直到遇到0结束符
                if( joineui >= f[0] && joineui <= f[1] )  // 检查JoinEUI是否在当前范围内
                    goto out1;                 // 找到匹配范围，跳出循环
            }
            // 未找到匹配范围，记录过滤日志并返回
            xprintf(lbuf, "Join EUI %E filtered", joineui);
            return 0;                          // 返回过滤结果
          out1:;                               // 通过过滤检查的跳转标签
        }
        
        // 提取Join帧的所有字段
        str_t msgtype = (ftype == FRMTYPE_JREQ ? "jreq" : "rejoin");  // 确定消息类型
        u1_t  mhdr = frame[OFF_mhdr];          // MAC头部
        uL_t  deveui = rt_rlsbf8(&frame[OFF_deveui]);    // 设备EUI(小端序8字节)
        u2_t  devnonce = rt_rlsbf2(&frame[OFF_devnonce]); // 设备随机数(小端序2字节)
        s4_t  mic = (s4_t)rt_rlsbf4(&frame[len-4]);      // 消息完整性校验(小端序4字节)
        
        // 编码为JSON格式
        uj_encKVn(buf,
                  "msgtype", 's', msgtype,     // 消息类型字符串
                  "MHdr",    'i', mhdr,        // MAC头部(整数)
                  rt_joineui,'E', joineui,     // JoinEUI(EUI格式)
                  rt_deveui, 'E', deveui,      // DevEUI(EUI格式)
                  "DevNonce",'i', devnonce,    // 设备随机数
                  "MIC",     'i', mic,         // 消息完整性校验码
                  NULL);                       // 结束标记
        
        // 生成可读的日志输出
        xprintf(lbuf, "%s MHdr=%02X %s=%:E %s=%:E DevNonce=%d MIC=%d",
                msgtype, mhdr, rt_joineui, joineui, rt_deveui, deveui, devnonce, mic);
        return 1;                              // 返回解析成功
    }
    
    // 处理数据帧：包含设备地址、计数器、选项和载荷
    u1_t foptslen = frame[OFF_fctrl] & 0xF;    // 提取FOpts长度(FCtrl的低4位)
    u1_t portoff = foptslen + OFF_fopts;       // 计算FPort字段的偏移位置
    if( portoff > len-4  )                     // 检查帧长度是否足够容纳FOpts和MIC
        goto badframe;                         // 长度不足，格式错误
    
    u4_t devaddr = rt_rlsbf4(&frame[OFF_devaddr]);  // 提取设备网络地址(小端序4字节)
    u1_t netid = devaddr >> (32-7);            // 提取NetID(设备地址的高7位)
    
    // NetID过滤检查：验证设备是否属于允许的网络
    if( ((1 << (netid & 0x1F)) & s2e_netidFilter[netid>>5]) == 0 ) {
        // 未通过NetID过滤，记录过滤日志并返回
        xprintf(lbuf, "DevAddr=%X with NetID=%d filtered", devaddr, netid);
        return 0;                              // 返回过滤结果
    }
    
    // 提取数据帧的所有字段
    u1_t  mhdr  = frame[OFF_mhdr];             // MAC头部
    u1_t  fctrl = frame[OFF_fctrl];            // 帧控制字段
    u2_t  fcnt  = rt_rlsbf2(&frame[OFF_fcnt]); // 帧计数器(小端序2字节)
    s4_t  mic   = (s4_t)rt_rlsbf4(&frame[len-4]); // 消息完整性校验(小端序4字节)
    
    // 确定帧方向：根据帧类型判断上行还是下行
    str_t dir   = ftype==FRMTYPE_DAUP || ftype==FRMTYPE_DCUP ? "updf" : "dndf";
    
    // 编码为JSON格式
    uj_encKVn(buf,
              "msgtype",   's', dir,           // 消息类型(上行/下行数据帧)
              "MHdr",      'i', mhdr,          // MAC头部
              "DevAddr",   'i', (s4_t)devaddr, // 设备地址(有符号整数)
              "FCtrl",     'i', fctrl,         // 帧控制
              "FCnt",      'i', fcnt,          // 帧计数器
              "FOpts",     'H', foptslen, &frame[OFF_fopts],  // 帧选项(十六进制)
              "FPort",     'i', portoff == len-4 ? -1 : frame[portoff],  // 端口号(-1表示无载荷)
              "FRMPayload",'H', max(0, len-5-portoff), &frame[portoff+1], // 载荷数据
              "MIC",       'i', mic,           // 消息完整性校验码
              NULL);                           // 结束标记
    
    // 生成可读的日志输出，包含所有关键字段
    xprintf(lbuf, "%s mhdr=%02X DevAddr=%08X FCtrl=%02X FCnt=%d FOpts=[%H] %4.2H mic=%d (%d bytes)",
            dir, mhdr, devaddr, fctrl, fcnt,
            foptslen, &frame[OFF_fopts],       // FOpts内容
            max(0, len-4-portoff), &frame[portoff], mic, len);  // 载荷内容和总长度
    return 1;                                  // 返回解析成功
}

//=============================================================================
// 函数功能：计算16位CRC校验码(无查表法实现)
// 参数说明：pdu - 待校验的数据缓冲区
//          len - 数据长度(字节)
// 返回值：16位CRC校验码
// 调用时机：信标帧生成时，计算帧内容的完整性校验码
// 算法实现：基于CRC-16-CCITT多项式(0x1021)的位移实现，性能较查表法稍低但节省内存
//=============================================================================
static int crc16_no_table(uint8_t* pdu, int len) {
    uint32_t remainder = 0;                    // CRC余数寄存器，初始化为0
    uint32_t polynomial = 0x1021;             // CRC-16-CCITT标准多项式
    
    for( int i=0; i<len; i++ ) {               // 逐字节处理输入数据
        remainder ^= pdu[i] << 8;              // 将当前字节载入余数寄存器高8位
        for( int bit=0; bit < 8; bit++ ) {     // 逐位处理当前字节
            if( remainder & 0x8000 ) {         // 检查最高位是否为1
                remainder = (remainder << 1) ^ polynomial;  // 左移并异或多项式
            } else {
                remainder <<= 1;               // 仅左移1位
            }
        }
    }
    return remainder & 0xFFFF;                 // 返回16位CRC结果
}

//=============================================================================
// 函数功能：构造LoRaWAN Class B信标帧
// 参数说明：layout - 信标帧布局配置数组：[时间偏移, 信息描述偏移, 总长度]
//          epoch_secs - GPS纪元时间(秒)，用于设备时间同步
//          infodesc - 信息描述字节，标识信标内容类型和格式
//          lat - 纬度坐标(度)，-90.0到+90.0
//          lon - 经度坐标(度)，-180.0到+180.0
//          pdu - 输出缓冲区，存放构造的信标帧
// 返回值：无
// 调用时机：需要发送Class B信标时调用，为Class B设备提供时间和位置信息
// 帧格式：| RFU | epoch_secs | CRC | infoDesc | lat | lon | RFU | CRC |
//=============================================================================
void s2e_make_beacon (uint8_t* layout, sL_t epoch_secs, int infodesc, double lat, double lon, uint8_t* pdu) {
    int time_off     = layout[0];              // 时间字段在帧中的偏移位置
    int infodesc_off = layout[1];              // 信息描述字段的偏移位置  
    int bcn_len      = layout[2];              // 信标帧的总长度
    
    memset(pdu, 0, bcn_len);                   // 清零整个信标帧缓冲区
    
    // 填充GPS纪元时间：4字节小端序格式
    for( int i=0; i<4; i++ ) 
        pdu[time_off+i] = epoch_secs>>(8*i);   // 按字节分解时间戳并存储
    
    // 转换地理坐标为定点数格式：经纬度映射到32位整数范围
    uint32_t ulon = (uint32_t)(lon / 180 * (1U<<31));  // 经度：-180°~+180° 映射到 31位有符号整数
    uint32_t ulat = (uint32_t)(lat /  90 * (1U<<31));  // 纬度：-90°~+90° 映射到 31位有符号整数
    
    // 填充地理坐标：各3字节小端序格式(舍弃最高字节以节省空间)
    for( int i=0; i<3; i++ ) {
        pdu[infodesc_off+1+i] = ulon>>(8*i);   // 经度的低3字节
        pdu[infodesc_off+4+i] = ulat>>(8*i);   // 纬度的低3字节
    } 
    pdu[infodesc_off] = infodesc;              // 填充信息描述字节
    
    // 计算并填充两个CRC校验码：分别校验时间部分和信息部分
    int crc1 = crc16_no_table(&pdu[0],infodesc_off-2);           // 时间部分的CRC
    int crc2 = crc16_no_table(&pdu[infodesc_off], bcn_len-2-infodesc_off);  // 信息部分的CRC
    
    // 填充CRC校验码：各2字节小端序格式
    for( int i=0; i<2; i++ ) {
        pdu[infodesc_off-2+i] = crc1>>(8*i);   // 时间部分CRC
        pdu[bcn_len-2+i]      = crc2>>(8*i);   // 信息部分CRC
    }
}
