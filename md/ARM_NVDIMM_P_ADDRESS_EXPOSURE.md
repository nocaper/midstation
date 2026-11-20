# ARM 架构 NVDIMM-P 地址上报流程详解

## 概述

本文档详细分析 ARM64（及其他基于 Device Tree 的平台）上标准 NVDIMM-P 持久内存的地址上报流程。ARM 平台与 x86 不同，采用 Device Tree 机制而非 ACPI，导致地址暴露的前期流程差异显著，但最终在内核 Region 层统一。

---

## 第一部分：ARM 平台 NVDIMM-P 的硬件与固件基础

### 1.1 典型 ARM 平台

| 平台 | 处理器 | 启动固件 | 应用场景 |
|------|--------|---------|---------|
| **Ampere Altra / Ampere A1** | ARM Cortex-A | TF-A + U-Boot 或 UEFI | 云计算数据中心 |
| **Marvell ThunderX3** | ARM Cortex-A | UEFI + Device Tree | 企业数据中心 |
| **Fujitsu A64FX** | ARM SVE | 专有固件 | 超级计算 |
| **嵌入式 ARM** | 各种 ARM 核心 | U-Boot + DTS | IoT/边缘计算 |

### 1.2 固件启动流程对比

#### x86 平台（ACPI）
```
UEFI BIOS 启动
  ↓
加载 ACPI 固件表（包括 NFIT）到内存
  ↓
系统调用 acpi_nfit_init()
  ↓
内核解析 NFIT 表并创建 Region
```

#### ARM 平台（Device Tree）
```
TF-A (ARM Trusted Firmware) 启动
  ↓
加载 Device Tree Binary (.dtb) 到内存
  ↓
U-Boot / UEFI 传递 DTB 地址给内核
  ↓
内核解析 Device Tree 并创建 Platform Device
  ↓
OF_PMEM 驱动探测 Platform Device 并创建 Region
```

### 1.3 Device Tree 中的持久内存定义

#### 基本结构（pmem-region 节点）

```dts
/ {
    memory {
        device_type = "memory";
        reg = <0x0 0x0 0x0 0x80000000>;  // 主系统 DRAM，2GB
    };

    pmem-region {
        compatible = "pmem-region-v2";
        reg = <0x0 0x80000000 0x0 0x20000000>;  // 持久内存：起始 0x80000000，大小 512MB
    };
};
```

#### 字段解释

- **compatible = "pmem-region-v2"**：驱动匹配标签
  - "pmem-region"：基础版本
  - "pmem-region-v2"：增强版本（推荐）

- **reg = <address-cells size-cells>**：
  - 格式取决于根节点的 `#address-cells` 和 `#size-cells`
  - 通常：`<0x0 0x80000000 0x0 0x20000000>` 表示 64 位地址 + 64 位大小
  - 示例中：地址 = 0x0_80000000，大小 = 0x0_20000000（512MB）

- **volatile（可选）**：布尔属性
  ```dts
  pmem-region {
      compatible = "pmem-region-v2";
      reg = <...>;
      volatile;  // 表示这是易失性内存（掉电丢失数据）
  };
  ```

#### 完整示例（Ampere Altra 风格）

```dts
/ {
    model = "Ampere Altra";
    compatible = "ampere,altra";

    #address-cells = <2>;
    #size-cells = <2>;

    memory@0 {
        device_type = "memory";
        reg = <0x0 0x0 0x0 0x100000000>;  // 4GB 主 DRAM
    };

    pmem-region@100000000 {
        compatible = "pmem-region-v2";
        reg = <0x0 0x100000000 0x0 0x200000000>;  // 8GB 持久内存
    };

    pmem-region@300000000 {
        compatible = "pmem-region-v2";
        reg = <0x0 0x300000000 0x0 0x200000000>;  // 又一个 8GB
        volatile;  // 这个是易失性（可选）
    };
};
```

---

## 第二部分：Device Tree 解析到 Region 创建的完整链路

### 2.1 内核 Device Tree 解析阶段

#### 2.1.1 Device Tree 加载

**流程**：
```
Bootloader（U-Boot / TF-A）
  ↓
将 Device Tree Binary (.dtb) 地址传给内核
  ↓ kernel/drivers/of/fdt.c
Device Tree 初始化：early_init_dt_scan_nodes()
  ↓
扫描所有设备节点，构建内核内部的 device_node 树
```

**关键代码**（内核侧）：
- 位置：`kernel/drivers/of/fdt.c` 和 `kernel/drivers/of/base.c`
- 这不是 NVDIMM 驱动代码，而是内核通用 Device Tree 框架
- NVDIMM 驱动只需使用已解析的结果

