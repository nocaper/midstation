# Optane/NVDIMM 内核源码分析：从 Region 到用户设备

## 概述

本文档详细分析 Linux 内核源码中，Region 对象如何逐步转化为用户可见的设备的完整过程：

```
ACPI NFIT 表
  ↓ (drivers/acpi/nfit/core.c)
nd_region 对象
  ↓ (drivers/nvdimm/)
  ├─ NUMA 节点注册
  ├─ Namespace 创建
  ├─ 块设备创建 (/dev/pmemX)
  └─ DAX 设备创建 (/dev/daxX.Y)
  ↓
用户态程序访问
```

---

## 第一部分：NUMA 节点上报机制

### 1.1 核心问题

当用户使用 `numa_alloc_onnode()` 分配内存时，内核是如何知道某个 NUMA 节点是 Optane/NVDIMM-P 的？

### 1.2 数据结构：proximity_domain 和 NUMA 映射

**位置**：`drivers/acpi/nfit/nfit.h`

```c
struct nfit_spa {
    struct list_head list;
    struct acpi_nfit_system_address *spa;  // 来自 ACPI 表
    size_t spa_size;
    u64 address;                            // 系统地址
    u64 length;                             // 地址范围大小
    enum nfit_spa_type type;
    int numa_node;                          // ← NUMA 节点编号（关键！）
    int iomem_altname;
    struct kref kref;
};
```

**关键字段说明**：
- `spa->proximity_domain`：来自 ACPI NFIT 表中的 SPA Range 结构
- `nfit_spa->numa_node`：解析后的 NUMA 节点编号

### 1.3 proximity_domain 到 NUMA 节点的映射

**函数**：`acpi_nfit_init()` 中的 SPA 范围处理  
**位置**：`drivers/acpi/nfit/core.c:add_spa()`

```c
// 行 698-750（简化版）
static int add_spa(struct acpi_nfit_desc *acpi_desc,
        struct list_head *spa_list, struct acpi_nfit_system_address *spa)
{
    struct device *dev = acpi_desc->dev;
    struct nfit_spa *nfit_spa;
    
    // 分配 nfit_spa 结构
    nfit_spa = devm_kzalloc(dev, sizeof(*nfit_spa), GFP_KERNEL);
    
    // 复制 SPA 数据
    nfit_spa->spa = spa;
    nfit_spa->spa_size = sizeof(*spa);
    
    // ===== 关键步骤 1：提取物理地址范围 =====
    // 行 712-713
    nfit_spa->address = spa->address;
    nfit_spa->length = spa->length;
    
    // ===== 关键步骤 2：proximity_domain → NUMA node =====
    // 行 720-730（伪代码）
    // proximity_domain 是 ACPI 的概念，表示"靠近哪个处理器/内存控制器"
    // 内核需要将其转换为 Linux NUMA 节点编号
    
    nfit_spa->numa_node = acpi_map_pxm_to_node(spa->proximity_domain);
    //                    ↑ 这个函数在 drivers/acpi/numa/ 或 arch/ 中定义
    
    if (nfit_spa->numa_node == NUMA_NO_NODE) {
        // 如果映射失败（例如没有启用 NUMA），使用默认节点 0
        dev_warn(dev, "SPA proximity_domain %d has no NUMA node, using node 0",
                 spa->proximity_domain);
        nfit_spa->numa_node = 0;
    }
    
    dev_dbg(dev, "SPA Range: address=0x%llx, length=%lld, proximity_domain=%d, NUMA node=%d",
            nfit_spa->address, nfit_spa->length, spa->proximity_domain, nfit_spa->numa_node);
    
    // 列表添加
    list_add_tail(&nfit_spa->list, spa_list);
    
    return 0;
}
```

**acpi_map_pxm_to_node() 的实现**  
**位置**：`drivers/acpi/numa/srat.c` 或架构相关代码

```c
// drivers/acpi/numa/srat.c
int __init acpi_map_pxm_to_node(int pxm)
{
    int node;
    
    // 查询 PXM（Proximity eXtent Mapping）到 NUMA 节点的映射表
    // 这个映射表在系统启动时从 SRAT 表（System Resource Affinity Table）构建
    
    node = pxm_to_node(pxm);
    // pxm_to_node 是一个简单的查表：
    // static int pxm_to_node_map[MAX_PXM_DOMAINS] = {...};
    // return pxm_to_node_map[pxm];
    
    if (node < 0 || node >= MAX_NUMNODES)
        return NUMA_NO_NODE;
    
    return node;
}
```

