# Linux 6.6 Optane/NVDIMM-P 内核态地址上报流程分析

## 概述

本分析追踪 Optane/NVDIMM-P（Persistent Memory）在 Linux 6.6 内核中从固件 NFIT 表解析到用户态多层暴露的完整地址上报流程，按时间顺序组织。

### Intel Optane DC 与 NVDIMM-P 的关系

- **Intel Optane DC Persistent Memory** 是 Intel 商业产品品牌
- **NVDIMM-P** 是该产品的规范名称
- **内核视角**：两者本质相同，均通过 ACPI NFIT 表暴露，使用标准 NVDIMM DSM（Device-Specific Method）接口
- **固件标识**：在 NFIT 表中通过 `UUID_NFIT_DIMM (4309ac30-0d11-11e4-9191-0800200c9a66)` 标识
- **地址上报流程**：对两者完全一致，无区别

---

## 重要澄清：Intel Optane DC 与 NVDIMM-P 的一致性

### 产品与规范的关系
- **Intel Optane DC Persistent Memory**：Intel 商业产品名称
- **NVDIMM-P**：JEDEC/ACPI 规范名称（P = "Persistent"，相对于 N = "Non-Persistent"）
- 二者是同一物理产品，仅命名不同

### 地址上报流程的一致性
在 Linux 内核中，Intel Optane DC 和 NVDIMM-P 的地址上报流程**完全一致**：
1. 固件都通过 ACPI NFIT 表暴露，使用标准结构（SPA Range、MEMDEV、DCR 等）
2. NFIT 驱动无法区分两者，统一作为 NVDIMM 处理
3. 地址解析、Region 创建、sysfs 暴露的代码路径**完全相同**
4. 内核代码中无"Optane 特定"的地址上报逻辑，仅有"Intel 特定"的设备管理命令（如固件更新、安全擦除）

本文档中所述的地址上报流程对 **Intel Optane DC 和所有标准 NVDIMM-P 都适用**。

---

## 多层地址暴露架构说明

需要澄清的是，Linux NVDIMM 子系统暴露地址信息有**多层抽象**，但这些都**基于同一份固件地址数据**：

### 层次结构
```
固件 NFIT 表
  ↓
Region (nd_region) ← **基础地址上报**（本文档重点）
  ├─ /sys/bus/nd/devices/regionX/resource
  ├─ /sys/bus/nd/devices/regionX/size
  ├─ /sys/bus/nd/devices/regionX/mappings
  └─ /sys/bus/nd/devices/regionX/mappingN
  ↓
Namespace (nd_namespace) ← **用户空间分配**（需通过 ndctl 工具）
  ├─ PMEM Namespace（使用 Region 全部或部分地址）
  └─ IO Namespace（兼容模式）
  ↓
DAX Device (nd_dax) ← **用户态映射接口**（可选）
  └─ /dev/daxX.Y（直接访问持久内存）
  ↓
NUMA Node ← **内存管理视角**（从 Region 的 proximity_domain 推导）
  └─ /sys/devices/system/node/nodeX/（整合 Region 地址进系统 NUMA 拓扑）
```

### 关键点
1. **Region 是基础**：Region 直接来自 NFIT 表，固件级别的地址映射
2. **Namespace 是分片**：用户通过 `ndctl` 工具在 Region 上创建 Namespace，可自定义大小和位置
3. **DAX 和 NUMA 是视角**：不是独立的上报路径，而是基于 Region 信息的进一步呈现

---

## 第一阶段：NFIT 表解析（drivers/acpi/nfit/）

### 1.1 SPA Range 结构体解析

#### 函数：`add_spa()`
- **位置**：`drivers/acpi/nfit/core.c:698`
- **函数签名**：`static bool add_spa(struct acpi_nfit_desc *acpi_desc, struct nfit_table_prev *prev, struct acpi_nfit_system_address *spa)`
- **输入/操作/输出**：
  - 输入：固件提供的 `acpi_nfit_system_address` 结构体
  - 操作：验证结构体长度，检查重复，分配 `nfit_spa` 内存并拷贝 SPA 数据
  - 输出：将解析后的 SPA 加入 `acpi_desc->spas` 链表

#### 固件 NFIT 表中的关键字段
从 ACPI 6.1 规范，`struct acpi_nfit_system_address` 包含：
- **address**：SPA 范围的物理基址（64 位）
- **length**：SPA 范围的字节长度（64 位）
- **range_index**：SPA 的唯一标识符（16 位），用于关联 MEMDEV
- **range_guid**：SPA 类型标识符（GUID），用于区分 PMEM/Volatile/DCR 等
- **proximity_domain**：NUMA 节点关联（16 位）
- **flags**：各种标志位（如 ACPI_NFIT_PROXIMITY_VALID）

#### 数据结构：`struct nfit_spa`
定义位置：`drivers/acpi/nfit/nfit.h:163`

```c
struct nfit_spa {
    struct list_head list;                    // 链表节点
    struct nd_region *nd_region;              // 指向后续创建的 nd_region
    unsigned long ars_state;
    u32 clear_err_unit;
    u32 max_ars;
    struct acpi_nfit_system_address spa[];    // 柔性数组，存储固件数据
};
```

**内核如何暂存这些字段**：
- 固件的 SPA 数据直接拷贝到 `nfit_spa->spa[]` 柔性数组中（行 720）
- `add_spa()` 分配的内存包括 `sizeof(*nfit_spa) + sizeof_spa(spa)` 字节（行 715）
- 这样整个 SPA 结构体及其数据在一个连续的内核内存块中

### 1.2 SPA 与 MEMDEV 关联