#### 2.1.2 Platform Device 创建

**流程**：
```
Device Tree 中的节点
  ↓ kernel/drivers/of/platform.c: of_platform_populate()
为每个匹配的节点创建 Platform Device
  ↓
Platform Device 结构体：
  {
    .name = 节点名（如 "pmem-region"）
    .resource[] = 从 reg 属性解析的资源数组
    .num_resources = 资源个数
    .dev = 标准 Linux device 结构
  }
```

**示例**：
```c
struct platform_device {
    const char *name;      // "pmem-region"
    int id;
    struct device dev;
    u32 num_resources;
    struct resource *resource;  // 指向 resource[] 数组
};
```

Device Tree 中的每个 `pmem-region` 节点 → 对应一个 `struct platform_device`

#### 2.1.3 reg 属性到 Platform Resource 的转换

**关键函数**：`of_address_to_resource()`（kernel/drivers/of/address.c）

```
Device Tree 中的 reg 属性
  例如：reg = <0x0 0x80000000 0x0 0x20000000>
    ↓ of_address_to_resource()
struct resource {
    resource_size_t start;     // 0x80000000
    resource_size_t end;       // 0x9fffffff (start + size - 1)
    unsigned long flags;       // IORESOURCE_MEM
    const char *name;
    ...
};
```

**多资源支持**：
如果 Device Tree 节点的 `reg` 属性包含多个地址范围（通过空格分隔），会创建多个 resource 条目。

```dts
pmem-region {
    compatible = "pmem-region-v2";
    reg = <0x0 0x80000000 0x0 0x10000000>,  // 资源 0：起始 0x80000000，大小 256MB
          <0x0 0x90000000 0x0 0x10000000>;  // 资源 1：起始 0x90000000，大小 256MB
};
```

这会创建一个 `platform_device`，其 `num_resources = 2`，`resource[0]` 和 `resource[1]` 分别对应两个范围。

---

### 2.2 OF_PMEM 驱动探测与 Region 创建

#### 2.2.1 驱动注册

**位置**：`drivers/nvdimm/of_pmem.c:97`

```c
static struct platform_driver of_pmem_region_driver = {
    .probe = of_pmem_region_probe,
    .remove = of_pmem_region_remove,
    .driver = {
        .name = "of_pmem",
        .of_match_table = of_pmem_region_match,
    },
};
module_platform_driver(of_pmem_region_driver);
```

**匹配规则**（行 91-95）：
```c
static const struct of_device_id of_pmem_region_match[] = {
    { .compatible = "pmem-region" },      // 匹配 compatible = "pmem-region"
    { .compatible = "pmem-region-v2" },   // 匹配 compatible = "pmem-region-v2"
    { },
};
```

当内核枚举 Device Tree 节点时，如果发现节点的 `compatible` 属性匹配上述任一值，就会：
1. 创建 `platform_device`
2. 调用 `of_pmem_region_probe()` 函数

#### 2.2.2 Probe 函数详解

**函数**：`of_pmem_region_probe()`  
**位置**：`drivers/nvdimm/of_pmem.c:17`  
**签名**：`static int of_pmem_region_probe(struct platform_device *pdev)`

**完整代码流程**（含行号）：

