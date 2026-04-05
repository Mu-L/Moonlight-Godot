# Notes

## （大致）架构图

![架构图](Moonlight-Godot.svg)

## Moonlight 客户端调用的服务端 API 

### ✅ 1. `/serverinfo`
> **用途**：获取主机基本信息、状态、HTTPS 端口、配对状态、当前游戏等。  
> **调用时机**：连接前探测、状态轮询、配对前准备。  
> **请求方式**：GET  
> **关键参数**：
- `uniqueid=0123456789ABCDEF`
- `uuid=<随机UUID>`

> **返回 XML 示例**：
```xml
<root status_code="200">
  <hostname>Html5syt</hostname>
  <appversion>7.1.431.-1</appversion>
  <GfeVersion>3.23.0.74</GfeVersion>
  <uniqueid>F98186B1-DE02-2328-279E-0A461FBCEC47</uniqueid>
  <HttpsPort>47984</HttpsPort>
  <ExternalPort>47989</ExternalPort>
  <MaxLumaPixelsHEVC>1869449984</MaxLumaPixelsHEVC>
  <mac>00:00:00:00:00:00</mac>
  <LocalIP>127.0.0.1</LocalIP>
  <ServerCodecModeSupport>196865</ServerCodecModeSupport>
  <PairStatus>0</PairStatus>
  <currentgame>1191261554</currentgame>
  <state>SUNSHINE_SERVER_BUSY</state>
</root>
```

> **客户端用途**：
- 获取 `HttpsPort`（用于后续 HTTPS 请求）
- 检查 `PairStatus` 是否已配对
- 检查 `ServerCodecModeSupport` 确认服务器支持的编码格式（H.264/HEVC/AV1）
- 判断是否正在串流（`state` 是否含 `_SERVER_BUSY`）

### ✅ 2. `/launch` 或 `/resume`
> **用途**：启动指定应用（`/launch`）或恢复已有会话（`/resume`）并建立串流。  
> **调用时机**：用户点击“开始游戏”时。若 `currentgame` 非 0，客户端通常调用 `/resume`。  
> **请求方式**：GET  
> **关键参数**（全部在 query string 中）：

| 参数                      | 说明                                             | 必须/随机生成/可选（默认参数） |
| -------------------------| ------------------------------------------------ | ------------------ |
| `appid`                   | 应用 ID（Launch 必需，Resume 用于验证）          |必须
| `mode`                    | 分辨率与帧率，格式 `WxHxFPS` (如 `1920x1080x60`) |可选
| `sops`                    | 是否启用屏幕优化/切换 (0/1)                      |可选
| `rikey`                   | 远程输入 AES 密钥（hex，128-bit）                |随机生成
| `rikeyid`                 | AES IV 种子（大端整数）                          |随机生成
| `localAudioPlayMode`      | 音频播放位置（1=主机播放，0=客户端播放）         |可选，默认1
| `surroundAudioInfo`       | 音频通道配置（如立体声/5.1/7.1）                 |可选
| `surroundParams`          | 额外的环绕声参数                                 |可选
| `remoteControllersBitmap` | 手柄位图掩码                                     |可选
| `gcmap`                   | 手柄映射掩码                                     |可选
| `additionalStates`        | 附加状态标志（固定 1）                           |可选
| `corever`                 | 协议版本（>=1 启用 RTSP 控制流加密）             |可选
| `continuousAudio`         | 持续音频（0/1）                                  |可选
| `hdrMode`                 | 启用 HDR（1=启用，可选）                         |可选，默认为0
| `clientHdrCapVersion`         | HDR 能力参数（当 HDR 启用时必需传递）                |可选，默认为0
| `clientHdrCapSupportedFlagsInUint32` | HDR 支持标志（当 HDR 启用时必需传递）               |可选，默认为0
| `clientHdrCapMetaDataId` | HDR 元数据 ID（当 HDR 启用时必需传递）                |可选，默认为NV_STATIC_METADATA_TYPE_1
| `clientHdrCapDisplayData` | HDR 显示数据（当 HDR 启用时必需传递）                |可选，默认为0x0x0x0x0x0x0x0x0x0x0

