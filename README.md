# <div align="center">🔒 VMPX - 基于 VMP 的加壳控制台 🔒</div>

简体中文

<p align="center">
<a><img alt="GitHub License" src="https://img.shields.io/github/license/lona-cn/vmpx"></a>
<a><img alt="GitHub Release" src="https://img.shields.io/github/v/release/lona-cn/vmpx"></a>
<a><img alt="GitHub Issues" src="https://img.shields.io/github/issues/lona-cn/vmpx"></a>
</p>

---

`VMPX` 是一款基于 C++ 后端与现代前端技术构建的 Web 软件，用于通过 VMProtect（VMP）对应用程序进行加壳保护。  

---

## 控制台

![VMPX 深色操作终端：应用目录、产品信息、授权序列号与上传部署](doc/images/vmpx_console.png)

*界面示意图使用示例应用与产品信息；实际列表和状态由当前服务返回。*

控制台分为四个工作区：

1. **应用目录**：查看已登记应用，点击「加载信息」填入产品信息，或点击「加壳下载」获取打包结果。
2. **产品信息**：编辑或粘贴 ProductInfo JSON，也可以设置 RSA 密钥位数并随机生成。
3. **授权序列号**：填写到期日期及可选的用户名、邮箱、硬件 ID，按需选择「忽略网卡信息」，生成并复制序列号。
4. **上传部署**：选择应用文件、填写应用名称和可选的 VMP 文件路径后上传。

桌面端以多栏呈现，窄屏设备按工作区纵向排列；顶部显示 API 连接状态。

---

## <div align="center">🚀 特性</div>

- **完整API接口**：支持产品信息随机生成、序列号生成、应用上传、加壳打包等  
- **战术风格响应式控制台**：深色面板、状态反馈与移动端布局；由 Alpine.js 驱动，无需前端构建步骤  
- **轻量部署**：纯 C++ 服务端，前端静态文件，适合多种部署环境  
- **开源协议**：采用 MIT 许可证，开放自由，便于集成和扩展  
- **平台支持**: 仅支持在windows/x64平台运行

---

## <div align="center">🛠 快速使用</div>

### 编译与启动

1. 克隆仓库并编译 C++ 后端服务。
2. 启动服务，在浏览器打开其地址（下方示例为 `http://localhost:11451/`）。服务会提供控制台静态页面及 `/api/v1/` 接口。
3. 在「上传部署」登记应用，然后从「应用目录」加载信息或加壳下载；生成序列号前需准备 ProductInfo JSON 和到期日期。

## <div align="center">🧩 API 简要</div>

| 接口                         | 方法 | 说明                 |
|------------------------------|------|----------------------|
| `/api/v1/gen_random_product_info` | POST | 生成随机 ProductInfo |
| `/api/v1/gen_serial_number`        | POST | 根据产品信息生成序列号 |
| `/api/v1/app/list`                 | GET  | 获取 App 列表        |
| `/api/v1/app/add`                  | POST | 上传新 App           |
| `/api/v1/app/pack`                 | POST | 对指定 App 进行加壳打包 |
| `/api/v1/app/product_info`         | GET  | 获取指定 App 产品信息 |

详细接口定义请查看项目 OpenAPI 规范。

---

## <div align="center">💻 开发指南</div>

### 前端技术栈

- [Alpine.js](https://alpinejs.dev/)：控制台的响应式状态与交互
- 原生 CSS：深色战术面板与响应式布局；运行时从 CDN 加载 Alpine.js 和字体

### 后端技术栈

- C++17/20 编写，提供高性能 HTTP API，方便二次开发和集成
- `libhv`、`log4cplus`、`Crypto++`、`boost`等10+第三方库，详细请参考`app/source/third/xmake.lua`

### 克隆 & 编译运行示例

```powershell
# 安装xmake
git clone https://github.com/lona-cn/vmpx.git
cd vmpx
./scripts/build-windows_x64_msvc.bat
# xmake run vmpx_server [ip] [port] [VMProtect_Con.exe文件路径]
xmake run vmpx_server 0.0.0.0 11451 VMProtect_Con.exe
```

注意：
  - 如果不指定`VMProtect_Con.exe文件路径`则不能使用加壳功能
  - `VMProtect_Con.exe`并不包含在本项目中，请自行购买VMProtect授权的软件
  - 添加新的软件，需要在`zip`内包含`.exe`文件和`.vmp`文件，请参考`doc/zip/sharpkeys.zip`

## 联系方式
QQ群：364057904