```c
static int of_pmem_region_probe(struct platform_device *pdev)
{
    struct of_pmem_private *priv;
    struct device_node *np;
    struct nvdimm_bus *bus;
    bool is_volatile;
    int i;

    // 行 25：获取与 platform_device 关联的 Device Tree 节点
    np = dev_of_node(&pdev->dev);
    if (!np)
        return -ENXIO;

    // 行 29-31：分配私有驱动数据
    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    // 行 33-35：初始化 NVDIMM Bus Descriptor
    priv->bus_desc.provider_name = kstrdup(pdev->name, GFP_KERNEL);
    priv->bus_desc.module = THIS_MODULE;
    priv->bus_desc.of_node = np;

    // 行 37-41：向内核 NVDIMM 子系统注册 Bus
    //   这个 bus 作为父设备，后续 Region 会在它下面创建
    priv->bus = bus = nvdimm_bus_register(&pdev->dev, &priv->bus_desc);
    if (!bus) {
        kfree(priv);
        return -ENODEV;
    }
    platform_set_drvdata(pdev, priv);

    // 行 44-46：检查 Device Tree 节点是否有 "volatile" 属性
    //   volatile 属性表示这是易失性内存（掉电数据丢失）
    is_volatile = !!of_find_property(np, "volatile", NULL);
    dev_dbg(&pdev->dev, "Registering %s regions from %pOF\n",
            is_volatile ? "volatile" : "non-volatile", np);

    // 行 48-76：遍历该 platform_device 的所有资源
    //   如果 Device Tree 中 reg 属性定义了多个地址范围，就有多个资源
    for (i = 0; i < pdev->num_resources; i++) {
        struct nd_region_desc ndr_desc;
        struct nd_region *region;

        // 行 56-60：初始化 nd_region_desc（Region 描述符）
        memset(&ndr_desc, 0, sizeof(ndr_desc));
        
        // 行 57-58：从 platform_device 的 NUMA 信息推导
        //   dev_to_node() 通过 platform_device 关联的 NUMA 节点编号
        ndr_desc.numa_node = dev_to_node(&pdev->dev);
        ndr_desc.target_node = ndr_desc.numa_node;
        
        // 行 59：最关键的一行！
        //   将 Device Tree 的 reg 属性解析后的 resource 指针传给 Region
        //   这就是 Device Tree 地址进入 NVDIMM 子系统的入口
        ndr_desc.res = &pdev->resource[i];  // ← 地址来源点
        
        // 行 60：记录 Device Tree 节点指针
        ndr_desc.of_node = np;
        
        // 行 61：设置标记（表示支持 DAX 映射）
        set_bit(ND_REGION_PAGEMAP, &ndr_desc.flags);

        // 行 63-68：根据 volatile 属性选择 Region 类型
        if (is_volatile) {
            // 易失性内存：使用 volatile region
            region = nvdimm_volatile_region_create(bus, &ndr_desc);
        } else {
            // 持久性内存：设置持久化标记，使用 pmem region
            set_bit(ND_REGION_PERSIST_MEMCTRL, &ndr_desc.flags);
            region = nvdimm_pmem_region_create(bus, &ndr_desc);
        }

        // 行 70-75：错误处理和日志
        if (!region)
            dev_warn(&pdev->dev, "Unable to register region %pR from %pOF\n",
                    ndr_desc.res, np);
        else
            dev_dbg(&pdev->dev, "Registered region %pR from %pOF\n",
                    ndr_desc.res, np);
    }

    return 0;
}
```

**关键设计决策**：

1. **一个 platform_device → 一个 NVDIMM Bus → 多个 Regions**
   - 对于每个资源（每个地址范围），都创建一个单独的 Region
   - 所有 Region 共享同一个 NVDIMM Bus

2. **volatile 标记的作用**
   - `volatile` 属性仅在 Device Tree 中存在
   - 用于区分易失性内存（断电丢失）和持久性内存（NVDIMM-P）
   - 影响创建的 Region 类型和 sysfs 属性

3. **nd_region_desc 的初始化**
   - `res` 字段：直接指向 `platform_device` 的 resource，无需复制
   - `numa_node` 和 `target_node`：从 platform_device 推导（可能与硬件不匹配，但这是 ARM 平台的限制）

---

### 2.3 从 nd_region_desc 到 nd_region 对象

#### 2.3.1 nvdimm_pmem_region_create() 函数

**位置**：`drivers/nvdimm/region_devs.c:1105`  
**签名**：`struct nd_region *nvdimm_pmem_region_create(struct nvdimm_bus *nvdimm_bus, struct nd_region_desc *ndr_desc)`

```c
struct nd_region *nvdimm_pmem_region_create(struct nvdimm_bus *nvdimm_bus,
        struct nd_region_desc *ndr_desc)
{
    ndr_desc->num_lanes = ND_MAX_LANES;  // 设置并发通道数
    return nd_region_create(nvdimm_bus, ndr_desc, &nd_pmem_device_type,
            __func__);
}
```

这是一个简单的包装函数，转发给通用的 `nd_region_create()`。

#### 2.3.2 nd_region_create() 函数

**位置**：`drivers/nvdimm/region_devs.c:1000`  
**签名**：`static struct nd_region *nd_region_create(...)`

**关键步骤**（与 x86 ACPI 路径完全相同）：

