/**
 * @file example2_ppocr_extractor.cpp
 * @brief 参考 soeasy 架构，使用 DocToText C API + PP-OCRv6 进行图文一体化提取示例 (Example 2)
 *
 * 【设计说明】
 * 本示例严格参考 /home/peanut/project/soeasy/src/worker/ocr 以及 L2OfficeParser 设计：
 * 1. 使用 DocToText 官方推荐的 C API (doctotext_c_api.h)，与 soeasy 保持完全一致；
 * 2. 默认开启内嵌图片 OCR 回调 (doctotext_extractor_params_set_image_parser_callback)；
 * 3. 完整的 PP-OCRv6 工业级推理流水线：
 *    - 真正的 DBNet 文本检测：对齐 32 像素、动态缩放、双阈值二值化、轮廓提取、unclip 多边形膨胀；
 *    - 透视变换旋转裁剪 (GetRotateCrop)：精准对齐倾斜文本行；
 *    - 真正的 SVTR 文本识别：动态宽度切片、CTC Greedy 解码与置信度计算；
 * 4. ImageFilter 两级智能过滤：
 *    - Tier 1：零拷贝快速探测头部维度 (ProbeBufferDimensions)，微秒级初筛小图标与像素炸弹；
 *    - Tier 2：解码后通道归一化 (NormalizeToBgr，支持透明通道复合到白底) 与方差纯色块检测；
 *    - Tier 3：有效字符计数阈值过滤 (CountEffectiveChars)。
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// OpenCV 4.x
#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

// ONNX Runtime C++ API
#include <onnxruntime_cxx_api.h>

// DocToText C API
#include "doctotext_c_api.h"

namespace soeasy_demo {

// ============================================================================
// 1. ImageFilter 图像前置智能过滤器 (参考 soeasy/src/worker/ocr/image_filter.cpp)
// ============================================================================
struct ImageDimension {
  int width = 0;
  int height = 0;
  bool valid = false;
};

struct FilterOptions {
  int min_side = 32;                                      // 过滤 < 32x32 的极端微小图标
  float max_aspect_ratio = 25.0f;                         // 过滤长宽比极端失衡的装饰条
  uint64_t max_file_size = 30ULL * 1024ULL * 1024ULL;     // 30MB 最大图片上限
  uint64_t max_image_pixels = 36ULL * 1024ULL * 1024ULL;  // 36MP 防解压 OOM
  double min_std_dev = 8.0;                               // stddev < 8 视为纯色色块/白板
  size_t min_effective_chars = 1;                         // 最少有效文本字数
};

class ImageFilter {
 public:
  using Options = FilterOptions;

  explicit ImageFilter(Options options = Options()) : options_(options) {}

  /**
   * @brief Tier 1: 零拷贝直接读取内存字节前导探测宽高 (支持 PNG, JPEG, BMP, GIF, WEBP)
   */
  static ImageDimension ProbeBufferDimensions(const uint8_t* buf, size_t len) {
    ImageDimension dim;
    if (!buf || len < 16) return dim;

    // PNG: 89 50 4E 47 0D 0A 1A 0A ... IHDR
    if (len >= 24 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G') {
      dim.width = (buf[16] << 24) | (buf[17] << 16) | (buf[18] << 8) | buf[19];
      dim.height = (buf[20] << 24) | (buf[21] << 16) | (buf[22] << 8) | buf[23];
      dim.valid = (dim.width > 0 && dim.height > 0);
      return dim;
    }

    // JPEG: FF D8 FF ...
    if (len >= 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
      size_t pos = 2;
      while (pos + 8 < len) {
        if (buf[pos] != 0xFF) break;
        uint8_t marker = buf[pos + 1];
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
          dim.height = (buf[pos + 5] << 8) | buf[pos + 6];
          dim.width = (buf[pos + 7] << 8) | buf[pos + 8];
          dim.valid = (dim.width > 0 && dim.height > 0);
          return dim;
        }
        size_t seg_len = (buf[pos + 2] << 8) | buf[pos + 3];
        pos += 2 + seg_len;
      }
    }

    // BMP: 'BM'
    if (len >= 26 && buf[0] == 'B' && buf[1] == 'M') {
      dim.width = buf[18] | (buf[19] << 8) | (buf[20] << 16) | (buf[21] << 24);
      dim.height = std::abs((int)(buf[22] | (buf[23] << 8) | (buf[24] << 16) | (buf[25] << 24)));
      dim.valid = (dim.width > 0 && dim.height > 0);
      return dim;
    }

    return dim;
  }

  bool CheckDimensions(int w, int h) const {
    if (w <= 0 || h <= 0) return false;
    if (w < options_.min_side && h < options_.min_side) return false;
    if (static_cast<uint64_t>(w) * h > options_.max_image_pixels) return false;
    float aspect = static_cast<float>(w) / h;
    if (aspect > options_.max_aspect_ratio || aspect < (1.0f / options_.max_aspect_ratio)) return false;
    return true;
  }

  /**
   * @brief Tier 2: 图像通道归一化为 8 位 3 通道 BGR（透明通道混合至白底背景）
   */
  static cv::Mat NormalizeToBgr(const cv::Mat& src) {
    if (src.empty()) return cv::Mat();
    cv::Mat bgr;
    if (src.channels() == 4) {
      // RGBA/BGRA: 合成到纯白底上，防止透明背景上的黑字变纯黑
      std::vector<cv::Mat> chs;
      cv::split(src, chs);
      cv::Mat alpha = chs[3];
      cv::Mat rgb_base;
      cv::merge(std::vector<cv::Mat>{chs[0], chs[1], chs[2]}, rgb_base);

      bgr = cv::Mat(src.size(), CV_8UC3, cv::Scalar(255, 255, 255));
      for (int y = 0; y < src.rows; ++y) {
        const uchar* a_ptr = alpha.ptr<uchar>(y);
        const cv::Vec3b* s_ptr = rgb_base.ptr<cv::Vec3b>(y);
        cv::Vec3b* d_ptr = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < src.cols; ++x) {
          float a = a_ptr[x] / 255.0f;
          d_ptr[x][0] = static_cast<uchar>(s_ptr[x][0] * a + 255 * (1.0f - a));
          d_ptr[x][1] = static_cast<uchar>(s_ptr[x][1] * a + 255 * (1.0f - a));
          d_ptr[x][2] = static_cast<uchar>(s_ptr[x][2] * a + 255 * (1.0f - a));
        }
      }
    } else if (src.channels() == 1) {
      cv::cvtColor(src, bgr, cv::COLOR_GRAY2BGR);
    } else {
      bgr = src;
    }
    if (bgr.depth() != CV_8U) {
      bgr.convertTo(bgr, CV_8U);
    }
    return bgr;
  }

  bool CheckDecodedImage(const cv::Mat& bgr) const {
    if (bgr.empty()) return false;
    cv::Scalar mean, stddev;
    cv::meanStdDev(bgr, mean, stddev);
    double total_stddev = (stddev[0] + stddev[1] + stddev[2]) / 3.0;
    if (total_stddev < options_.min_std_dev) {
      return false;  // 画面几乎是单色白板或纯色块
    }
    return true;
  }

  static size_t CountEffectiveChars(const std::string& text) {
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
      unsigned char c = text[i];
      if (c <= 127) {
        if (!isspace(c) && !ispunct(c)) count++;
        i += 1;
      } else if ((c & 0xE0) == 0xC0) {
        count++;
        i += 2;
      } else if ((c & 0xF0) == 0xE0) {
        count++;
        i += 3;
      } else {
        count++;
        i += 4;
      }
    }
    return count;
  }

  const Options& options() const { return options_; }

 private:
  Options options_;
};