### 1.4 NUMA 节点信息在 Region 中的传播

**从 nfit_spa 到 nd_region**  
**位置**：`drivers/acpi/nfit/core.c:acpi_nfit_register_region()`

```c
// 行 2596（简化版）
static int acpi_nfit_register_region(struct acpi_nfit_desc *acpi_desc,
        struct nfit_spa *nfit_spa)
{
    struct nd_region_desc ndr_desc;
    
    // ===== 关键步骤：NUMA 信息从 nfit_spa 传递到 nd_region_desc =====
    // 行 2634-2635
    ndr_desc.numa_node = nfit_spa->numa_node;  // ← 这里！
    ndr_desc.target_node = nfit_spa->numa_node;
    
    // ... 其他字段 ...
    
    // 创建 Region
    nd_region = nvdimm_pmem_region_create(
        acpi_desc->nvdimm_bus, &ndr_desc);
    
    return nd_region ? 0 : -ENOMEM;
}
```

**nd_region_desc 到 nd_region**  
**位置**：`drivers/nvdimm/region_devs.c:nd_region_create()`

```c
// 行 1000（关键部分）
static struct nd_region *nd_region_create(...)
{
    struct nd_region *nd_region;
    
    // ... 内存分配 ...
    
    // ===== 关键步骤：NUMA 信息写入 nd_region 对象 =====
    // 行 1086-1087
    nd_region->numa_node = ndr_desc->numa_node;
    nd_region->target_node = ndr_desc->target_node;
    
    // ... 其他初始化 ...
    
    return nd_region;
}
```

### 1.5 NUMA 节点在 sysfs 中的暴露

**属性文件**：`/sys/bus/nd/devices/regionX/numa_node`

**show 函数**  
**位置**：`drivers/nvdimm/region_devs.c`

```c
// 行 ~200（搜索 "static ssize_t numa_node_show"）
static ssize_t numa_node_show(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct nd_region *nd_region = to_nd_region(dev);
    
    // 直接输出 nd_region->numa_node
    return sprintf(buf, "%d\n", nd_region->numa_node);
}

// 属性定义
static DEVICE_ATTR_RO(numa_node);  // 只读
```

**查询示例**：
```bash
cat /sys/bus/nd/devices/region0/numa_node
# 输出：1

# 说明 region0 对应的 Optane 内存被识别为 NUMA node 1
```

### 1.6 内存管理子系统中的 NUMA 节点

当内核注册了 NUMA 节点后，`numactl` 等工具如何看到 Optane 内存？

**关键：MEMBLOCK 和 NUMA 初始化顺序**

```
系统启动
  ↓
SRAT 表解析 → PXM 到 NUMA 节点映射表
  ↓
ACPI NFIT 表解析 → SPA Range + proximity_domain
  ↓
MEMBLOCK 阶段：为每个 SPA Range 分配 NUMA 节点
  ↓
内存管理初始化完成
  ↓
用户查询 numactl --show 时，看到 Optane 对应的 NUMA 节点
```

**代码路径**：
```c
// kernel/mm/page_alloc.c
// 当 allocate_node 被调用时，会查询 NUMA 节点对应的内存范围
```

---

## 第二部分：Namespace 创建机制

### 2.1 核心问题

用户运行 `ndctl create-namespace` 时，内核是如何从 Region 创建出 Namespace 的？

### 2.2 Namespace 数据结构

**位置**：`drivers/nvdimm/nd.h`

```c
struct nd_namespace_common {
    struct device dev;
    struct list_head list;
    struct nd_region *region;           // ← 指向所属的 Region
    u8 *uuid;
    enum nd_claim_class claim_class;
};

// 两种具体类型的 Namespace

struct nd_namespace_pmem {
    struct nd_namespace_common common;
    unsigned long lbasize;              // 扇区大小
    struct kernfs_node *badblocks_state;
};

struct nd_namespace_blk {
    struct nd_namespace_common common;
    // 块寻址模式特定字段
};
```

### 2.3 ndctl 工具与内核的交互

**用户运行**：
```bash
ndctl create-namespace --region region0 --size 100G --mode devdax
```

