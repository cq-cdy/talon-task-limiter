# 第三方来源

依赖通过 CMake 引入，不将第三方源码复制进版本库。

| 依赖 | 用途 | FetchContent 固定提交 | 许可证 |
| --- | --- | --- | --- |
| [cq-cdy/talon-status](https://github.com/cq-cdy/talon-status) | 公共 Status / StatusOr 类型 | `aaf333da1e5c9f494d5750f52fd0838aeb4e4205` | Apache-2.0 |
| [google/googletest](https://github.com/google/googletest/tree/v1.14.0) | 仅构建测试 | `f8d7d77c06936315286eb55f8de22cd23c188571`（v1.14.0） | BSD-3-Clause |

`Threads::Threads` 是 CMake 对平台标准线程支持的抽象，不是另一个下载依赖。生产库不链接 GoogleTest、GoogleMock 或 Abseil。

优先复用调用者提供的 target 或 package，因此使用这些覆盖时版本由调用者管理。talon-status package 最低要求 0.1，GoogleTest package 最低要求 1.11。升级固定提交需要重新执行 API、异常路径、sanitizer、安装消费和编译失败测试。

安装时保留 talon-status 自身的安装与许可证规则；使用外部 package 时由其提供者负责安装该依赖。请在分发二进制时同时保留适用的许可证和声明。
