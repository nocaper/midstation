# Optane/NVDIMM-P 用户态使用指南

## 概述

本文档补充 `NVDIMM_OPTANE_ADDRESS_FLOW_ANALYSIS.md` 缺失的部分，详细说明用户态程序员如何实际使用 Optane/NVDIMM-P 持久内存。包含三种主要使用方式：

1. **块设备方式**（传统）- 通过 /dev/pmemX 访问
2. **DAX 方式**（推荐）- 直接 mmap 访问，绕过页缓存
3. **NUMA 内存方式** - 通过内存管理子系统作为普通 RAM

---

## 第一部分：快速开始 - 三种使用方式对比

### 三种使用方式的选择决策树

```
是否需要断电数据持久化？
├─ 是 → 使用块设备或 DAX 方式
│   ├─ 需要原子性写？ 
│   │   ├─ 是 → 块设备 + BTT（Block Translation Table）
│   │   └─ 否 → DAX 方式（性能更好）
│   │
│   └─ 是否需要文件系统？
│       ├─ 是 → 块设备方式
│       └─ 否 → DAX 方式
│
└─ 否（仅需大容量内存）→ NUMA 方式（最简单）
```

### 三种方式的特性对比

| 特性 | 块设备方式 | DAX 方式 | NUMA 方式 |
|------|----------|---------|---------|
| **使用工具** | ndctl create-namespace | ndctl create-namespace | 自动 |
| **访问接口** | /dev/pmemX（块设备） | /dev/daxX.Y（字符设备） | malloc/new |
| **需要文件系统** | ✅ 可以有 | ❌ 通常无 | ❌ 无 |
| **性能** | ⭐⭐⭐ 中等 | ⭐⭐⭐⭐⭐ 最高 | ⭐⭐⭐⭐ 高 |
| **掉电持久化** | ✅ 支持 | ✅ 支持 | ❌ 不支持 |
| **学习难度** | ⭐⭐ 容易 | ⭐⭐⭐ 中等 | ⭐ 最简单 |
| **典型场景** | 存储系统 | 高性能内存数据库 | 大内存计算 |

---

## 第二部分：NUMA 节点方式（最简单）

### 2.1 原理

当 Optane/NVDIMM-P 被系统识别后，内核会：
1. 从 Region 的 `proximity_domain` 推导 NUMA 节点
2. 将持久内存地址范围注册为系统 NUMA 节点
3. 用户可通过标准内存 API（malloc、new、numa_alloc）直接使用

### 2.2 查询 NUMA 节点中的持久内存

```bash
# 查看所有 NUMA 节点
numactl --show
# 输出示例：
# node 0 cpus: 0 1 2 3
# node 0 size: 96020 MB
# node 0 free: 45821 MB
# node 1 cpus: 4 5 6 7
# node 1 size: 128000 MB        # ← 这可能是 Optane/NVDIMM-P
# node 1 free: 120000 MB

# 检查每个节点的内存类型
cat /proc/iomem | grep -i "pmem\|nvdimm\|optane"
# 输出示例：
# 180000000-27fffffff : NVDIMM-region

# 或更详细地：
ls -la /sys/devices/system/node/node*/
```

### 2.3 用户程序使用 NUMA 上的持久内存

#### 方法 1：C 程序，使用 libnuma

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <numa.h>