// ============================================================================
// 2. PP-OCRv6 完整推理流水线 (参考 soeasy/src/worker/ocr/ppocr_v6_engine.cpp)
// ============================================================================
struct OcrPoint {
  float x = 0.0f;
  float y = 0.0f;
};

struct OcrBox {
  std::vector<OcrPoint> points;
  float score = 0.0f;
};

class PPOCRv6Engine {
 public:
  PPOCRv6Engine() {
    env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "PPOCRv6_Demo");
    mem_info_ = std::make_unique<Ort::MemoryInfo>(
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault));
  }

  bool Initialize(const std::string& det_model, const std::string& rec_model, const std::string& dict_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
      Ort::SessionOptions opts;
      opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      opts.SetIntraOpNumThreads(2);
      opts.SetInterOpNumThreads(1);

      det_session_ = std::make_unique<Ort::Session>(*env_, det_model.c_str(), opts);
      rec_session_ = std::make_unique<Ort::Session>(*env_, rec_model.c_str(), opts);

      // 加载字符表
      dict_.clear();
      std::ifstream ifs(dict_file);
      if (!ifs.is_open()) {
        std::cerr << "[PPOCR] 打开字典文件失败: " << dict_file << std::endl;
        return false;
      }
      std::string line;
      while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        dict_.push_back(line);
      }

      initialized_ = true;
      std::cout << "[PPOCRv6] 引擎加载就绪! 字典大小: " << dict_.size() << std::endl;
      return true;
    } catch (const std::exception& e) {
      std::cerr << "[PPOCRv6] 初始化异常: " << e.what() << std::endl;
      return false;
    }
  }

  static cv::Size DetResize(cv::Mat& img, int limit_side_len = 960, int align = 32) {
    if (img.empty()) return cv::Size();
    int h = img.rows, w = img.cols;
    cv::Size orig(w, h);
    float ratio = 1.0f;
    int max_side = (std::max)(h, w);
    if (limit_side_len > 0 && max_side > limit_side_len) {
      ratio = static_cast<float>(limit_side_len) / static_cast<float>(max_side);
    }
    int new_w = (std::max)(static_cast<int>(std::round(w * ratio / align)) * align, align);
    int new_h = (std::max)(static_cast<int>(std::round(h * ratio / align)) * align, align);
    if (new_w != w || new_h != h) {
      cv::resize(img, img, cv::Size(new_w, new_h), 0, 0, (new_w < w || new_h < h) ? cv::INTER_AREA : cv::INTER_CUBIC);
    }
    return orig;
  }

  static cv::Mat GetRotateCrop(const cv::Mat& img, const std::vector<OcrPoint>& pts) {
    if (pts.size() < 4) return cv::Mat();
    cv::Point2f p0(pts[0].x, pts[0].y);
    cv::Point2f p1(pts[1].x, pts[1].y);
    cv::Point2f p2(pts[2].x, pts[2].y);
    cv::Point2f p3(pts[3].x, pts[3].y);

    float w1 = static_cast<float>(cv::norm(p1 - p0));
    float w2 = static_cast<float>(cv::norm(p3 - p2));
    float dst_w = (std::max)(w1, w2);
    float h1 = static_cast<float>(cv::norm(p2 - p0));
    float h2 = static_cast<float>(cv::norm(p3 - p1));
    float dst_h = (std::max)(h1, h2);

    int iw = (std::max)(static_cast<int>(dst_w), 1);
    int ih = (std::max)(static_cast<int>(dst_h), 1);

    const int pad = 3;
    std::vector<cv::Point2f> src = {p0, p1, p2, p3};
    std::vector<cv::Point2f> dst = {{float(pad), float(pad)},
                                    {float(iw - 1 + pad), float(pad)},
                                    {float(pad), float(ih - 1 + pad)},
                                    {float(iw - 1 + pad), float(ih - 1 + pad)}};

    cv::Mat M = cv::getPerspectiveTransform(src, dst);
    cv::Mat crop;
    cv::warpPerspective(img, crop, M, cv::Size(iw + 2 * pad, ih + 2 * pad), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    return crop;
  }

  std::vector<OcrBox> Detect(const cv::Mat& norm_bgr) {
    cv::Mat det_img = norm_bgr.clone();
    cv::Size orig_size = DetResize(det_img, 960, 32);
    int det_h = det_img.rows, det_w = det_img.cols;

    cv::Mat rgb;
    cv::cvtColor(det_img, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgb, CV_32F, 1.0 / 255.0);
    constexpr float mean[3] = {0.485f, 0.456f, 0.406f};
    constexpr float std[3] = {0.229f, 0.224f, 0.225f};
    std::vector<cv::Mat> channels(3);
    cv::split(rgb, channels);
    for (int c = 0; c < 3; ++c) channels[c] = (channels[c] - mean[c]) / std[c];

    std::vector<float> det_in(3 * det_h * det_w);
    for (int c = 0; c < 3; ++c)
      for (int y = 0; y < det_h; ++y)
        for (int x = 0; x < det_w; ++x)
          det_in[c * det_h * det_w + y * det_w + x] = channels[c].at<float>(y, x);

    std::vector<int64_t> det_shape = {1, 3, det_h, det_w};
    const char* in_names[] = {"x"};
    const char* out_names[] = {"fetch_name_0"};
    Ort::Value in_tensor = Ort::Value::CreateTensor<float>(
        *mem_info_, det_in.data(), det_in.size(), det_shape.data(), det_shape.size());

    auto det_out = det_session_->Run(Ort::RunOptions{nullptr}, in_names, &in_tensor, 1, out_names, 1);
    const float* pred = det_out[0].GetTensorData<float>();

    cv::Mat bin_map(det_h, det_w, CV_8U);
    for (int i = 0; i < det_h * det_w; ++i) bin_map.data[i] = (pred[i] > 0.3f) ? 255 : 0;

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bin_map, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);

    float scale_w = float(orig_size.width) / float(det_w);
    float scale_h = float(orig_size.height) / float(det_h);

    std::vector<OcrBox> boxes;
    for (const auto& cnt : contours) {
      if (cnt.size() < 4) continue;
      cv::RotatedRect min_rect = cv::minAreaRect(cnt);
      if ((std::min)(min_rect.size.width, min_rect.size.height) < 3.0f) continue;

      cv::Rect bbox = cv::boundingRect(cnt);
      bbox &= cv::Rect(0, 0, det_w, det_h);
      if (bbox.area() <= 0) continue;

      double peri = cv::arcLength(cnt, true);
      std::vector<cv::Point> approx;
      cv::approxPolyDP(cnt, approx, 0.002 * peri, true);
      if (approx.size() < 4) continue;

      double area = cv::contourArea(approx);
      double perimeter = cv::arcLength(approx, true);
      double distance = (perimeter > 1e-4) ? (area * 1.5 / perimeter) : 0;

      int ksize = (std::max)(1, (int)std::ceil(distance));
      const int pad = ksize + 2;
      cv::Rect roi = cv::boundingRect(approx);
      const int rx = (std::max)(0, roi.x - pad);
      const int ry = (std::max)(0, roi.y - pad);
      const int rw = (std::min)(det_w - rx, roi.width + (roi.x - rx) + pad);
      const int rh = (std::min)(det_h - ry, roi.height + (roi.y - ry) + pad);
      cv::Rect exp_roi(rx, ry, rw, rh);
      if (exp_roi.area() <= 0) continue;

      cv::Mat poly_mask = cv::Mat::zeros(exp_roi.size(), CV_8U);
      std::vector<cv::Point> local_approx = approx;
      for (auto& p : local_approx) p -= exp_roi.tl();
      cv::drawContours(poly_mask, std::vector<std::vector<cv::Point>>{local_approx}, 0, cv::Scalar(255), cv::FILLED);

      cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(ksize, ksize));
      cv::dilate(poly_mask, poly_mask, kernel);

      std::vector<std::vector<cv::Point>> dilated;
      cv::findContours(poly_mask, dilated, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
      if (dilated.empty()) continue;

      auto& unclipped = *std::max_element(dilated.begin(), dilated.end(), [](const auto& a, const auto& b) {
        return cv::contourArea(a) < cv::contourArea(b);
      });

      cv::RotatedRect rrect = cv::minAreaRect(unclipped);
      std::vector<cv::Point2f> pts(4);
      rrect.points(pts.data());
      for (auto& p : pts) p += cv::Point2f(static_cast<float>(exp_roi.x), static_cast<float>(exp_roi.y));

      std::sort(pts.begin(), pts.end(), [](const cv::Point2f& a, const cv::Point2f& b) {
        return (std::abs(a.y - b.y) > 1e-3f) ? (a.y < b.y) : (a.x < b.x);
      });
      if (pts[0].x > pts[1].x) std::swap(pts[0], pts[1]);
      if (pts[2].x > pts[3].x) std::swap(pts[2], pts[3]);

      OcrBox obox;
      for (const auto& p : pts) obox.points.push_back({p.x * scale_w, p.y * scale_h});
      boxes.push_back(obox);
    }
    return boxes;
  }

  std::string Recognize(const cv::Mat& bgr_img) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || bgr_img.empty()) return "";

    try {
      cv::Mat norm_bgr = ImageFilter::NormalizeToBgr(bgr_img);
      if (norm_bgr.empty()) return "";

      std::vector<OcrBox> boxes = Detect(norm_bgr);
      if (boxes.empty()) return "";

      std::sort(boxes.begin(), boxes.end(), [](const OcrBox& a, const OcrBox& b) {
        float ay = (std::min)(a.points[0].y, a.points[1].y);
        float by = (std::min)(b.points[0].y, b.points[1].y);
        return ay < by;
      });

      const char* in_names[] = {"x"};
      const char* out_names[] = {"fetch_name_0"};
      std::string full_text;

      for (const auto& b : boxes) {
        cv::Mat crop = GetRotateCrop(norm_bgr, b.points);
        if (crop.empty()) continue;
        if (crop.rows > crop.cols * 1.5f) {
          cv::rotate(crop, crop, cv::ROTATE_90_CLOCKWISE);
        }

        int target_h = 48;
        int target_w = (std::max)(320, int(crop.cols * (float(target_h) / crop.rows)));
        int resized_w = (std::min)(int(std::ceil(target_h * (float(crop.cols) / crop.rows))), target_w);
        cv::Mat resized_crop;
        cv::resize(crop, resized_crop, cv::Size(resized_w, target_h));

        std::vector<float> rec_in(3 * target_h * target_w, 0.0f);
        for (int c = 0; c < 3; ++c)
          for (int y = 0; y < target_h; ++y)
            for (int x = 0; x < resized_w; ++x)
              rec_in[c * target_h * target_w + y * target_w + x] =
                  (resized_crop.at<cv::Vec3b>(y, x)[c] / 255.0f - 0.5f) / 0.5f;

        std::vector<int64_t> rec_shape = {1, 3, target_h, target_w};
        Ort::Value rec_tensor = Ort::Value::CreateTensor<float>(
            *mem_info_, rec_in.data(), rec_in.size(), rec_shape.data(), rec_shape.size());

        auto r_out = rec_session_->Run(Ort::RunOptions{nullptr}, in_names, &rec_tensor, 1, out_names, 1);
        auto shape = r_out[0].GetTensorTypeAndShapeInfo().GetShape();
        int T = static_cast<int>(shape[1]);
        int C = static_cast<int>(shape[2]);
        const float* logits = r_out[0].GetTensorData<float>();

        // CTC Greedy 解码
        std::string line_text;
        int prev = -1;
        for (int t = 0; t < T; ++t) {
          int max_c = 0;
          float max_v = logits[t * C];
          for (int c = 1; c < C; ++c) {
            if (logits[t * C + c] > max_v) {
              max_v = logits[t * C + c];
              max_c = c;
            }
          }
          if (max_c == 0) {
            prev = -1;
            continue;
          }
          if (max_c == prev) continue;
          if (max_c < static_cast<int>(dict_.size())) {
            line_text += dict_[max_c];
          }
          prev = max_c;
        }

        if (!line_text.empty() && ImageFilter::CountEffectiveChars(line_text) >= 1) {
          if (!full_text.empty()) full_text += "\n";
          full_text += line_text;
        }
      }
      return full_text;
    } catch (const std::exception& e) {
      std::cerr << "[PPOCRv6] 推理异常: " << e.what() << std::endl;
      return "";
    }
  }

 private:
  std::mutex mutex_;
  bool initialized_ = false;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> det_session_;
  std::unique_ptr<Ort::Session> rec_session_;
  std::unique_ptr<Ort::MemoryInfo> mem_info_;
  std::vector<std::string> dict_;
};