```c
static struct nd_region *nd_region_create(struct nvdimm_bus *nvdimm_bus,
        struct nd_region_desc *ndr_desc,
        const struct device_type *dev_type, const char *caller)
{
    struct nd_region *nd_region;
    struct device *dev;
    unsigned int i;

    // 验证 mapping 对齐（ARM 设备通常没有 mapping，此检查被跳过）

    // 分配 nd_region 对象
    nd_region = kzalloc(struct_size(nd_region, mapping, ndr_desc->num_mappings),
            GFP_KERNEL);
    if (!nd_region)
        return NULL;

    // 分配 Region ID
    nd_region->id = memregion_alloc(GFP_KERNEL);
    if (nd_region->id < 0)
        goto err_id;

    // 初始化 lane 并发控制结构
    nd_region->lane = alloc_percpu(struct nd_percpu_lane);
    if (!nd_region->lane)
        goto err_percpu;

    // 复制 mapping 信息（ARM Device Tree 路径通常 mapping 为空）
    for (i = 0; i < ndr_desc->num_mappings; i++) {
        struct nd_mapping_desc *mapping = &ndr_desc->mapping[i];
        nd_region->mapping[i].nvdimm = mapping->nvdimm;
        nd_region->mapping[i].start = mapping->start;
        nd_region->mapping[i].size = mapping->size;
    }
    nd_region->ndr_mappings = ndr_desc->num_mappings;

    // ===== 最关键的字段赋值 =====
    // 行 1083-1084：直接来自 Device Tree 的 reg 属性
    nd_region->ndr_size = resource_size(ndr_desc->res);  // = size
    nd_region->ndr_start = ndr_desc->res->start;         // = address

    // 其他字段
    nd_region->provider_data = ndr_desc->provider_data;
    nd_region->nd_set = ndr_desc->nd_set;
    nd_region->num_lanes = ndr_desc->num_lanes;
    nd_region->flags = ndr_desc->flags;
    nd_region->numa_node = ndr_desc->numa_node;
    nd_region->target_node = ndr_desc->target_node;

    // 初始化 IDA（device ID 分配器）
    ida_init(&nd_region->ns_ida);
    ida_init(&nd_region->btt_ida);
    ida_init(&nd_region->pfn_ida);
    ida_init(&nd_region->dax_ida);

    // 初始化标准 Linux device
    dev = &nd_region->dev;
    dev_set_name(dev, "region%d", nd_region->id);
    dev->parent = &nvdimm_bus->dev;
    dev->type = dev_type;  // &nd_pmem_device_type
    dev->groups = ndr_desc->attr_groups;
    dev->of_node = ndr_desc->of_node;

    // 初始化 device 并向 sysfs 注册
    device_initialize(dev);  // 初始化 device 结构
    nd_device_register(dev);  // 注册到内核，创建 sysfs 属性

    return nd_region;

err_percpu:
    memregion_free(nd_region->id);
err_id:
    kfree(nd_region);
    return NULL;
}
```

**ARM Device Tree 路径的特点**：
- `ndr_desc->num_mappings` 通常为 0（因为 Device Tree 没有 DIMM 映射信息，不像 ACPI NFIT）
- `ndr_desc->res->start` 和 `ndr_desc->res->end` 直接来自 Device Tree 的 reg 属性
- `nd_region->ndr_start` 和 `nd_region->ndr_size` 与 ACPI 路径的意义完全相同

---

### 2.4 sysfs 属性创建

当 `nd_device_register(dev)` 被调用时，内核会：

1. 在 `/sys/bus/nd/devices/` 下创建目录 `regionX/`
2. 根据 `dev->type->groups` 创建属性文件
3. 属性文件链接到相应的 show 函数

**创建的属性**（完全相同，无论 x86 还是 ARM）：

```
/sys/bus/nd/devices/region0/
├── resource          → resource_show()       输出：0x80000000
├── size              → size_show()           输出：536870912
├── mappings          → mappings_show()       输出：0 (ARM 通常无 mapping)
├── subsystem         → ../../../bus/nd
├── uevent
├── power
└── ... (其他属性)
```

---

## 第三部分：ARM 设备树配置实例分析

### 3.1 Ampere Altra 示例配置

```dts
/ {
    model = "Ampere Altra";
    compatible = "ampere,altra";

    #address-cells = <2>;      // 地址用 2 个 32 位值表示（64 位）
    #size-cells = <2>;         // 大小用 2 个 32 位值表示（64 位）

    // ===== 主系统 DRAM =====
    memory@0 {
        device_type = "memory";
        reg = <0x0 0x0 0x0 0x80000000>;    // 0x0 ~ 0x80000000 (2GB)
    };

    // ===== NVDIMM-P 持久内存区域 1 =====
    pmem-region@80000000 {
        compatible = "pmem-region-v2";
        reg = <0x0 0x80000000 0x0 0x100000000>;  // 0x80000000 ~ 0x180000000 (4GB)
    };

    // ===== NVDIMM-P 持久内存区域 2 =====
    pmem-region@180000000 {
        compatible = "pmem-region-v2";
        reg = <0x0 0x180000000 0x0 0x100000000>;  // 0x180000000 ~ 0x280000000 (4GB)
    };

    // ===== 易失性内存（可选） =====
    pmem-region@280000000 {
        compatible = "pmem-region-v2";
        reg = <0x0 0x280000000 0x0 0x80000000>;   // 0x280000000 ~ 0x300000000 (2GB)
        volatile;  // 标记为易失性
    };
};
```

