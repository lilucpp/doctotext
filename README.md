# DocToText (Enhanced Fork)

[![Release](https://img.shields.io/github/v/release/lilucpp/doctotext?include_prereleases)](https://github.com/lilucpp/doctotext/releases)
[![Build and Release](https://github.com/lilucpp/doctotext/actions/workflows/build.yml/badge.svg)](https://github.com/lilucpp/doctotext/actions/workflows/build.yml)
[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](COPYING.GPL)

An enhanced, cross-platform fork of **DocToText** (originally developed by SILVERCODERS and previously maintained by lithiumFlower). 

DocToText is a powerful C/C++ library and command-line utility for extracting plain text, metadata, and annotations from dozens of document formats (Word, Excel, PowerPoint, RTF, OpenDocument, PDF, and more).

---

## 🌟 Key Improvements over Upstream (`lithiumFlower/doctotext`)

This repository includes significant modernizations, architectural fixes, and major feature additions compared to the upstream:

### 1. 🖼️ Embedded Image Extraction & In-Place Callback
- **Multi-Format Image Extraction**: Added support for extracting embedded images from **DOC, PPT, XLS, XLSX, and DOCX** documents.
- **Custom Callback Hooks**: Introduced `ImageParserCallback` in both C++ and C APIs. You can now register a callback function that receives the raw image bytes (`image_buffer`, `image_size`, `image_name`) in real-time.
- **In-Place OCR / Content Injection**: Text returned by the callback (e.g. from an OCR engine like Tesseract or PaddleOCR) is automatically inserted into the extracted plain text stream at the exact image position.

### 2. 🌐 Modern Cross-Platform & Architecture Support
- **Linux ARM64 / aarch64**: Fully ported to ARM64, including modernized `config.guess` / `config.sub`, patching `libcharsetdetect` and `wv2` for 64-bit ARM Linux.
- **Modern Windows MinGW-w64**:
  - Implemented SJLJ exception unwinding stubs (`src/sjlj_compat.cpp`) allowing modern GCC/MinGW toolchains to link smoothly with precompiled legacy libraries without symbol conflicts.
  - Fixed Windows link flags (`-lxml2`, missing system libraries `ws2_32`, `ole32`, etc.).
  - Produced separate DLL import libraries and standalone runtime bundles.
- **Linux Multi-Arch (x86_64, x86, ARM64)**: Cleaned build scripts and resolved system package prerequisite issues.

### 3. 📦 Resilient 3rdparty Dependency Management
- **Dual-Source Fallback**: Automated fallback to GitHub Release mirrors (`v3rdparty-deps`) if the upstream third-party URLs are unavailable or broken.
- **Checksum Verification**: Added SHA1 integrity verification with automatic retry and error resilience.
- **Automated Download Script**: Added `scripts/download_all_3rdparty.py` for one-step dependency preparation.

### 4. 🚀 Modern CI/CD Workflows & Automated Releases
- **Matrix Builds**: GitHub Actions CI workflow covering Windows (x86/x64), Linux (x86_64/x86/ARM64).
- **Automated Packaging**: Automatically builds relocatable archives with proper `$ORIGIN` / `@rpath` configurations on every release tag.

---

## 📄 Supported Formats & Architecture Support Matrix

DocToText 支持丰富的办公文档原生解析、纯文本及编程脚本降级解析，并通过外部回调支持图像与内嵌图像 OCR 提取。

### 1. 原生办公与复合文档格式（自动探测解析）

| 类别 | 支持格式 | 说明 |
| :--- | :--- | :--- |
| **Microsoft Office 经典二进制** | `doc`, `xls`, `xlsb`, `ppt` | 完整支持 OLE 复合流与 BIFF/XLSB 结构，支持内嵌图片提取回调 |
| **Office Open XML (OOXML)** | `docx`, `xlsx`, `pptx` | 完整支持文档正文、表格，支持内嵌图片提取回调 |
| **OpenDocument 格式 (ODF)** | `odt`, `ods`, `odp`, `odg`<br>`fodt`, `fods`, `fodp`, `fodg` | 完整支持标准 ZIP 打包版与 Flat XML 纯文本版，支持内嵌图片提取回调 |
| **苹果办公套件 (Apple iWork)** | `pages`, `numbers`, `key`, `keynote` | 完整解析归档包与 `index.xml` |
| **便携式文档 (PDF)** | `pdf` (常规文字排版) | 原生内置 PDFParser 提取排版文字 |
| **网页与富文本文档** | `html`, `htm`, `rtf` | 自动剔除标签与格式控制符，提取纯文本与超链接 |
| **电子邮件格式 (Email)** | `eml` (RFC 822) | 完整解析邮件头、正文与附件元信息 |

### 2. 图像解析与 OCR 回调（统一回调机制）

支持将复合文档中的**内嵌图片**或**独立图像文件**统一路由至外接 OCR 解析器：

| 类别 | 支持格式 | 处理机制 |
| :--- | :--- | :--- |
| **独立图像格式** | `jpg`, `jpeg`, `png`, `webp`, `bmp`, `tiff`, `tif`, `jfif`, `gif`, `pnm` | 内部自动识别图片文件头与魔数，自动调用 `ImageParserCallback` 并将 OCR 文本直接返回 |
| **文档内嵌图片** | DOC, PPT, XLS, DOCX, PPTX, XLSX, ODP, ODS 等内嵌图像 | 解析正文时自动提取二进制图片流，触发回调就地（In-Place）嵌入 OCR 文字 |

### 3. 编程源码、标记语言与结构化配置（显式指定 `PARSER_TXT` 降级支持）

此类格式为纯文本及其衍生语法，通过显式指定 `PARSER_TXT`（或命令行 `--txt`）可完美提取文本，内置 `libcharsetdetect` 引擎支持 **UTF-8、GBK / GB18030、UTF-16LE（带BOM）等多编码自动探测**：

* **层叠样式表 (CSS)**：`css`（已验证多编码中文支持）
* **编程语言与脚本**：`c`, `cpp`, `cc`, `cxx`, `h`, `hpp`, `hxx`, `py`, `java`, `go`, `rs`, `js`, `ts`, `jsx`, `tsx`, `sh`, `bat`, `cmd`, `ps1`, `sql`, `php`, `rb`, `swift`, `kt`, `scala`, `lua`, `r`, `pl`, `perl`, `asm`, `cmake`, `cs`, `d`, `pas`, `tcl` 等
* **标记语言与结构化表格**：`xml`, `xsd`, `xsl`, `csv`
* **结构化数据与配置文件**：`json`, `yml`, `yaml`, `toml`, `ini`, `conf`, `cfg`
* **纯文本文档与日志**：`txt`, `text`, `md`, `markdown`, `log`

### 4. 路线图与规划中 (TODO)

- [ ] **扫描版纯图片 PDF**：目前原生 PDFParser 仅提取文字指令流，纯扫描版 PDF 暂无法抽取内嵌图片进行 OCR，计划增加 PDF XObject 图像提取并接入 `ImageParserCallback`。
- [ ] **归档压缩格式文本扫描**：`zip`, `tar`, `gz`, `7z`, `rar` 目前仅供内部解压 docx/odf 容器，后续计划支持直接读取压缩包内包含的文档内容。

### 5. 暂不支持的私有/专有格式

* **邮件私有数据库**：`pst`, `ost`（属 Outlook 专有复杂数据库）
* **医疗专有格式**：`dcm`（DICOM 医学成像）

---

## 💻 Quickstart & API Usage

### C++ API Example (with Image Callback)

```cpp
#include <iostream>
#include <string>
#include <doctotext/plain_text_extractor.h>

// Optional callback for OCR or image processing
std::string my_image_callback(const std::string& image_name,
                              const char* image_buffer,
                              size_t image_size,
                              void* user_data)
{
    std::cout << "[Image Found] " << image_name << " (" << image_size << " bytes)" << std::endl;
    // Call OCR engine here, or return placeholder/extracted text:
    return "[IMAGE_TEXT: Extracted from " + image_name + "]";
}

int main()
{
    doctotext::PlainTextExtractor extractor;
    
    // Register image callback
    extractor.setImageParserCallback(my_image_callback, nullptr);

    std::string text;
    if (extractor.processFile("sample.docx", text))
    {
        std::cout << "Extracted Document Text:\n" << text << std::endl;
    }
    return 0;
}
```

### C API Example

```c
#include <stdio.h>
#include <doctotext/doctotext_c_api.h>

const char* c_image_callback(const char* image_name, const char* buffer, size_t size, void* user_data)
{
    // Return OCR text or NULL
    return "[OCR TEXT]";
}

int main()
{
    DocToTextExtractorParams* params = doctotext_extractor_params_new();
    doctotext_extractor_params_set_image_parser_callback(params, c_image_callback, NULL);

    DocToTextException* error = NULL;
    char* text = doctotext_process_file("sample.xlsx", params, &error);

    if (text) {
        printf("%s\n", text);
        doctotext_free_text_buffer(text);
    }
    doctotext_extractor_params_free(params);
    return 0;
}
```

### Command Line Interface (CLI)

```bash
# Basic extraction
./doctotext sample.doc

# Extract metadata
./doctotext --metadata sample.docx

# Extract comments / annotations
./doctotext --annotations sample.odt
```

---

## 🔨 Building from Source

### 1. Linux (Ubuntu / Debian)

#### Prerequisites (x86_64)
```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libxml2-dev zlib1g-dev doxygen
```

#### Prerequisites (ARM64 / aarch64)
```bash
sudo apt-get update
sudo apt-get install -y build-essential autoconf automake libtool pkg-config cmake doxygen \
  libxml2-dev zlib1g-dev libmimetic-dev libhtmlcxx-dev libgsf-1-dev libglib2.0-dev
```

#### Build
```bash
make -j$(nproc)
```
The output binaries and libraries will be placed under the `build/` directory.

---

### 2. Windows (MSYS2 + MinGW-w64)

1. Open **MSYS2 MinGW 64-bit** or **32-bit** shell.
2. Install dependencies:
   ```bash
   pacman -Syu
   pacman -S base-devel git mingw-w64-x86_64-toolchain doxygen unzip
   ```
3. Build:
   ```bash
   # For 64-bit Windows:
   ARCH=win64 make -j4

   # For 32-bit Windows:
   ARCH=win32 make -j4
   ```

---

### 3. macOS

Install dependencies via Homebrew:
```bash
brew install doxygen libffi zlib xz gcc
make -j$(sysctl -n hw.ncpu)
```

---

### 4. Running Tests

- **Run Standard Unit Tests**:
  ```bash
  make test
  ```
  Runs full test suites across binary/XML/ODF/CSS-TXT formats.

- **Run Image & Embedded Image OCR Tests**:
  ```bash
  make test_img
  ```
  Parses all standalone images, documents with embedded pictures, and scanned PDFs under [`tests_img/`](file:///home/peanut/project/doctotext/tests_img), printing in-place OCR extraction results.

---

## 📦 Relocatable Binaries and Shared Libraries

DocToText binaries and libraries are compiled to be fully relocatable:
- **Windows**: Search path naturally resolves adjacent DLLs.
- **Linux**: Configured with `rpath=$ORIGIN`, meaning all dependent shared libraries placed in the same folder are automatically resolved without requiring `LD_LIBRARY_PATH` or root installation.
- **macOS**: Configured with `@rpath` and `@loader_path`.

---

## 📜 License & Credits

- Originally created by **[SILVERCODERS](http://silvercoders.com/)** (2006–2013).
- Maintained and modernized by **lithiumFlower** (2020–2022).
- Enhanced and maintained by **[lilucpp](https://github.com/lilucpp)** (2025–present).

This project is licensed under the **GNU General Public License version 2 (GPL-2.0)**. See [COPYING.GPL](COPYING.GPL) for details.