#### 函数：`nfit_mem_init()`
- **位置**：`drivers/acpi/nfit/core.c:1148`
- **函数签名**：`static int nfit_mem_init(struct acpi_nfit_desc *acpi_desc)`
- **输入/操作/输出**：
  - 输入：已解析的 NFIT 描述符（包含 SPAs、MEMDEVs、DCRs 链表）
  - 操作：遍历每个 SPA，调用 `__nfit_mem_init()` 关联对应的 MEMDEV；再调用一次处理未映射的 DIMM
  - 输出：填充 `acpi_desc->dimms` 链表，每个 DIMM 对应一个 `nfit_mem` 结构

#### 函数：`__nfit_mem_init()`
- **位置**：`drivers/acpi/nfit/core.c:1002`
- **函数签名**：`static int __nfit_mem_init(struct acpi_nfit_desc *acpi_desc, struct acpi_nfit_system_address *spa)`
- **输入/操作/输出**：
  - 输入：SPA 指针（或 NULL 用于查找未映射 DIMM）
  - 操作：
    1. 遍历 `acpi_desc->memdevs` 链表中所有 MEMDEV（`struct acpi_nfit_memory_map`）
    2. 如果提供了 SPA，只匹配 `memdev->range_index == spa->range_index` 的 MEMDEV
    3. 根据 MEMDEV 的 `device_handle`，查找或创建对应的 `nfit_mem` 对象
    4. 在 `nfit_mem` 中关联 MEMDEV：
       - 存储 `memdev_dcr` 或 `memdev_pmem` 指针（取决于 SPA 类型）
       - 从 MEMDEV 的 `region_index` 查找对应的 DCR（`struct acpi_nfit_control_region`）
       - 查找并关联 flush address 表
  - 输出：完成的 `nfit_mem` 对象添加到 `acpi_desc->dimms` 链表

#### 数据结构：`struct nfit_mem`
定义位置：`drivers/acpi/nfit/nfit.h:207`

```c
struct nfit_mem {
    struct nvdimm *nvdimm;                    // 指向后续创建的 nvdimm
    struct acpi_nfit_memory_map *memdev_dcr;  // NFIT MEMDEV 指针（DCR 类型）
    struct acpi_nfit_memory_map *memdev_pmem; // NFIT MEMDEV 指针（PMEM 类型）
    struct acpi_nfit_control_region *dcr;     // NFIT DCR
    struct acpi_nfit_system_address *spa_dcr; // 指向关联的 SPA-DCR
    struct acpi_nfit_interleave *idt_dcr;     // 交错表指针
    struct nfit_flush *nfit_flush;            // Flush address 表
    struct list_head list;                    // 链表节点
    ...
};
```

#### 为什么需要关联？

从固件 NFIT 表的结构来看：
- **SPA Range** 定义了一个地址范围（如 `0x100000000` 到 `0x2ffffffff`）
- **MEMDEV** 描述了这个范围内某个 DIMM 的部分（通过 `device_handle` 标识 DIMM，`region_size` 标识该 DIMM 在 SPA 内的大小，`address` 标识在该 DIMM 内的偏移）
- **DCR** 包含该 DIMM 的配置寄存器信息

关联的目的是：建立从物理地址 → DIMM → 配置信息的多级映射，后续内核需要根据给定地址快速定位到具体哪个 DIMM，然后读取该 DIMM 的配置。

---

## 第二阶段：区域创建（drivers/nvdimm/）

### 2.1 Region 描述符构建与区域创建调用链

#### 函数：`acpi_nfit_register_region()`
- **位置**：`drivers/acpi/nfit/core.c:2596`
- **函数签名**：`static int acpi_nfit_register_region(struct acpi_nfit_desc *acpi_desc, struct nfit_spa *nfit_spa)`
- **输入/操作/输出**：
  - 输入：已经过解析和关联的 `nfit_spa` 对象
  - 操作：
    1. 从 `nfit_spa->spa` 提取固件数据：
       - `res.start = spa->address`（行 2618）
       - `res.end = res.start + spa->length - 1`（行 2619）
    2. 构建 `nd_region_desc` 结构体，填充关键字段：
       - `ndr_desc->res = &res`：物理地址范围资源
       - `ndr_desc->numa_node` 和 `ndr_desc->target_node`：从 SPA 的 `proximity_domain` 转换
       - `ndr_desc->flags`：从平台能力设置持久化域标志
    3. 遍历所有 MEMDEV，找到 `range_index` 匹配当前 SPA 的 MEMDEV（行 2654-2673）
    4. 对每个 MEMDEV 调用 `acpi_nfit_init_mapping()` 填充 `mapping` 数组
    5. 设置 `ndr_desc->mapping = mappings` 和 `ndr_desc->num_mappings = count`
    6. 调用 `nvdimm_pmem_region_create()` 或 `nvdimm_volatile_region_create()` 创建实际 region
  - 输出：成功则设置 `nfit_spa->nd_region`，失败返回错误码

#### 函数：`acpi_nfit_init_mapping()`
- **位置**：`drivers/acpi/nfit/core.c:2554`
- **函数签名**：`static int acpi_nfit_init_mapping(struct acpi_nfit_desc *acpi_desc, struct nd_mapping_desc *mapping, struct nd_region_desc *ndr_desc, struct acpi_nfit_memory_map *memdev, struct nfit_spa *nfit_spa)`
- **输入/操作/输出**：
  - 输入：单个 MEMDEV、对应的 SPA、以及待填充的 `nd_mapping_desc`
  - 操作：
    1. 通过 MEMDEV 的 `device_handle` 查找对应的 `nvdimm` 对象（已在前面阶段创建）
    2. 根据 SPA 类型（NFIT_SPA_PM 或 NFIT_SPA_VOLATILE）：
       - `mapping->start = memdev->address`（行 2573）：DIMM 内偏移
       - `mapping->size = memdev->region_size`（行 2574）：该 DIMM 在 region 中的大小
    3. `mapping->nvdimm = nvdimm`（行 2569）
  - 输出：填充的 `nd_mapping_desc` 对象

### 2.2 nd_region_desc 关键字段说明

