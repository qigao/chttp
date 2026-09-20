# OpenAPI 源码注释

生成器从函数前相邻的文档注释生成 OpenAPI 3.1.0 JSON/YAML。`@...` 是源码输入约定，
输出关键字遵循 [OpenAPI 3.1](https://spec.openapis.org/oas/v3.1.0) 和
[JSON Schema 2020-12](https://json-schema.org/draft/2020-12/json-schema-validation)。
目前内置 C 语言插件；不从函数参数或结构体自动推断请求 schema。

## HTML / Jinja + HTMX 接口页面

OpenAPI UI 现在采用服务端渲染：`CHttp::Web` 在启动时冻结并编译固定的 Jinja 模板。
`OpenAPI::Generator` 保持与 Web/Jinja 无关；可选的 `OpenAPI::UIModel` 把 OpenAPI 文档投影为 Web-facing CMeta presentation model，并通过 `CHttp::Web` 的 descriptor bridge 获得可渲染的字符串/序列语义。`/docs` 返回完整页面，接口筛选和详情通过 HTMX 请求服务端 fragment。浏览器不再加载
Alpine.js，也不再从 OpenAPI 文档在客户端重建页面。

固定路由包括：

- `/docs`、`/docs/`：完整 Jinja 页面；
- `/docs/operations`：接口列表 fragment，支持 `q` 搜索；
- `/docs/operations/:key`：单个接口详情 fragment；
- `/docs/style.css`、`/docs/htmx.js`、`/docs/tryit.js`：本地静态资源；
- `/openapi.json`：原始 OpenAPI JSON。

模板名和静态文件路径均在服务启动时固定，不由请求参数决定；不提供目录浏览、任意模板名、
任意文件路径或服务端 Try-it 代理。HTMX 固定为本地 vendored v4.0.0，来源与许可证见
[HTMX.md](ui/vendor/HTMX.md) 和
[htmx-4.0.0.LICENSE](ui/vendor/htmx-4.0.0.LICENSE)。

在已配置的开发环境中（Windows 使用 VsDevCmd 并保留对应 Salts/vcpkg runtime PATH）：

```powershell
cmake --build --preset win-release-user --target openapi_ui_server openapi_examples
./build/Msvc-Release/bin/openapi_ui_server.exe ./build/Msvc-Release/openapi/examples/openapi.json ./openapi/ui 8087
```

打开 `http://127.0.0.1:8087/docs`。端口省略或传入 0 时由系统分配，终端打印实际地址；
按 Enter 停止服务。示例只监听 loopback。启动阶段读取并校验六个固定模板和静态资源；
模板单文件上限 64 KiB，文档/静态文件上限 2 MiB，操作数上限 4096。运行期间资源应保持不变，
更新文档或模板后重启服务。

Try it 由本地 [tryit.js](ui/tryit.js) 在浏览器中执行，不经过 CHTTP 服务端代理。
默认使用文档的 `servers[0].url`，也允许用户修改服务地址；只有点击发送后才发出请求。
当前支持默认序列化的标量 path/query/header 参数和文本/JSON 请求体；复杂参数、Cookie 参数、
TRACE/CONNECT、GET/HEAD 请求体、requestBody `$ref` 等不支持路径会 fail closed。
请求不携带浏览器凭据（`credentials: omit`），不跟随重定向，跨域请求遵循浏览器 CORS。
请求体上限 64 KiB，展示响应上限 1 MiB，请求超时 15 秒；限制集中在
[tryit.js](ui/tryit.js) 的 `LIMITS`。

页面内容依赖 `CHttp::Web` 提供的 Jinja autoescape；title、summary、description、schema/request/response JSON
均按文本输出。当前模板不包含 Alpine `x-*` 指令、inline `<script>` 或 HTMX `hx-on*`
表达式。示例服务器通过 `CHttp::Web` security middleware 统一发送 `X-Content-Type-Options: nosniff`，
但不默认发送 Content-Security-Policy：strict same-origin reference CSP 的 `connect-src 'self'` 会改变
当前允许用户输入跨域服务地址的 browser-only Try-it 边界。部署方如启用 CSP，应按实际服务地址显式配置并验证。

```powershell
cmake --build --preset win-release-user --target openapi_ui_server test_generator chttp_file_transfer_test
ctest --preset win-release-user -R "^(generator(_conformance)?|openapi_ui_.*|chttp_file_transfer_test)$" --output-on-failure
```

UI 回归覆盖完整页/fragment、搜索与非法 key、Jinja escaping、请求编码和 Try-it fail-closed
约束、固定静态路由、缺失/非法/超限模板或资源、顺序 render 状态恢复以及 server restart。
最终 UI 依赖面保持隔离：`OpenAPI::Generator` 与 `CHttp::Server` 都不能到达 `CHttp::Web`/Jinja；
`OpenAPI::UIModel` 是显式的 presentation-layer bridge，并只通过 `CHttp::Web` 间接进入 `Salts::JinjaCMeta`。
OpenAPI production 源码不直接 include 或链接 Jinja runtime，HTMX 仍仅作为示例 UI 的本地静态资源。

## 声明与约束

以下注释可放在 C 函数定义之前；完整可编译示例见 [pets.c](examples/pets.c)。

```c
/**
 * @route POST /pets
 * @summary Create a pet
 * @body application/json object required Pet to create
 * @field name string required Pet name
 * @minLength body.name 1
 * @maxLength body.name 80
 * @response 201 Pet created
 */
```

声明先于约束。已有 `@route`、`@param`、`@body`、`@field`、`@response`、`@produces`、
`@operationId`、`@summary`/`@brief`、`@description`/`@details`、`@tag`、`@deprecated` 保持可用。

| 声明 | 参数 |
| --- | --- |
| `@route` | `METHOD /path` |
| `@param` | `name query\|path\|header\|cookie type required\|optional description` |
| `@body` | `media-type type required\|optional description` |
| `@field` | `name type required\|optional description`，声明当前对象正文的直接属性 |
| `@response` | `status description` |
| `@produces` | `status media-type type`，先声明对应 response |

类型支持 `string`、`integer`、`number`、`boolean`、`object` 及最多八层 `[]` 数组；
保留 `char*`、`int`、`float`、`double`、`bool` 别名。
`@body` 和 `@produces` 的媒体类型按 [HTTP token 与参数语法](https://www.rfc-editor.org/rfc/rfc9110.html#section-8.3.1)
校验，支持 `application/vnd.api+json`、`text/*`、`text/plain;charset="utf-8"` 等，原样保存 content 键。
当前注释参数由空白分隔，因此媒体参数必须紧凑书写，不支持其中包含空白（包括引号内空白）。
不要在 C 块注释中直接写 `*/*`，其中的 `*/` 会提前结束注释。
非法分隔符、缺失参数值、未闭合引号会报错；不查询 IANA 注册表，也不规范化大小写或参数顺序。
路径参数必须 required，并与路由中的 `{name}` 双向对应；未声明、额外声明或不完整的花括号均报错。
header 参数名遵循 [HTTP field-name](https://www.rfc-editor.org/rfc/rfc9110.html#section-5.1)，
cookie 参数名遵循 [Cookie name](https://www.rfc-editor.org/rfc/rfc6265.html#section-4.1.1) 的 token 语法：
允许 ASCII 字母、数字及 token 标点，拒绝冒号、等号、控制字符及非 ASCII 字符。
该限制仅用于 header/cookie；名称和大小写原样输出，query/path 不套用 token 限制。
`/pets/{id}` 和 `/pets/{name}` 这类同构路径不能并存，即使 HTTP 方法不同；
同一路径上的不同方法和 `/pets/mine` 这样的具体路径可以共存。
生成器保留“至少声明一条 response”的项目要求；OpenAPI 3.1 本身并未把 Operation.responses 列为必填。
正文属性的 required 生成父 schema 的 `required` 数组，
不生成属性级布尔 required，也不暗中要求非空字符串。

约束格式为 `@关键字 目标 值`：

| 关键字 | 目标类型 | 值 |
| --- | --- | --- |
| `minimum`、`maximum`、`exclusiveMinimum`、`exclusiveMaximum` | integer / number | 有限数值；exclusive 系列使用 3.1 数值语义 |
| `multipleOf` | integer / number | 大于零的有限数值 |
| `minLength`、`maxLength` | string | 非负十进制整数 |
| `pattern` | string | 行内剩余文本，保留内部空格和反斜杠 |
| `format` | 任意已声明类型 | 格式名称，作为 schema 注解输出 |
| `minItems`、`maxItems` | array | 非负十进制整数 |
| `uniqueItems` | array | `true` / `false` |
| `minProperties`、`maxProperties` | object | 非负十进制整数 |
| `additionalProperties` | object | `true` / `false`；当前不支持 schema 形式 |
| `enum` | string / integer / number / boolean | 每行一个同类型值，重复注解追加枚举成员 |

目标使用 `query.limit`、`path.id`、`header.X-Key`、`cookie.session`、`body` 或 `body.name`。
尾随 `[]` 定位数组元素，例如 `body.tags[]`、`body[][]`；这里只定位已声明的 schema，
不会创建缺失属性。`@field` 暂不声明嵌套对象属性。裸参数名保持兼容，但跨位置同名时必须限定位置。
`body` 是保留目标，始终指向请求正文；名为 body 的参数必须使用 `query.body`、
`header.body` 等限定位置，避免目标随声明上下文改变。

字符串枚举取行内剩余文本，不需要引号，例如 `@enum body.status in progress`。
整数、数值和布尔枚举输出对应 JSON 类型；整数枚举只接受十进制整数文本（可带负号），
范围为 ±9007199254740991。每个 enum 最多 256 个成员，重复成员报错。当前不支持空字符串枚举、
null、对象或数组枚举。除 enum 外，同一目标重复设置同一关键字报错。

长度/数量值上限为 9007199254740991，避免超过现有数值比较的精确整数范围。
普通数值约束和 number 枚举使用 JSON 十进制数语法，保留原始数值文本，避免经 double 往返改变约束。
不接受 `+2`、`0x10`、`01` 这类非 JSON 数值文本；溢出和下溢报错。
范围检查仍受现有有限 double 比较范围限制；number 枚举中按 double 比较不可区分的成员会被拒绝，
不承诺任意精度的枚举去重。整数枚举及数量约束仍遵循上文的精确整数上限。
YAML 指数数值附加 `!!float` 标签，避免某些读取器将 `1e3` 解释为字符串。
字符串长度的标准含义是字符数，不是 UTF-8 字节数；
本工具只生成约束，不计算请求值的长度。pattern 按标准的 ECMA-262 语义书写，
生成阶段不编译正则；format 不表示已经执行格式校验。

未知关键字、缺失目标、类型不匹配、非法数值和重复约束会立即返回带函数名及注释行号的错误。
一次源码添加失败时，已有文档保持不变。这里没有请求校验 middleware 或切面操作。
JSON/YAML 是输出格式，不接受嵌入式 `@openapi { ... }` 输入。

## 构建与验证

Tree-sitter 和 tree-sitter-c 由仓库 `vcpkg.json` 安装。CMake 使用
`find_package(unofficial-tree-sitter CONFIG REQUIRED)` 和 `find_library(... REQUIRED)`；
没有源码下载、替代依赖或自动降级。Salts 依赖沿用仓库 preset。

在 MSVC 开发环境中，按根目录 README 设置使用正斜杠的环境路径后运行：
OpenAPI 测试还需要 Python 3、Node.js（PATH 中的 `node`）及以下 Python 测试依赖。
缺少依赖时测试失败，不跳过一致性检查；它们不进入生成器或 HTTP 库的运行时依赖。

```powershell
python -m pip install -r openapi/tests/requirements-conformance.txt
cmake --preset win-release-user -DBUILD_OPENAPI=ON
cmake --build --preset win-release-user --target test_generator openapi_examples
ctest --preset win-release-user -R '^generator(_conformance)?$' --output-on-failure
```

生成器位于 `build/Msvc-Release/bin/openapi-gen.exe`，插件为同目录的 `openapi_c.dll`。
生成示例位于 `build/Msvc-Release/openapi/examples/openapi.json` 和 `openapi.yaml`。
OpenAPI 工具当前只在构建目录提供，不改变 HTTP 库的安装接口。

测试检查标准 schema 的字段、类型和定位、JSON/YAML 一致性、非法输入的事务性失败以及旧注释兼容性。

`generator_conformance` 从注释实际生成 44 个操作，使用固定的官方 OpenAPI 文档 schema，
再独立检查 79 个 Schema Object、2 个 JavaScript 正则和 66 个实例边界。
包含全部已支持约束、8 个 HTTP 方法、4 个参数位置、类型别名、数组元素、
required、范围响应和 default 响应。测试不联网；官方 schema 来源和校验和见 [schema/README.md](tests/schema/README.md)。
另有 8 个反例证明校验器确实拒绝错误结构、schema、参数名、路径语义和正则，
其中一个正则反例来自生成器实际接受的注释输入。

也可以检查本生成器输出的具体 JSON/YAML 文件，失败时返回非零退出码：

```powershell
python openapi/tests/conformance.py --document build/Msvc-Release/openapi/examples/openapi.json
python openapi/tests/conformance.py --document build/Msvc-Release/openapi/examples/openapi.yaml
```

此入口仍限定本生成器的子集，不是对任意外部 OpenAPI 文档的通用校验器。

源码（包括未标注的部分）、标题和版本必须为有效 UTF-8；不自动转码或替换非法字节。
源码编码错误返回从 0 开始的字节偏移，且不改变已有文档。原先使用其他编码的源码需先转换为 UTF-8。
本地 `generator` 回归覆盖中文、四字节 Unicode 字符，以及孤立续字节、过长编码、代理码点、
超出 Unicode 范围和截断序列；同时检查 JSON/YAML 往返与失败后的文档一致性。

此检查针对当前生成器的文档子集，不是完整 OpenAPI 3.1 认证：不涵盖尚未实现的 `$ref`、
组合 schema、security 或参数序列化选项。format 保持 annotation 语义，不执行格式断言。
正则语法检查由测试中的 JavaScript `RegExp`（无 flags）执行，生成器自身仍只保存 pattern 文本。
测试区分规范错误与无解约束：例如 minimum 大于 maximum 仍是合法 schema，只是没有满足它的数值。
