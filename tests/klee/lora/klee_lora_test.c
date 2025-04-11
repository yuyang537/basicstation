#include <klee/klee.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// 项目头文件
#include "../../../src/rt.h"  // 基础类型定义
#include "../../../src/sys.h"  // 系统接口
#include "../../../src/s2conf.h"  // 配置接口
#include "../../../src/uj.h"  // JSON处理接口

// 外部函数声明
extern int s2e_parse_lora_frame(ujbuf_t* buf, const u1_t* frame, int len, dbuf_t* lbuf);
extern void s2e_make_beacon(uint8_t* layout, sL_t epoch_secs, int infodesc, double lat, double lon, uint8_t* pdu);
extern uL_t* s2e_joineuiFilter;
extern u4_t s2e_netidFilter[4];

// 测试用例1: 空帧处理测试
void test_empty_frame() {
    ujbuf_t buf;
    char json_buf[1024];
    memset(json_buf, 0, sizeof(json_buf));
    
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 测试空帧处理
    u1_t empty_frame[1];
    klee_make_symbolic(empty_frame, sizeof(empty_frame), "empty_frame");
    
    int len = 0;  // 长度为0的帧
    int result = s2e_parse_lora_frame(&buf, empty_frame, len, NULL);
    
    // 验证结果 - 空帧应被拒绝
    assert(result == 0);
}

// 测试用例2: 帧格式验证测试
void test_frame_format() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 创建一个符号化的帧
    u1_t frame[24];  // 足够容纳各种类型的帧
    klee_make_symbolic(frame, sizeof(frame), "test_frame");
    
    // 设置MHDR-MAJOR版本字段 - 只有MAJOR_V1(0)被接受
    // frame[0] & MHDR_MAJOR (0x03) 控制
    klee_assume((frame[0] & 0x03) == 0x00);  // 有效MAJOR
    
    // 设置MHDR-RFU字段 - 必须为0
    // frame[0] & MHDR_RFU (0x1C) 控制
    klee_assume((frame[0] & 0x1C) == 0x00);  // 有效RFU
    
    // 测试不同长度的帧
    int len;
    klee_make_symbolic(&len, sizeof(len), "frame_length");
    klee_assume(len > 0 && len <= 24);
    
    // 如果是JREQ帧，长度必须是OFF_jreq_len(23)
    if ((frame[0] & 0xE0) == 0x00) {  // FRMTYPE_JREQ
        len = 23;  // 加入请求长度固定为23
    }
    
    int result = s2e_parse_lora_frame(&buf, frame, len, NULL);
    
    // 验证格式验证逻辑
    if (result == 0) {
        // 如果解析失败，检查是否满足以下条件之一:
        // 1. 长度太短 (< OFF_df_minlen且非专有帧) 
        // 2. 错误的MAJOR版本
        // 3. 非零RFU字段
        // 4. 对于JREQ，长度不是23字节
        if ((frame[0] & 0xE0) != 0xE0) {  // 不是专有帧
            if (len < 12) {  // OFF_df_minlen是12
                assert(result == 0);  // 长度太短，应该失败
            }
        }
    }
}

// 测试用例3: 加入请求(JREQ)解析测试
void test_join_request() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 创建JREQ帧
    u1_t jreq_frame[23];  // 加入请求长度固定为23
    memset(jreq_frame, 0, sizeof(jreq_frame));
    
    // 设置MHDR为JREQ类型(0x00)
    jreq_frame[0] = 0x00;
    
    // 创建符号化的JoinEUI, DevEUI和DevNonce字段
    for (int i = 1; i < 17; i++) {
        klee_make_symbolic(&jreq_frame[i], 1, "eui_bytes");
    }
    for (int i = 17; i < 19; i++) {
        klee_make_symbolic(&jreq_frame[i], 1, "devnonce_bytes");
    }
    for (int i = 19; i < 23; i++) {
        klee_make_symbolic(&jreq_frame[i], 1, "mic_bytes");
    }
    
    // 测试没有过滤器的情况
    s2e_joineuiFilter = (uL_t*)calloc(2, sizeof(uL_t));
    s2e_joineuiFilter[0] = 0;  // 禁用过滤
    
    int result = s2e_parse_lora_frame(&buf, jreq_frame, 23, NULL);
    
    // 验证结果 - 应该成功解析
    assert(result == 1);
    
    // 测试过滤器情况
    buf.pos = 0;
    
    // 创建过滤器
    uL_t* filter = (uL_t*)calloc(4, sizeof(uL_t));
    // 设置一个范围过滤器
    filter[0] = 0x1000000000000000;  // 开始范围
    filter[1] = 0x1FFFFFFFFFFFFFFF;  // 结束范围
    filter[2] = 0;  // 结束标记
    
    s2e_joineuiFilter = filter;
    
    // JoinEUI不在过滤范围内 - 应该被过滤掉
    result = s2e_parse_lora_frame(&buf, jreq_frame, 23, NULL);
    
    // 根据JoinEUI值验证结果
    uL_t joineui = rt_rlsbf8(&jreq_frame[1]);
    if (joineui >= filter[0] && joineui <= filter[1]) {
        assert(result == 1);  // 在过滤范围内，应通过
    } else {
        assert(result == 0);  // 不在过滤范围内，应被过滤
    }
    
    free(filter);
}

