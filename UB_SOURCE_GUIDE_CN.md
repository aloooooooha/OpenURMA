# versions/umq：UB 模拟链路源码

六月官方 UMQ RM/CTP verbs 功能模拟 profile；不是 CTP 硬件报文/重传/时序模型。

已验证源码基线：`146289c5f36c06472538e0c682888aa745657866`。本次由 aloooooooha 发布索引，不重写历史、不改已验证源码。

- [配套 SCache](https://github.com/ultra-shuffle/SCache/tree/versions/umq)
- [共用 Spark](https://github.com/ultra-shuffle/spark-3.5-scache/tree/versions/scache-shared)
- [集成目录](integration/umdk/)
- [provider 操作实现](integration/umdk/provider/openurma_provider.c)：队列、MR、SEND/READ/WRITE、CQ 等模拟接口。
- [功能连线](integration/umdk/provider/openurma_nic_functional.cpp)：RM/CTP 功能模拟，旧 RTP flit 提交为失败。
- [六月 TLV 查询适配](integration/umdk/shim/openurma_ioctl_624.c)：仅模拟 fake device 的设备能力查询。
- [fake device 创建](integration/umdk/tools/openurma_mkdev.sh)。

真实硬件运行由厂商 liburma/provider/内核处理，不使用这个模拟 provider。
模拟通过不代表现场 UBAGG/UBMAD 或 B103 ABI 已通过。
原版、URPC、UMQ 分别位于 versions/direct-urma、versions/urpc、versions/umq。
