#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "plain_text_extractor.h"

using namespace doctotext;

static const char* TESSERACT_BIN = "/home/peanut/project/docwire/vcpkg/installed/x64-linux-dynamic/tools/tesseract/tesseract";
static const char* TESSDATA_DIR = "/home/peanut/project/docwire/vcpkg/installed/x64-linux-dynamic/share/tessdata-fast";
static const char* VCPKG_LIB_DIR = "/home/peanut/project/docwire/vcpkg/installed/x64-linux-dynamic/lib";

// OCR callback using Tesseract CLI
std::string ocr_callback(const std::string& image_name, const char* image_buffer, size_t image_size, void* user_data)
{
    char tmp_template[] = "/tmp/doctotext_img_XXXXXX.png";
    int fd = mkstemps(tmp_template, 4);
    if (fd < 0) return "";
    write(fd, image_buffer, image_size);
    close(fd);

    std::string out_base = std::string(tmp_template) + "_out";
    std::string cmd = std::string("LD_LIBRARY_PATH=") + VCPKG_LIB_DIR + ":$LD_LIBRARY_PATH " +
                      "TESSDATA_PREFIX=" + TESSDATA_DIR + " " +
                      TESSERACT_BIN + " " + tmp_template + " " + out_base + " -l eng+chi_sim >/dev/null 2>&1";
    int ret = system(cmd.c_str());

    std::string result;
    if (ret == 0) {
        std::string out_file = out_base + ".txt";
        std::ifstream ifs(out_file.c_str());
        if (ifs.is_open()) {
            std::stringstream buffer;
            buffer << ifs.rdbuf();
            result = buffer.str();
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
                result.pop_back();
            unlink(out_file.c_str());
        }
    }
    unlink(tmp_template);

    if (result.empty()) {
        return "\n[IMAGE: " + image_name + " (No text detected)]\n";
    }
    return "\n[OCR INJECTED (" + image_name + "): " + result + "]\n";
}

int main(int argc, char* argv[])
{
    std::string target_dir = "tests_img";
    if (argc >= 2) {
        target_dir = argv[1];
    }

    std::cout << "==========================================================================" << std::endl;
    std::cout << "  DocToText Image & Embedded Image Test Runner (Target: " << target_dir << ")" << std::endl;
    std::cout << "==========================================================================" << std::endl;

    DIR* dir = opendir(target_dir.c_str());
    if (!dir) {
        std::cerr << "Failed to open directory: " << target_dir << std::endl;
        return 1;
    }

    std::vector<std::string> test_files;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        std::string path = target_dir + "/" + entry->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            test_files.push_back(path);
        }
    }
    closedir(dir);

    PlainTextExtractor extractor;
    extractor.setImageParserCallback(ocr_callback, NULL);

    int total = 0;
    int success = 0;

    for (size_t i = 0; i < test_files.size(); ++i) {
        total++;
        const std::string& path = test_files[i];
        std::cout << "\n--------------------------------------------------------------------------" << std::endl;
        std::cout << "📄 Testing File [" << (i + 1) << "/" << test_files.size() << "]: " << path << std::endl;
        std::cout << "--------------------------------------------------------------------------" << std::endl;

        std::string text;
        bool ok = extractor.processFile(path, text);
        if (ok) {
            success++;
            std::cout << "状态: [解析成功] (输出字符数: " << text.length() << ")" << std::endl;
            if (text.empty() || text.find_first_not_of(" \t\n\r") == std::string::npos) {
                std::cout << "📝 提取内容: [空文本] (注: 若为纯图片扫描版PDF，原生暂无内置OCR，建议外置渲染)" << std::endl;
            } else {
                std::cout << "📝 提取内容预览:\n" << text << std::endl;
            }
        } else {
            std::cout << "状态: [解析失败]" << std::endl;
        }
    }

    std::cout << "\n==========================================================================" << std::endl;
    std::cout << "  测试完成! 总计: " << total << ", 成功处理: " << success << std::endl;
    std::cout << "==========================================================================" << std::endl;

    return 0;
}
