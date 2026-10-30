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

## 📄 Supported Formats

| Category | Formats |
| :--- | :--- |
| **Microsoft Office Binary** | DOC, XLS, XLSB, PPT |
| **Office Open XML (OOXML)** | DOCX, XLSX, PPTX |
| **OpenDocument Format (ODF)**| ODT, ODS, ODP, ODG, FODP, FODS, FODT |
| **iWork** | PAGES, NUMBERS, KEYNOTE |
| **Others** | PDF, RTF, HTML, EML (Email), TXT |

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
