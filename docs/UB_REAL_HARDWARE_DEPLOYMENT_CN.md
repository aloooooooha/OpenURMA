# Spark + SCache 真 UB 设备部署要点

## 1. 部署架构

```text
Spark 3.5.7
  → SCache UB distributed pool
  → libSparkUrma.so
  → 官方 UMDK / liburma
  → 厂商 URMA provider
  → uburma / ubcore / 厂商驱动
  → 真实 UB 网卡
```

真机不使用 `openurma_vdev`、timed-NUMA、Tier-S shim 或 SystemC provider。

## 2. 环境要求

| 项目 | 要求 |
|---|---|
| 服务器 | 至少 2 台，CPU 架构和 NUMA 拓扑尽量一致 |
| UB | 每台至少 1 个 UB 设备，接入同一 UB fabric |
| 系统 | 厂商认证的 openEuler/OLK；kernel、firmware、UMDK、provider 和驱动版本匹配 |
| 运行环境 | JDK 17、Spark 3.5.7、Hadoop 3.3.4、Scala 2.13 |
| 构建环境 | GCC/G++ 11+、CMake 3.16+、Maven、sbt 1.11.7、Git、numactl |
| NUMA | UB 网卡、SCache Client 和 registered arena 绑定到同一 NUMA 节点 |
| 内存 | 需覆盖 Spark heap、SCache JVM 和 UB arena；arena 按峰值 Shuffle 数据量配置 |

## 3. 获取代码

先按目标机实际目录设置路径；以下值只是示例，可放在任意本地磁盘或共享目录：

```bash
export UB_DEPLOY_ROOT=/data/ultrashuffle
export UB_SRC_ROOT=${UB_DEPLOY_ROOT}/src
export UB_INSTALL_ROOT=${UB_DEPLOY_ROOT}/runtime
export UB_SCACHE_SRC=${UB_SRC_ROOT}/SCache
export UB_SPARK_SRC=${UB_SRC_ROOT}/spark-3.5-scache
export UB_OPENURMA_SRC=${UB_SRC_ROOT}/OpenURMA
export UB_JAVA_HOME=/path/to/jdk17
export UB_UMDK_BUILD=/path/to/umdk/build

mkdir -p "${UB_SRC_ROOT}" "${UB_INSTALL_ROOT}"

git clone --depth 1 -b freeze/ub-ultrashuffle-phase8-20260720 \
  https://github.com/ultra-shuffle/SCache.git "${UB_SCACHE_SRC}"

git clone --depth 1 -b freeze/ub-ultrashuffle-phase8-20260720 \
  https://github.com/ultra-shuffle/spark-3.5-scache.git "${UB_SPARK_SRC}"

# 仅用于接口与验证资料，真机运行不依赖其模拟 provider
git clone --depth 1 -b freeze/ub-ultrashuffle-phase8-20260720 \
  https://github.com/aloooooooha/OpenURMA.git "${UB_OPENURMA_SRC}"
```

`OpenURMA` 和 `SCache` 已公开。修改版 Spark 仓库需由 `ultra-shuffle` 组织管理员开放；开放前使用交付方提供的冻结源码包或二进制包，不能用原版 Spark 替代。

## 4. 部署步骤

### 4.1 安装并检查 UB 栈

按设备厂商说明安装 firmware、`ubcore`、`uburma`、设备驱动、UMDK/liburma 和 provider，然后检查：

```bash
lsmod | grep -E 'ubcore|uburma'
ls -l /dev/uburma/
urma_admin show
ldconfig -p | grep liburma
numactl --hardware
```

两台机器必须先通过厂商 URMA READ/WRITE 测试，再部署 Spark。

### 4.2 构建 JNI 和 SCache

```bash
cd "${UB_SCACHE_SRC}"
export JAVA_HOME=${UB_JAVA_HOME}
export OU_UMDK_BUILD=${UB_UMDK_BUILD}
export SPARK_URMA_INSTALL_PREFIX=${UB_INSTALL_ROOT}

native/spark_urma/build_spark_urma.sh
ldd "${UB_INSTALL_ROOT}/lib/libSparkUrma.so"
sbt publishM2
scripts/build-release.sh
```

`ldd` 不得出现 `not found`，且必须加载真机 `liburma.so`。

### 4.3 构建 Spark

```bash
cd "${UB_SPARK_SRC}"
./build/mvn -DskipTests -Phadoop-3 -Pscala-2.13 package
```

将修改版 Spark、SCache assembly JAR、`libSparkUrma.so`、UMDK 和 provider 同步到所有节点。

## 5. 核心配置

SCache：

```properties
scache.master.ip=<master-ip>
scache.master.port=6388
scache.client.port=15678
scache.blockTransfer.backend=ub
scache.ub.transport=real
scache.storage.network.enabled=false

spark.urma.strict=true
spark.urma.device.listen=<urma-device>
spark.urma.device.connect=<urma-device>
spark.urma.queueDepth=64
spark.urma.chunkSize=10485760
spark.urma.controlPort=18080
spark.urma.controlBindHost=<local-management-ip>
spark.urma.controlAdvertiseHost=<local-management-ip>
spark.urma.arenaBytes=268435456
spark.urma.arenaCount=<按容量计算>
```

Spark：

```properties
spark.scache.enable=true
spark.scache.strict=true
spark.scache.shuffle.noLocalFiles=true
spark.shuffle.useOldFetchProtocol=true
spark.scache.productionFallback.enabled=false
spark.scache.home=<SCache实际安装目录>
spark.scache.jars=<SCache-assembly.jar>
spark.driver.extraLibraryPath=<JNI库目录>:<UMDK库目录>:<UMDK公共库目录>
spark.executor.extraLibraryPath=<JNI库目录>:<UMDK库目录>:<UMDK公共库目录>
```

当前适配层若仍要求兼容字段，两端分别配置 `listen`/`connect`，并令配置与环境变量一致：

```bash
export OPENURMA_WIRE_ROLE=<listen-or-connect>
export OPENURMA_WIRE_PATH=hardware
```

## 6. 启动与验收

```bash
cd "${UB_SCACHE_SRC}"
sbin/start-scache.sh
```

验收只看以下结果：

1. `urma_admin show` 能识别真实 UB 设备；
2. Spark 输出与 Vanilla Spark 一致；
3. URMA 传输字节非零且 submitted/completed 一致；
4. TCP/Netty Shuffle payload 和 fallback 均为 0；
5. 无 timeout、failed chunk、token/generation error 和 task retry；
6. 作业后 lease、MR import、mapping、token 和 inflight operation 归零；
7. 完成跨节点、故障恢复、NUMA 和 24 小时稳定性测试。

现有性能数据来自软件 vdev/timed-NUMA。真实设备性能以现场验收为准。