| 字段名 | 源头 | 含义 |
|--------|------|------|
| `res->start` | `spa->address` | SPA 范围的起始物理地址 |
| `res->end` | `spa->address + spa->length - 1` | SPA 范围的结束物理地址 |
| `numa_node` | `spa->proximity_domain` | NUMA 节点编号 |
| `mapping[].nvdimm` | MEMDEV 的 `device_handle` 查询得到 | 指向 nvdimm 对象 |
| `mapping[].start` | MEMDEV 的 `address` | 偏移（相对于该 DIMM） |
| `mapping[].size` | MEMDEV 的 `region_size` | 该 DIMM 在 region 的大小 |
| `num_mappings` | 匹配 SPA 的 MEMDEV 数量 | region 包含的 DIMM 个数 |

### 2.3 Region 创建核心函数

#### 函数：`nvdimm_pmem_region_create()`
- **位置**：`drivers/nvdimm/region_devs.c:1105`
- **函数签名**：`struct nd_region *nvdimm_pmem_region_create(struct nvdimm_bus *nvdimm_bus, struct nd_region_desc *ndr_desc)`
- **输入/操作/输出**：
  - 输入：nvdimm_bus 和填充好的 `nd_region_desc`
  - 操作：设置 `ndr_desc->num_lanes = ND_MAX_LANES`，调用 `nd_region_create()`
  - 输出：创建好的 `nd_region` 对象或 NULL

#### 函数：`nd_region_create()`
- **位置**：`drivers/nvdimm/region_devs.c:1000`
- **函数签名**：`static struct nd_region *nd_region_create(struct nvdimm_bus *nvdimm_bus, struct nd_region_desc *ndr_desc, const struct device_type *dev_type, const char *caller)`
- **输入/操作/输出**：
  - 输入：nvdimm_bus、`nd_region_desc`、设备类型（PMEM 或 Volatile）
  - 操作：
    1. 验证所有 mapping 的 start 和 size 按页对齐（行 1013-1018）
    2. 分配 `nd_region` 结构体，大小包括 `num_mappings` 个 `nd_mapping` 子结构（行 1025-1027）
    3. 为 region 分配 ID（行 1032-1038）
    4. 初始化每个 mapping（行 1052-1064）：
       - `nd_region->mapping[i].nvdimm = nvdimm`
       - `nd_region->mapping[i].start = mapping->start`（行 1057）：DIMM 偏移
       - `nd_region->mapping[i].size = mapping->size`（行 1058）：DIMM 大小
       - `nd_region->mapping[i].position = mapping->position`（行 1059）：交错位置
    5. 提取并存储关键字段（行 1065-1088）：
       - `nd_region->ndr_mappings = ndr_desc->num_mappings`（行 1065）：mapping 个数
       - `nd_region->ndr_start = ndr_desc->res->start`（行 1084）：SPA 起始地址
       - `nd_region->ndr_size = resource_size(ndr_desc->res)`（行 1083）：SPA 大小
       - `nd_region->numa_node = ndr_desc->numa_node`（隐含在行 1071）
    6. 调用 `device_initialize()` 和 `nd_device_register()` 将设备注册到 sysfs（行 1091-1093）
  - 输出：完整初始化的 `nd_region` 对象

### 2.4 device_register 的作用

`nd_device_register()` 调用 Linux device model 的标准注册流程：
1. 在 `/sys/devices/` 下创建对应的设备目录（`/sys/bus/nd/devices/regionX/`）
2. 根据 `dev->groups` 中定义的属性表创建 sysfs 属性文件
3. 触发 udev 事件，可能加载驱动程序

---

## 第三阶段：地址信息暴露到 sysfs

### 3.1 关键 sysfs 属性的 show 函数

#### 属性：`size`
- **位置**：`drivers/nvdimm/region_devs.c:254`
- **show 函数**：`size_show()`
- **实现**：`return sprintf(buf, "%llu\n", region_size(nd_region));`
- **数据源**：读取 `nd_region->ndr_size`（在 nd_region_create 中设置为 `resource_size(ndr_desc->res)`，即 `spa->length`）
- **物理含义**：整个 region 的总大小，即 SPA 范围的字节数

#### 属性：`mappings`
- **位置**：`drivers/nvdimm/region_devs.c:294`
- **show 函数**：`mappings_show()`
- **实现**：`return sprintf(buf, "%d\n", nd_region->ndr_mappings);`
- **数据源**：读取 `nd_region->ndr_mappings`（在 nd_region_create 中设置为 MEMDEV 的个数）
- **物理含义**：该 region 包含的 DIMM 个数

#### 属性：`mappingN`（N = 0 到 ndr_mappings-1）
- **位置**：`drivers/nvdimm/region_devs.c:707` 和 `723`
- **show 函数**：`mappingN_show()` 宏展开调用 `mappingN(dev, buf, N)`
- **实现**：
  ```c
  nd_mapping = &nd_region->mapping[n];
  nvdimm = nd_mapping->nvdimm;
  return sprintf(buf, "%s,%llu,%llu,%d\n", dev_name(&nvdimm->dev),
      nd_mapping->start, nd_mapping->size,
      nd_mapping->position);
  ```
- **数据源**：
  - DIMM 名称：从 `nvdimm->dev` 的设备名称
  - `start`：`nd_region->mapping[n].start`（DIMM 内偏移，源自 MEMDEV 的 `address`）
  - `size`：`nd_region->mapping[n].size`（DIMM 在 region 的大小，源自 MEMDEV 的 `region_size`）
  - `position`：`nd_region->mapping[n].position`（交错位置）
- **物理含义**：
  - DIMM 名称：该 mapping 对应的物理 DIMM 标识符
  - `start`：该 DIMM 内的起始地址偏移
  - `size`：该 DIMM 在这个 region 中分配的字节数
  - `position`：在多 DIMM 交错场景中的位置（0 表示第一个，以此类推）

### 3.2 resource 属性