**内核解析结果**：

```
Device Tree 解析
    ↓
创建 4 个 platform_device：
    1. of_pmem_region_driver for pmem-region@80000000
    2. of_pmem_region_driver for pmem-region@180000000
    3. of_pmem_region_driver for pmem-region@280000000
    
OF_PMEM 驱动 probe
    ↓
创建 3 个 NVDIMM Bus（每个 pmem-region 一个）：
    bus0, bus1, bus2
    
每个 bus 创建 1 个 Region（因为每个 pmem-region 通常只有 1 个 reg）：
    region0 (来自 pmem-region@80000000)  -> 0x80000000, size 4GB
    region1 (来自 pmem-region@180000000) -> 0x180000000, size 4GB
    region2 (来自 pmem-region@280000000) -> 0x280000000, size 2GB (volatile)

sysfs 树：
/sys/bus/nd/
├── devices/
│   ├── nmem0@bus0/
│   ├── nmem0@bus1/
│   ├── nmem0@bus2/
│   ├── region0/          (persistent)
│   ├── region1/          (persistent)
│   └── region2/          (volatile)
```

### 3.2 包含多个地址范围的配置

```dts
pmem-region@80000000 {
    compatible = "pmem-region-v2";
    reg = <0x0 0x80000000 0x0 0x40000000>,   // 资源 0：0x80000000, 1GB
          <0x0 0xC0000000 0x0 0x40000000>;   // 资源 1：0xC0000000, 1GB
};
```

**内核解析结果**：

```
platform_device 创建 1 个，但 num_resources = 2
    ↓
of_pmem_region_probe() 被调用 1 次
    ↓
在 for 循环中（i = 0, 1）创建 2 个 Region：
    region0 -> 0x80000000, size 1GB
    region1 -> 0xC0000000, size 1GB
    ↓
sysfs：
/sys/bus/nd/devices/
├── region0/
└── region1/
```

---

## 第四部分：ARM 与 x86 地址上报的比较

### 4.1 数据流对比

#### x86 ACPI 路径
```
BIOS/UEFI
  ↓
ACPI NFIT 表加载到内存
  ↓ drivers/acpi/nfit/core.c
  解析 SPA Range：
    - spa->address (固件字段)
    - spa->length (固件字段)
  ↓
nd_region_desc.res = 从 spa 构建的 resource
  ↓
nd_region->ndr_start = resource.start
nd_region->ndr_size = resource_size(resource)
  ↓
sysfs: resource_show() 输出 ndr_start
       size_show() 输出 ndr_size
```

#### ARM Device Tree 路径
```
Bootloader (TF-A / U-Boot)
  ↓
Device Tree Binary 加载到内存
  ↓ kernel/drivers/of/ (通用 DT 解析)
  解析 pmem-region 节点：
    - reg 属性转换为 resource.start
    - reg 属性转换为 resource.end
  ↓ drivers/nvdimm/of_pmem.c
nd_region_desc.res = &platform_device.resource[i]
  ↓
nd_region->ndr_start = resource.start
nd_region->ndr_size = resource_size(resource)
  ↓
sysfs: resource_show() 输出 ndr_start
       size_show() 输出 ndr_size
```

### 4.2 关键差异

| 维度 | x86 ACPI | ARM Device Tree |
|------|----------|-----------------|
| **固件源** | BIOS/UEFI ACPI 表 | Bootloader 传递的 DTB |
| **驱动解析** | drivers/acpi/nfit/ | drivers/nvdimm/of_pmem.c |
| **DIMM 映射** | 完整的 MEMDEV + DCR 映射 | 无（只有地址范围） |
| **platform_device** | 不使用 | 用于持久内存区域描述 |
| **num_mappings** | 可以 > 0 | 通常 = 0 |
| **NUMA 信息** | 来自 SPA proximity_domain | 来自 platform_device 节点位置 |
| **sysfs 属性名** | 完全相同 | 完全相同 |
| **用户查询命令** | 完全相同 | 完全相同 |

### 4.3 sysfs 属性查询示例

**两种平台的查询结果格式完全相同**：

