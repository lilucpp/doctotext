# DocToText 示例工具集

本目录包含 DocToText 的核心集成示例与本地辅助对比工具：
- **`viewer/` & `run_viewer.sh`**：文件对比预览工作台（左栏源文件插件预览 + 右栏解析正文对比，零外部依赖）。
- **`sample_ppocr_extractor.cpp` / `example2_ppocr_extractor.cpp`**：PP-OCRv6 图像就地识别集成示例。

---

## 工具一：文件对比预览工作台 (`examples/viewer`)

提供一个轻量、可跨机部署的左右分栏对比页面（无任何 hardcoded 绝对路径，均采用相对路径自动探测 + 命令行参数/环境变量覆盖）：
- **文件夹与文件选择**：支持输入路径浏览本地目录、常用相对测试目录一键跳转（`tests/`、`tests_img/`），以及通过浏览器系统对话框选择本地文件夹或拖拽单文件对比；
- **左侧源文件预览**：
  - `.pdf` 使用浏览器原生 PDF 插件预览，支持切换 `pdftoppm` 按页栅格模式与 DPI/缩放调节；
  - `.doc` / `.docx` / `.xls` / `.xlsx` / `.ppt` / `.pptx` / `.odt` / `.ods` / `.odp` / `.rtf` / `.tiff` 等格式自动通过 LibreOffice 无头转换（缓存在系统临时目录或 `DOCTOTEXT_CACHE_DIR`）后以 PDF 插件展示；
  - 图片（PNG/JPG/BMP/GIF/WebP/SVG）与 HTML/TXT/EML 原生展示；
- **右侧解析结果区**：
  - 实时调用 `build/doctotext`（含 `--compact-whitespace`、`--meta`、`--verbose`）或 `example2_ppocr_extractor`（C API + PP-OCRv6）；
  - 支持空白符可视化（`·` 空格、`→` 制表符、`↵` 换行符）、全文搜索高亮、行号/换行切换，以及可选的 Apache Tika 并排 / Unified Diff 对比。

### 跨机部署与三方依赖说明

工作台本身**无需 `pip install` 或 `npm install`**，可随仓库直接拷贝到其他机器运行。各项依赖按功能拆分如下（可选依赖缺失时服务仍可正常启动，对应按钮会自动降级或提示）：

| 组件 | 级别 | 作用 | 安装 / 准备方式 |
| :--- | :--- | :--- | :--- |
| **Python 3 (`>= 3.8`)** | 必需 | 运行 Web 服务（仅用标准库） | Ubuntu/Debian: `sudo apt install -y python3`<br>RHEL/Fedora: `sudo dnf install -y python3` |
| **DocToText 编译产物** | 必需 | 提供核心解析引擎 (`doctotext`, `libdoctotext.so`) | 在仓库根目录完成编译（生成 `build/doctotext` 与 `build/libdoctotext.so`），或通过 `--doctotext-bin` / `DOCTOTEXT_BIN` 指定 |
| **LibreOffice (`libreoffice` / `soffice`)** | 可选（推荐） | 左侧将 `.doc/.docx/.xls/.xlsx/.ppt/.pptx/.odt/.rtf/.tiff` 转为 PDF 插件预览 | Ubuntu/Debian: `sudo apt install -y libreoffice`<br>RHEL/Fedora: `sudo dnf install -y libreoffice`<br>macOS: `brew install --cask libreoffice` |
| **Poppler 工具 (`poppler-utils`)** | 可选 | 提供 `pdftoppm` 与 `pdfinfo`，启用左侧「按页栅格」PNG 预览与无插件浏览器降级预览 | Ubuntu/Debian: `sudo apt install -y poppler-utils`<br>RHEL/Fedora: `sudo dnf install -y poppler-utils`<br>macOS: `brew install poppler` |
| **Java (`>= 11`) + Apache Tika (`tika-app.jar`)** | 可选 | 启用右侧「对比 Tika」并排与 Unified Diff 对比 | 1. 安装 JRE：`sudo apt install -y default-jre`<br>2. 从 Apache Tika 官网下载 `tika-app-<version>.jar`（例如 `tika-app.jar`）<br>3. 放置于 `./tools/tika-app.jar`（自动识别），或通过 `--tika-jar` / `TIKA_JAR` 指定 |
| **PP-OCRv6 模型与 ONNX Runtime** | 可选 | 启用 `C API + PP-OCRv6 (含图片OCR)` 引擎 | 仅需内嵌图片 OCR 对比时编译 `examples/example2_ppocr_extractor`，并通过 `--ppocr-models` / `PPOCR_MODELS_DIR` 与 `ONNXRUNTIME_LIB_DIR` 指定路径 |

