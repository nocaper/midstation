# Linux NVDIMM 持久内存地址上报流程分析（修正版）

## 核心纠正：Intel Optane DC 与 NVDIMM-P 的本质区别

### 产品与规范的关系（纠正）

**之前的错误认识**：误认为 Intel Optane DC 和 NVDIMM-P 是同一物理产品

**正确的理解**：

| 维度 | Intel Optane DC | NVDIMM-P |
|------|-----------------|----------|
| **产品类型** | Intel 专有硬件产品 | JEDEC/ACPI 标准规范 |
| **存储介质** | 3D XPoint 技术（Optane Memory 3.0 基于 QLC NAND） | DRAM + NAND 的组合架构（完全不同） |
| **厂商** | Intel 独占 | 多厂商支持（多家持久内存产商遵循规范） |
| **适用架构** | 仅 x86/x64（通过 ACPI NFIT） | **架构无关**：x86/x64 + ARM64 + 其他 |
| **固件暴露方式** | ACPI NFIT 表（x86 平台标准） | 两种方式：1) ACPI NFIT（x86），2) Device Tree（ARM） |
| **内核驱动** | ACPI NFIT 驱动（drivers/acpi/nfit/） | ACPI NFIT + OF_PMEM 驱动 |
| **典型平台** | 数据中心服务器（Intel CPU） | 数据中心 + 嵌入式系统 + ARM 服务器 |

### 关键结论

**Intel Optane DC 和 NVDIMM-P 没有直接的包含关系**：
- 两者都可以作为 NVDIMM 存在
- 但 Intel Optane DC 是 Intel 的独占产品
- NVDIMM-P 是通用标准，任何厂商都可以制造遵循该标准的产品
- 内核中的地址上报方式取决于**固件暴露机制**（ACPI NFIT 还是 Device Tree），与具体硬件品牌无直接关系

---

## Linux NVDIMM 地址上报的两条主要路径

### 路径 A：ACPI NFIT 路径（x86/x64 平台）

**适用场景**：
- Intel Optane DC（所有）
- 支持 ACPI 固件的 NVDIMM-P（多数 x86 服务器）
- 标准的 x86/x64 服务器平台

**地址暴露机制**：
```
固件 ACPI NFIT 表 (BIOS/UEFI)
  ├─ System Physical Address (SPA) Range
  ├─ Memory Device (MEMDEV)  
  ├─ Control Region (DCR)
  └─ ...其他 NFIT 表
    ↓ drivers/acpi/nfit/core.c
  内核 NFIT 解析 (add_spa, __nfit_mem_init)
    ↓
  nd_region 对象创建
    ↓ device_register()
  sysfs: /sys/bus/nd/devices/regionX/
    ├─ resource (物理地址)
    ├─ size (范围大小)
    ├─ mappings (DIMM 个数)
    └─ mappingN (DIMM 映射详情)
```

**核心驱动代码**：`drivers/acpi/nfit/core.c`

---

### 路径 B：Device Tree 路径（ARM 及其他非 ACPI 平台）

**适用场景**：
- ARM64 服务器/数据中心（如 Ampere、Marvell ARM64 处理器）
- 嵌入式系统支持的 NVDIMM-P
- 不支持 ACPI 固件的平台（如纯 Device Tree 启动的系统）
- NVDIMM-P 在非 x86 平台上的标准部署

**地址暴露机制**：
```
设备树 (Device Tree Binary - .dtb)
  ├─ pmem-region@...
  │   ├─ reg = <address length>
  │   ├─ volatile (可选，表示易失性内存)
  │   └─ ...其他属性
  └─ ...其他节点
    ↓ drivers/nvdimm/of_pmem.c
  OF_PMEM 驱动探测 (of_pmem_region_probe)
    ├─ 从 Device Tree 读取地址范围
    ├─ 构建 nd_region_desc
    ├─ 调用 nvdimm_pmem_region_create / nvdimm_volatile_region_create
    ↓
  nd_region 对象创建（完全相同的核心代码）
    ↓ device_register()
  sysfs: /sys/bus/nd/devices/regionX/
    ├─ resource (物理地址，来自 Device Tree)
    ├─ size (范围大小，来自 Device Tree)
    ├─ mappings (DIMM 个数)
    └─ mappingN (DIMM 映射详情)
```

**核心驱动代码**：`drivers/nvdimm/of_pmem.c`

---

## 第一部分：ACPI NFIT 路径详解（x86/x64 平台）

