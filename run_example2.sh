#!/usr/bin/env bash
# 一键运行 DocToText C API + PP-OCRv6 提取 Demo

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${PROJECT_ROOT}/examples/example2_ppocr_extractor"

export LD_LIBRARY_PATH="${PROJECT_ROOT}/build:/home/peanut/project/soeasy/third_party/onnxruntime/lib:${LD_LIBRARY_PATH}"

if [ -z "$1" ]; then
    echo "==================================================================="
    echo "  DocToText C API + PP-OCRv6 一键运行脚本"
    echo "==================================================================="
    echo "用法: $0 <PDF/文档路径> [模型目录(可选)]"
    echo "示例: $0 tests_img/scanned_image.pdf"
    echo "示例: $0 /home/peanut/project/test_dataset/tika-1.x/tika-parsers/src/test/resources/test-documents/testOCR.pdf"
    exit 1
fi

"$BIN" "$@"