**内核接收路径**：

```
ndctl 用户工具
  ↓ (写入 sysfs)
写入 /sys/bus/nd/devices/region0/create_namespace
  ↓ (内核 sysfs 回调)
drivers/nvdimm/region_devs.c:create_namespace_show()
  ↓ (字符串解析)
解析参数：size, mode, uuid...
  ↓ (调用创建函数)
nvdimm_create_namespace_pmem() 或 nvdimm_create_namespace_blk()
  ↓
nd_namespace 对象创建
```

### 2.4 创建 PMEM Namespace 的源码

**函数**：`nvdimm_create_namespace_pmem()`  
**位置**：`drivers/nvdimm/namespace_devs.c` 和 `drivers/nvdimm/region_devs.c`

**创建过程**（简化版）：

```c
// drivers/nvdimm/namespace_devs.c
static struct nd_namespace_pmem *create_namespace_pmem(...)
{
    struct nd_namespace_pmem *ns_pmem;
    struct device *dev;
    
    // ===== 步骤 1：分配 nd_namespace_pmem 结构 =====
    ns_pmem = devm_kzalloc(region->dev.parent,
                           sizeof(*ns_pmem), GFP_KERNEL);
    if (!ns_pmem)
        return ERR_PTR(-ENOMEM);
    
    // ===== 步骤 2：初始化 common 字段 =====
    ns_pmem->common.region = region;          // ← 关键：指向 Region
    ns_pmem->common.uuid = requested_uuid;    // UUID（可选）
    
    // ===== 步骤 3：从 Region 计算 Namespace 大小 =====
    // 用户可以指定：
    //   - --size 100G：创建 100GB 的 Namespace
    //   - 不指定：使用 Region 的全部空间
    
    if (requested_size > 0) {
        // 验证 requested_size 不超过 region 的可用空间
        if (requested_size > nd_region_available_dpa(region)) {
            dev_err(region->dev.parent, "Size %lld exceeds region capacity\n",
                    requested_size);
            return ERR_PTR(-ENOSPC);
        }
        ns_pmem->size = requested_size;
    } else {
        // 使用 Region 全部空间
        ns_pmem->size = nd_region_available_dpa(region);
    }
    
    // ===== 步骤 4：初始化 device 结构 =====
    dev = &ns_pmem->common.dev;
    dev_set_name(dev, "namespace%d.%d",
                 region->id,              // Region ID（如 0）
                 region->ns_seed++);      // Namespace 序列号
    
    dev->parent = &region->dev;
    dev->type = &namespace_pmem_device_type;  // 设置设备类型
    
    // ===== 步骤 5：向内核注册设备 =====
    device_initialize(dev);
    nd_device_register(dev);  // 创建 sysfs 条目
    
    return ns_pmem;
}
```

### 2.5 Namespace 在 sysfs 中的暴露

**创建后的 sysfs 路径**：
```
/sys/bus/nd/devices/
├── namespace0.0/           ← 第一个 Namespace
│   ├── size               → Namespace 的大小
│   ├── uuid               → Namespace 的 UUID
│   ├── mode               → 模式（raw/fsdax/devdax）
│   ├── daxdev             → DAX 设备指针（如果是 devdax 模式）
│   ├── blockdev           → 块设备指针（如果是 fsdax/raw 模式）
│   └── type               → 类型（pmem/blk）
│
├── namespace0.1/           ← 第二个 Namespace
│   └── ...
│
└── region0/
    └── ...
```

**sysfs 属性的 show 函数**  
**位置**：`drivers/nvdimm/namespace_devs.c`

```c
// 输出 Namespace 大小
static ssize_t size_show(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct nd_namespace_pmem *ns_pmem = to_nd_namespace_pmem(dev);
    return sprintf(buf, "%llu\n", ns_pmem->size);
}

// 输出 Namespace 模式
static ssize_t mode_show(struct device *dev,
        struct device_attribute *attr, char *buf)
{
    struct nd_namespace_pmem *ns_pmem = to_nd_namespace_pmem(dev);
    const char *mode_name;
    
    switch (ns_pmem->mode) {
    case ND_PFN_MODE_RAM:
        mode_name = "raw";
        break;
    case ND_PFN_MODE_PMEM:
        mode_name = "fsdax";
        break;
    case ND_PFN_MODE_DAX:
        mode_name = "devdax";
        break;
    default:
        mode_name = "unknown";
    }
    
    return sprintf(buf, "%s\n", mode_name);
}
```