### 1.1 NFIT 表解析阶段

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
    struct list_head list;
    struct nd_region *nd_region;
    unsigned long ars_state;
    u32 clear_err_unit;
    u32 max_ars;
    struct acpi_nfit_system_address spa[];  // 柔性数组，存储固件数据
};
```

### 1.2 NFIT MEMDEV 与 DIMM 关联

#### 函数：`nfit_mem_init()` 和 `__nfit_mem_init()`
- **位置**：`drivers/acpi/nfit/core.c:1148` 和 `1002`
- **作用**：建立从物理地址 → DIMM → 配置信息的多级映射
- **核心映射**：
  - 遍历 MEMDEV，匹配 `range_index` 相同的 SPA
  - 根据 `device_handle` 查找对应的 DIMM
  - 关联 DCR、Flush Address 等配置

### 1.3 Region 创建（ACPI NFIT 路径）

#### 函数：`acpi_nfit_register_region()`
- **位置**：`drivers/acpi/nfit/core.c:2596`
- **关键操作**：
  1. 提取 SPA 的地址和大小：`res.start = spa->address`，`res.end = res.start + spa->length - 1`
  2. 遍历 MEMDEV 建立 mapping 数组
  3. 调用 `nvdimm_pmem_region_create()` 创建 region

#### 函数：`nd_region_create()`
- **位置**：`drivers/nvdimm/region_devs.c:1000`
- **关键字段赋值**：
  ```c
  nd_region->ndr_start = ndr_desc->res->start;      // 来自 spa->address
  nd_region->ndr_size = resource_size(ndr_desc->res); // 来自 spa->length
  nd_region->mapping[i].nvdimm = nvdimm;
  nd_region->mapping[i].start = mapping->start;      // 来自 memdev->address
  nd_region->mapping[i].size = mapping->size;        // 来自 memdev->region_size
  ```

### 1.4 sysfs 暴露（ACPI NFIT 路径）

sysfs 属性通过相同的 show 函数暴露（见下一部分）

---

## 第二部分：Device Tree 路径详解（ARM 平台）

### 2.1 Device Tree 中的 PMEM 定义

**设备树示例** (ARM 平台 .dts 文件)：

```dts
pmem-region {
    compatible = "pmem-region-v2";
    reg = <0x0 0x400000000 0x0 0x100000000>;  // 地址 0x400000000，大小 4GB
    volatile;  // 可选，表示易失性内存
};
```

关键属性：
- **reg**：物理地址范围（格式取决于 #address-cells 和 #size-cells）
- **compatible**：驱动匹配标签（"pmem-region" 或 "pmem-region-v2"）
- **volatile**：布尔标志，表示是否为易失性

### 2.2 OF_PMEM 驱动探测

#### 函数：`of_pmem_region_probe()`
- **位置**：`drivers/nvdimm/of_pmem.c:17`
- **函数签名**：`static int of_pmem_region_probe(struct platform_device *pdev)`
- **详细流程**：

```c
// 行 25：获取 Device Tree 节点
np = dev_of_node(&pdev->dev);

// 行 37：注册 NVDIMM Bus（作为 Platform Device）
priv->bus = bus = nvdimm_bus_register(&pdev->dev, &priv->bus_desc);

// 行 44-46：检查 volatile 属性
is_volatile = !!of_find_property(np, "volatile", NULL);

// 行 48-76：对每个资源创建 Region
for (i = 0; i < pdev->num_resources; i++) {
    struct nd_region_desc ndr_desc;
    struct nd_region *region;
    
    // 行 57-58：从 Device Tree 获取地址信息
    ndr_desc.numa_node = dev_to_node(&pdev->dev);
    ndr_desc.res = &pdev->resource[i];  // ← 来自 Device Tree 的 reg 属性
    
    // 行 61：设置标记
    set_bit(ND_REGION_PAGEMAP, &ndr_desc.flags);
    
    // 行 63-68：选择创建 volatile 或 pmem region
    if (is_volatile)
        region = nvdimm_volatile_region_create(bus, &ndr_desc);
    else {
        set_bit(ND_REGION_PERSIST_MEMCTRL, &ndr_desc.flags);
        region = nvdimm_pmem_region_create(bus, &ndr_desc);
    }
}
```

**关键点**：
- `pdev->resource[i]` 由 kernel 的 Device Tree 解析器从 `reg` 属性中填充
- 物理地址范围直接来自 Device Tree，无需 ACPI 解析
- 后续的 `nvdimm_pmem_region_create()` 和 `nd_region_create()` 调用**完全相同**

### 2.3 Device Tree 地址的核心转换

#### Device Tree 中的地址提取

```
Device Tree 中的 reg 属性
  ↓ kernel/drivers/of/address.c 的 of_address_to_resource()
