# DocToText + PP-OCR 图像就地识别集成示例

本目录提供了在 **DocToText** 中通过回调机制无缝集成 **PP-OCR (PaddleOCR / PP-OCRv6)** 的完整示例代码。

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