---

## 第三部分：块设备创建（/dev/pmemX）

### 3.1 核心问题

当创建了 fsdax/raw 模式的 Namespace 后，`/dev/pmemX` 是如何被创建的？

### 3.2 PMEM 驱动注册

**位置**：`drivers/nvdimm/pmem.c`

```c
// 行 ~500
static struct platform_driver pmem_driver = {
    .probe = pmem_probe,
    .remove = pmem_remove,
    .driver = {
        .name = "nd_pmem",
        .of_match_table = nd_pmem_of_match,
    },
};

// 驱动注册
module_platform_driver(pmem_driver);
```

**驱动如何被匹配**：

当 Namespace 被创建时，内核会：
1. 为 Namespace 创建 `platform_device`
2. 设置 `platform_device.driver_data` 指向 `nd_namespace_pmem`
3. 内核总线驱动匹配：`nd_pmem_driver` 识别 `nd_namespace_pmem` 设备
4. 调用 `pmem_probe()` 函数

### 3.3 pmem_probe() 函数详解

**函数**：`pmem_probe()`  
**位置**：`drivers/nvdimm/pmem.c:pmem_probe()`

```c
// 行 ~400（简化版）
static int pmem_probe(struct platform_device *pdev)
{
    struct nd_namespace_pmem *nspm;
    struct pmem_device *pmem;
    struct nd_region *nd_region;
    struct device *dev = &pdev->dev;
    struct gendisk *disk;
    
    // ===== 步骤 1：获取 Namespace 指针 =====
    nspm = dev_get_drvdata(dev);  // 从 platform_device 中获取
    if (!nspm)
        return -ENXIO;
    
    nd_region = to_nd_region(nspm->common.dev.parent);
    
    // ===== 步骤 2：分配 pmem_device 结构 =====
    pmem = devm_kzalloc(dev, sizeof(*pmem), GFP_KERNEL);
    if (!pmem)
        return -ENOMEM;
    
    // ===== 步骤 3：初始化物理地址信息 =====
    // 从 Namespace 获取地址范围
    // 行 ~420
    pmem->phys_addr = nd_region->ndr_start;    // Region 的起始地址
    pmem->size = nspm->size;                    // Namespace 的大小
    pmem->virt_addr = memremap(pmem->phys_addr,
                               pmem->size,
                               MEMREMAP_WB);    // 映射到内核虚拟地址空间
    
    if (!pmem->virt_addr)
        return -ENOMEM;
    
    // ===== 步骤 4：初始化 Bad Block（坏块）追踪 =====
    // NVDIMM 可能有不可读的"坏块"，需要追踪
    pmem->bb.dev = dev;
    
    // ===== 步骤 5：创建块设备 =====
    // 行 ~450
    // 为块设备分配主编号和从编号
    pmem->queue = blk_alloc_queue(NUMA_NO_NODE);
    if (!pmem->queue)
        return -ENOMEM;
    
    // ===== 步骤 6：初始化块设备属性 =====
    pmem->queue->queuedata = pmem;
    blk_queue_logical_block_size(pmem->queue, 512);  // 逻辑块大小
    blk_queue_physical_block_size(pmem->queue, 4096); // 物理块大小
    blk_queue_max_hw_sectors(pmem->queue, 1024);
    
    // 设置块设备的 I/O 处理函数
    pmem->queue->make_request_fn = pmem_make_request;
    
    // ===== 步骤 7：创建 gendisk（通用磁盘）结构 =====
    // 行 ~470
    disk = alloc_disk(0);  // 0 表示没有分区（整个设备作为一个磁盘）
    if (!disk)
        return -ENOMEM;
    
    pmem->disk = disk;
    disk->major = pmem_major;         // 块设备主编号
    disk->first_minor = pmem_minor;   // 块设备从编号
    disk->fops = &pmem_fops;          // 文件操作（read/write/ioctl）
    disk->private_data = pmem;        // 关键：保存 pmem_device 指针
    disk->queue = pmem->queue;
    
    // 设置设备名称
    sprintf(disk->disk_name, "pmem%d", ...);  // 例如 "pmem0"
    
    // 设置容量
    set_capacity(disk, nspm->size >> 9);  // 容量 = size / 512
    
    // ===== 步骤 8：向系统注册块设备 =====
    // 行 ~500
    add_disk(disk);  // 这会在 /dev 中创建 /dev/pmem0
    
    // ===== 步骤 9：创建 sysfs 属性 =====
    // 例如 /sys/block/pmem0/
    disk_to_dev(disk)->devt = MKDEV(pmem_major, pmem_minor);
    
    platform_set_drvdata(pdev, pmem);
    
    dev_info(dev, "Created block device %s, capacity %lld bytes\n",
             disk->disk_name, nspm->size);
    
    return 0;
}
```