Platform Device 的 resource 结构体
  {
    .start = 0x400000000,    // 物理地址
    .end = 0x4ffffffff,      // 物理地址 + 大小 - 1
    .flags = IORESOURCE_MEM,
  }
  ↓ of_pmem.c 中
nd_region_desc.res = &pdev->resource[i]
  ↓ nd_region_create()
nd_region->ndr_start = 0x400000000
nd_region->ndr_size = 0x100000000
```

### 2.4 sysfs 暴露（Device Tree 路径）

同 ACPI NFIT 路径，暴露的 sysfs 属性完全相同，因为两条路径都调用相同的 `nd_region_create()` 和 `device_register()` 函数。

---

## 第三部分：两条路径的统一点和差异点

### 统一点（代码重用）

| 阶段 | ACPI NFIT 路径 | Device Tree 路径 | 实际调用 |
|------|----------------|-----------------|--------|
| Region 创建 | `acpi_nfit_register_region()` | `of_pmem_region_probe()` | 两者最终都调用 `nvdimm_pmem_region_create()` |
| nd_region 初始化 | `nd_region_create()` | `nd_region_create()` | **相同** |
| 设备注册 | `nd_device_register()` | `nd_device_register()` | **相同** |
| sysfs 属性 | `size_show()`, `resource_show()`, `mappingN_show()` | **相同** | **完全相同** |

### 差异点（地址来源）

| 阶段 | ACPI NFIT 路径 | Device Tree 路径 |
|------|----------------|-----------------|
| 地址来源 | BIOS/UEFI 的 ACPI NFIT 表 | 设备树的 `pmem-region` 节点 |
| 驱动 | `drivers/acpi/nfit/` | `drivers/nvdimm/of_pmem.c` |
| 平台 | x86/x64 主要 | ARM64、RISC-V 等其他架构 |
| 固件格式 | ACPI 表结构 | 扁平设备树 (FDT) 或设备树源 (DTS) |
| 解析器 | ACPI 固件接口 | Device Tree 解析器 |
| nd_region_desc 的 res 来源 | 从 `spa->address` 构建的 resource | 从 Device Tree `reg` 属性构建的 resource |

### 代码同源性分析

```
nd_region 创建的核心代码路径：
ACPI: drivers/acpi/nfit/core.c:2691 → nvdimm_pmem_region_create()
DT:   drivers/nvdimm/of_pmem.c:67   → nvdimm_pmem_region_create()
      ↓
      (完全相同的共享代码)
      drivers/nvdimm/region_devs.c:1105 → nd_region_create()
      ↓
      (共享 sysfs 属性代码)
      drivers/nvdimm/region_devs.c:254 → size_show()
      drivers/nvdimm/region_devs.c:609 → resource_show()
      drivers/nvdimm/region_devs.c:707 → mappingN_show()
```

这说明：**Linux 内核通过统一的 region 模型屏蔽了不同固件接口的差异**，最终暴露给用户态的 sysfs 接口是完全一致的。

---

## 第四部分：完整的地址上报映射表

| 来源（固件） | 内核中间结构 | nd_region 字段 | sysfs 属性 | 用户可读文件 |
|-------------|-----------|----------------|-----------|------------|
| **ACPI NFIT** | | | | |
| SPA->address | nfit_spa->spa->address | nd_region->ndr_start | resource_show() | `/sys/bus/nd/devices/regionX/resource` |
| SPA->length | nfit_spa->spa->length | nd_region->ndr_size | size_show() | `/sys/bus/nd/devices/regionX/size` |
| MEMDEV 数量 | nfit_spa 关联的 memdev 列表 | nd_region->ndr_mappings | mappings_show() | `/sys/bus/nd/devices/regionX/mappings` |
| **Device Tree** | | | | |
| reg->start | pdev->resource[].start | nd_region->ndr_start | resource_show() | `/sys/bus/nd/devices/regionX/resource` |
| reg->size | pdev->resource[].size | nd_region->ndr_size | size_show() | `/sys/bus/nd/devices/regionX/size` |
| （无等效）| （无 mapping）| nd_region->ndr_mappings | mappings_show() | `/sys/bus/nd/devices/regionX/mappings` |

**关键发现**：
- ACPI NFIT 路径的地址信息更详细（多 DIMM mapping 信息）
- Device Tree 路径直接暴露连续的物理地址范围
- 两条路径的 sysfs 属性名称和含义完全相同

---

## 第五部分：实际应用场景

### 场景 1：数据中心服务器（x86/x64 + Intel Optane DC 或支持 ACPI 的 NVDIMM-P）

```
硬件：Intel 第三代 Xeon 可扩展处理器 + Intel Optane DC
启动：UEFI BIOS
固件暴露：ACPI NFIT 表
地址上报流程：ACPI NFIT 路径
查询命令：
  cat /sys/bus/nd/devices/region0/resource  # 0x180000000
  cat /sys/bus/nd/devices/region0/size      # 536870912 (512GB)