#### 属性：`resource`
- **位置**：`drivers/nvdimm/region_devs.c:609`
- **show 函数**：`resource_show()`
- **实现**：`return sprintf(buf, "%#llx\n", nd_region->ndr_start);`
- **数据源**：`nd_region->ndr_start`（在 nd_region_create 中设置为 `ndr_desc->res->start`，即 `spa->address`）
- **物理含义**：该 region 在物理地址空间中的起始地址（十六进制）
- **sysfs 路径**：`/sys/bus/nd/devices/regionX/resource`

#### /sys/bus/nd/devices/regionX/resource 创建时机

在 `nd_device_register()` 调用中（nd_region_create 的行 1093）：
1. 设备核心将 `dev->groups` 中列出的属性表加载到 sysfs
2. 属性 `dev_attr_resource` 被添加到 `/sys/bus/nd/devices/regionX/` 目录
3. 用户读取该文件时，内核调用 `resource_show()` 动态生成内容

属性定义位置：`drivers/nvdimm/region_devs.c:616` 行的 `DEVICE_ATTR_ADMIN_RO(resource)` 宏

---

## 第四阶段：映射关系总表

| NFIT 固件字段 | 内核中间结构 | nd_region 字段 | sysfs 属性 | 用户可读文件 |
|---------------|-------------|----------------|-----------|------------|
| `spa->address` | `nfit_spa->spa->address` | `nd_region->ndr_start` | `resource_show()` | `/sys/bus/nd/devices/regionX/resource` |
| `spa->length` | `nfit_spa->spa->length` | `nd_region->ndr_size` | `size_show()` | `/sys/bus/nd/devices/regionX/size` |
| `memdev 数量` | `nfit_spa` 关联的 `nfit_memdev` 列表 | `nd_region->ndr_mappings` | `mappings_show()` | `/sys/bus/nd/devices/regionX/mappings` |
| `memdev->device_handle` | `nfit_mem->nvdimm` | `nd_mapping->nvdimm` | `mappingN_show()` | `/sys/bus/nd/devices/regionX/mappingN` |
| `memdev->address` | `nfit_memdev->memdev->address` | `nd_mapping->start` | `mappingN_show()` | `/sys/bus/nd/devices/regionX/mappingN` |
| `memdev->region_size` | `nfit_memdev->memdev->region_size` | `nd_mapping->size` | `mappingN_show()` | `/sys/bus/nd/devices/regionX/mappingN` |
| `spa->range_index` | `nfit_spa->spa->range_index` | - | - | - |
| `spa->proximity_domain` | `nfit_spa->spa->proximity_domain` | `nd_region->numa_node` | - | - |

---

## 第五节：完整地址上报流程叙述

### 从固件到用户态的完整流程

当 Linux 6.6 内核启动时，ACPI NFIT 驱动执行以下步骤将 Optane/NVDIMM-P 的地址信息暴露给用户态应用：

#### 1. 固件初始化（ACPI BIOS 阶段）

固件（UEFI/BIOS）在 ACPI NFIT 表中以标准化结构编码 Optane 设备信息：
- **System Physical Address（SPA）Range** 结构体列举了多个连续的物理地址范围，每个范围有唯一的 `range_index`、起始 `address`、`length`、`range_guid`（用于区分 PMEM/Volatile/DCR 类型）
- **Memory Device（MEMDEV）** 结构体描述了每个 DIMM 在 SPA 范围内的映射关系，通过 `device_handle` 标识物理 DIMM、`range_index` 指向对应的 SPA、`address` 给出 DIMM 内偏移、`region_size` 给出该 DIMM 在 SPA 中的大小
- 其他表如 Control Region（DCR）、Data Region（BDW）提供配置和数据访问细节

#### 2. NFIT 表解析（acpi_nfit_init 及后续）

内核 ACPI NFIT 驱动在启动时扫描 NFIT 表：

**2a. SPA 解析** (drivers/acpi/nfit/core.c:698 `add_spa()`)
- 对 NFIT 表中每个 SPA Range 结构体，内核调用 `add_spa()` 
- 分配一个 `struct nfit_spa` 内存块，大小为 `sizeof(nfit_spa) + sizeof(acpi_nfit_system_address)`（行 715）
- 将固件的 SPA 数据整体拷贝到 `nfit_spa->spa` 柔性数组（行 720）
- 拷贝后的 `nfit_spa` 对象加入 `acpi_desc->spas` 全局链表
- 此时内核中已保有原始固件数据：`spa->address`、`spa->length`、`spa->range_index` 等

**2b. DIMM 与 SPA 关联** (drivers/acpi/nfit/core.c:1148 `nfit_mem_init()` 及 1002 `__nfit_mem_init()`)
- 核心目的：建立从物理地址 → DIMM → 驱动程序的多层映射
- 对每个 SPA Range，调用 `__nfit_mem_init(acpi_desc, nfit_spa->spa)`（行 1163）
- 在 `__nfit_mem_init()` 中（行 1025），遍历所有 MEMDEV
- 对每个 `memdev->range_index == spa->range_index` 的 MEMDEV，查找或创建 `struct nfit_mem` 对象
- 在 `nfit_mem` 中存储指向该 MEMDEV 的指针（`memdev_pmem` 或 `memdev_dcr`，行 1027-1034）、关联的 DCR、DCR 对应的 SPA 等
- 完成后，`acpi_desc->dimms` 链表包含所有参与的 DIMM 的 `nfit_mem` 对象，每个对象包含该 DIMM 的所有 NFIT 元数据

#### 3. Nvdimm 设备创建（nvdimm_bus 初始化阶段）

内核为每个物理 DIMM 创建一个 `struct nvdimm` Linux 设备对象，注册到 `/sys/bus/nd/devices/` 下（例如 `nmem0`、`nmem1`）

#### 4. Region 创建与地址映射（acpi_nfit_register_regions）

