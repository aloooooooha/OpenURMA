# versions/urpc：UB 模拟链路源码

官方 URPC RC framework 所用的旧 RTP 模拟集成，包含 JFR 查询/flush 适配。

已验证源码基线：`9b74b0c82684a31e756b5085662707fbe9bfbe10`。本次由 aloooooooha 发布索引，不重写历史、不改已验证源码。

- [配套 SCache](https://github.com/ultra-shuffle/SCache/tree/versions/urpc)
- [共用 Spark](https://github.com/ultra-shuffle/spark-3.5-scache/tree/versions/scache-shared)
- [集成目录](integration/umdk/)
- [provider 操作实现](integration/umdk/provider/openurma_provider.c)：队列、MR、SEND/READ/WRITE、CQ 等模拟接口。
- [fake device 创建](integration/umdk/tools/openurma_mkdev.sh)。

真实硬件运行由厂商 liburma/provider/内核处理，不使用这个模拟 provider。
模拟通过不代表现场 UBAGG/UBMAD 或 B103 ABI 已通过。
原版、URPC、UMQ 分别位于 versions/direct-urma、versions/urpc、versions/umq。