### 3.4 块设备 I/O 处理

**函数**：`pmem_make_request()`  
**位置**：`drivers/nvdimm/pmem.c`

当用户运行 `dd if=/dev/pmem0` 时，内核会调用此函数：

```c
static blk_qc_t pmem_make_request(struct request_queue *q,
                                   struct bio *bio)
{
    struct pmem_device *pmem = q->queuedata;
    
    // 从 bio（I/O 请求）中获取：
    //   - 操作类型（READ/WRITE）
    //   - 起始偏移量
    //   - 数据长度
    
    if (bio_op(bio) == REQ_OP_READ) {
        // 读操作
        // 使用 pmem->phys_addr + offset 进行 memcpy
        pmem_do_read(pmem, bio);
    } else if (bio_op(bio) == REQ_OP_WRITE) {
        // 写操作
        // 使用 pmem->phys_addr + offset 进行 memcpy + cache flush
        pmem_do_write(pmem, bio);
    }
    
    // 标记 I/O 完成
    bio_endio(bio);
    
    return BLK_QC_T_NONE;
}
```

### 3.5 块设备创建后的 sysfs 结构

```
/sys/block/pmem0/
├── size                    → 容量（以 512 字节块为单位）
├── dev                     → 主从编号（259:0）
├── queue/
│   ├── logical_block_size  → 512
│   ├── physical_block_size → 4096
│   └── ...
└── ...

/dev/pmem0                 ← 实际块设备文件
```

---

## 第四部分：DAX 设备创建（/dev/daxX.Y）

### 4.1 核心问题

当创建 devdax 模式的 Namespace 后，`/dev/daxX.Y` 是如何被创建的？

### 4.2 DAX 架构概览

```
nd_region（持久内存范围）
  ↓
nd_namespace_pmem（用户虚拟分配）
  ↓ (mode=devdax)
nd_pfn（Page Frame Number 映射）
  ↓
nd_dax（DAX 设备）
  ↓
/dev/daxX.Y（字符设备）
```

### 4.3 DAX 驱动注册

**位置**：`drivers/dax/pmem/core.c` 和 `drivers/dax/device.c`

```c
// drivers/dax/pmem/core.c
static struct platform_driver dax_pmem_driver = {
    .probe = dax_pmem_probe,
    .remove = dax_pmem_remove,
    .driver = {
        .name = "dax_pmem",
    },
};
module_platform_driver(dax_pmem_driver);
```

### 4.4 DAX Probe 函数详解

**函数**：`dax_pmem_probe()`  
**位置**：`drivers/dax/pmem/core.c`

```c
// 行 ~150（简化版）
static int dax_pmem_probe(struct platform_device *pdev)
{
    struct nd_pfn_sb *pfn_sb;
    struct nd_namespace_pmem *nspm;
    struct dax_device_data *dax_data;
    struct dax_device *dax_dev;
    struct device *dev = &pdev->dev;
    phys_addr_t phys_addr;
    size_t size;
    
    // ===== 步骤 1：从 Namespace 获取 PFN 元数据 =====
    nspm = dev_get_drvdata(dev);
    pfn_sb = nspm->pfn_sb;  // PFN 超级块（元数据）
    
    // ===== 步骤 2：获取物理地址和大小 =====
    // 从 PFN 超级块中读取：
    // - pfn_sb->physical_offset：物理地址
    // - pfn_sb->dataoff：数据区偏移
    
    phys_addr = PFN_PHYS(le64_to_cpu(pfn_sb->physical_offset));
    size = nspm->size;
    
    // ===== 步骤 3：创建 DAX 设备数据结构 =====
    dax_data = kzalloc(sizeof(*dax_data), GFP_KERNEL);
    if (!dax_data)
        return -ENOMEM;
    
    dax_data->phys_addr = phys_addr;
    dax_data->size = size;
    dax_data->region = to_nd_region(nspm->common.dev.parent);
    
    // ===== 步骤 4：创建 DAX 设备 =====
    // 行 ~180
    dax_dev = devm_create_dax_dev(dev, dax_data);
    if (IS_ERR(dax_dev))
        return PTR_ERR(dax_dev);
    
    platform_set_drvdata(pdev, dax_dev);
    
    dev_info(dev, "Created DAX device %s, capacity %zu bytes\n",
             dev_name(dax_file_inode(dax_dev)), size);
    
    return 0;
}
```