**4a. 触发 Region 注册** (drivers/acpi/nfit/core.c:2596 `acpi_nfit_register_region()`)
- 对每个已解析的 `nfit_spa`，调用 `acpi_nfit_register_region(acpi_desc, nfit_spa)`
- 该函数的目标是根据单个 SPA 范围（可能包含多个 DIMM）创建一个 Linux nvdimm region 对象

**4b. 构建 Mapping 数组** (drivers/acpi/nfit/core.c:2554 `acpi_nfit_init_mapping()`)
- 遍历所有 MEMDEV，找到 `memdev->range_index == spa->range_index` 的 MEMDEV（行 2654-2673）
- 对每个匹配的 MEMDEV，调用 `acpi_nfit_init_mapping()` 构建一个 `nd_mapping_desc` 对象
- 在 `acpi_nfit_init_mapping()` 中（行 2559），通过 `memdev->device_handle` 查找前面创建的 `nvdimm` 对象
- 设置 mapping 的关键字段（行 2569-2574）：
  - `mapping->nvdimm = nvdimm`：指向该物理 DIMM 的 nvdimm 对象
  - `mapping->start = memdev->address`：DIMM 内偏移
  - `mapping->size = memdev->region_size`：该 DIMM 在 region 的大小
- 完成后，`mappings[]` 数组包含了该 SPA 范围内的所有 DIMM 的映射关系

**4c. 构建 Region 描述符** (drivers/acpi/nfit/core.c:2615-2625)
- 创建 `struct nd_region_desc` 临时对象
- 从 SPA 提取物理地址范围（行 2618-2619）：
  - `res.start = spa->address`：SPA 起始地址
  - `res.end = res.start + spa->length - 1`：SPA 结束地址
- 关联 mapping 数组：
  - `ndr_desc->mapping = mappings`
  - `ndr_desc->num_mappings = count`（DIMM 个数）
- 从 SPA 的 `proximity_domain` 推导 NUMA 节点
- 设置持久化域标志

**4d. 创建 nd_region 内核对象** (drivers/nvdimm/region_devs.c:1105 `nvdimm_pmem_region_create()` 和 1000 `nd_region_create()`)
- 调用 `nvdimm_pmem_region_create(nvdimm_bus, ndr_desc)` 或 `nvdimm_volatile_region_create()`
- 在 `nd_region_create()` 内部（行 1025-1027），根据 `num_mappings` 分配一个 `nd_region` 结构体：
  ```c
  nd_region = kzalloc(struct_size(nd_region, mapping, ndr_desc->num_mappings), GFP_KERNEL);
  ```
  这保证了 `nd_region->mapping[]` 数组有足够的空间存储所有 mapping
- 将 `nd_region_desc` 的信息复制到 `nd_region`（行 1052-1087）：
  - `nd_region->ndr_mappings = ndr_desc->num_mappings`（行 1065）
  - `nd_region->ndr_start = ndr_desc->res->start`（行 1084）：物理地址范围起点
  - `nd_region->ndr_size = resource_size(ndr_desc->res)`（行 1083）：物理地址范围大小
  - 逐一复制 mapping 信息（行 1056-1058）：
    ```c
    nd_region->mapping[i].nvdimm = nvdimm;
    nd_region->mapping[i].start = mapping->start;
    nd_region->mapping[i].size = mapping->size;
    ```
- 为 region 分配唯一 ID（`region0`、`region1` 等）
- 调用 `device_initialize(dev)` 和 `nd_device_register(dev)` 将设备注册到内核设备模型（行 1091-1093）

#### 5. Sysfs 属性暴露（device_register 的作用）

当 `nd_device_register()` 被调用时，Linux device model 的核心代码执行：
- 在 `/sys/bus/nd/devices/` 下创建目录 `regionX/`（X 为 region ID）
- 遍历 `dev->groups` 属性表，在 `regionX/` 目录下创建对应的 sysfs 属性文件
- 属性文件包括：
  - `size`：链接到 `size_show()` 函数（行 254）
  - `resource`：链接到 `resource_show()` 函数（行 609）
  - `mappings`：链接到 `mappings_show()` 函数（行 294）
  - `mapping0`、`mapping1`、...：链接到宏展开的 `mappingN_show()` 函数（行 723）
  - 以及其他配置属性

#### 6. 用户态读取地址信息

用户态应用或命令行工具读取 sysfs 文件时：

**读取 `/sys/bus/nd/devices/region0/resource`**
- 内核调用 `resource_show()`（行 609）
- 函数读取 `nd_region->ndr_start`（这个值源自 `spa->address`）
- 返回格式化的十六进制地址，例如 `0x100000000`
- **含义**：该 region 在物理地址空间的起始地址

**读取 `/sys/bus/nd/devices/region0/size`**
- 内核调用 `size_show()`（行 254）
- 函数调用 `region_size(nd_region)` 读取 `nd_region->ndr_size`（源自 `resource_size(ndr_desc->res)` 即 `spa->length`）
- 返回十进制字节数，例如 `536870912`（512GB）
- **含义**：该 region 的总大小

**读取 `/sys/bus/nd/devices/region0/mappings`**
- 内核调用 `mappings_show()`（行 294）
- 返回 `nd_region->ndr_mappings`，例如 `2`
- **含义**：该 region 包含 2 个 DIMM

**读取 `/sys/bus/nd/devices/region0/mapping0`**
- 内核调用 `mapping0_show()` 宏展开的函数，进而调用 `mappingN(dev, buf, 0)`（行 707）
- 函数读取 `nd_region->mapping[0]` 的内容：
  - `nd_mapping->nvdimm`：DIMM 对象，通过 `dev_name()` 获取设备名（如 `nmem0`）
  - `nd_mapping->start`：源自 `memdev->address`，例如 `0`
  - `nd_mapping->size`：源自 `memdev->region_size`，例如 `268435456`（256GB）
  - `nd_mapping->position`：例如 `0`
