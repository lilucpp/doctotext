#include "../build/plain_text_extractor.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <stdint.h>
#include <string>
#include <vector>

using namespace doctotext;

namespace
{
	struct CallbackState
	{
		int expected_w;
		int expected_h;
		int hits;
		bool valid_bmp;
		std::string error;

		CallbackState(int w, int h)
			: expected_w(w), expected_h(h), hits(0), valid_bmp(false)
		{}
	};

	uint32_t read_u32_le(const unsigned char* p)
	{
		return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	}

	uint16_t read_u16_le(const unsigned char* p)
	{
		return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
	}

	std::string verify_bmp_callback(const std::string& image_name,
	                                const char* image_buffer,
	                                size_t image_size,
	                                void* user_data)
	{
		CallbackState* state = static_cast<CallbackState*>(user_data);
		state->hits++;

		if (!image_buffer || image_size < 62)
		{
			state->error = "Buffer too small for 1-bpp BMP";
			return "";
		}

		const unsigned char* buf = reinterpret_cast<const unsigned char*>(image_buffer);
		if (buf[0] != 'B' || buf[1] != 'M')
		{
			state->error = "Missing BMP signature";
			return "";
		}

		uint32_t offset = read_u32_le(buf + 10);
		uint32_t width = read_u32_le(buf + 18);
		int32_t height = (int32_t)read_u32_le(buf + 22);
		uint16_t bpp = read_u16_le(buf + 28);

		if (width != (uint32_t)state->expected_w || (height != state->expected_h && height != -state->expected_h))
		{
			state->error = "Unexpected BMP dimensions";
			return "";
		}
		if (bpp != 1 || offset >= image_size)
		{
			state->error = "Unexpected bpp or pixel offset";
			return "";
		}

		// Verify decoded pixel area is non-uniform (contains both ink and background)
		bool has_zero = false;
		bool has_nonzero = false;
		for (size_t i = offset; i < image_size; ++i)
		{
			if (buf[i] != 0x00)
				has_nonzero = true;
			if (buf[i] != 0xFF)
				has_zero = true;
			if (has_zero && has_nonzero)
				break;
		}
		if (!has_zero || !has_nonzero)
		{
			state->error = "Decoded BMP pixel buffer is uniform/blank";
			return "";
		}

		state->valid_bmp = true;
		return "[DECODED_IMAGE:" + image_name + "]";
	}

	bool run_case(const std::string& file_name,
	              int expected_w,
	              int expected_h,
	              const std::string& required_substr)
	{
		// 1. File-path extraction test (requires explicit setParseDocumentImages(true))
		{
			CallbackState state(expected_w, expected_h);
			PlainTextExtractor extractor;
			extractor.setImageParserCallback(verify_bmp_callback, &state);
			extractor.setParseDocumentImages(true);
			std::string text;
			if (!extractor.processFile(file_name, text))
			{
				std::cerr << "FAIL (processFile): " << file_name << std::endl;
				return false;
			}
			if (state.hits < 1 || !state.valid_bmp)
			{
				std::cerr << "FAIL (callback check on file): " << file_name
				          << " hits=" << state.hits << " err=" << state.error << std::endl;
				return false;
			}
			if (text.find("[DECODED_IMAGE:") == std::string::npos ||
			    (!required_substr.empty() && text.find(required_substr) == std::string::npos))
			{
				std::cerr << "FAIL (missing injected OCR/text on file): " << file_name << std::endl;
				return false;
			}
		}

		// 2. Memory-buffer extraction test
		{
			std::ifstream src(file_name.c_str(), std::ios_base::in | std::ios_base::binary);
			if (!src.is_open())
			{
				std::cerr << "FAIL (cannot open file): " << file_name << std::endl;
				return false;
			}
			std::string content((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
			CallbackState state(expected_w, expected_h);
			PlainTextExtractor extractor;
			extractor.setImageParserCallback(verify_bmp_callback, &state);
			extractor.setParseDocumentImages(true);
			std::string text;
			if (!extractor.processFile(content.c_str(), content.size(), text))
			{
				std::cerr << "FAIL (processFile buffer): " << file_name << std::endl;
				return false;
			}
			if (state.hits < 1 || !state.valid_bmp)
			{
				std::cerr << "FAIL (callback check on buffer): " << file_name
				          << " hits=" << state.hits << " err=" << state.error << std::endl;
				return false;
			}
			if (text.find("[DECODED_IMAGE:") == std::string::npos ||
			    (!required_substr.empty() && text.find(required_substr) == std::string::npos))
			{
				std::cerr << "FAIL (missing injected OCR/text on buffer): " << file_name << std::endl;
				return false;
			}
		}

		std::cout << "PASS: " << file_name << " (" << expected_w << "x" << expected_h << " 1-bpp BMP decoded)" << std::endl;
		return true;
	}
}

int main()
{
	// Verify default parse_document_images=false does not dispatch document images even with callback set
	{
		CallbackState state(352, 91);
		PlainTextExtractor extractor;
		extractor.setImageParserCallback(verify_bmp_callback, &state);
		std::string text;
		if (!extractor.processFile("testPDF_JBIG2.pdf", text) || state.hits != 0)
		{
			std::cerr << "FAIL: default parse_document_images=false invoked callback hits=" << state.hits << std::endl;
			return 1;
		}
	}

	if (!run_case("testPDF_JBIG2.pdf", 352, 91, "This is a sample file to test images compressed using JBIG2."))
		return 1;
	if (!run_case("testPDF_CCITT_G4.pdf", 400, 100, ""))
		return 1;
	if (!run_case("testPDF_CCITT_G3.pdf", 400, 100, ""))
		return 1;
	return 0;
}