> **返回 XML 关键字段**：
```xml
<root status_code="200">
  <sessionUrl0>rtsp://192.168.x.x:48010/xxx</sessionUrl0>
  <udpPort>48012</udpPort>
  <tcpPort>48014</tcpPort>
</root>
```

> **客户端用途**：
- 提取 `sessionUrl0` 用于 RTSP 连接
- 若 `corever >= 1`，使用 `rikey` 对 RTSP 控制通道进行加密

### ✅ 3. `/cancel`
> **用途**：终止当前串流会话（退出游戏）。  
> **调用时机**：用户点击“退出”或断开连接。  
> **请求方式**：GET  
> **参数**：仅 `uniqueid` 和 `uuid`  
> **返回**：成功返回 `<root status_code="200">`  
> **注意**：若非启动者调用，GFE 可能仍返回 200，需再查 `/serverinfo` 确认 `currentgame` 是否清零。

### ✅ 4. `/applist`
> **用途**：获取主机上可串流的应用列表。  
> **调用时机**：连接成功后加载游戏列表。  
> **请求方式**：GET  
> **返回 XML 示例**：
```xml
<root status_code="200">
  <App>
    <AppTitle>Milthm</AppTitle>
    <ID>705288528</ID>
    <IsHdrSupported>0</IsHdrSupported>
    <IsAppCollectorGame>0</IsAppCollectorGame>
  </App>
  <App>
    <AppTitle>桌面</AppTitle>
    <ID>1191261554</ID>
    ...
  </App>
</root>
```

> **客户端用途**：填充 UI 中的游戏/应用列表。

### ✅ 5. `/appasset`
> **用途**：获取应用的封面图（Box Art）。  
> **调用时机**：显示游戏列表时加载图标。  
> **请求方式**：GET  
> **参数**：
- `appid` (必需，对应 applist 中的 ID)
- `AssetType` (可选)
- `AssetIdx` (可选)

> **返回**：PNG/JPEG 图像二进制数据（非 XML）  
> **客户端用途**：显示游戏封面。

### ✅ 6. （隐式）SSL/TLS 双向认证（mTLS）
> 虽然不是独立 API，但所有 HTTPS 请求（`/serverinfo`, `/launch`, `/applist` 等）都要求：
- 客户端提供有效的 **客户端证书 + 私钥**（CN = `NVIDIA GameStream Client`）
- 服务端验证该证书（Sunshine/GFE 默认接受此 CN 的自签名证书）

### 🔒 安全与认证机制总结

| 机制                    | 说明                                                             |
| ----------------------- | ---------------------------------------------------------------- |
| **配对（Pairing）**     | 通过 `/pair`（未在此类中实现，但在其他模块）完成，生成客户端证书 |
| **mTLS**                | 所有 HTTPS 请求必须携带有效客户端证书                            |
| **Server Cert Pinning** | 客户端保存服务端证书（`m_ServerCert`），用于 SSL 错误处理中比对  |
| **Unique ID**           | 固定 `uniqueid=0123...` 允许多个 Moonlight 实例互相接管会话      |

### 📌 总结：Moonlight 调用的服务端 API 列表

| API 路径      | 方法 | 必需      | 主要用途                              |
| ------------- | ---- | --------- | ------------------------------------- |
| `/serverinfo` | GET  | ✅         | 获取主机状态、HTTPS 端口、配对状态    |
| `/launch`     | GET  | ✅         | 启动应用，获取 RTSP 会话 URL 和流端口 |
| `/cancel`     | GET  | ✅         | 退出当前应用                          |
| `/applist`    | GET  | ✅         | 获取可串流应用列表                    |
| `/appasset`   | GET  | ❌（可选） | 获取应用封面图                        |
| （隐式）mTLS  | —    | ✅         | 所有 HTTPS 请求的身份认证             |

> ⚠️ 注意：`/pair`、`/unpair` 等配对接口虽未在此 `NvHTTP` 类中直接调用，但属于完整 GameStream 协议的一部分，通常在 `PairingManager` 或类似模块中实现。


## 🔐 配对流程概览

NVIDIA GameStream 的配对是一个 **多阶段交互式认证过程**（在此实现中分为 0~5 共 6 个阶段），目的是：
1. 安全交换客户端证书
2. 验证用户输入的 PIN 码
3. 防止中间人攻击（MITM）
4. 最终建立双向 TLS 信任（mTLS）