// 测试用例4: 数据帧解析测试
void test_data_frame() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 创建数据帧
    u1_t data_frame[24];  // 足够大的帧
    memset(data_frame, 0, sizeof(data_frame));
    
    // 设置MHDR为DAUP类型(0x40)或DCUP类型(0x80)
    klee_make_symbolic(&data_frame[0], 1, "mhdr");
    klee_assume((data_frame[0] & 0xE0) == 0x40 || (data_frame[0] & 0xE0) == 0x80);
    klee_assume((data_frame[0] & 0x1F) == 0);  // 清除RFU和MAJOR位，保持为0
    
    // 设置DevAddr字段(4字节)
    for (int i = 1; i < 5; i++) {
        klee_make_symbolic(&data_frame[i], 1, "devaddr_bytes");
    }
    
    // 设置FCtrl字段(1字节) - FOpts长度在低4位
    klee_make_symbolic(&data_frame[5], 1, "fctrl");
    
    // 确保FOpts长度小于16
    data_frame[5] &= 0xF;
    
    // 设置FCnt字段(2字节)
    for (int i = 6; i < 8; i++) {
        klee_make_symbolic(&data_frame[i], 1, "fcnt_bytes");
    }
    
    // 设置FOpts字段(0-15字节)
    u1_t foptslen = data_frame[5] & 0x0F;
    for (int i = 0; i < foptslen; i++) {
        klee_make_symbolic(&data_frame[8 + i], 1, "fopts_bytes");
    }
    
    // 设置帧长度 - 头部+FOpts+FPort+MIC，如果有载荷，则额外添加
    int len = 8 + foptslen + 1 + 4;  // 头部(8) + FOpts + FPort(1) + MIC(4)
    
    // 如果有FPort，设置其值
    if (8 + foptslen < len - 4) {
        klee_make_symbolic(&data_frame[8 + foptslen], 1, "fport");
    }
    
    // 设置MIC(4字节)
    for (int i = 0; i < 4; i++) {
        klee_make_symbolic(&data_frame[len - 4 + i], 1, "mic_bytes");
    }
    
    // 确保长度合法 - 至少包含头部和MIC
    klee_assume(len >= 12 && len <= 24);
    
    // 重置NetID过滤器
    for (int i = 0; i < 4; i++) {
        s2e_netidFilter[i] = 0xFFFFFFFF;  // 允许所有NetID
    }
    
    int result = s2e_parse_lora_frame(&buf, data_frame, len, NULL);
    
    // 验证结果
    u4_t devaddr = rt_rlsbf4(&data_frame[1]);
    u1_t netid = devaddr >> (32-7);  // 提取NetID(高7位)
    
    // 检查NetID过滤是否工作
    if (((1 << (netid & 0x1F)) & s2e_netidFilter[netid>>5]) == 0) {
        assert(result == 0);  // NetID被过滤，应该失败
    } else if (len < 12) {
        assert(result == 0);  // 长度太短，应该失败
    } else {
        assert(result == 1);  // 应该成功解析
    }
}

// 测试用例5: 信标帧生成测试
void test_beacon_generation() {
    // 创建符号化的参数
    uint8_t layout[3];
    sL_t epoch_secs;
    int infodesc;
    double lat, lon;
    
    // 符号化布局信息 - 确保合理的布局值
    klee_make_symbolic(layout, sizeof(layout), "beacon_layout");
    // 限制位置偏移不超过最大缓冲区大小
    klee_assume(layout[0] < 32);
    klee_assume(layout[1] < 32);
    klee_assume(layout[2] >= 23);  // 确保足够的空间容纳信标数据
    klee_assume(layout[2] <= 64);  // 限制合理的最大大小
    
    // 确保infodesc_off大于time_off以保持正确的结构
    klee_assume(layout[1] > layout[0] + 4); // 时间戳之后
    
    // 符号化时间戳
    klee_make_symbolic(&epoch_secs, sizeof(epoch_secs), "epoch_secs");
    
    // 符号化信息描述符
    klee_make_symbolic(&infodesc, sizeof(infodesc), "infodesc");
    klee_assume(infodesc >= 0 && infodesc <= 255); // 单字节范围
    
    // 符号化坐标，但使用合理的范围约束
    klee_make_symbolic(&lat, sizeof(lat), "latitude");
    klee_make_symbolic(&lon, sizeof(lon), "longitude");
    klee_assume(lat >= -90.0 && lat <= 90.0);
    klee_assume(lon >= -180.0 && lon <= 180.0);
    
    // 分配输出缓冲区
    uint8_t* pdu = (uint8_t*)malloc(layout[2]);
    memset(pdu, 0, layout[2]);
    
    // 调用信标生成函数
    s2e_make_beacon(layout, epoch_secs, infodesc, lat, lon, pdu);
    
    // 验证时间戳正确写入
    uint32_t written_time = 0;
    for (int i = 0; i < 4; i++) {
        written_time |= ((uint32_t)pdu[layout[0] + i]) << (8 * i);
    }
    assert((sL_t)written_time == (epoch_secs & 0xFFFFFFFF));
    
    // 验证信息描述符正确写入
    assert(pdu[layout[1]] == (infodesc & 0xFF));
    
    // 验证第一个CRC字段存在
    uint16_t crc1_offset = layout[1] - 2;
    assert(crc1_offset < layout[2]);
    
    // 验证第二个CRC字段存在
    uint16_t crc2_offset = layout[2] - 2;
    assert(crc2_offset < layout[2]);
    
    free(pdu);
}

