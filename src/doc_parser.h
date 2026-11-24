#ifndef DOCTOTEXT_DOC_PARSER_H
#define DOCTOTEXT_DOC_PARSER_H

#include "link.h"
#include <string>
#include <vector>

namespace doctotext
{
	class FormattingStyle;
	class Metadata;
	std::string extract_doc_eq_field_text(const std::string& utf8_params);
	typedef std::string (*ImageParserCallback)(const std::string& image_name,
	                                          const char* image_buffer,
	                                          size_t image_size,
	                                          void* user_data);
}
class ThreadSafeOLEStorage;

using namespace doctotext;

class DOCParser
{
	private:
		struct Implementation;
		Implementation* impl;

	public:
		DOCParser(const std::string& file_name);
		DOCParser(const char* buffer, size_t size);
		~DOCParser();
		void setVerboseLogging(bool verbose);
		void setLogStream(std::ostream& log_stream);
		void setImageParserCallback(doctotext::ImageParserCallback callback, void* user_data = NULL);
		bool isDOC();
		void getLinks(std::vector<Link>& links);
		std::string plainText(const FormattingStyle& formatting);
		std::string plainText(ThreadSafeOLEStorage& storage, const FormattingStyle& formatting);
		Metadata metaData();
		bool error();
		bool encrypted();
};

#endif