```bash
# ARM 平台查询
# cat /sys/bus/nd/devices/region0/resource
0x80000000

# cat /sys/bus/nd/devices/region0/size
4294967296

# x86 平台查询（结果格式相同）
# cat /sys/bus/nd/devices/region0/resource
0x80000000

# cat /sys/bus/nd/devices/region0/size
4294967296
```

---

## 第五部分：ARM 平台地址上报的完整流程叙述

### 从设备树到用户态的完整链路

**第 0 步：Bootloader 准备**
- Bootloader（TF-A + U-Boot）扫描硬件，找到持久内存模块
- 生成或加载 Device Tree Binary (.dtb)，在其中定义 pmem-region 节点
- Bootloader 计算 pmem-region 的物理地址范围，填入 reg 属性
- 例如：`reg = <0x0 0x80000000 0x0 0x100000000>` 表示地址 0x80000000，大小 4GB

**第 1 步：内核 Device Tree 初始化**（kernel/drivers/of/）
- 内核启动时，接收 Bootloader 传递的 DTB 地址
- 调用 `early_init_dt_scan_nodes()` 解析 Device Tree Binary
- 扫描所有节点，为每个 compatible = "pmem-region" 或 "pmem-region-v2" 的节点创建 `struct device_node`
- 调用 `of_platform_populate()` 为每个 pmem-region 节点创建 `platform_device`

**第 2 步：Platform Device 资源初始化**（kernel/drivers/of/address.c）
- 对每个 platform_device，内核调用 `of_address_to_resource()` 函数
- 将 Device Tree 中的 reg 属性的值转换为 `struct resource` 结构体
  - `resource.start` = reg 中的地址值（0x80000000）
  - `resource.end` = start + size - 1（0x180000000 - 1）
  - `resource.flags` = IORESOURCE_MEM
- 填充 platform_device 的 `resource[]` 数组和 `num_resources` 字段

**第 3 步：OF_PMEM 驱动探测**（drivers/nvdimm/of_pmem.c:of_pmem_region_probe）
- 内核枚举所有 platform_device，发现 compatible 匹配 "pmem-region-v2"
- 调用 `of_pmem_region_probe(&platform_device)`
- 从 platform_device 中读取 Device Tree 节点指针 `np`
- 检查节点是否有 "volatile" 属性，决定是否为易失性内存
- **关键步骤**：创建 `nvdimm_bus_descriptor` 和 `nvdimm_bus`
  - 这个 bus 作为后续 Region 的父设备
  - bus 的名称来自 platform_device.name（如 "of_pmem"）

**第 4 步：Region 描述符构建与创建**（drivers/nvdimm/of_pmem.c，行 48-76）
- 对 platform_device 的每个资源（resource[]），执行以下操作：
- 初始化 `nd_region_desc`：
  - `ndr_desc.res = &pdev->resource[i]`：指向 Device Tree 解析的资源
  - `ndr_desc.numa_node = dev_to_node(&pdev->dev)`：从 platform_device 推导 NUMA 节点
  - `ndr_desc.num_mappings = 0`：Device Tree 不提供 DIMM 映射
  - `ndr_desc.flags`：根据 volatile 属性设置 ND_REGION_PERSIST_MEMCTRL 等标记
- 调用 `nvdimm_pmem_region_create(bus, &ndr_desc)` 或 `nvdimm_volatile_region_create()`

**第 5 步：nd_region 对象创建**（drivers/nvdimm/region_devs.c:nd_region_create）
- 分配 `struct nd_region` 对象（包含 mapping[] 数组）
- **关键字段赋值**（来自 Device Tree 的 reg 属性）：
  ```c
  nd_region->ndr_start = ndr_desc.res->start;       // 0x80000000
  nd_region->ndr_size = resource_size(ndr_desc.res); // 4GB
  nd_region->ndr_mappings = 0;  // Device Tree 无 mapping
  nd_region->numa_node = ndr_desc.numa_node;        // 从 platform_device 推导
  ```
- 分配 Region ID（region0, region1, ...）
- 初始化 IDA、lane、其他字段
- 调用 `device_initialize()` 和 `nd_device_register()`

**第 6 步：sysfs 设备注册**（drivers/base/core.c:device_register）
- 内核设备核心代码接管
- 在 `/sys/bus/nd/devices/` 下创建目录 `regionX/`
- 从 `dev->type->groups` 中遍历属性列表，创建 sysfs 属性文件
- 属性文件与相应的 show 函数绑定