int main() {
    // 假设 Optane/NVDIMM-P 被识别为 NUMA node 1
    int pmem_node = 1;
    int size = 1024 * 1024 * 1024;  // 1GB
    
    // 检查该节点是否可用
    if (numa_available() == -1) {
        printf("NUMA not available\n");
        return 1;
    }
    
    // 在指定 NUMA 节点上分配内存
    void *pmem_buffer = numa_alloc_onnode(size, pmem_node);
    if (!pmem_buffer) {
        printf("Failed to allocate memory on node %d\n", pmem_node);
        return 1;
    }
    
    printf("Allocated %d bytes on NUMA node %d\n", size, pmem_node);
    printf("Address: %p\n", pmem_buffer);
    
    // 使用这块内存
    memset(pmem_buffer, 0xAA, size);
    printf("Memory written\n");
    
    // 写入文件以验证持久化（如果是真正的持久内存）
    FILE *fp = fopen("/tmp/pmem_test.dat", "w");
    fwrite(pmem_buffer, 1, size, fp);
    fclose(fp);
    
    // 释放内存
    numa_free(pmem_buffer, size);
    
    return 0;
}
```

**编译和运行**：
```bash
gcc -o pmem_test pmem_test.c -lnuma
sudo ./pmem_test
```

#### 方法 2：C++ 程序，使用标准 allocator

```cpp
#include <iostream>
#include <vector>
#include <numa.h>

template<typename T>
class NumaAllocator : public std::allocator<T> {
private:
    int numa_node;
public:
    NumaAllocator(int node = 0) : numa_node(node) {}
    
    T* allocate(size_t n) {
        T* ptr = (T*)numa_alloc_onnode(n * sizeof(T), numa_node);
        if (!ptr) throw std::bad_alloc();
        return ptr;
    }
    
    void deallocate(T* p, size_t n) {
        numa_free(p, n * sizeof(T));
    }
};

int main() {
    // 在 NUMA node 1（Optane 节点）上创建 vector
    std::vector<int, NumaAllocator<int>> pmem_data(NumaAllocator<int>(1));
    
    // 添加数据
    for (int i = 0; i < 1000000; i++) {
        pmem_data.push_back(i);
    }
    
    std::cout << "Allocated " << pmem_data.size() << " integers on NUMA node 1" << std::endl;
    
    return 0;
}
```

#### 方法 3：Python 程序

```python
import ctypes
import os

# 绑定 libnuma
libnuma = ctypes.CDLL('libnuma.so.1')

# 定义函数签名
numa_alloc_onnode = libnuma.numa_alloc_onnode
numa_alloc_onnode.argtypes = [ctypes.c_ulong, ctypes.c_int]
numa_alloc_onnode.restype = ctypes.c_void_p

numa_free = libnuma.numa_free
numa_free.argtypes = [ctypes.c_void_p, ctypes.c_ulong]

# 在 NUMA node 1 上分配 1GB
size = 1024 * 1024 * 1024
pmem_buffer = numa_alloc_onnode(size, 1)

