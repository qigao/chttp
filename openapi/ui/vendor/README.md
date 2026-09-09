# Alpine.js

固定版本：3.14.9，MIT 许可，原始文件未修改。

- 上游：https://github.com/alpinejs/alpine/tree/v3.14.9
- 分发文件：https://cdn.jsdelivr.net/npm/alpinejs@3.14.9/dist/cdn.min.js
- SHA-256：`3ed1eed252488921df65e363d6715deb04d7f92aaedb9e52199fdf73cb1e0ad3`
- 许可证：[LICENSE.alpine.md](LICENSE.alpine.md)

通过本地 HTTP 路由加载，不在 CMake 或页面运行时下载依赖。使用官方浏览器构建，
无 C ABI、线程或原生平台依赖；页面运行于支持 Fetch、Streams、AbortSignal.timeout 的现代浏览器。
Alpine 标准构建使用动态表达式求值，部署 CSP 时需允许 unsafe-eval，不能直接套用严格 CSP。
升级需重新核对上游安全公告、许可证和文件摘要，并运行 UI 回归。此处不声明已完成全面安全审计。