### 启动与配置参数

```bash
# 1. 默认本地启动（监听 127.0.0.1:8765）
./examples/run_viewer.sh

# 2. 局域网/跨机访问并指定端口、默认目录与 Apache Tika
./examples/run_viewer.sh \
  --host 0.0.0.0 \
  --port 9000 \
  --dir ./tests \
  --tika-jar /path/to/tika-app.jar
```

支持的 CLI 参数与等价环境变量：
- `--host` / `HOST`：监听地址（默认 `127.0.0.1`，跨机访问可设为 `0.0.0.0`）
- `--port` / `PORT`：监听端口（默认 `8765`）
- `--dir` / `DOCTOTEXT_VIEWER_INITIAL_DIR`：启动时默认打开的文档目录（默认自动探测 `./tests` 或仓库根目录）
- `--doctotext-bin` / `DOCTOTEXT_BIN`：`doctotext` 可执行文件路径（默认自动探测 `./build/doctotext`、`./src/doctotext` 或 `PATH`）
- `--tika-jar` / `TIKA_JAR`：`tika-app*.jar` 路径（默认自动探测仓库根目录、`tools/`、`examples/viewer/` 等相对目录）
- `--ppocr-bin` / `PPOCR_BIN`、`--ppocr-models` / `PPOCR_MODELS_DIR`、`--onnxruntime-lib` / `ONNXRUNTIME_LIB_DIR`：PP-OCRv6 示例引擎、模型目录与 ONNX Runtime 动态库目录
- `--cache-dir` / `DOCTOTEXT_CACHE_DIR`：PDF 转换与分页栅格缓存目录（默认使用系统临时目录下的 `doctotext_preview_cache`）

启动后在浏览器访问服务打印的地址即可；若服务运行在远端开发机，可在本地通过浏览器访问 `http://<服务器IP>:8765` 或使用 SSH 端口转发（`ssh -L 8765:127.0.0.1:8765 user@host`）。

---

## 示例二：PP-OCR 图像就地识别集成 (`sample_ppocr_extractor.cpp`)

## 核心文件

- **`sample_ppocr_extractor.cpp`**：
  - 演示如何将 DocToText 的 `ImageParserCallback`（图像解析回调）与 OpenCV 内存解码 (`cv::imdecode`) 及 ONNX Runtime 推理引擎对接。
  - 包含智能过滤机制（排除微小图标、分割线与纯色块背景，避免算力浪费）。
  - 支持 C++ 原生接口（直接返回 `std::string`）与 C 语言接口（通过 `thread_local` 缓存指针）。

## 架构调用时序

```
文档 (docx / pptx / xlsx / pdf)
   │
   ▼
DocToText 正在逐段解析文本
   │
   ├─► [遇到正文/表格] ───────────────► 直接提取
   │
   └─► [遇到内嵌插图] ───────────────► 触发 ImageParserCallback (内存字节流)
                                                │
                                                ▼
                                      cv::imdecode 零落盘解码
                                                │
                                                ▼
                                      ImageFilter 尺寸/方差智能初筛
                                                │
                                                ▼
                                      PP-OCR (ONNX Runtime 推理)
                                                │
                                                ▼
                                      返回识别文本 [图片文字: ...]
                                                │
                                                ▼
                                  就地拼入 DocToText 当前段落上下文
```

## 编译依赖说明（如需独立构建）

本示例演示了生产级集成的全流程逻辑。如需编译为独立二进制可执行程序，需链接以下三方依赖：
- **DocToText 核心库**：`libdoctotext.so` 或静态库
- **OpenCV 4.x**：`opencv_core`, `opencv_imgcodecs`, `opencv_imgproc`
- **ONNX Runtime**：`onnxruntime` (C++ API)
- **模型文件**：PP-OCRv6 ONNX 推理模型 (`inference.onnx`) 与字符字典 (`chars.txt`)