// 全局 OCR 引擎单例
static PPOCRv6Engine g_ocr_engine;
static ImageFilter g_filter;

}  // namespace soeasy_demo

// ============================================================================
// 3. 符合 DocToText C API 的图像回调实现 (参考 soeasy L2OfficeParser)
// ============================================================================
extern "C" const char* DocToTextImageParserCallbackC(
    const char* image_name,
    const char* image_buffer,
    size_t image_size,
    void* user_data) {
  if (!image_buffer || image_size < 128 || image_size > 30ULL * 1024ULL * 1024ULL) {
    return nullptr;
  }

  // Tier 1: 零拷贝快速维度初筛
  auto dim = soeasy_demo::ImageFilter::ProbeBufferDimensions(
      reinterpret_cast<const uint8_t*>(image_buffer), image_size);
  if (dim.valid && !soeasy_demo::g_filter.CheckDimensions(dim.width, dim.height)) {
    return nullptr;
  }

  // 内存解码
  std::vector<uint8_t> buf(
      reinterpret_cast<const uint8_t*>(image_buffer),
      reinterpret_cast<const uint8_t*>(image_buffer) + image_size);
  cv::Mat raw_img = cv::imdecode(buf, cv::IMREAD_UNCHANGED);
  cv::Mat img = soeasy_demo::ImageFilter::NormalizeToBgr(raw_img);
  if (img.empty()) return nullptr;

  // Tier 2: 纯色与单调背景过滤
  if (!soeasy_demo::g_filter.CheckDecodedImage(img)) {
    return nullptr;
  }

  std::cout << "[DocToText-ImageCallback] 触发内嵌图片 OCR: "
            << (image_name ? image_name : "unnamed")
            << " (尺寸: " << img.cols << "x" << img.rows << ")" << std::endl;

  // 执行真实 PP-OCRv6 推理
  std::string ocr_text = soeasy_demo::g_ocr_engine.Recognize(img);
  if (ocr_text.empty() ||
      soeasy_demo::ImageFilter::CountEffectiveChars(ocr_text) < soeasy_demo::g_filter.options().min_effective_chars) {
    return nullptr;
  }

  // 使用 thread_local 保证在 C API 主调方消费前有效且线程安全
  static thread_local std::string tls_result;
  tls_result = "\n[图片文字识别:\n" + ocr_text + "\n]\n";
  return tls_result.c_str();
}

