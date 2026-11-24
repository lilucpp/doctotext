#ifndef DOCTOTEXT_RTF_PARSER_H
#define DOCTOTEXT_RTF_PARSER_H

#include "link.h"
#include <cstddef>
#include <string>
#include <vector>

namespace doctotext
{
	struct FormattingStyle;
	class Metadata;
	typedef std::string (*ImageParserCallback)(const std::string& image_name,
	                                          const char* image_buffer,
	                                          size_t image_size,
	                                          void* user_data);
}
using namespace doctotext;

class RTFParser
{
	private:
		struct Implementation;
		Implementation* impl;

	public:
		RTFParser(const std::string& file_name);
		RTFParser(const char* buffer, size_t size);
		~RTFParser();
		void setVerboseLogging(bool verbose);
		void setLogStream(std::ostream& log_stream);
		void setImageParserCallback(doctotext::ImageParserCallback callback, void* user_data = NULL);
		bool isRTF();
		void getLinks(std::vector<Link>& links);
		std::string plainText();
		std::string plainText(const FormattingStyle& formatting);
		Metadata metaData();
		bool error();
};

#endif
