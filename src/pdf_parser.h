#ifndef DOCTOTEXT_PDF_PARSER_H
#define DOCTOTEXT_PDF_PARSER_H

#include "link.h"
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

class PDFParser
{
	private:
		struct Implementation;
		Implementation* impl;

	public:
		PDFParser(const std::string& file_name);
		PDFParser(const char* buffer, size_t size);
		~PDFParser();
		void setVerboseLogging(bool verbose);
		void setLogStream(std::ostream& log_stream);
		void setImageParserCallback(doctotext::ImageParserCallback callback, void* user_data = NULL);
		bool isPDF();
		void getLinks(std::vector<Link>& links);
		std::string plainText(const FormattingStyle& formatting);
		bool error();
		Metadata metaData();
};

#endif