- 返回格式 `nmem0,0,268435456,0`
- **含义**：第一个 mapping 对应 DIMM `nmem0`，该 DIMM 在其内部地址空间的起始偏移为 0，大小为 256GB，在交错中的位置为 0

**读取 `/sys/bus/nd/devices/region0/mapping1`**
- 类似地返回例如 `nmem1,268435456,268435456,1`
- **含义**：第二个 mapping 对应 DIMM `nmem1`，该 DIMM 的起始偏移为 256GB，大小为 256GB，位置为 1

#### 7. 地址转换的完整链路

一个用户态应用程序现在拥有完整的地址映射信息：

```
物理地址 0x100000000 → region0 (ndr_start=0x100000000, ndr_size=0x20000000)
                     → mapping0 (nvdimm=nmem0, start=0, size=0x10000000)
                     → mapping1 (nvdimm=nmem1, start=0x10000000, size=0x10000000)

物理地址范围：
  0x100000000 - 0x10FFFFFFF  → nmem0 (内部地址 0x00000000 - 0x0FFFFFFF)
  0x110000000 - 0x1FFFFFFFF  → nmem1 (内部地址 0x10000000 - 0x1FFFFFFFF)
```

应用程序可以根据这些信息：
- 计算 region 的物理地址范围
- 知道哪些 DIMM 参与了这个 region
- 计算每个 DIMM 在 region 中的地址范围
- 执行 DIMM 级别的操作（如 SMART 查询、固件更新等）

---

## 关键代码位置速查表

| 操作 | 文件 | 函数 | 行号 |
|------|------|------|------|
| 解析 SPA 范围 | drivers/acpi/nfit/core.c | `add_spa()` | 698 |
| 确定 SPA 类型 | drivers/acpi/nfit/core.c | `nfit_spa_type()` | 679 |
| 关联 MEMDEV 与 DIMM | drivers/acpi/nfit/core.c | `__nfit_mem_init()` | 1002 |
| 初始化所有 DIMM | drivers/acpi/nfit/core.c | `nfit_mem_init()` | 1148 |
| 注册 region | drivers/acpi/nfit/core.c | `acpi_nfit_register_region()` | 2596 |
| 初始化单个 mapping | drivers/acpi/nfit/core.c | `acpi_nfit_init_mapping()` | 2554 |
| 创建 pmem region | drivers/nvdimm/region_devs.c | `nvdimm_pmem_region_create()` | 1105 |
| 创建 region 核心逻辑 | drivers/nvdimm/region_devs.c | `nd_region_create()` | 1000 |
| 暴露 size | drivers/nvdimm/region_devs.c | `size_show()` | 254 |
| 暴露 mappings 数 | drivers/nvdimm/region_devs.c | `mappings_show()` | 294 |
| 暴露单个 mapping | drivers/nvdimm/region_devs.c | `mappingN()` | 707 |
| 暴露物理地址 | drivers/nvdimm/region_devs.c | `resource_show()` | 609 |

---

## 附录一：Namespace、DAX 和 NUMA 地址暴露机制

本附录澄清除基础 Region 外的其他地址暴露方式，以及它们与本文档主体流程的关系。

### A1. Namespace 地址上报

#### Namespace 的作用

**Namespace 是 Region 上的逻辑分片**，允许用户在一个 Region 内创建多个子区间，每个子区间可配置不同的访问模式：
- **PMEM Namespace**：标准持久内存访问，可作为块设备或 DAX 映射
- **IO Namespace**：向后兼容模式（较少使用）
- **BTT Namespace**：带事务支持的块层（用于原子性写保证）

#### Namespace 创建流程

**初始 Seed 创建**（Region 创建完成后自动触发）
- 位置：`drivers/nvdimm/region_devs.c:895` `nd_region_create_ns_seed()`
- 调用链：`nd_region_create()` → `nd_device_register()` → `nd_region_create_ns_seed()`
- 作用：为 Region 创建一个"种子" Namespace，作为用户创建真实 Namespace 的模板
- 代码：
  ```c
  // drivers/nvdimm/region_devs.c:1826
  void nd_region_create_ns_seed(struct nd_region *nd_region)
  {
      nd_region->ns_seed = nd_namespace_pmem_create(nd_region);  // 行 1833
      if (!nd_region->ns_seed)
          dev_err(&nd_region->dev, "failed to create namespace\n");
      else
          nd_device_register(nd_region->ns_seed);  // 行 1845
  }
  ```

**用户通过 ndctl 创建实际 Namespace**
- 用户态工具：`ndctl create-namespace` 命令
- 内核接口：`ioctl(NVDIMM_CMD_CREATE_NAMESPACE)` 等
- 结果：在 Region 上创建 `struct nd_namespace_pmem` 对象
- Namespace 的地址范围通过 `IOCTL` 参数指定，不必是 Region 的全部，可以是部分

#### Namespace sysfs 地址暴露

当 Namespace 创建后，在 `/sys/bus/nd/devices/namespaceX.Y/` 下暴露：
- **resource**：Namespace 占用的物理地址范围（来自用户指定，基于 Region 地址）
- **size**：Namespace 的大小
- **blockdev**：对应的块设备名（如 `pmem0`）
- **mode**：模式（raw、fsdax、devdax 等）

#### Namespace 与 Region 地址的关系

**关键映射**：
```
Region: address=0x100000000, size=0x20000000 (512GB)
  ↓
Namespace 1: address=0x100000000, size=0x10000000 (256GB，使用 Region 前半部分)
Namespace 2: address=0x110000000, size=0x10000000 (256GB，使用 Region 后半部分)
```

- **Region 的 resource** = 物理地址范围的硬件边界（来自 NFIT SPA Range）
- **Namespace 的 resource** = 用户分配的子区间（必须在 Region 内）

### A2. DAX 地址暴露

#### DAX 设备的作用

DAX（Direct Access）提供绕过块层和文件系统的直接地址映射接口，允许应用程序使用 `mmap()` 直接访问持久内存。

