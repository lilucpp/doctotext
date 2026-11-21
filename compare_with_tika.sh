#!/usr/bin/env bash
# 一键对比 DocToText 与 Apache Tika 提取结果

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIKA_JAR="/home/peanut/project/test_dataset/tika-app.jar"

if [ -z "$1" ]; then
    echo "用法: $0 <PDF文件路径>"
    echo "示例: $0 /home/peanut/project/test_dataset/tika-1.x/tika-parsers/src/test/resources/test-documents/testAnnotations.pdf"
    exit 1
fi

FILE="$1"

if ! command -v java &>/dev/null; then
    echo "[-] 提示: 未检测到系统 JRE (java)。"
    echo "    请先在宿主机执行轻量 JRE 安装命令:"
    echo "    sudo apt update && sudo apt install -y openjdk-21-jre-headless"
    exit 1
fi

if [ ! -f "$TIKA_JAR" ]; then
    echo "[-] tika-app.jar 正在下载中，请稍等片刻..."
    exit 1
fi

echo "========================================================================="
echo "  1. DocToText + PP-OCRv6 (当前工程)"
echo "========================================================================="
"${PROJECT_ROOT}/run_example2.sh" "$FILE"

echo ""
echo "========================================================================="
echo "  2. Apache Tika (官方基准引擎)"
echo "========================================================================="
java -jar "$TIKA_JAR" -t "$FILE"
echo "========================================================================="
