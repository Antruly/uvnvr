# uvp 使用说明

UvNVR 使用 **uvp-agents 0.4.0** 作为开发期脚手架和验证工具。uvp 不进入运行时，生成的代码就是普通的 libuvcpp C++ / Vue 3 工程，可以直接用 CMake 和 npm 构建。

## 已使用的 uvp 能力

### 1. 工作区生成

通过 uvp MCP 的 `uvp_workspace_create` 生成：

- 模板：`rest-api`
- libuvcpp 特性：`tls`、`http2`、`websocket`、`soap`、`db`
- 前端：`vue3`
- 端口：`8080`
- 生成的清单：`uvp.json`

生成内容包括：

```text
CMakeLists.txt
src/main.cpp
uvp.json
frontend/                 # Vue 3 + Vite 起步工程
```

生成后做了以下项目化改造：

- 删除模板的 `/items` 示例表和 CRUD 路由，换成 `/healthz` 与 `/api/v1/system/info`。
- CMake 升级到 C++20，加入版本头文件、libuvcpp 1.8.1 固定路径、安装规则。
- 前端升级为 TypeScript，加入 Vue Router、Pinia、vue-i18n、Naive UI、ECharts、mpegts.js。
- 加入首次安装、鉴权、通道、录像等模块的目录规划。

这符合 uvp 的定位：生成的代码是起点，不是必须保留的成品。

### 2. 环境检查

`uvp_doctor` 用于确认本机工具链和 libuvcpp 构建能力。它当时报告：

- Linux x64、GCC 13、CMake 3.28、Ninja 可用；
- 本机已安装 libuvcpp 1.8.0；
- 项目随后固定使用 libuvcpp **1.8.1** 的 Linux x64 Release 归档。

### 3. 构建与接口验证

`uvp_verify` 对工作区执行：

1. CMake 配置与构建；
2. 启动服务；
3. 发起 HTTPS 请求；
4. 校验状态码和 JSON 字段；
5. 停止服务并返回证据包。

M0 的验证结果：

```text
status: verified
build: succeeded
service.ready: true
GET /healthz                 200
GET /api/v1/system/info      200
assertions_passed: 5
assertions_failed: 0
```

为了在本地验证 HTTPS/HTTP2 路径，服务增加了仅开发使用的 `--tls-self-signed` 开关：

```bash
./build/uvnvr --port 8080 --tls-self-signed
```

生产环境仍使用 `UVNVR_TLS_CERT` / `UVNVR_TLS_KEY`。

## 复现方式

```bash
# 安装/运行 uvp 0.4.0（开发机已有 MCP 服务时可不执行）
npx uvp-agents@0.4.0 setup
npx uvp-agents@0.4.0 doctor

# 仓库本身不依赖 uvp 才能构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVNVR_LIBUVCPP_ROOT=/path/to/libuvcpp-1.8.1-linux-x64
cmake --build build -j

# 前端
cd frontend && npm ci && npm run build
```

`uvp.json` 记录了生成时的模板、特性和二进制名，供 uvp 工具识别工作区；CI 不要求安装 uvp。

## 后续如何使用 uvp

- M1 用 uvp 模板/模块组织认证、RBAC、审计、系统设置的骨架，再落到本仓库的模块边界。
- 每个里程碑用 `uvp_verify` 做一次“构建 + 启动 + 接口断言”的回归。
- 把可复用的后台模块和前端模块整理成 `uvp.template/1` 包，向
  [Antruly/uvp-templates](https://github.com/Antruly/uvp-templates) 提 PR。
- 发现 libuvcpp 的流式响应、WebSocket 广播、HTTP Range、异步文件接口等缺口时，先在本项目做适配层，再向上游提 issue/PR。

## 版本基线

| 组件 | 版本 |
|---|---|
| uvp-agents | 0.4.0 |
| libuvcpp | 1.8.1 |
| Node.js | 24.x（CI） |
| Vue | 3.5.x |
| CMake | 3.24+ |
| C++ | C++20 |