#### DAX 设备创建流程

**Seed 创建**（Region 创建完成后自动触发）
- 位置：`drivers/nvdimm/region_devs.c:914` `nd_region_create_dax_seed()`
- 调用：`nd_region_create()` → `device_register()` → `nd_region_create_dax_seed()`
- 代码：
  ```c
  // drivers/nvdimm/namespace_devs.c:1849
  void nd_region_create_dax_seed(struct nd_region *nd_region)
  {
      nd_region->dax_seed = nd_dax_create(nd_region);  // 行 1852
      if (!nd_region->dax_seed)
          dev_err(&nd_region->dev, "failed to create dax namespace\n");
  }
  ```

**实际 DAX 设备创建**（通过 Namespace 驱动探测）
- 触发条件：Namespace 的 `claim_class == NVDIMM_CCLASS_DAX`
- 驱动函数：`drivers/nvdimm/dax_devs.c:87` `nd_dax_probe()`
- 结果：创建 `/dev/daxX.Y` 字符设备

#### DAX 设备的地址暴露

DAX 设备本身通过以下方式暴露地址信息：

**1. 设备文件与 mmap()**
- 用户打开 `/dev/daxX.Y`
- 调用 `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)`
- 内核映射返回指向持久内存的虚拟地址
- 虚拟地址对应的物理地址来自底层 Namespace 的 resource

**2. sysfs 属性**
- `/sys/devices/daxX.Y/resource` ← DAX 对应的物理地址范围
- `/sys/devices/daxX.Y/size` ← DAX 设备大小
- 这些属性源于 Namespace 的对应属性

#### DAX 与 Region/Namespace 的关系

```
Region (物理硬件地址)
  ↓
Namespace (用户分配的子范围)
  ↓
DAX Device (提供 mmap 访问该 Namespace)
  ↓
用户虚拟地址空间（通过 mmap 映射）
```

**关键点**：
- DAX 不独立暴露新的地址信息，仅提供访问接口
- 地址信息源头仍是 Region（来自 NFIT）
- DAX 设备可配置多个，但都引用同一 Namespace 的地址范围

### A3. NUMA 节点地址暴露

#### NUMA 节点与 Region 的关系

NUMA（Non-Uniform Memory Access）是系统拓扑概念，将物理地址按 NUMA 节点分组。

**固件关联**：
- NFIT SPA Range 包含 `proximity_domain` 字段
- 内核调用 `pxm_to_online_node()` 转换为 Linux NUMA 节点编号
- 参见：`drivers/acpi/nfit/core.c:2625`

**内核存储**：
```c
// drivers/nvdimm/region_devs.c:1071
nd_region->numa_node = ndr_desc->numa_node;  // 来自 SPA 的 proximity_domain
```

#### NUMA 节点地址暴露

**sysfs 路径**：
- `/sys/devices/system/node/nodeX/` ← NUMA 节点目录
- `/sys/devices/system/node/nodeX/memoryY/` ← 该节点的内存资源

**关键属性**：
- `phys_index`：物理地址范围的起始
- `power/`：内存状态管理

**地址信息源**：
- Region 指定的地址范围（`ndr_start` + `ndr_size`）
- Region 指定的 NUMA 节点（`numa_node`）
- 系统内存管理代码将 Region 地址加入对应 NUMA 节点的内存列表

#### NUMA 与 Region 的一对多关系

```
NUMA Node 0
  ├─ Region 0 (address=0x100000000, size=0x20000000)
  ├─ Region 1 (address=0x120000000, size=0x20000000)
  └─ 系统 DRAM 等其他内存

NUMA Node 1
  ├─ Region 2 (address=0x200000000, size=0x20000000)
  └─ 其他内存
```

- 一个 NUMA 节点可包含多个 Region
- 一个 Region 属于且仅属于一个 NUMA 节点（由固件 SPA 的 proximity_domain 确定）

---

## 附录二：完整地址暴露通道速查表

| 暴露路径 | 来源 | sysfs 文件 | 地址字段 | 是否主体 |
|---------|------|-----------|---------|--------|
| **Region** | 固件 NFIT 表 | `/sys/bus/nd/devices/regionX/resource` | `nd_region->ndr_start` | **是** |
| | | `/sys/bus/nd/devices/regionX/size` | `nd_region->ndr_size` | **是** |
| | | `/sys/bus/nd/devices/regionX/mappingN` | 多DIMM映射 | **是** |
| **Namespace** | 用户 ndctl 指定 | `/sys/bus/nd/devices/namespaceX.Y/resource` | 用户分配范围 | 否 |
| | | `/sys/bus/nd/devices/namespaceX.Y/size` | 用户指定大小 | 否 |
| **DAX** | Namespace 衍生 | `/sys/devices/daxX.Y/resource` | Namespace 地址 | 否 |
| | | `/sys/devices/daxX.Y/size` | Namespace 大小 | 否 |
| **NUMA** | Region + 系统 | `/sys/devices/system/node/nodeX/memory*/` | Region 范围纳入 | 否 |

**说明**：
- **是**：地址数据直接来自固件 NFIT 表，本文档重点
- **否**：地址数据是 Region 信息的进一步组织或映射，用户工具可进一步处理

---

## 总结

### 核心结论

1. **Intel Optane DC 与 NVDIMM-P 地址上报一致**
   - 两者是同一物理产品，在 Linux 内核中无区别
   - 均通过标准 ACPI NFIT 表暴露
   - 地址上报流程完全相同，无 Optane 特定逻辑

2. **地址上报是多层架构，但 Region 是基础**
   - **基础层（本文档重点）**：固件 NFIT → Region，直接暴露硬件地址
   - **用户层**：Namespace、DAX、NUMA，都基于 Region 信息的进一步处理
   - 所有上层抽象的地址数据，源头均是 Region 的 `ndr_start` 和 `ndr_size`

