# BasicStation 系统核心模块 (sys.c) 技术总结

## 模块概述

sys.c 是 BasicStation 的系统核心模块，提供了关键的系统级功能和配置管理服务。该模块承担着整个系统的基础设施角色，确保配置更新的原子性、文件系统操作的安全性以及多进程环境下的路径管理。

### 核心设计原则

1. **原子性保障** - 所有配置更新操作都通过事务机制确保原子性
2. **故障恢复** - 系统重启后能自动检测并完成未完成的操作
3. **多进程支持** - 文件路径系统支持主从进程架构
4. **安全第一** - 凭证管理采用标准ASN.1格式和严格的验证机制

## 主要功能模块

### 1. 文件路径管理系统

#### 特殊路径前缀支持
- `~/` - 解析为主配置目录 (homeDir)
- `~temp/` - 解析为临时文件目录 (tempDir)
- 绝对路径和相对路径的直接支持

#### 多进程路径展开
```c
// 路径变量展开机制
// # 占位符：仅在从进程模式下展开为 "-进程号"
// ? 占位符：总是展开为进程号（主进程为0）

// 示例：
// 主进程 (sys_slaveIdx = -1):
//   "log#.txt" → "log.txt"
//   "config?.json" → "config0.json"
//
// 从进程1 (sys_slaveIdx = 1):
//   "log#.txt" → "log-1.txt"
//   "config?.json" → "config1.json"
```

#### 路径缓存机制
- 支持可选的路径缓存，避免重复计算
- 自动管理内存分配和释放
- 提供可读性检查，返回NULL如果文件不存在

### 2. 配置文件管理框架

#### 文件命名体系
配置文件采用结构化命名规则：`{类别}{集合}.{扩展名}`

**类别 (Category)**：
- `tc` - Traffic Controller 配置
- `cups` - CUPS 服务配置

**集合 (Set)**：
- 无后缀 - 正式配置文件
- `-bak` - 备份配置文件
- `-boot` - 引导配置文件  
- `-temp` - 临时配置文件

**扩展名 (Extension)**：
- `.uri` - 服务器URI配置
- `.trust` - CA信任链
- `.crt` - 客户端证书
- `.key` - 私钥文件

#### 动态文件名生成
```c
// 宏定义实现高效的文件名构建
#define configFilename(cat,set,ext) \
    (CFNS[(cat)*(nFN_SET*nFN_EXT + nFN_TAF)+((set)*nFN_EXT)+(ext)])

// 示例生成的文件名：
// configFilename(FN_TC, FN_REG, FN_URI) → "tc.uri"
// configFilename(FN_CUPS, FN_BAK, FN_CRT) → "cups-bak.crt"
```

### 3. 安全凭证管理

#### ASN.1 结构解析
支持标准的ASN.1格式凭证包，能够解析包含以下组件的凭证：

1. **信任链 (Trust Chain)** - CA证书链，用于验证服务器身份
2. **客户端证书 (Client Certificate)** - 可选的客户端身份证书
3. **私钥 (Private Key)** - 支持ASN.1格式私钥和Token格式

#### 凭证接收流程
```c
// 分段接收大型凭证数据
sys_credStart(cred_cat, total_len);          // 初始化接收缓冲区
sys_credWrite(cred_cat, data, offset, len);  // 写入数据块
sys_credComplete(cred_cat, total_len);       // 完成接收并解析
```

#### ASN.1解析算法
```c
// 解析ASN.1序列长度的内联函数
static inline int asn1_seqlen(char * ptr) {
    if( ptr[1] & 0x80 ) {
        // 长格式：使用后续字节表示长度
        return (( ((u2_t) ptr[2] & 0xff) << 8) | 
                ( (u2_t) ptr[3] & 0xff)) + 4;
    } else {
        // 短格式：直接使用第二字节
        return ptr[1] + 2;
    }
}
```

### 4. 事务型配置更新

#### 三阶段提交协议
配置更新采用类似数据库的三阶段提交机制：

**阶段1：准备 (Prepare)**
- 接收并验证新配置数据
- 写入临时文件 (`*-temp.*`)
- 验证ASN.1格式和完整性

**阶段2：备份 (Backup)**
- 创建当前配置的备份副本
- 使用事务标记文件确保操作原子性
- 备份文件命名为 `*-bak.*`

**阶段3：提交 (Commit)**
- 原子性地将临时文件重命名为正式配置
- 清理事务标记文件
- 更新内部状态和缓存

#### 事务标记文件系统
```
{tc,cups}-temp.upd   # 标记临时文件准备就绪，可以提交
{tc,cups}-temp.cpy   # 标记正在进行备份复制操作
{tc,cups}-bak.done   # 标记备份操作已完成且有效
```

