# kama-webserver 学习与面试复习笔记

> 目标读者：有一定 C++ / Linux 系统编程 / 网络编程基础的同学  
> 项目定位：Muduo 风格的教学型高性能网络服务器骨架（Reactor + epoll + One Loop Per Thread）

## 1. 项目总览（一句话）

这是一个基于 **非阻塞 socket + epoll + 回调驱动** 的多 Reactor 网络框架，主线程负责接入连接，工作线程负责连接读写，并配套了 Buffer、定时器和异步日志模块。

---

## 2. 目录结构与职责

- `include/`：接口与核心抽象
  - 网络核心：`EventLoop`、`Poller/EPollPoller`、`Channel`、`Socket`、`Acceptor`、`TcpServer`、`TcpConnection`
  - 线程模型：`Thread`、`EventLoopThread`、`EventLoopThreadPool`、`CurrentThread`
  - IO 辅助：`Buffer`、`InetAddress`
  - 定时器：`Timer`、`TimerQueue`
  - 日志：`Logger`、`LogStream`、`FixedBuffer`、`AsyncLogging`、`LogFile`、`FileUtil`
  - 扩展模块：`memoryPool.h`、`LFU.h`
- `src/`：网络主干实现和 `main.cc`
- `log/`：日志子系统实现（异步刷盘主逻辑）
- `memory/`：内存池实现
- `CMakeLists.txt`：把网络、日志、内存模块编译链接

---

## 3. 启动流程（从 `main` 开始）

`src/main.cc` 的启动顺序很典型：

1. **初始化异步日志**
   - 创建 `logs` 目录
   - 创建 `AsyncLogging`
   - `Logger::setOutput(asyncLog)` 将默认输出改为异步落盘
   - `log.start()` 启动日志后端线程
2. **初始化扩展组件**
   - 内存池 `memoryPool::HashBucket::initMemoryPool()`
   - LFU 缓存对象（当前示例中仅初始化，未接入请求处理链）
3. **启动网络框架**
   - 创建主循环 `EventLoop loop`
   - 创建 `EchoServer`（内部组合 `TcpServer`）
   - 注册连接/消息回调（`onConnection` / `onMessage`）
   - `setThreadNum(3)`：启动 3 个 subLoop
   - `server.start()`：先启动线程池，再让 `Acceptor` 开始监听
   - `loop.loop()`：进入主事件循环

---

## 4. 运行时主链路（面试重点）

## 4.1 新连接到来

1. `Acceptor` 持有 `listenfd`（非阻塞）和 `acceptChannel`
2. `acceptChannel` 监听读事件，触发 `Acceptor::handleRead()`
3. `accept4` 得到 `connfd`
4. 回调 `TcpServer::newConnection(sockfd, peerAddr)`
5. `EventLoopThreadPool::getNextLoop()` 轮询选一个 subLoop
6. 创建 `TcpConnection`，绑定回调（连接、消息、写完成、关闭）
7. `ioLoop->runInLoop(connectEstablished)` 在所属 IO 线程真正建立连接

## 4.2 接收数据

1. subLoop 的 `epoll_wait` 返回活跃 `Channel`
2. `Channel::handleEvent()` 分发读写关闭错误事件
3. `TcpConnection::handleRead()` 调 `inputBuffer_.readFd()`
4. 读到数据后调用上层 `messageCallback_`
5. Echo 示例里直接 `conn->send(msg)` 回写

## 4.3 发送数据（背压关键）

`TcpConnection::send -> sendInLoop`：

- 快路径：若当前没在监听写事件、输出缓冲为空，先直接 `write`
- 慢路径：没写完就把剩余数据放入 `outputBuffer_`，并 `enableWriting()` 关注 `EPOLLOUT`
- `handleWrite()` 在可写时继续发，发完 `disableWriting()`，避免空转

这就是“**应用层发送快于内核发送能力**”时的背压处理策略。

## 4.4 关闭连接

- 被动关闭：`readFd` 返回 0 -> `handleClose()`
- 主动关闭：`shutdown()` 先进入 `kDisconnecting`，等输出缓冲清空后 `shutdownWrite()`
- 最终走 `TcpServer::removeConnection`，从连接表删除，并在所属 loop 销毁连接

---

## 5. 架构与原理

## 5.1 Reactor 分层

- `Poller/EPollPoller`：只做事件检测（`epoll_wait/ctl`）
- `Channel`：fd 与事件、回调的绑定器
- `EventLoop`：驱动者，循环拿活跃事件并执行回调

职责分离：**检测事件** 和 **处理事件** 解耦。

## 5.2 One Loop Per Thread

- 每个线程持有一个 `EventLoop`
- `TcpConnection` 只在归属 loop 线程内操作
- 跨线程通过 `runInLoop/queueInLoop + eventfd(wakeup)` 通信

优点：减少共享状态竞争，避免粗粒度锁。

## 5.3 主从 Reactor

- mainLoop：只负责 accept 新连接
- subLoop：负责已连接 fd 的读写和关闭
- 分配策略：轮询（`getNextLoop`）

## 5.4 Buffer 原理

`Buffer` 是可扩容线性缓冲，维护 `readerIndex_` 和 `writerIndex_`。

- 读 socket：`readv` 同时写入内部可写区 + 栈上 `extrabuf`
- 写 socket：从 `peek()` 开始写 `readableBytes()`
- 空间不够时：扩容或搬移可读数据

特点：系统调用少、内存复制受控，适合网络 IO。

## 5.5 定时器原理

- `TimerQueue` 基于 `timerfd + Channel + std::set`
- 新定时器通过 `runInLoop` 插入，保证线程安全
- 到期时 `handleRead` 取过期任务并执行回调
- 重复任务 `restart` 后重新插入