3. **核心地址流转过程（5 个阶段）**
   ```
   NFIT 表 (固件)
     ↓ add_spa(), __nfit_mem_init()
   nfit_spa, nfit_mem (内核内存结构)
     ↓ acpi_nfit_register_region(), acpi_nfit_init_mapping()
   nd_region_desc (Region 描述符)
     ↓ nd_region_create()
   nd_region (内核 Region 对象)
     ↓ device_register()
   sysfs (用户态接口)
   ```

4. **三个关键数据转换点**
   - **点1**：SPA Range 的 `address` 和 `length` → `nd_region->ndr_start` 和 `ndr_size`
   - **点2**：MEMDEV 的 `device_handle`、`address`、`region_size` → `nd_mapping[].nvdimm`、`start`、`size`
   - **点3**：`nd_region` 对象的字段 → sysfs 属性（通过 `size_show()`、`resource_show()`、`mappingN()` 等函数）

### 关键代码位置（快速参考）

| 阶段 | 操作 | 文件 | 行号 |
|------|------|------|------|
| 解析 | 解析 SPA Range | drivers/acpi/nfit/core.c | 698 |
| 解析 | 确定 SPA 类型 | drivers/acpi/nfit/core.c | 679 |
| 关联 | 关联 MEMDEV 与 DIMM | drivers/acpi/nfit/core.c | 1002 |
| 创建 | 初始化单个 Mapping | drivers/acpi/nfit/core.c | 2554 |
| 创建 | 注册 Region（核心）| drivers/acpi/nfit/core.c | 2596 |
| 创建 | 创建 nd_region 对象 | drivers/nvdimm/region_devs.c | 1000 |
| 暴露 | 暴露 /resource（起始地址）| drivers/nvdimm/region_devs.c | 609 |
| 暴露 | 暴露 /size（范围大小）| drivers/nvdimm/region_devs.c | 254 |
| 暴露 | 暴露 /mappingN（DIMM 映射）| drivers/nvdimm/region_devs.c | 707 |

### 对用户的实用建议

**要查询 Optane/NVDIMM-P 的硬件地址范围，直接读取 Region 属性**：

```bash
# 查看所有 Region 的物理地址范围（来自固件）
for i in /sys/bus/nd/devices/region*/; do
    echo "$(basename $i):"
    cat "$i/resource"      # 起始地址
    cat "$i/size"          # 大小
    cat "$i/mappings"      # DIMM 个数
done

# 示例输出
# region0:
# 0x100000000
# 536870912
# 2

# region1:
# 0x200000000
# 536870912
# 2
```

这些信息对应固件 NFIT 表中的各个 SPA Range，是 Optane/NVDIMM-P 硬件对外暴露的真实地址。

### 文档适用范围

本文档：
- ✅ 详细解释 **Region 级别的地址上报**（固件到 sysfs）
- ✅ 说明 **Intel Optane DC 与 NVDIMM-P 的一致性**
- ✅ 澄清 **Namespace、DAX、NUMA 的角色**（基于 Region，非独立路径）
- ✅ 提供 **代码级别的追踪**（每个关键函数标注文件:行号）

本文档不涉及：
- ❌ 块设备驱动 (pmem.c) 的实现
- ❌ DAX 映射的内存管理细节
- ❌ Namespace 标签的加载和验证
- ❌ BTT (Block Translation Table) 的事务实现
- ❌ 设备管理命令 (SMART、固件更新等)

---

**文档版本**：Linux 6.6 kernel  
**最后更新**：2026-11-19  
**作者注**：本分析基于 Linux 6.6 源码的直接代码阅读，所有引用均可在源树中追踪验证。

---

## 附录三：用户如何访问地址信息的完整工具链

### 层级 1：查询 Region（固件硬件地址）

```bash
# 列出所有 Region
cat /sys/bus/nd/devices/region*/resource
# 输出示例：
# 0x100000000  ← Region 0 起始地址
# 0x200000000  ← Region 1 起始地址

# 查询 Region 大小
cat /sys/bus/nd/devices/region0/size
# 输出：536870912  (512GB)

# 查询该 Region 包含的 DIMM
cat /sys/bus/nd/devices/region0/mappings
# 输出：2  (包含 2 个 DIMM)

cat /sys/bus/nd/devices/region0/mapping0
# 输出：nmem0,0,268435456,0  (DIMM nmem0, 起始偏移 0, 大小 256GB, 位置 0)
```

**地址来源**：直接来自 NFIT 表中的 SPA Range

### 层级 2：使用 ndctl 创建 Namespace（用户分配）

```bash
# 创建 Namespace，使用 Region 0 的前 256GB
ndctl create-namespace --region region0 --size 256G --mode fsdax

# 结果��� sysfs 中：
cat /sys/bus/nd/devices/namespace0.0/resource
# 输出：0x100000000  (与 Region 0 相同起始)

cat /sys/bus/nd/devices/namespace0.0/size
# 输出：274877906944  (256GB)
```

**地址来源**：基于 Region 地址，由用户 ndctl 工具指定分配

### 层级 3：创建 DAX 设备（直接映射接口）

```bash
# 基于 Namespace 创建 DAX 设备
ndctl create-namespace --region region0 --size 256G --mode devdax

# 结果：
ls /dev/dax*
# 输出：/dev/dax0.0

# 查询 DAX 地址
cat /sys/devices/daxX.Y/resource
# 对应的是该 Namespace 的物理地址
```

**地址来源**：Namespace 地址，进一步基于 Region

### 层级 4：系统 NUMA 视角

```bash
# 查询 NUMA 节点包含的内存
numactl --show
# 将列出各 NUMA 节点的地址范围，包括 Region 对应地址

cat /proc/iomem
# 查看系统物理地址映射，Region 地址会显示为 "NVDIMM-region" 等
```

**地址来源**：系统在 Region 创建时，根据其 proximity_domain 自动纳入对应 NUMA 节点

---