### 4.5 DAX 字符设备创建

**函数**：`devm_create_dax_dev()`  
**位置**：`drivers/dax/device.c`

```c
// 行 ~400（简化版）
struct dax_device *devm_create_dax_dev(struct device *parent,
                                       struct dax_device_data *data)
{
    struct dax_device *dax_dev;
    struct inode *inode;
    struct device *dev;
    dev_t devt;
    int minor;
    
    // ===== 步骤 1：分配 DAX 设备编号 =====
    // DAX 使用主编号 251（预定义）
    // 从编号由 DAX 子系统自动分配
    minor = ida_alloc(&dax_minor_ida, GFP_KERNEL);
    if (minor < 0)
        return ERR_PTR(minor);
    
    devt = MKDEV(DAX_MAJOR, minor);  // DAX_MAJOR = 251
    
    // ===== 步骤 2：创建字符设备 =====
    dax_dev = kzalloc(sizeof(*dax_dev), GFP_KERNEL);
    if (!dax_dev)
        goto err_alloc;
    
    dev = &dax_dev->dev;
    dev_set_name(dev, "dax%d.%d", dax_region_id, minor);  // 例如 "dax0.0"
    dev->parent = parent;
    dev->devt = devt;
    dev->class = &dax_class;  // 使用 "dax" 字符设备类
    
    // ===== 步骤 3：创建字符设备 inode =====
    // DAX 使用特殊的 "DAX aware" inode
    inode = dax_inode_alloc();
    if (!inode)
        goto err_inode;
    
    dax_dev->inode = inode;
    inode->i_rdev = devt;
    
    // ===== 步骤 4：注册字符设备 =====
    // 行 ~450
    cdev_init(&dax_dev->cdev, &dax_fops);  // 设置 mmap/ioctl 处理
    cdev_add(&dax_dev->cdev, devt, 1);
    
    // ===== 步骤 5：向内核注册设备 =====
    device_register(dev);  // 创建 /sys/class/dax/daxX.Y/
    
    dev_info(dev, "Created DAX device, capacity %zu bytes\n", data->size);
    
    return dax_dev;

err_inode:
    kfree(dax_dev);
err_alloc:
    ida_free(&dax_minor_ida, minor);
    return ERR_PTR(-ENOMEM);
}
```

### 4.6 DAX 文件操作（mmap）

**文件操作结构**  
**位置**：`drivers/dax/device.c`

```c
static const struct file_operations dax_fops = {
    .llseek = dax_llseek,
    .read = dax_read,
    .write = dax_write,
    .mmap = dax_mmap,           // ← 关键：mmap 操作
    .unlocked_ioctl = dax_ioctl,
    // ...
};
```

**mmap 实现**：

```c
// 行 ~300
static int dax_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct inode *inode = file_inode(filp);
    struct dax_device *dax_dev = inode->i_dax;
    phys_addr_t phys_addr = dax_dev->phys_addr + vma->vm_pgoff * PAGE_SIZE;
    
    // 关键：直接映射 NVDIMM 的物理地址到用户空间
    // 这是 DAX 的精髓：无页缓存，直接映射到硬件地址
    
    return remap_pfn_range(vma,
                           vma->vm_start,
                           phys_addr >> PAGE_SHIFT,  // 物理页帧号
                           vma->vm_end - vma->vm_start,
                           vma->vm_page_prot);
}
```

### 4.7 DAX 设备创建后的 sysfs 结构

```
/sys/class/dax/dax0.0/
├── dev                    → 主从编号（251:0）
├── size                   → 容量
├── modalias               → dax:pmem
└── ...

/dev/dax0.0                ← 实际字符设备文件
```

