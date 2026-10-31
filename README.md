# DocToText

[English](README.md) | [简体中文](README_zh.md)

[![Release](https://img.shields.io/github/v/release/lilucpp/doctotext?include_prereleases)](https://github.com/lilucpp/doctotext/releases)
[![Build and Release](https://github.com/lilucpp/doctotext/actions/workflows/build.yml/badge.svg)](https://github.com/lilucpp/doctotext/actions/workflows/build.yml)
[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](COPYING.GPL)

DocToText is a compact, high-performance C/C++ library and command-line utility for extracting plain text, annotations, and metadata from diverse document formats—including Microsoft Office (DOC, DOCX, XLS, XLSX, PPT, PPTX), OpenDocument (ODT, ODS, ODP), PDF, RTF, HTML, iWork, source code, and images.

---

## Architecture & Engineering Highlights

- **Decoupled OCR via Streaming Callbacks**  
  Rather than bundling heavyweight neural network runtimes, DocToText utilizes an inversion-of-control pattern via `ImageParserCallback`. When embedded graphics (in Word, Excel, PowerPoint, or ODF) or standalone images are encountered, raw image buffers are dispatched directly to the host application's callback (e.g., Tesseract, ONNX Runtime, PaddleOCR, or vision APIs). The recognized text is seamlessly stitched back into the document stream in-place.
- **Unified Extraction Pipeline**  
  Combines specialized native parsers for legacy OLE2 compound files (BIFF, WordDocument streams), OpenXML packaging, ODF XML trees, and PDF content streams with an encoding-agnostic text fallback layer that leverages `libcharsetdetect` for automatic character set detection (UTF-8, GBK / GB18030, UTF-16LE).
- **C ABI First: Maximum Interoperability**  
  The pure **C API** (`doctotext_c_api.h`) is the **primary recommended interface**. On Windows where shared libraries may be compiled via MinGW-w64 GCC, consuming C++ interfaces across different compilers (such as MSVC) frequently introduces runtime ABI and STL layout incompatibilities. The C API provides standard C linkage (`__cdecl`), ensuring seamless integration across MSVC, GCC, Clang, C#, Python, Go, and Rust.
- **Relocatable, Zero-Configuration Deployment**  
  Binaries and shared libraries are linked with `$ORIGIN` runpaths on Linux and standard adjacent DLL search order on Windows. Artifacts function out-of-the-box in containerized and bare-metal environments without root privileges or `LD_LIBRARY_PATH` configuration.

---

## Format Support Matrix

### Compound & Structured Documents

| Format Family | Extensions | Parser Engine | In-Place Embedded Images | Metadata & Notes |
| :--- | :--- | :--- | :---: | :---: |
| **Microsoft Office (Legacy)** | `doc`, `xls`, `xlsb`, `ppt` | Native OLE2 / BIFF | Yes | Yes |
| **Office Open XML (OOXML)** | `docx`, `xlsx`, `pptx` | Native XML / Unzip | Yes | Yes |
| **OpenDocument (ODF)** | `odt`, `ods`, `odp`, `odg`, `fodt`, `fods`, `fodp` | Native XML / ZIP / Flat | Yes | Yes |
| **Apple iWork** | `pages`, `numbers`, `key`, `keynote` | Native Package Parser | - | Text only |
| **Portable Document Format** | `pdf` (Typeset text) | Native PDFParser | - | Text only |
| **Web & Rich Text** | `html`, `htm`, `rtf` | HTML / RTF Parser | - | Hyperlinks |
| **Email Message** | `eml` (RFC 822) | Mimetic MIME Parser | - | Headers & Parts |

### Standalone Images (via OCR Callback)

| Category | Supported Formats | Detection Method |
| :--- | :--- | :--- |
| **Raster Images** | `png`, `jpg`, `jpeg`, `webp`, `bmp`, `tiff`, `tif`, `jfif`, `gif`, `pnm` | Magic byte header sniffing & file extension |

### Plain Text, Source Code & Data (Explicit `--txt` / `PARSER_TXT`)

Provides automatic multi-encoding detection (UTF-8, GBK, GB18030, UTF-16LE):
* **Stylesheets & Markup**: `css`, `xml`, `xsd`, `xsl`, `csv`
* **Configuration**: `json`, `yaml`, `yml`, `toml`, `ini`, `conf`, `cfg`
* **Code & Scripts**: `c`, `cpp`, `h`, `hpp`, `py`, `java`, `go`, `rs`, `js`, `ts`, `sh`, `bat`, `cmd`, `ps1`, `sql`, `php`, `rb`, `swift`, `kt`, `scala`, `lua`, `r`, `pl`, `asm`, `cmake`, `cs`, `d`, etc.
* **Text & Documentation**: `txt`, `text`, `md`, `markdown`, `log`

### Archive Formats (Format Sniffing & Type Identification)

| Category | Supported Extensions | Sniffing Method | Status |
| :--- | :--- | :--- | :--- |
| **Compressed Archives** | `zip`, `tar`, `gz`, `tgz`, `7z`, `rar`, `bz2`, `xz` | Binary magic bytes (PK, ustar, 1F8B, BZh, 7z, Rar!, etc.) | Format sniffing supported (`PARSER_ARCHIVE`); recursive content extraction planned |

---

## API Usage & Code Samples

> **Recommendation**: Use the **C API** (`doctotext_c_api.h`) for production systems and cross-compiler environments (e.g., MinGW-built DLL called from MSVC applications).

### 1. Basic File Extraction (C API - Recommended)

```c
#include <stdio.h>
#include <doctotext/doctotext_c_api.h>

int main()
{
    DocToTextExtractorParams* params = doctotext_create_extractor_params();
    DocToTextException* error = NULL;

    // Process file (auto-detects format)
    DocToTextExtractedData* data = doctotext_process_file("sample.docx", params, &error);

    if (data) {
        const char* text = doctotext_extracted_data_get_text(data);
        printf("Extracted Text:\n%s\n", text);
        doctotext_free_extracted_data(data);
    } else if (error) {
        size_t count = doctotext_exception_error_messages_count(error);
        for (size_t i = 0; i < count; ++i) {
            fprintf(stderr, "Error: %s\n", doctotext_exception_get_error_message(error, i));
        }
        doctotext_free_exception(error);
    }

    doctotext_free_extractor_params(params);
    return 0;
}
```

### 2. In-Memory Buffer Extraction (C API)

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

### 3. In-Place OCR Callback for Images (C API)

```c
#include <stdio.h>
#include <string.h>
#include <doctotext/doctotext_c_api.h>

// Invoked for standalone images or graphics embedded in DOCX/PPTX/XLSX/ODF
const char* ocr_callback(const char* image_name, const char* buffer, size_t size, void* user_data)
{
    printf("[Image Dispatched] %s (%zu bytes)\n", image_name, size);
    // Forward buffer to local Tesseract, ONNX Runtime, or external OCR service:
    return "[OCR TEXT: Simulated recognition]";
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

### 4. Metadata Extraction (C API)

```c
#include <stdio.h>
#include <doctotext/doctotext_c_api.h>

int main()
{
    DocToTextExtractorParams* params = doctotext_create_extractor_params();
    DocToTextMetadata* meta = doctotext_extract_metadata("report.pdf", params, NULL);

    if (meta) {
        printf("Author: %s\n", doctotext_metadata_author(meta));
        printf("Page Count: %zu\n", doctotext_metadata_pages_count(meta));
        printf("Word Count: %zu\n", doctotext_metadata_words_count(meta));
        doctotext_free_metadata(meta);
    }
    doctotext_free_extractor_params(params);
    return 0;
}
```

### 5. File Format Sniffing & Character Encoding Detection (C API)

```c
#include <stdio.h>
#include <string.h>
#include <doctotext/doctotext_c_api.h>

int main()
{
    // A. File type detection via short-circuit binary header sniffing
    doctotext_parser_type parser_type = DOCTOTEXT_PARSER_AUTO;
    if (doctotext_parser_type_by_file_content("unknown_document.dat", &parser_type)) {
        printf("Detected document format type ID: %d\n", parser_type);
    }

    // B. Standalone character encoding detection
    // Recommended sample buffer size: 4 KB to 16 KB (fast ~0.04ms with >99% confidence)
    const char* sample_text = "中文编码测试文本，自动检测编码。";
    const char* enc = doctotext_detect_encoding(sample_text, strlen(sample_text));
    printf("Detected memory buffer encoding: %s\n", enc);

    // C. File character encoding detection (automatically samples the initial 16 KB)
    const char* file_enc = doctotext_detect_file_encoding("legacy_script.css");
    printf("Detected file encoding: %s\n", file_enc ? file_enc : "Unknown");

    return 0;
}
```

### 6. C++ API (Alternative)

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

### 6. Command-Line Interface (CLI)

```bash
# Extract text to standard output
./doctotext sample.docx

# Parse code, CSS, or plain text with automatic encoding detection
./doctotext --txt app.css

# Extract metadata or embedded annotations
./doctotext --metadata paper.pdf
./doctotext --annotations review.odt

# Unix stream processing pipeline
./doctotext report.doc | grep -i "confidential" | head -n 10
```

---

## Building from Source

### Linux (Ubuntu / Debian)

Install build prerequisites:
```bash
# x86_64
sudo apt-get update && sudo apt-get install -y build-essential pkg-config libxml2-dev zlib1g-dev doxygen

# ARM64 / aarch64
sudo apt-get update && sudo apt-get install -y build-essential autoconf automake libtool pkg-config cmake doxygen \
  libxml2-dev zlib1g-dev libmimetic-dev libhtmlcxx-dev libgsf-1-dev libglib2.0-dev
```

Build:
```bash
make -j$(nproc)
```
Outputs (`doctotext`, `libdoctotext.so`, headers) are placed in `build/`.

### Windows (MSYS2 + MinGW-w64)

From an MSYS2 MinGW-w64 shell:
```bash
pacman -Syu && pacman -S base-devel git mingw-w64-x86_64-toolchain doxygen unzip

# 64-bit Windows:
ARCH=win64 make -j4

# 32-bit Windows:
ARCH=win32 make -j4
```

### macOS

```bash
brew install doxygen libffi zlib xz gcc
make -j$(sysctl -n hw.ncpu)
```

---

## Verification & Testing

- **Regression Unit Tests**:
  ```bash
  make test
  ```
  Executes regression suites across binary OLE, OOXML, ODF, and multi-encoding plain text files.

- **OCR & Image Extraction Tests**:
  ```bash
  make test_img
  ```
  Parses documents with embedded images, standalone picture files (PNG, JPEG, WebP, BMP, TIFF, GIF), and scanned PDFs under `tests_img/`, printing in-place OCR extraction results.

---

## License & Credits

- Originally developed by **[SILVERCODERS](http://silvercoders.com/)** (2006–2013).
- Modernized and maintained by **lithiumFlower** (2020–2022).
- Enhanced and maintained by **[lilucpp](https://github.com/lilucpp)** (2025–present).

Licensed under the **GNU General Public License version 2 (GPL-2.0)**. See [COPYING.GPL](COPYING.GPL).