**第 7 步：sysfs 属性暴露**（drivers/nvdimm/region_devs.c）
- 创建的属性文件及其 show 函数：
  ```
  /sys/bus/nd/devices/region0/
  ├── resource  → resource_show()     [行 609]
  │              return sprintf(buf, "%#llx\n", nd_region->ndr_start);
  │              输出：0x80000000
  │
  ├── size      → size_show()         [行 254]
  │              return sprintf(buf, "%llu\n", region_size(nd_region));
  │              输出：4294967296
  │
  └── mappings  → mappings_show()     [行 294]
                 return sprintf(buf, "%d\n", nd_region->ndr_mappings);
                 输出：0 (无 DIMM mapping)
  ```

**第 8 步：用户态查询**
- 用户或工具（如 ndctl）读取 sysfs 文件
- 内核执行对应的 show 函数
- 返回地址信息给用户
- 例如：
  ```bash
  cat /sys/bus/nd/devices/region0/resource
  # 输出：0x80000000
  
  ndctl list -r
  # 列出所有 region 及其属性
  ```

---

## 第六部分：常见问题与故障排查

### 6.1 为什么我的 Device Tree 配置没有被识别？

**症状**：
```bash
# 查询不到 region
ls /sys/bus/nd/devices/region*/
# 返回空或不包含预期的 region
```

**排查步骤**：

1. **检查 compatible 属性**
   ```bash
   # 确保 Device Tree 中使用了正确的 compatible
   grep -A 2 "pmem-region" /proc/device-tree/*/compatible
   # 应该输出 "pmem-region" 或 "pmem-region-v2"
   ```

2. **检查 OF_PMEM 驱动是否加载**
   ```bash
   lsmod | grep of_pmem
   # 应该显示 of_pmem 模块已加载
   
   # 如果未加载，查看 dmesg
   dmesg | grep -i "of_pmem\|pmem"
   ```

3. **检查 platform_device 是否创建**
   ```bash
   cat /proc/device-tree/pmem-region*/reg | od -x
   # 应该显示 reg 属性的十六进制值
   ```

4. **检查内核配置**
   ```bash
   grep -i "of_pmem\|libnvdimm" /boot/config-$(uname -r)
   # 应该显示 CONFIG_OF_PMEM=y 或 =m
   # 应该显示 CONFIG_LIBNVDIMM=y 或 =m
   ```

### 6.2 Device Tree reg 属性格式错误

**常见错误 1：地址位数不匹配**
```dts
/ {
    #address-cells = <2>;  // 说明地址是 64 位（两个 32 位值）
    #size-cells = <2>;     // 说明大小是 64 位（两个 32 位值）

    pmem-region {
        compatible = "pmem-region-v2";
        reg = <0x80000000 0x100000000>;  // ❌ 错误！只有 2 个值
                                          // 应该有 4 个值（2 个地址 + 2 个大小）
    };
};
```

**正确写法**：
```dts
pmem-region {
    compatible = "pmem-region-v2";
    reg = <0x0 0x80000000 0x0 0x100000000>;  // ✓ 正确：4 个值
                                              // 前 2 个是地址，后 2 个是大小
};
```

**常见错误 2：超过 32 位地址的表示**
```dts
/ {
    #address-cells = <1>;  // ❌ 错误！只能表示 32 位地址
    #size-cells = <1>;

    pmem-region {
        compatible = "pmem-region-v2";
        reg = <0x100000000 0x100000000>;  // ❌ 地址 0x100000000 超过 32 位
    };
};
```

**正确写法**：
```dts
/ {
    #address-cells = <2>;  // ✓ 两个 32 位单元 = 64 位地址
    #size-cells = <2>;     // ✓ 两个 32 位单元 = 64 位大小

    pmem-region {
        compatible = "pmem-region-v2";
        reg = <0x0 0x100000000 0x0 0x100000000>;  // ✓ 正确
    };
};
```

### 6.3 NUMA 节点信息不准确

**症状**：
```bash
cat /sys/bus/nd/devices/region0/numa_node
# 输出与实际硬件 NUMA 拓扑不符
```

**原因**：
- Device Tree 中 pmem-region 节点的位置不代表实际的 NUMA 节点
- OF_PMEM 驱动使用 `dev_to_node(&pdev->dev)` 推导 NUMA 节点
- 这可能与硬件的真实 NUMA 拓扑不一致

**解决方案**（如果平台固件支持）：
- 在 Device Tree 中为 pmem-region 节点添加 `numa-node-id` 属性
- 或在 bootloader 中正确配置设备树的 NUMA 信息
- 某些新版本的 OF_PMEM 驱动可能支持从设备树读取 NUMA 提示

---

## 第七部分：完整代码路径总结

### 关键代码位置（ARM Device Tree 路径）