---

## 第五部分：从 sysfs 配置到设备创建的完整调用链

### 5.1 用户运行 ndctl 的完整过程

```bash
ndctl create-namespace --region region0 --size 100G --mode devdax
```

**内核调用链**：

```
1. ndctl 写入 sysfs
   └─ echo "namespace_0_0" > /sys/bus/nd/devices/region0/create_namespace

2. sysfs 属性回调
   └─ drivers/nvdimm/region_devs.c:create_namespace_store()

3. 参数解析
   └─ 解析 size=100G, mode=devdax, ...

4. Namespace 对象创建
   └─ nvdimm_create_namespace_pmem()
      └─ create_namespace_pmem()
         └─ device_register()  ← 在 /sys/bus/nd/devices/ 中注册

5. Platform Device 创建（由 nd_bus 自动处理）
   └─ drivers/nvdimm/bus.c 中的总线驱动
      └─ 为 Namespace 创建 platform_device
         └─ 设置 drvdata 指向 nd_namespace_pmem

6. DAX 驱动 probe（总线匹配）
   └─ drivers/dax/pmem/core.c:dax_pmem_probe()
      └─ devm_create_dax_dev()
         └─ cdev_add() + device_register()  ← 在 /dev 中创建 /dev/dax0.0
```

### 5.2 查询创建的设备

```bash
# 查看 Namespace
ls /sys/bus/nd/devices/namespace0.0/
size  uuid  mode  daxdev  ...

# 查看 DAX 设备
ls -l /dev/dax*
crw------- root root 251, 0 /dev/dax0.0

# 查看块设备
ls /sys/block/pmem0/
size  dev  queue  ...

lsblk
pmem0    259:0    0  100G  0 disk
```

---

## 第六部分：关键代码位置速查表

| 功能 | 文件 | 函数 | 行号 |
|------|------|------|------|
| **NUMA 映射** | drivers/acpi/nfit/core.c | add_spa() | 698 |
| **NUMA 写入 Region** | drivers/nvdimm/region_devs.c | nd_region_create() | 1086 |
| **Namespace 创建** | drivers/nvdimm/namespace_devs.c | create_namespace_pmem() | ~300 |
| **块设备驱动** | drivers/nvdimm/pmem.c | pmem_probe() | ~400 |
| **块设备 I/O** | drivers/nvdimm/pmem.c | pmem_make_request() | ~200 |
| **DAX 驱动** | drivers/dax/pmem/core.c | dax_pmem_probe() | ~150 |
| **DAX mmap** | drivers/dax/device.c | dax_mmap() | ~300 |
| **DAX 字符设备** | drivers/dax/device.c | devm_create_dax_dev() | ~400 |

---

## 总结

### 完整的信息流

```
ACPI NFIT 表
  ↓ proximity_domain
  
NUMA 节点 (node 1)
  ├─ /sys/bus/nd/devices/region0/numa_node
  ├─ /proc/meminfo (可见 NUMA 内存)
  └─ numactl --show (显示 NUMA 拓扑)
  
  ↓ ndctl create-namespace
  
Namespace (namespace0.0)
  ├─ /sys/bus/nd/devices/namespace0.0/size
  ├─ /sys/bus/nd/devices/namespace0.0/mode
  └─ /sys/bus/nd/devices/namespace0.0/daxdev
  
  ↓ 驱动匹配 & probe
  
  ├─ 块设备驱动 (mode=fsdax/raw)
  │  └─ /dev/pmemX
  │
  └─ DAX 驱动 (mode=devdax)
     └─ /dev/daxX.Y
     
  ↓ 用户程序
  
用户访问持久内存
```

### 关键设计原则

1. **分层抽象**：Region → Namespace → 块设备/DAX 设备
2. **驱动动态绑定**：Namespace 创建后，通过 platform_device 机制自动匹配驱动
3. **NUMA 感知**：从固件 proximity_domain 一路传递到内核 NUMA 子系统
4. **灵活配置**：同一个 Region 可以创建多个 Namespace，每个独立配置

---

**文档版本**：Linux 6.6 kernel  
**范围**：从 Region 到用户设备的完整内核源码分析  
**补充**：USERLAND_OPTANE_USAGE_GUIDE.md 的源码层面分析
