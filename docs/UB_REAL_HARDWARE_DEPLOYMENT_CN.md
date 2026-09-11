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

```bash
mkdir -p /opt/ultrashuffle-src
cd /opt/ultrashuffle-src

git clone --depth 1 -b freeze/ub-ultrashuffle-phase8-20260720 \
  https://github.com/ultra-shuffle/SCache.git

git clone --depth 1 -b freeze/ub-ultrashuffle-phase8-20260720 \
  https://github.com/ultra-shuffle/spark-3.5-scache.git

# 仅用于接口与验证资料，真机运行不依赖其模拟 provider
git clone --depth 1 -b freeze/ub-ultrashuffle-phase8-20260720 \
  https://github.com/aloooooooha/OpenURMA.git
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
cd /opt/ultrashuffle-src/SCache
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk
export OU_UMDK_BUILD=/opt/umdk/build
export SPARK_URMA_INSTALL_PREFIX=/opt/ultrashuffle

native/spark_urma/build_spark_urma.sh
ldd /opt/ultrashuffle/lib/libSparkUrma.so
sbt publishM2
scripts/build-release.sh
```

`ldd` 不得出现 `not found`，且必须加载真机 `liburma.so`。

### 4.3 构建 Spark

```bash
cd /opt/ultrashuffle-src/spark-3.5-scache
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
spark.scache.jars=<SCache-assembly.jar>
spark.driver.extraLibraryPath=/opt/ultrashuffle/lib:/opt/umdk/lib
spark.executor.extraLibraryPath=/opt/ultrashuffle/lib:/opt/umdk/lib
```

当前适配层若仍要求兼容字段，两端分别配置 `listen`/`connect`，并令配置与环境变量一致：

```bash
export OPENURMA_WIRE_ROLE=<listen-or-connect>
export OPENURMA_WIRE_PATH=hardware
```

## 6. 启动与验收

```bash
cd /opt/ultrashuffle-src/SCache
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