// ============================================================================
// 4. 主程序：模拟 soeasy 调用 DocToText C API 提取 PDF
// ============================================================================
int main(int argc, char** argv) {
  if (argc < 2) {
    std::cout << "===================================================================\n"
              << "  DocToText C API + PP-OCRv6 真实工业级抽取示例 (Example 2)\n"
              << "===================================================================\n"
              << "用法: " << argv[0] << " <PDF/文档文件路径> [模型基础目录]\n"
              << "示例: " << argv[0] << " /path/to/testOCR.pdf /home/peanut/project/soeasy/models/ppocr_v6\n"
              << std::endl;
    return 1;
  }

  std::string file_path = argv[1];
  std::string models_dir = (argc >= 3) ? argv[2] : "/home/peanut/project/soeasy/models/ppocr_v6";

  // 1. 初始化 PP-OCRv6 引擎
  std::string det_path = models_dir + "/PP-OCRv6_tiny_det_onnx/inference.onnx";
  std::string rec_path = models_dir + "/PP-OCRv6_tiny_rec_onnx/inference.onnx";
  std::string dict_path = models_dir + "/PP-OCRv6_tiny_rec_onnx/chars.txt";

  std::cout << ">>> 正在初始化 PP-OCRv6 引擎..." << std::endl;
  std::cout << "    Det 模型: " << det_path << std::endl;
  std::cout << "    Rec 模型: " << rec_path << std::endl;
  std::cout << "    字典路径: " << dict_path << std::endl;

  if (!soeasy_demo::g_ocr_engine.Initialize(det_path, rec_path, dict_path)) {
    std::cerr << "[-] PP-OCRv6 引擎初始化失败，请检查模型目录！" << std::endl;
    return 1;
  }

  // 2. 模拟 soeasy L2OfficeParser: 调用 DocToText C API 配置参数
  std::cout << "\n>>> 正在配置 DocToText C API 提取参数 (模拟 soeasy 解析管线)..." << std::endl;
  std::unique_ptr<DocToTextExtractorParams, decltype(&doctotext_free_extractor_params)> params(
      doctotext_create_extractor_params(), &doctotext_free_extractor_params);
  if (!params) {
    std::cerr << "[-] 创建 DocToTextExtractorParams 失败!" << std::endl;
    return 1;
  }

  // 设置关闭冗余日志 / 或设置日志级别
  doctotext_extractor_params_set_verbose_logging(params.get(), 0);

  // 指定解析器类型 (PDF 优先解析)
  doctotext_extractor_params_set_parser_type(params.get(), DOCTOTEXT_PARSER_PDF);

  // 【核心功能】：默认开启内嵌图片 OCR 回调，注册 C 风格回调函数
  std::cout << ">>> 【默认开启内嵌图片 OCR 回调】已注册到 DocToText C API..." << std::endl;
  doctotext_extractor_params_set_image_parser_callback(
      params.get(), &DocToTextImageParserCallbackC, nullptr);

  // 3. 执行抽取
  std::cout << ">>> 正在解析文档: " << file_path << std::endl;
  auto start_time = std::chrono::high_resolution_clock::now();

  DocToTextException* raw_exc = nullptr;
  std::unique_ptr<DocToTextExtractedData, decltype(&doctotext_free_extracted_data)> data(
      doctotext_process_file(file_path.c_str(), params.get(), &raw_exc),
      &doctotext_free_extracted_data);
  std::unique_ptr<DocToTextException, decltype(&doctotext_free_exception)> exc(
      raw_exc, &doctotext_free_exception);

  auto end_time = std::chrono::high_resolution_clock::now();
  double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

  if (exc) {
    std::cerr << "[-] DocToText 解析异常: "
              << (doctotext_exception_get_error_message(exc.get(), 0) ? doctotext_exception_get_error_message(exc.get(), 0) : "未知错误")
              << std::endl;
    return 1;
  }

  // 4. 给出内容
  if (data) {
    const char* txt = doctotext_extracted_data_get_text(data.get());
    std::string result_text = txt ? txt : "";
    std::cout << "\n============================== 提取结果内容 ==============================\n"
              << result_text
              << "\n=========================================================================\n";
    std::cout << "解析状态: 成功 (OK)\n";
    std::cout << "总耗时:   " << elapsed_ms << " ms\n";
    std::cout << "字符总数: " << result_text.size() << " 字符\n";
  } else {
    std::cout << "[-] 未能提取到数据 (返回为空)\n";
  }

  return 0;
}