整个过程全部通过 `/pair` 接口的不同参数组合完成（第 0 阶段除外），并在失败时调用 `/unpair` 清理状态。

## ✅ 服务端 API 列表（配对部分）

### 0. **`/serverinfo` (Preflight)**
> **用途**：第 0 阶段 — 预检查。在开始正式配对前，获取主机 `uniqueid`、HTTPS 端口及当前配对状态。
> **请求方式**：GET (HTTP)
> **请求参数**：
- `uniqueid` / `uuid`

> **逻辑**：
- 如果返回 `PairStatus=1` 且本地配置中已存在该主机，则直接视为配对成功。
- 更新本地缓存的 HTTPS 端口。
- 若未配稳，进入第 1 阶段。

### 1. **`/pair?phrase=getservercert&...`**
> **用途**：第一阶段 — 上报生成的随机盐与客户端证书，获取服务端证书。服务端会在此阶段阻塞等待用户输入 PIN 码。  
> **请求参数**：
- `devicename=roth`（固定设备名）
- `updateState=1`
- `phrase=getservercert`
- `salt=<16字节随机盐的 hex>`
- `clientcert=<客户端 PEM 证书的 hex>` (注意：发送的是 PEM 字符串的 hex 编码，而非 DER)

> **响应参数（XML）**：
- `plaincert`: 服务端 X.509 证书的 hex 编码字符串。

> **加密准备**：
- 客户端等待响应（服务端用户输入PIN）。
- 计算 AES Key: `SHA256(salt + PIN)`，取前16字节用于 AES-128-ECB。

> **客户端动作**：
- 解析 `plaincert` 得到服务端证书
- 在配置文件中按照服务端分类存储该证书用于后续 HTTPS 请求（证书绑定）

### 2. **`/pair?clientchallenge=...`**
> **用途**：第二阶段 — 发送加密的客户端挑战，验证 PIN 码正确性。  
> **请求参数**：
- `clientchallenge=<AES-ECB(16字节随机数, AES密钥).toHex()>`

> **响应参数（XML）**：
- `challengeresponse`: Hex 编码的加密数据。
- **解密后内容** (AES-128-ECB): `SHA256(ClientRandom) + ServerRandom(16字节)`。

> **客户端动作**：
- 用相同 AES 密钥解密 `challengeresponse`
- 提取服务端挑战、签名等数据用于下一阶段

### 3. **`/pair?serverchallengeresp=...`**
> **用途**：第三阶段 — 响应服务端挑战，防止中间人攻击。  
> **请求参数**：
- `serverchallengeresp=<加密的哈希值 hex>`

> **加密构造逻辑**：
1. 数据拼接: `ServerRandom` (来自阶段2解密结果的后16字节) + `ClientCertSignature` (客户端证书签名部分) + `ClientSecret` (新生成的16字节随机数)。
2. 哈希: `SHA256(拼接数据)`。
3. 加密: `AES-128-ECB(哈希值, AES密钥)`。

> **响应参数（XML）**：
- `pairingsecret`: Hex 编码数据。

> **客户端动作**：
- **解析**：`ServerSecret(16字节) + ServerSignature`。
- 验证服务端是否知道正确 PIN（通过比对哈希）

### 4. **`/pair?clientpairingsecret=...`**
> **用途**：第四阶段 — 提交客户端配对密钥和签名。  
> **请求参数**：
- `clientpairingsecret=<ClientSecret + Signature(ClientSecret) 的 hex>`

> **客户端动作**：确认服务端接受客户端身份

### 5. **`/pair?phrase=pairchallenge`**（通过 HTTPS）
> **用途**：第五阶段 — 最终验证（使用已建立的 mTLS）  
> **请求方式**：**HTTPS**（此时已信任服务端证书）  
> **请求参数**：
- `devicename=roth`
- `updateState=1`
- `phrase=pairchallenge`

> **意义**：确认整个配对链在安全通道下完成，服务端正式接受此客户端证书。

### ❌ 错误清理：`/unpair`
> **用途**：主动取消当前配对会话（如某阶段失败）  
> **效果**：服务端清除正在进行的配对状态，允许重新开始
