# TaskForge

TaskForge 是一个使用 **C++20** 开发、运行于 Linux 环境的**资源受控任务执行引擎**，主要面向 CI 构建、自动化测试以及其他需要执行外部进程的任务场景。

它关注的不是单纯“启动一个进程”，而是任务执行过程中一系列更完整的工程问题：如何可靠地创建和回收子进程、捕获标准输出与错误输出、处理超时和取消、清理进程树、限制并发与队列压力，以及通过 Linux cgroup v2 对任务进行 CPU、内存和进程数限制。

在此基础上，TaskForge 进一步实现了基于逻辑 CPU / 内存预算的资源准入机制，使任务只有在资源预算允许时才会进入执行阶段，并通过有界输出捕获、运行诊断和压力测试控制任务执行过程中的资源风险。

## 核心特性

- **Linux 进程执行**
  - 基于 `fork / exec` 启动外部程序
  - 使用 pipe 捕获 `stdout / stderr`
  - 基于 `poll` 同时读取多个输出流
  - 使用独立启动错误通道区分 `exec` 失败与程序正常返回 `127`
  - 使用 `waitpid` 完成子进程回收

- **任务生命周期管理**
  - 提供 `queued / ready / starting / running / success / failed / timeout / cancelled` 等任务状态
  - 使用原子操作和 CAS 控制并发状态转换

- **超时与取消**
  - 基于 `std::stop_token` 支持主动取消
  - 基于 `std::chrono::steady_clock` 实现超时控制
  - 处理正常退出、取消和超时之间的竞争关系

- **进程树清理**
  - 为任务建立独立进程组
  - 处理 `setpgid` 的父子进程竞争
  - 支持 `SIGTERM → grace period → SIGKILL` 的分阶段终止
  - 在发送进程组信号前确认 PGID 归属，降低误杀风险

- **并发执行与背压**
  - 固定大小 Worker Pool
  - 有界 FIFO 等待队列
  - 非阻塞 `submit`
  - 队列满时返回 `queue_full`
  - 支持排队任务取消和 drain shutdown

- **cgroup v2 资源控制**
  - 为单个任务创建独立 cgroup
  - 支持配置 `cpu.max`
  - 支持配置 `memory.max`
  - 支持配置 `pids.max`
  - 在任务 `exec` 前完成 cgroup attach，避免任务先运行后限额
  - 读取 `cpu.stat`、`memory.events`、`pids.events` 等运行诊断信息

- **资源感知准入**
  - 使用 `ResourceCapacity` 表示执行器逻辑资源容量
  - 使用 `ResourceRequest` 描述单任务资源需求
  - CPU 与内存资源在调度时原子检查和预留
  - 使用严格 FIFO 策略，不进行 backfill
  - 使用 RAII 保证任务结束后资源预算释放

- **有界输出捕获**
  - `stdout / stderr` 可分别设置保留上限
  - 达到保留上限后仍继续读取 pipe，避免子进程因管道写满而阻塞
  - 同时记录实际输出总量、保留量以及是否发生截断

- **工程质量与验证**
  - CMake + CTest
  - GCC / Clang 双编译器 CI
  - Production Target 使用 `-Wall -Wextra -Wpedantic`，CI 可开启 `-Werror`
  - ASan / LSan / UBSan 检查
  - ThreadSanitizer 并发测试
  - GitHub Actions 中执行真实 cgroup v2 内核集成测试
  - 提供 Release Benchmark 与压力测试