| 阶段 | 文件 | 函数 | 行号 | 说明 |
|------|------|------|------|------|
| DT 解析 | kernel/drivers/of/fdt.c | early_init_dt_scan_nodes() | - | 通用 DT 解析（非 NVDIMM 特定） |
| Platform Device 创建 | kernel/drivers/of/platform.c | of_platform_populate() | - | 为每个 pmem-region 创建 platform_device |
| 资源转换 | kernel/drivers/of/address.c | of_address_to_resource() | - | 将 reg 属性转换为 resource 结构 |
| **OF_PMEM 驱动注册** | drivers/nvdimm/of_pmem.c | module_platform_driver() | 106 | 驱动注册宏 |
| **OF_PMEM 驱动匹配** | drivers/nvdimm/of_pmem.c | of_pmem_region_match[] | 91-95 | compatible 匹配表 |
| **OF_PMEM 驱动探测** | drivers/nvdimm/of_pmem.c | of_pmem_region_probe() | 17 | 核心驱动函数 |
| NVDIMM Bus 注册 | drivers/nvdimm/bus.c | nvdimm_bus_register() | - | 创建 nvdimm_bus |
| **Region 创建（PMEM）** | drivers/nvdimm/region_devs.c | nvdimm_pmem_region_create() | 1105 | 创建 PMEM region |
| **Region 创建（通用）** | drivers/nvdimm/region_devs.c | nd_region_create() | 1000 | 核心 region 创建逻辑 |
| 设备注册 | drivers/base/core.c | device_register() | - | Linux 通用设备注册 |
| **sysfs 属性：resource** | drivers/nvdimm/region_devs.c | resource_show() | 609 | 暴露物理地址 |
| **sysfs 属性：size** | drivers/nvdimm/region_devs.c | size_show() | 254 | 暴露范围大小 |
| **sysfs 属性：mappings** | drivers/nvdimm/region_devs.c | mappings_show() | 294 | 暴露 DIMM 个数 |

### 关键数据结构

| 结构体 | 定义文件 | 说明 |
|--------|---------|------|
| `platform_device` | include/linux/platform_device.h | Bootloader 传递的 DTB 解析成的设备 |
| `struct resource` | include/linux/ioport.h | 物理地址范围资源（来自 reg 属性） |
| `struct nd_region_desc` | include/linux/nd.h | Region 描述符（中间抽象） |
| `struct nd_region` | drivers/nvdimm/nd.h | 内核 Region 对象 |
| `struct device_node` | include/linux/of.h | Device Tree 节点对象 |

---

## 总结

### ARM 平台 NVDIMM-P 地址上报的核心流程

```
Device Tree (pmem-region 节点)
  ↓ of_address_to_resource()
platform_device.resource[] (物理地址范围)
  ↓ of_pmem_region_probe()
nd_region_desc (中间描述符)
  ↓ nvdimm_pmem_region_create() + nd_region_create()
nd_region 对象
  ├─ ndr_start (来自 Device Tree reg 属性)
  ├─ ndr_size  (来自 Device Tree reg 属性)
  ├─ numa_node (从 platform_device 推导)
  └─ ...
  ↓ device_register()
sysfs: /sys/bus/nd/devices/regionX/
  ├─ resource (输出 ndr_start)
  ├─ size    (输出 ndr_size)
  ├─ mappings (输出 ndr_mappings，通常为 0)
  └─ ...
  ↓ 用户查询
用户态应用获得持久内存地址信息
```

### 与 x86 ACPI 路径的关键区别

1. **固件来源**：Device Tree 而非 ACPI NFIT 表
2. **驱动**：OF_PMEM (drivers/nvdimm/of_pmem.c) 而非 ACPI NFIT 驱动
3. **DIMM 映射**：通常无（Device Tree 不提供）
4. **sysfs 接口**：完全相同
5. **用户使用**：完全相同

### 关键洞察

- **Linux 内核的统一抽象**：尽管前期驱动不同，但都汇聚到 `nd_region` 对象和相同的 sysfs 属性
- **架构无关的上层设计**：一旦地址被解析为 `resource`，后续的处理完全相同
- **Device Tree 的优势**：相比 ACPI NFIT，Device Tree 更灵活，支持多种架构和嵌入式系统
- **用户体验统一**：无论 ARM 还是 x86，用户查询持久内存地址的命令完全相同

---

**文档版本**：Linux 6.6 kernel  
**范围**：仅包含 ARM 平台 NVDIMM-P 的地址上报流程  
**参考**：所有代码位置和引用均基于 Linux 6.6 源码
