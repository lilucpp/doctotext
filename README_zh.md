# DocToText

[English](README.md) | [简体中文](README_zh.md)

[![Release](https://img.shields.io/github/v/release/lilucpp/doctotext?include_prereleases)](https://github.com/lilucpp/doctotext/releases)
[![Build and Release](https://github.com/lilucpp/doctotext/actions/workflows/build.yml/badge.svg)](https://github.com/lilucpp/doctotext/actions/workflows/build.yml)
[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](COPYING.GPL)

DocToText 是一个轻量、高性能的 C/C++ 文档正文、元数据及批注文本提取库与命令行工具，支持 Microsoft Office（DOC, DOCX, XLS, XLSX, PPT, PPTX）、OpenDocument（ODT, ODS, ODP）、PDF、RTF、HTML、iWork、图像以及多种编程源码等格式。

---

## 架构与核心工程设计

- **流式解耦的 OCR 回调机制（Inversion-of-Control）**  
  不强行捆绑庞大的深度学习推理框架，而是通过 `ImageParserCallback` 提供控制反转接口。在解析复合文档（Word, Excel, PowerPoint, ODF）遇到内嵌图片或处理独立图像时，原始二进制缓冲流直接派发给宿主程序注册的回调函数（可灵活接入本地 Tesseract、ONNX Runtime、PaddleOCR 或云端视觉模型），并将识别文本就地（In-Place）精确缝合回文档正文字符流中。
- **统一多源解析管道**  
  融合了面向经典二进制 OLE2 复合文档（BIFF 流、WordDocument 流）、OpenXML 打包容器、ODF 树形结构与 PDF 页面排版的原生解析引擎，并配备基于 `libcharsetdetect` 的字符集自适应文本降级层，支持自动识别并转换多编码文本（UTF-8, GBK / GB18030, UTF-16LE）。
- **C 接口优先：兼顾极致跨平台 ABI 稳定性**  
  原生纯 **C 接口**（`doctotext_c_api.h`）为**首选推荐接口**。特别是在 Windows 平台，当核心库由 MinGW-w64 GCC 编译为动态链接库（DLL）时，若在 MSVC 等不同编译器环境中直接调用 C++ 接口，易因标准库 STL 内存布局、函数符号粉碎（Name Mangling）及异常运行时差异引发崩溃。C 接口提供纯正标准 C 链接（`__cdecl`），可被 MSVC、GCC、Clang、C#、Python、Go、Rust 等无缝可靠调用。
- **零配置便携式分发部署**  
  Linux 下二进制与共享库默认配置 `$ORIGIN` 运行时路径，Windows 下按同级目录规则优先解析依赖 DLL。编译产物在容器或物理机上解压即用，无需 root 权限或额外配置 `LD_LIBRARY_PATH`。

---

## 格式支持矩阵

### 复合与结构化办公文档

| 格式家族 | 文件扩展名 | 底层解析引擎 | 内嵌图片就地抽取 | 批注与元数据 |
| :--- | :--- | :--- | :---: | :---: |
| **Microsoft Office 经典二进制** | `doc`, `xls`, `xlsb`, `ppt` | 原生 OLE2 / BIFF | 支持 | 支持 |
| **Office Open XML (OOXML)** | `docx`, `xlsx`, `pptx` | 原生 XML / Unzip | 支持 | 支持 |
| **OpenDocument (ODF)** | `odt`, `ods`, `odp`, `odg`, `fodt`, `fods`, `fodp` | 原生 XML / ZIP / Flat | 支持 | 支持 |
| **Apple iWork** | `pages`, `numbers`, `key`, `keynote` | 原生 Package 解析 | - | 仅文本 |
| **便携式文档格式 (PDF)** | `pdf` (排版文字) | 原生 PDFParser | - | 仅文本 |
| **网页与富文本** | `html`, `htm`, `rtf` | HTML / RTF Parser | - | 超链接 |
| **电子邮件格式 (Email)** | `eml` (RFC 822) | Mimetic MIME Parser | - | 邮件头与附件 |

### 独立图像格式（统一走 OCR 回调）

| 类别 | 支持格式 | 识别机制 |
| :--- | :--- | :--- |
| **光栅图像** | `png`, `jpg`, `jpeg`, `webp`, `bmp`, `tiff`, `tif`, `jfif`, `gif`, `pnm` | 文件头魔数嗅探与扩展名结合 |

### 纯文本、编程源码与数据（显式 `--txt` / `PARSER_TXT`）

支持编码自适应探测（UTF-8, GBK, GB18030, UTF-16LE）：
* **样式表与标记语言**：`css`, `xml`, `xsd`, `xsl`, `csv`
* **配置文件**：`json`, `yaml`, `yml`, `toml`, `ini`, `conf`, `cfg`
* **编程语言与运维脚本**：`c`, `cpp`, `h`, `hpp`, `py`, `java`, `go`, `rs`, `js`, `ts`, `sh`, `bat`, `cmd`, `ps1`, `sql`, `php`, `rb`, `swift`, `kt`, `scala`, `lua`, `r`, `pl`, `asm`, `cmake`, `cs`, `d` 等
* **文档与日志**：`txt`, `text`, `md`, `markdown`, `log`

### 压缩与归档文件格式（支持类型嗅探识别）

| 类别 | 支持扩展名 | 识别机制 | 支持状态 |
| :--- | :--- | :--- | :--- |
| **压缩与打包格式** | `zip`, `tar`, `gz`, `tgz`, `7z`, `rar`, `bz2`, `xz` | 头部魔数短路匹配（PK, ustar, 1F8B, BZh, 7z, Rar! 等） | 已支持类型识别 (`PARSER_ARCHIVE`)；内部递归正文扫描规划中 |

---

## 接口使用指南与代码示例

> **开发建议**：在跨编译器（尤其是 Windows 下 MinGW 编译的库在 MSVC 中使用）、跨语言调用及生产级封装时，**优先推荐使用 C 接口**（`doctotext_c_api.h`）。

### 1. 基础文件正文提取（C 接口 - 推荐）

```c
#include <stdio.h>
#include <doctotext/doctotext_c_api.h>

int main()
{
    DocToTextExtractorParams* params = doctotext_create_extractor_params();
    DocToTextException* error = NULL;

    // 解析文档（自动识别格式）
    DocToTextExtractedData* data = doctotext_process_file("sample.docx", params, &error);

    if (data) {
        const char* text = doctotext_extracted_data_get_text(data);
        printf("提取正文:\n%s\n", text);
        doctotext_free_extracted_data(data);
    } else if (error) {
        size_t count = doctotext_exception_error_messages_count(error);
        for (size_t i = 0; i < count; ++i) {
            fprintf(stderr, "错误: %s\n", doctotext_exception_get_error_message(error, i));
        }
        doctotext_free_exception(error);
    }

    doctotext_free_extractor_params(params);
    return 0;
}
```

### 2. 内存 Buffer 文本提取（C 接口）

```c
#include <stdio.h>
#include <stdlib.h>
#include <doctotext/doctotext_c_api.h>

void extract_from_memory(const char* buffer, size_t size)
{
    DocToTextExtractorParams* params = doctotext_create_extractor_params();
    DocToTextException* error = NULL;

    DocToTextExtractedData* data = doctotext_process_file_from_buffer(buffer, size, params, &error);
    if (data) {
        printf("%s\n", doctotext_extracted_data_get_text(data));
        doctotext_free_extracted_data(data);
    }
    if (error) doctotext_free_exception(error);
    doctotext_free_extractor_params(params);
}
```

### 3. 图像就地 OCR 回调（C 接口）

```c
#include <stdio.h>
#include <string.h>
#include <doctotext/doctotext_c_api.h>

// 当解析到独立图像或 Word/PPT/Excel/ODF 中的内嵌插图时触发
const char* ocr_callback(const char* image_name, const char* buffer, size_t size, void* user_data)
{
    printf("[触发图片提取] %s (%zu 字节)\n", image_name, size);
    // 可将 buffer 派发给本地 Tesseract、PaddleOCR 或 ONNX 推理引擎：
    return "[OCR 识别文字: 示例]";
}

int main()
{
    DocToTextExtractorParams* params = doctotext_create_extractor_params();
    doctotext_extractor_params_set_image_parser_callback(params, ocr_callback, NULL);

    DocToTextExtractedData* data = doctotext_process_file("embedded_presentation.pptx", params, NULL);
    if (data) {
        printf("%s\n", doctotext_extracted_data_get_text(data));
        doctotext_free_extracted_data(data);
    }
    doctotext_free_extractor_params(params);
    return 0;
}
```

### 4. 文档元数据提取（C 接口）

```c
#include <stdio.h>
#include <doctotext/doctotext_c_api.h>

int main()
{
    DocToTextExtractorParams* params = doctotext_create_extractor_params();
    DocToTextMetadata* meta = doctotext_extract_metadata("report.pdf", params, NULL);

    if (meta) {
        printf("作者: %s\n", doctotext_metadata_author(meta));
        printf("页数: %zu\n", doctotext_metadata_pages_count(meta));
        printf("总字数: %zu\n", doctotext_metadata_words_count(meta));
        doctotext_free_metadata(meta);
    }
    doctotext_free_extractor_params(params);
    return 0;
}
```

### 5. 文件类型判断与字符编码探测（C 接口）

```c
#include <stdio.h>
#include <string.h>
#include <doctotext/doctotext_c_api.h>

int main()
{
    // A. 基于二进制头部短路嗅探判断文件格式类型
    doctotext_parser_type parser_type = DOCTOTEXT_PARSER_AUTO;
    if (doctotext_parser_type_by_file_content("unknown_document.dat", &parser_type)) {
        printf("检测到格式类型编号: %d\n", parser_type);
    }

    // B. 独立字符串编码判断（推荐采样长度：4KB ~ 16KB，耗时约 0.04ms，置信度 >99%）
    const char* sample_text = "中文编码测试文本，自动检测编码。";
    const char* enc = doctotext_detect_encoding(sample_text, strlen(sample_text));
    printf("内存字符串编码检测结果: %s\n", enc);

    // C. 文本文件编码判断（内部自动短路截取前 16KB 采样进行高效统计推断）
    const char* file_enc = doctotext_detect_file_encoding("legacy_script.css");
    printf("文件编码检测结果: %s\n", file_enc ? file_enc : "未知");

    return 0;
}
```

### 6. C++ API（备选用例）

```cpp
#include <iostream>
#include <string>
#include <doctotext/plain_text_extractor.h>

int main()
{
    doctotext::PlainTextExtractor extractor;
    std::string text;

    if (extractor.processFile("document.docx", text)) {
        std::cout << text << std::endl;
    }
    return 0;
}
```

### 6. 命令行工具使用 (CLI)

```bash
# 提取文档正文至标准输出
./doctotext sample.docx

# 指定按纯文本/多编码检测提取源码或 CSS
./doctotext --txt app.css

# 提取元数据与嵌入批注
./doctotext --metadata paper.pdf
./doctotext --annotations review.odt

# Unix 管道组合处理
./doctotext report.doc | grep -i "confidential" | head -n 10
```

---

## 从源码编译

### Linux (Ubuntu / Debian)

安装依赖：
```bash
# x86_64
sudo apt-get update && sudo apt-get install -y build-essential pkg-config libxml2-dev zlib1g-dev doxygen

# ARM64 / aarch64
sudo apt-get update && sudo apt-get install -y build-essential autoconf automake libtool pkg-config cmake doxygen \
  libxml2-dev zlib1g-dev libmimetic-dev libhtmlcxx-dev libgsf-1-dev libglib2.0-dev
```

编译：
```bash
make -j$(nproc)
```
编译产物（`doctotext` 可执行程序、`libdoctotext.so` 共享库及头文件）自动输出至 `build/` 目录。

### Windows (MSYS2 + MinGW-w64)

在 MSYS2 MinGW-w64 终端中运行：
```bash
pacman -Syu && pacman -S base-devel git mingw-w64-x86_64-toolchain doxygen unzip

# 64 位构建:
ARCH=win64 make -j4

# 32 位构建:
ARCH=win32 make -j4
```

### macOS

```bash
brew install doxygen libffi zlib xz gcc
make -j$(sysctl -n hw.ncpu)
```

---

## 验证与测试

- **全量回归测试**：
  ```bash
  make test
  ```
  执行覆盖二进制 OLE、OOXML、ODF 及多编码纯文本文件的自动化回归测试集。

- **图像与内嵌 OCR 测试**：
  ```bash
  make test_img
  ```
  处理 `tests_img/` 目录下的独立图片（PNG, JPEG, WebP, BMP, TIFF, GIF）、内嵌图片复合文档及扫描件，输出实测 OCR 识别结果。

---

## 许可证与致谢

- 最初由 **[SILVERCODERS](http://silvercoders.com/)** 开发 (2006–2013)。
- 由 **lithiumFlower** 维护并现代化 (2020–2022)。
- 由 **[lilucpp](https://github.com/lilucpp)** 维护 (2025–至今)。

本项目采用 **GNU General Public License version 2 (GPL-2.0)** 许可证。详见 [COPYING.GPL](COPYING.GPL)。