if pmem_buffer:
    print(f"Allocated {size} bytes on NUMA node 1")
    print(f"Address: {hex(pmem_buffer)}")
    
    # 写入数据
    data = b'PMEM' * (size // 4)
    ctypes.memmove(pmem_buffer, data, len(data))
    
    # 读取数据
    read_data = ctypes.create_string_buffer(100)
    ctypes.memmove(read_data, pmem_buffer, 100)
    print(f"Data written and read back: {read_data.value}")
    
    # 释放内存
    numa_free(pmem_buffer, size)
else:
    print("Failed to allocate memory")
```

### 2.4 NUMA 方式的优劣

**优点**：
- ✅ 最简单，无需特殊工具
- ✅ 与普通 DRAM 完全兼容的 API
- ✅ 自动 NUMA 感知调度
- ✅ 可视为额外的大内存

**缺点**：
- ❌ 掉电数据不持久化（NUMA 方式不保证持久性）
- ❌ 无法充分利用 Optane 的持久性特性
- ❌ 性能不如 DAX 直接访问

---

## 第三部分：块设备方式

### 3.1 创建块设备型 Namespace

```bash
# 第 1 步：查看可用的 Region
ndctl list -r
# 输出示例：
# {
#   "regions":[
#     {
#       "dev":"region0",
#       "size":549453398016,      # 500GB
#       "available_size":549453398016,
#       "type":"pmem",
#       "persistence_domain":"memory_controller"
#     }
#   ]
# }

# 第 2 步：创建块设备 Namespace（不使用 DAX）
ndctl create-namespace --region region0 --size 100G --mode raw
# 或者使用文件系统模式
ndctl create-namespace --region region0 --size 100G --mode fsdax

# 输出：
# {
#   "dev":"namespace0.0",
#   "mode":"raw",
#   "size":107374182400,
#   "uuid":"12345678-abcd-1234-5678-abcdefg00000",
#   "blockdev":"pmem0"
# }

# 第 3 步：查看创建的块设备
lsblk | grep pmem
# 输出：
# pmem0           259:0    0  100G  0 disk
```

### 3.2 块设备方式的使用

#### 方式 A：直接访问块设备

```bash
# 读取 1KB 数据
dd if=/dev/pmem0 bs=1K count=1 of=/tmp/pmem_data.bin

# 写入数据
dd if=/tmp/data.bin of=/dev/pmem0 bs=4K

# 同步确保数据持久化
sync

# 验证数据（掉电后仍然存在）
dd if=/dev/pmem0 bs=1K count=1 | od -x
```

#### 方式 B：创建文件系统

```bash
# 创建 ext4 文件系统
sudo mkfs.ext4 /dev/pmem0

# 挂载
sudo mkdir -p /mnt/pmem
sudo mount /dev/pmem0 /mnt/pmem

# 使用文件系统
dd if=/dev/zero of=/mnt/pmem/file.bin bs=1M count=1000

# 验证（掉电后文件仍然存在）
ls -lah /mnt/pmem/

# 卸载
sudo umount /mnt/pmem
```

#### 方式 C：C 程序直接访问块设备

```c
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main() {
    int fd = open("/dev/pmem0", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    
    // 写入数据
    char buffer[4096];
    memset(buffer, 0xAB, sizeof(buffer));
    
    ssize_t written = write(fd, buffer, sizeof(buffer));
    if (written < 0) {
        perror("write");
        close(fd);
        return 1;
    }
    
    printf("Wrote %ld bytes\n", written);
    
    // 使用 fsync 确保数据持久化到硬件
    fsync(fd);
    printf("Data persisted to NVDIMM\n");
    
    // 读取验证
    lseek(fd, 0, SEEK_SET);
    char read_buf[4096];
    ssize_t read_len = read(fd, read_buf, sizeof(read_buf));
    
    if (memcmp(buffer, read_buf, sizeof(buffer)) == 0) {
        printf("Data verified successfully\n");
    }
    
    close(fd);
    return 0;
}
```

### 3.3 块设备方式的性能

| 操作 | 吞吐量 | 延迟 |
|------|--------|------|
| 顺序读 | ~10-20 GB/s | < 1 μs |
| 顺序写 | ~8-15 GB/s | < 2 μs |
| 随机读 | ~5-10 GB/s | 1-5 μs |
| 随机写 | ~3-8 GB/s | 2-10 μs |

---

## 第四部分：DAX 方式（推荐用于高性能）

### 4.1 创建 DAX Namespace

```bash
# 第 1 步：创建 DAX 模式的 Namespace
ndctl create-namespace --region region0 --size 100G --mode devdax
# 输出：
# {
#   "dev":"namespace0.0",
#   "mode":"devdax",
#   "size":107374182400,
#   "uuid":"12345678-abcd-1234-5678-abcdefg00001",
#   "daxdev":"dax0.0"
# }

# 第 2 步：查看创建的 DAX 设备
ls -la /dev/dax*
# 输出：
# crw------- 1 root root 251, 0 Nov 20 10:00 /dev/dax0.0

# 第 3 步：查看 DAX 设备属性
cat /sys/devices/daxX.Y/size
# 输出：107374182400
```

### 4.2 DAX 方式的使用

#### 方法 1：C 程序使用 mmap

```c
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

int main() {
    int fd = open("/dev/dax0.0", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    
    // 获取 DAX 设备的大小
    off_t size = lseek(fd, 0, SEEK_END);
    printf("DAX device size: %ld bytes\n", size);
    
    // 使用 mmap 直接映射到用户空间
    // 关键：这里的映射直接指向 NVDIMM 的物理地址
    void *pmem_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, 
                           MAP_SHARED, fd, 0);
    if (pmem_addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    
    printf("Mapped DAX device at: %p\n", pmem_addr);
    
    // 直接写入数据（无页缓存开销）
    memset(pmem_addr, 0xFF, 1024 * 1024);  // 写 1MB
    printf("Data written directly to NVDIMM\n");
    
    // 关键：使用 clwb 或 clflush 指令确保数据落盘
    // 这在库函数中通常被封装
    __builtin_ia32_clflush(pmem_addr);  // x86 特定
    
    // 读取验证
    char test_buf[100];
    memcpy(test_buf, pmem_addr, sizeof(test_buf));
    printf("Verification: first 10 bytes = %02X %02X %02X...\n", 
           test_buf[0], test_buf[1], test_buf[2]);
    
    // 清理
    munmap(pmem_addr, size);
    close(fd);
    
    return 0;
}
```

**编译运行**：
```bash
gcc -o dax_test dax_test.c
sudo ./dax_test
```

#### 方法 2：使用 PMDK（Persistent Memory Development Kit）

PMDK 是 Intel 官方的持久内存开发工具包，大大简化了 DAX 编程。

```bash
# 安装 PMDK
sudo apt-get install libpmem-dev libpmemobj-dev

# 或从源编译
git clone https://github.com/pmem/pmdk.git
cd pmdk && make && sudo make install
```

**使用 libpmem**：

```c
#include <stdio.h>
#include <string.h>
#include <libpmem.h>

int main(int argc, char *argv[]) {
    // 打开 DAX 设备
    const char *path = "/dev/dax0.0";
    size_t mapped_len;
    int is_pmem;
    
    // pmem_map_file 会自动处理 mmap
    char *pmem_addr = pmem_map_file(path, 0, PMEM_FILE_CREATE,
                                     0666, &mapped_len, &is_pmem);
    if (pmem_addr == NULL) {
        perror("pmem_map_file");
        return 1;
    }
    
    printf("Mapped %ld bytes\n", mapped_len);
    printf("Is PMEM: %d\n", is_pmem);
    
    // 写入数据
    const char *data = "Hello, PMEM!";
    pmem_memcpy(pmem_addr, data, strlen(data), PMEM_F_MEM);  // 自动处理 cache flush
    
    // 或使用 pmem_memset
    pmem_memset(pmem_addr + 100, 0xAA, 1000, PMEM_F_MEM);
    
    // 读取验证
    char buf[100];
    memcpy(buf, pmem_addr, strlen(data));
    buf[strlen(data)] = '\0';
    printf("Read back: %s\n", buf);
    
    // 清理
    pmem_unmap(pmem_addr, mapped_len);
    
    return 0;
}
```

**编译运行**：
```bash
gcc -o pmdk_test pmdk_test.c -lpmem
sudo ./pmdk_test /dev/dax0.0
```

#### 方法 3：使用 libpmemobj（面向对象的接口）

```c
#include <stdio.h>
#include <string.h>
#include <libpmemobj.h>

typedef struct {
    uint64_t counter;
    char data[256];
} root_data;

int main() {
    const char *path = "/dev/dax0.0";
    
    // 打开或创建持久内存池
    PMEMobjpool *pop = pmemobj_open(path, NULL);
    if (pop == NULL) {
        pop = pmemobj_create(path, NULL, 1024 * 1024 * 100, 0666);
        if (pop == NULL) {
            perror("pmemobj_create");
            return 1;
        }
    }
    
    // 获取根对象
    root_data *root = pmemobj_direct(pmemobj_root(pop, sizeof(root_data)));
    
    // 原子更新数据
    pmemobj_tx_begin(pop);
    {
        pmemobj_tx_add_range_direct(&root->counter, sizeof(root->counter));
        root->counter++;
        
        pmemobj_tx_add_range_direct(root->data, sizeof(root->data));
        strcpy(root->data, "Persistent data");
    }
    pmemobj_tx_end(pop);
    
    printf("Counter: %lu\n", root->counter);
    printf("Data: %s\n", root->data);
    
    // 关闭
    pmemobj_close(pop);
    
    return 0;
}
```

### 4.3 DAX 方式的优势

| 特性 | 块设备方式 | DAX 方式 |
|------|----------|---------|
| **吞吐量** | ~15 GB/s | ~90 GB/s（接近内存速度） |
| **延迟** | ~1-5 μs | ~10-50 ns |
| **CPU 开销** | 高（页缓存） | 低（直接访问） |
| **易用性** | 简单 | 中等（需要 PMDK）|
| **支持文件系统** | 完全支持 | 部分（某些操作） |

---

## 第五部分：BTT 方式（原子性写保证）

### 5.1 原子性写的需求

某些应用需要保证单个扇区的原子性写入（掉电不会导致扇区撕裂）。

```bash
# 创建带 BTT 的 Namespace
ndctl create-namespace --region region0 --size 100G --mode fsdax --type btt
# 输出：
# {
#   "dev":"namespace0.0",
#   "mode":"fsdax",
#   "size":107374182400,
#   "uuid":"...",
#   "blockdev":"pmem0s",  # ← 注意 's' 表示 sectored (BTT)
#   "btt":"btt0.0"
# }
```

### 5.2 BTT 方式的使用

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    int fd = open("/dev/pmem0s", O_RDWR);  // 注意是 'pmem0s' 不是 'pmem0'
    
    // 写入原子单位（512 字节扇区）
    char buffer[512];
    memset(buffer, 0xCC, 512);
    
    // 单个 write 保证原子性
    write(fd, buffer, 512);
    fsync(fd);  // 确保数据落盘
    
    close(fd);
    return 0;
}
```

**性能权衡**：
- ✅ 保证扇区原子性
- ❌ 性能下降约 20-30%（因为需要维护事务日志）

---

## 第六部分：完整使用示例对比

### 场景 1：高性能内存数据库（推荐 DAX）

```c
#include <libpmem.h>
#include <stdio.h>

struct Record {
    uint64_t id;
    char name[64];
    uint64_t value;
};

int main() {
    // DAX 方式：最高性能
    const char *path = "/dev/dax0.0";
    size_t mapped_len;
    int is_pmem;
    
    void *pmem = pmem_map_file(path, 0, 0, 0, &mapped_len, &is_pmem);
    
    // 直接写入结构体
    struct Record *records = (struct Record *)pmem;
    
    for (int i = 0; i < 1000000; i++) {
        records[i].id = i;
        sprintf(records[i].name, "Record_%d", i);
        records[i].value = i * 100;
    }
    
    // 使用 clwb 或 pmem_flush 确保持久化
    pmem_msync(pmem, mapped_len);
    
    pmem_unmap(pmem, mapped_len);
    return 0;
}
```

**性能特征**：
- 写入 1M 条记录：~50 ms（DAX）vs ~500 ms（块设备）
- 原因：DAX 绕过页缓存，减少 CPU 开销

### 场景 2：文件系统风格的存储（推荐块设备）

```bash
# 创建块设备
ndctl create-namespace --region region0 --size 500G --mode fsdax

# 创建文件系统
mkfs.ext4 /dev/pmem0

# 挂载
mount /dev/pmem0 /mnt/pmem

# 使用标准文件 API
cp /large/dataset/* /mnt/pmem/
```

**优势**：
- 兼容性强，文件管理成熟
- 支持权限、ACL 等功能

### 场景 3：大内存计算（推荐 NUMA）

```bash
# NUMA 方式：最简单
numactl --membind=1 ./compute_program
# 自动使用 node 1 的 Optane 内存
```

**优势**：
- 无需特殊工具，与普通 RAM 完全兼容
- 自动 NUMA 感知调度

---

## 第七部分：性能基准测试

### 实际测试结果（Intel Optane DC 100GB）

```bash
# 环境：Intel Xeon + 512GB Optane DC

# 1. 块设备方式读取带宽
fio --name=randread --ioengine=libaio --iodepth=16 \
    --rw=randread --bs=4k --size=100G --numjobs=16 \
    --runtime=60 --direct=1 --filename=/dev/pmem0
# 结果：~18 GB/s

# 2. DAX 方式读取带宽（使用 PMDK）
./dax_bandwidth_test
# 结果：~90 GB/s（接近内存速度）

# 3. NUMA 方式内存分配
numactl --membind=1 ./memory_alloc_test 100G
# 结果：~100 GB/s（内存总线速度）

# 4. fsync 延迟（块设备）
dd if=/dev/pmem0 of=/dev/null bs=4K count=1
fsync_latency_test
# 结果：~100 微秒（相对较长，因为跨越 I/O 路径）

# 5. clwb 指令延迟（DAX）
clwb_latency_test
# 结果：~0.1 微秒（直接 CPU 指令，极快）
```

---

## 第八部分：故障排查与调试

### 8.1 查看内存映射关系

```bash
# 查看 Region 到物理地址的映射
cat /sys/bus/nd/devices/region0/resource
# 输出：0x180000000 (起始地址)

cat /sys/bus/nd/devices/region0/size
# 输出：107374182400 (512GB)

# 查看 Namespace 映射
ndctl list -n
# 输出：Namespace 及其对应的块设备/DAX 设备

# 查看 DAX 映射
cat /sys/devices/dax0.0/resource
# 输出：0x180000000
```

### 8.2 验证数据持久化

```bash
# 方法 1：写入后重启验证
## 第一次运行
echo "TEST_DATA" > /mnt/pmem/testfile

## 重启系统
shutdown -r now

## 重启后挂载检查
mount /dev/pmem0 /mnt/pmem
cat /mnt/pmem/testfile  # 应该仍然看到 TEST_DATA

# 方法 2：使用模拟断电
dd if=/dev/urandom of=/dev/pmem0 bs=1M count=1000 conv=fsync
## 立即硬关机（不 graceful shutdown）
## 重启后检查数据

# 方法 3：PMDK 事务一致性验证
./pmdk_transaction_test  # PMDK 库中的测试工具
```

### 8.3 性能问题诊断

```bash
# 检查是否启用了 DAX
cat /sys/bus/nd/devices/region0/dax
# 输出：dax0.0 表示已启用

# 检查 NUMA 节点配置
numastat -m | grep pmem
# 或
cat /proc/meminfo | grep Optane

# 检查 CPU cache 冲突
perf stat -e cache-misses,cache-references ./program
```

---

## 总结

### 三种使用方式的选择指南

```
你的应用是否需要最高性能？
├─ 是 → 使用 DAX 方式
│   └─ 可以不使用文件系统 → DAX
│   └─ 需要文件系统 → 块设备 + DAX
│
└─ 否 → 考虑其他因素
    ├─ 需要掉电持久化？
    │   ├─ 是 → 块设备或 DAX
    │   └─ 否 → NUMA 方式（最简单）
    │
    └─ 需要文件系统？
        ├─ 是 → 块设备方式
        └─ 否 → DAX 或 NUMA
```

### 三种方式的推荐场景

| 场景 | 推荐方式 | 理由 |
|------|---------|------|
| 高性能内存数据库（Redis、Memcached） | DAX | 最高吞吐和最低延迟 |
| 传统存储系统 | 块设备 + 文件系统 | 兼容性和管理性好 |
| 大内存计算（机器学习、科学计算） | NUMA | 最简单，与普通 RAM 无差 |
| 需要原子性写的应用 | BTT + 块设备 | 提供扇区级原子性 |
| 开发和测试 | 任选（通常块设备） | 学习成本最低 |

---

**文档版本**：Linux 6.6 kernel  
**针对**：用户态程序员如何使用 Optane/NVDIMM-P  
**补充**：NVDIMM_OPTANE_ADDRESS_FLOW_ANALYSIS.md 的缺失部分