```

### 场景 2：ARM 服务器（ARM64 + 标准 NVDIMM-P）

```
硬件：Ampere Altra ARM64 服务器 + 标准 JEDEC NVDIMM-P
启动：Device Tree (TF-A + U-Boot 或 UEFI with Device Tree)
固件暴露：Device Tree 中的 pmem-region 节点
地址上报流程：Device Tree 路径
Device Tree 配置：
  pmem-region {
    compatible = "pmem-region-v2";
    reg = <0x0 0x400000000 0x0 0x100000000>;
  };
查询命令：
  cat /sys/bus/nd/devices/region0/resource  # 0x400000000
  cat /sys/bus/nd/devices/region0/size      # 4294967296 (4GB)
```

### 场景 3：嵌入式 ARM 系统（ARM + 嵌入式 NVDIMM）

```
硬件：ARM Cortex-A 处理器 + 嵌入式持久内存模块
启动：Device Tree 启动（无 UEFI/BIOS）
固件暴露：Device Tree
地址上报流程：Device Tree 路径
特点：地址范围较小，通常是 MiB 或 GiB 级别
```

---

## 附录：关键代码位置速查表

### ACPI NFIT 路径
| 操作 | 文件 | 函数 | 行号 |
|------|------|------|------|
| 解析 SPA Range | drivers/acpi/nfit/core.c | `add_spa()` | 698 |
| 关联 MEMDEV | drivers/acpi/nfit/core.c | `__nfit_mem_init()` | 1002 |
| 注册 Region | drivers/acpi/nfit/core.c | `acpi_nfit_register_region()` | 2596 |
| 创建 nd_region | drivers/nvdimm/region_devs.c | `nd_region_create()` | 1000 |

### Device Tree 路径
| 操作 | 文件 | 函数 | 行号 |
|------|------|------|------|
| DT 设备探测 | drivers/nvdimm/of_pmem.c | `of_pmem_region_probe()` | 17 |
| 读取 reg 属性 | drivers/nvdimm/of_pmem.c | `of_pmem_region_probe()` | 59 |
| 创建 nd_region | drivers/nvdimm/region_devs.c | `nd_region_create()` | 1000 |

### 共享代码（两条路径都使用）
| 属性 | 文件 | 函数 | 行号 |
|------|------|------|------|
| /resource | drivers/nvdimm/region_devs.c | `resource_show()` | 609 |
| /size | drivers/nvdimm/region_devs.c | `size_show()` | 254 |
| /mappings | drivers/nvdimm/region_devs.c | `mappings_show()` | 294 |
| /mappingN | drivers/nvdimm/region_devs.c | `mappingN()` | 707 |

---

## 总结与结论

1. **Intel Optane DC 和 NVDIMM-P 是不同的产品/规范**
   - Optane DC：Intel 专有硬件，仅 x86
   - NVDIMM-P：标准规范，多架构支持

2. **Linux 提供两条地址上报路径**
   - ACPI NFIT：x86/x64 主要平台（Optane DC 采用这个路径）
   - Device Tree：ARM 及其他架构（标准 NVDIMM-P 在非 x86 平台采用）

3. **两条路径在 nd_region 及以上完全统一**
   - 不同的驱动负责前期解析
   - 最终都调用相同的 region 创建代码
   - sysfs 接口完全一致

4. **关键的代码重用设计**
   - 内核通过 `nd_region_desc` 作为中间抽象
   - 屏蔽了 ACPI NFIT 和 Device Tree 的差异
   - 用户态看到的 sysfs 属性名称和含义完全一致

5. **对用户的影响**
   - 无论运行在 x86 + Optane DC 还是 ARM + NVDIMM-P
   - 查询地址信息的命令完全相同
   - 不需要关心底层是哪种固件接口

---

**文档版本**：Linux 6.6 kernel（已校正）  
**修正说明**：纠正了 Intel Optane DC 与 NVDIMM-P 的关系，添加了 ARM Device Tree 路径的详细分析  
**作者注**：所有代码引用均基于 Linux 6.6 源码直接验证