#### 前向恢复机制
系统启动时自动检测未完成的事务：

```c
void checkRollForward() {
    // 检查是否有未完成的更新操作
    if (存在 *.upd 文件) {
        完成 temp -> reg 重命名操作;
        删除事务标记文件;
    }
    
    // 检查是否有未完成的备份操作
    if (存在 *.cpy 文件) {
        完成 reg -> bak 复制操作;
        创建 *.done 标记;
        删除 *.cpy 标记;
    }
    
    // 恢复备份状态标志
    if (存在 *.done 文件) {
        设置 bakDone[cat] = 1;
    }
}
```

### 5. 系统功能服务

#### EUI (Extended Unique Identifier) 管理
```c
uL_t sys_eui() {
    if ((protoEUI >> 48) != 0)  // 已设置完整EUI
        return protoEUI;
        
    if ((prefixEUI & 0xFFFFffffFFFF) != 0) {
        // MAC转EUI方案：插入0xFFFE到MAC地址中间
        return ((protoEUI & 0xFFFFFF000000) << 16) | 
               0xFFFE000000 | 
               (protoEUI & 0xFFFFFF);
    }
    
    return prefixEUI | protoEUI;  // 直接组合
}
```

#### URI缓存机制
- 自动缓存已读取的URI配置，避免重复文件I/O
- 支持多个配置集合的独立缓存
- 内置URI格式验证和协议检查

#### TCP Keep-Alive 配置
```c
void sys_keepAlive(int fd) {
    // 启用TCP Keep-Alive机制
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable);
    
    // 配置Keep-Alive参数
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count);    // 探测次数
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle);    // 空闲时间  
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl);  // 探测间隔
}
```

## 错误处理与日志

### 错误处理策略
1. **致命错误** - 路径过长等系统级错误直接调用 `rt_fatal()`
2. **可恢复错误** - 文件操作失败记录日志并返回错误码
3. **静默失败** - 可选操作失败时静默处理（如缓存清理）

### 日志记录
```c
// 详细的ASN.1解析日志
LOG(MOD_SYS|INFO, " credComplete - trust_off=%4u, trust_len=%4u ...", 
    trust_offset, trust_length);
LOG(MOD_SYS|INFO, " credComplete - cert_off =%4u, cert_len =%4u ...", 
    cert_offset, cert_length);
LOG(MOD_SYS|INFO, " credComplete - key_off  =%4u, key_len  =%4u ...", 
    key_offset, key_length);
```

## 性能优化特性

### 内存管理优化
- 使用栈缓冲区进行路径构建，减少动态分配
- 及时释放临时缓冲区，避免内存泄漏
- 缓存机制减少重复文件操作

### 文件I/O优化
- 批量文件操作减少系统调用次数
- 使用 `fs_sync()` 确保关键操作的持久性
- 预分配缓冲区大小，避免动态扩展

### 路径计算优化
- 预编译的字符串常量表
- 宏定义实现O(1)时间复杂度的文件名构建
- 缓存机制避免重复路径计算

## 常见问题与注意事项

### Q: 为什么需要事务标记文件？
A: 事务标记文件确保配置更新的原子性。如果系统在更新过程中崩溃，重启时能够根据标记文件的存在状态判断操作进行到哪一步，并自动完成剩余操作。

### Q: 多进程环境下如何避免文件冲突？
A: 使用 `#` 和 `?` 占位符实现进程特定的文件路径，每个进程操作独立的文件集合，避免竞争条件。

### Q: ASN.1解析失败如何处理？
A: 解析失败时会记录详细的错误日志，包括期望的数据格式和实际接收的数据，然后清理缓冲区并返回错误状态，不会影响现有配置。

### Q: 配置备份的时机是什么？
A: 备份操作在每次配置更新前自动触发，且每个配置类别只备份一次（通过 bakDone 标志控制），直到下次系统重启。

## 依赖关系

- **rt.h** - 运行时基础功能（内存管理、字符串操作）
- **fs.h** - 文件系统抽象层（跨平台文件操作）
- **uj.h** - JSON解析器（配置文件处理）
- **net.h** - 网络功能（URI验证）
- **s2conf.h** - Station配置管理

## 总结

sys.c 模块是 BasicStation 的核心基础设施，它通过精心设计的事务机制、路径管理系统和凭证处理流程，为整个系统提供了可靠、安全、高效的配置管理服务。其设计充分考虑了多进程环境、故障恢复、性能优化等关键需求，是一个工业级的系统组件。 