## 5.6 日志原理（异步）

- 前端：`LOG_INFO` 宏构造 `Logger`，写入 `LogStream`
- 输出函数被重定向到 `AsyncLogging::append`
- 后端：独立线程批量取缓冲区写 `LogFile`

设计要点：双缓冲/多缓冲 + 生产者消费者，降低业务线程阻塞。

---

## 6. 关键类关系（文字图）

- `main`
  -> `EchoServer`
  -> `TcpServer`
- `TcpServer`
  - 组合：`Acceptor`（mainLoop）
  - 组合：`EventLoopThreadPool`（subLoops）
  - 管理：`connections_`（`TcpConnectionPtr`）
- `TcpConnection`
  - 归属一个 `EventLoop`
  - 组合：`Socket + Channel + inputBuffer + outputBuffer`
  - 通过 `Channel` 接收 epoll 事件并回调业务
- `EventLoop`
  - 组合：`Poller(EPollPoller)`、`wakeupFd/eventfd`、`TimerQueue`
- 日志链路：
  - `Logger -> AsyncLogging -> LogFile -> FileUtil`

---

## 7. 面试高频问答（可直接背）

1. **Q：为什么要 One Loop Per Thread？**  
   A：让连接和事件处理线程归属固定，减少共享数据和锁竞争，跨线程用任务投递。

2. **Q：`runInLoop` 和 `queueInLoop` 区别？**  
   A：`runInLoop` 在本线程可立即执行，`queueInLoop` 一定入队，随后唤醒 loop。

3. **Q：为什么发送完要 `disableWriting`？**  
   A：`EPOLLOUT` 常态可写，不关会造成无意义唤醒与 CPU 空转。

4. **Q：`Channel::tie` 解决什么问题？**  
   A：事件回调期间提升 weak_ptr 为 shared_ptr，防止连接对象提前析构。

5. **Q：为什么 `send` 要分“直接写 + 输出缓冲”两段？**  
   A：直接写降低延迟；写不完时进缓冲并靠 `EPOLLOUT` 续写，处理背压。

6. **Q：`Buffer::readFd` 为什么用 `readv` + `extrabuf`？**  
   A：一次系统调用尽可能多读，减少扩容和二次读取开销。

7. **Q：定时器为什么用 `timerfd`？**  
   A：可统一纳入 epoll 事件循环，不需要额外 sleep 线程。

8. **Q：主从 Reactor 中 mainLoop 的职责是什么？**  
   A：专注 accept 与分发连接，读写工作交给 subLoop。

9. **Q：异步日志比同步日志优势？**  
   A：业务线程不阻塞磁盘 IO，吞吐更稳定，尾延迟更低。

10. **Q：连接关闭为什么要经过 `TcpServer` 统一移除？**  
    A：连接容器在 `TcpServer` 维护，统一删除可保证生命周期和状态一致。

11. **Q：为什么 listen fd 和 conn fd 都要非阻塞？**  
    A：Reactor 需要“就绪通知 + 非阻塞读写”，阻塞 fd 会卡死事件循环。

12. **Q：线程池如何负载均衡？**  
    A：当前实现是轮询分发，简单、低成本、可预期。

---

## 8. 源码中值得注意的风险点（复习加分项）

1. **`EventLoop` 的 `timerQueue_` 仅在头文件声明，构造函数中未见初始化。**  
   若调用 `runAt/runAfter/runEvery` 可能触发空指针问题。

2. **`Acceptor` 构造函数忽略 `reuseport` 形参。**  
   当前直接 `setReusePort(true)`，与 `TcpServer::Option` 配置语义不一致。

3. **`Logger.h` 的 `setOutput/setFlush` 参数命名与赋值变量不一致。**  
   代码写法 `setOutput(OutputFunc) { g_output = out; }` 可疑，建议修正为显式参数名。

4. **`Timestamp::toString()` 使用 `localtime(&microSecondsSinceEpoch_)`。**  
   传入的是微秒计数，不是秒，存在时间转换错误风险。

5. **热点路径日志级别偏高。**  
   `poll`、`updateChannel`、`handleEvent` 中频繁 `LOG_INFO`，高并发下会显著放大开销。

6. **`sendFileInLoop` 通过重复 `queueInLoop` 自旋式续传。**  
   未复用 `EPOLLOUT + outputBuffer` 背压机制，可能导致任务队列堆积。

7. **`TimerQueue::reset` 在循环内多次 `resetTimerfd`。**  
   可以优化为循环结束后按最早过期时间设置一次。

---

## 9. 如何向面试官讲这个项目（30秒口语版）

这个项目是一个 Muduo 思路的高性能 C++ 网络框架，底层用 epoll 做 Reactor，采用 One Loop Per Thread。主线程只做 accept，新连接按轮询分发到多个 IO 线程；每个连接由 TcpConnection 管理，收发通过 Channel + Buffer 驱动，发送支持背压。线程间通过 eventfd 唤醒 loop 处理任务，避免粗锁。项目还实现了 timerfd 定时器和异步日志，整体覆盖了 Linux 网络编程、并发模型和工程化落盘能力。

---

## 10. 复习建议（按优先级）

1. 先背“主流程时序图”：启动 -> accept -> 分发 -> 读 -> 写 -> 关
2. 再背“线程安全模型”：连接归属 + 跨线程投递 + wakeup
3. 准备 3 个细节亮点：`tie` 生命周期保护、`send` 背压、`readv` 双缓冲
4. 准备 2 个改进点：定时器初始化问题、热点日志降级
5. 最后准备一个扩展：如何接入 HTTP 解析/业务线程池/限流