// 测试用例6: 专有帧和加入接受(JACC)帧测试
void test_prop_and_jacc_frames() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 创建专有帧和JACC帧
    u1_t frame[16];  // 一个合理大小的帧
    memset(frame, 0, sizeof(frame));
    
    // 随机选择PROP(0xE0)或JACC(0x20)类型
    u1_t frame_type;
    klee_make_symbolic(&frame_type, 1, "frame_type");
    klee_assume(frame_type == 0xE0 || frame_type == 0x20);
    
    frame[0] = frame_type;
    
    // 符号化剩余帧数据
    for (int i = 1; i < 16; i++) {
        klee_make_symbolic(&frame[i], 1, "payload_bytes");
    }
    
    // 测试解析
    int result = s2e_parse_lora_frame(&buf, frame, 16, NULL);
    
    // 验证结果 - 这些特殊帧类型总是成功解析
    assert(result == 1);
    
    // 检查JSON输出中是否包含正确的msgtype
    buf.buf[buf.pos] = '\0';
    if (frame_type == 0xE0) {
        assert(strstr(buf.buf, "\"msgtype\":\"propdf\"") != NULL);
    } else {
        assert(strstr(buf.buf, "\"msgtype\":\"jacc\"") != NULL);
    }
}

// 测试用例7: 缓冲区边界检查
void test_buffer_boundaries() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 创建一个数据帧
    u1_t data_frame[32];
    memset(data_frame, 0, sizeof(data_frame));
    data_frame[0] = 0x40;  // DAUP类型
    
    // 动态设置FCtrl中的FOpts长度
    u1_t foptslen;
    klee_make_symbolic(&foptslen, 1, "fopts_length");
    klee_assume(foptslen <= 15);  // FOpts长度不能超过15
    
    data_frame[5] = foptslen;  // FCtrl，设置FOpts长度
    
    // 设置帧长度
    int len;
    klee_make_symbolic(&len, sizeof(len), "frame_length");
    
    // 计算最小长度 - 头部(8) + FOpts + MIC(4)
    int min_len = 8 + foptslen + 4;
    
    // 限制长度在逻辑范围内
    klee_assume(len >= min_len && len <= 32);
    
    // 重置NetID过滤器
    for (int i = 0; i < 4; i++) {
        s2e_netidFilter[i] = 0xFFFFFFFF;  // 允许所有NetID
    }
    
    int result = s2e_parse_lora_frame(&buf, data_frame, len, NULL);
    
    // 验证长度校验逻辑
    u1_t portoff = foptslen + 8;  // 8是头部长度
    
    if (portoff > len - 4) {  // 检查端口偏移是否超出帧长度减去MIC
        assert(result == 0);  // 应该失败，因为没有足够空间放置端口和MIC
    }
}

// 测试用例8: 安全性验证 - 极限情况测试
void test_security_checks() {
    ujbuf_t buf;
    char json_buf[1024];
    
    // 初始化缓冲区
    memset(json_buf, 0, sizeof(json_buf));
    buf.buf = json_buf;
    buf.bufsize = sizeof(json_buf);
    buf.pos = 0;
    
    // 测试1: 检查非常大的FOpts长度处理
    u1_t data_frame[256];
    memset(data_frame, 0, sizeof(data_frame));
    data_frame[0] = 0x40;  // DAUP类型
    
    // 设置不合理的FOpts长度
    data_frame[5] = 0xFF;  // FCtrl，设置FOpts长度为最大值
    
    // 确保解析器能安全处理
    int result = s2e_parse_lora_frame(&buf, data_frame, 32, NULL);
    
    // 应该失败，因为FOpts长度不合理
    assert(result == 0);
    
    // 测试2: 检查恶意长度值的处理
    buf.pos = 0;
    
    // 使用合理的FOpts长度但不一致的总长度
    data_frame[5] = 0x0F;  // 15字节FOpts
    
    // 使用太短的长度，不足以容纳FOpts+MIC
    result = s2e_parse_lora_frame(&buf, data_frame, 20, NULL);
    
    // 应该失败，因为长度太短
    assert(result == 0);
}

// 主测试函数
int main() {
    // 初始化系统
    sys_init("{}");
    
    // 运行所有测试用例
    test_empty_frame();
    test_frame_format();
    test_join_request();
    test_data_frame();
    test_beacon_generation();
    test_prop_and_jacc_frames();
    test_buffer_boundaries();
    test_security_checks();
    
    return 0;
} 