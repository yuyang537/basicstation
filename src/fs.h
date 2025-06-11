/*
 * FS - 文件系统模块头文件
 * =======================
 * 功能：BasicStation的Flash存储文件系统实现
 * 核心：基于Flash的日志结构化文件系统、垃圾回收、加密存储
 */

#ifndef _fs_h_
#define _fs_h_

// Flash底层操作API
void  sys_eraseFlash (u4_t faddr, uint pagecnt);        // 擦除Flash页：擦除指定地址的Flash页面
void  sys_writeFlash (u4_t faddr, u4_t* data, uint u4cnt); // 写入Flash：向Flash写入数据块
void  sys_readFlash  (u4_t faddr, u4_t* data, uint u4cnt);  // 读取Flash：从Flash读取数据块
u4_t* sys_ptrFlash   ();                                 // 获取Flash指针：返回Flash基地址指针
void  sys_iniFlash   ();                                 // 初始化Flash：配置Flash控制器

// 便捷Flash读写函数
u4_t rdFlash1 (u4_t faddr);                             // 读取单个字：从Flash读取一个32位字
void rdFlashN (u4_t faddr, u4_t* daddr, uint u4cnt);   // 读取多个字：批量读取Flash数据

void wrFlash1 (u4_t faddr, u4_t data);                 // 写入单个字：向Flash写入一个32位字
void wrFlashN (u4_t faddr, u4_t* daddr, uint u4cnt, int keepData); // 写入多个字：批量写入Flash数据

// POSIX兼容文件操作API
int  fs_open   (str_t filename, int mode, ...);        // 打开文件：创建或打开指定文件
int  fs_read   (int fd,       void* buf, int size);    // 读取文件：从文件描述符读取数据
int  fs_write  (int fd, const void* buf, int size);    // 写入文件：向文件描述符写入数据
int  fs_close  (int fd);                               // 关闭文件：关闭文件描述符并释放资源
int  fs_rename (str_t from, str_t to);                 // 重命名文件：修改文件名称
int  fs_unlink (str_t from);                           // 删除文件：从文件系统中删除文件
int  fs_chdir  (str_t dir);                            // 切换目录：改变当前工作目录
int  fs_access (str_t fn, int mode);                   // 检查访问权限：测试文件访问权限
int  fs_stat   (str_t fn, struct stat* st);            // 获取文件状态：读取文件元数据信息
int  fs_lseek  (int fd, int offset, int whence);       // 文件定位：移动文件读写指针
void fs_sync   ();                                     // 同步文件系统：强制写入所有缓存数据

// 文件名处理工具
int  fs_fnNormalize (const char* fn, char* wb, int maxsz); // 标准化文件名：规范化文件路径

// 文件系统管理API
int  fs_ini   (u4_t key[4]);                           // 初始化文件系统：设置加密密钥并启动
int  fs_ck    ();                                      // 检查文件系统：验证数据完整性
void fs_erase ();                                      // 擦除文件系统：格式化整个Flash存储
void fs_gc    (int emergency);                         // 垃圾回收：回收无效空间，emergency=1表示强制回收
int  fs_dump  (void (*logfn)(u1_t mod_level, const char* fmt, ...)); // 转储调试信息：输出文件系统状态
int  fs_shell (char* cmdline);                         // Shell命令：执行文件系统调试命令

/**
 * struct fsinfo - 文件系统信息结构
 * 功能：提供文件系统的详细状态和统计信息
 */
typedef struct fsinfo {
    void* fbasep;       // Flash基指针：Flash存储区的内存映射地址
    u4_t  fbase;        // Flash基地址：Flash存储区的物理地址
    u2_t  pagecnt;      // 页面总数：Flash总页面数量
    u2_t  pagesize;     // 页面大小：每个Flash页面的字节数
    u1_t  activeSection;// 活动分区：当前正在使用的分区编号(0或1)
    u2_t  gcCycles;     // GC周期数：垃圾回收执行次数统计
    u4_t  records;      // 记录总数：文件系统中的记录条目数
    u4_t  used;         // 已用空间：已分配的Flash空间字节数
    u4_t  free;         // 可用空间：剩余可用的Flash空间字节数
    u4_t  key[4];       // 加密密钥：128位XOR加密密钥
} fsinfo_t;

void fs_info(fsinfo_t* infop);                         // 获取文件系统信息：填充fsinfo结构

#endif // _fs_h_
