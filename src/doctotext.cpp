/****************************************************************************
**
** DocToText - Converts DOC, XLS, XLSB, PPT, RTF, ODF (ODT, ODS, ODP),
**             OOXML (DOCX, XLSX, PPTX), iWork (PAGES, NUMBERS, KEYNOTE),
**             ODFXML (FODP, FODS, FODT), PDF, EML and HTML documents to plain text.
**             Extracts metadata and annotations.
**
** Copyright (c) 2006-2013, SILVERCODERS(R)
** http://silvercoders.com
**
** Project homepage: http://silvercoders.com/en/products/doctotext
**
** This program may be distributed and/or modified under the terms of the
** GNU General Public License version 2 as published by the Free Software
** Foundation and appearing in the file COPYING.GPL included in the
** packaging of this file.
**
** Please remember that any attempt to workaround the GNU General Public
** License using wrappers, pipes, client/server protocols, and so on
** is considered as license violation. If your program, published on license
** other than GNU General Public License version 2, calls some part of this
** code directly or indirectly, you have to buy commercial license.
** If you do not like our point of view, simply do not use the product.
**
** Licensees holding valid commercial license for this product
** may use this file in accordance with the license published by
** SILVERCODERS and appearing in the file COPYING.COM
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
**
*****************************************************************************/

#include <fstream>
#include "misc.h"
#include "doctotext_unzip.h"
#include <iostream>
#include "metadata.h"
#include "plain_text_extractor.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#ifdef DEBUG
	#include "tracing.h"
#endif
#include "../version.h"
using namespace std;

static void version()
{
	printf("DocToText v%s\n"
		"Converts DOC, XLS, XLSB, PPT, RTF, ODF (ODT, ODS, ODP, ODG), OOXML (DOCX, XLSX, PPTX),\n"
		"iWork (PAGES, NUMBERS, KEY), ODFXML, PDF, EML, HTML, and Images to plain text.\n"
		"Copyright (c) 2006-2013 SILVERCODERS(R) (http://silvercoders.com)\n"
		"Modernized and maintained by lithiumFlower (2020-2022) and lilucpp (2025-present)\n",
		VERSION);
}

static void help()
{
	version();
	printf("\nUsage: doctotext [OPTION]... [FILE]\n\n");
	printf("FILE\t\tname of file to convert\n\n");
	printf("Options:\n");
	printf("--meta\textract metadata instead of text.\n");
	printf("--rtf\ttry to parse as RTF document first.\n");
	printf("--odf\ttry to parse as ODF/OOXML document first.\n");
	printf("--ooxml\ttry to parse as ODF/OOXML document first.\n");
	printf("--xls\ttry to parse as XLS document first.\n");
	printf("--xlsb\ttry to parse as XLSB document first.\n");
	printf("--iwork\ttry to parse as iWork document first.\n");
	printf("--ppt\ttry to parse as PPT document first.\n");
	printf("--doc\ttry to parse as DOC document first.\n");
	printf("--html\ttry to parse as HTML document first.\n");
	printf("--pdf\ttry to parse as PDF document first.\n");
	printf("--eml\ttry to parse as EML document first.\n");
	printf("--odfxml\ttry to parse as ODFXML (Open Document Flat XML) document first.\n");
	printf("--txt\ttry to parse as plain text document first.\n");
	printf("--image\ttry to parse as image document first.\n");
	printf("--table-style=[one-row|one-col|table-look]\tselect how tables are rendered (default: table-look).\n");
	printf("--url-style=[text-only|extended|underscored]\tselect how hyperlinks are rendered (default: underscored).\n");
	printf("--list-style-prefix=[PREFIX]\tprefix used for list items (default: \" * \").\n");
	printf("--fix-xml\ttry to fix corrupted xml files (ODF, OOXML)\n");
	printf("--strip-xml\tstrip xml tags instead of parsing them (ODF, OOXML)\n");
	printf("--unzip-cmd=[COMMAND]\tuse COMMAND to unzip files from archives (ODF, OOXML)\n"
		"\tinstead of build-in decompression library.\n"
		"\t%%d in the command is substituted with destination directory path\n"
		"\t%%a in the command is substituted with name of archive file\n"
		"\t%%f in the command is substituted with name of file to extract\n"
		"\tExample: --unzip-cmd=\"unzip -d %%d %%a %%f\"\n"
	);
	printf("--verbose\tturn on verbose logging\n");
	printf("--log-file=[PATH]\twrite logs to specified file.\n");
	printf("--compact-whitespace\tcollapse excess whitespace and blank lines.\n");
	printf("--parse-embedded\tenable parsing embedded files/objects (default).\n");
	printf("--no-parse-embedded\tdisable parsing embedded files/objects.\n");
	printf("--parse-doc-images\tenable dispatching images inside documents to image callback.\n");
	printf("--no-parse-doc-images\tdisable dispatching images inside documents (default).\n");
	printf("--version, -V\tprint version information and exit.\n");
	printf("--help, -h\tdisplay this help message and exit.\n");
}

int main(int argc, char* argv[])
{
	#ifdef DEBUG
		doctotext_init_tracing("doctotext.trace");
	#endif

	if (argc < 2)
	{
		help();
		return EXIT_FAILURE;
	}

	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0)
		{
			version();
			return EXIT_SUCCESS;
		}
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
		{
			help();
			return EXIT_SUCCESS;
		}
	}

	std::string arg;

	bool extract_metadata = false;

	PlainTextExtractor::ParserType parser_type = PlainTextExtractor::PARSER_AUTO;
	XmlParseMode mode = PARSE_XML;

	FormattingStyle options;
	options.table_style = TABLE_STYLE_TABLE_LOOK;
	options.list_style.setPrefix(" * ");
	options.url_style = URL_STYLE_UNDERSCORED;

	bool verbose = false;
	ofstream* log_stream = NULL;

	// The last argument is always the input FILE; everything before it is an option.
	const char* input_file = argv[argc - 1];
	if (strncmp(input_file, "--", 2) == 0)
	{
		cerr << "Error: missing FILE argument (got option \"" << input_file << "\" instead).\n\n";
		help();
		return EXIT_FAILURE;
	}

	for (int i = 1; i + 1 < argc; ++i)
	{
		arg = argv[i];

		if (arg == "--meta")
			extract_metadata = true;
		else if (arg == "--rtf")
			parser_type = PlainTextExtractor::PARSER_RTF;
		else if (arg == "--odfxml")
			parser_type = PlainTextExtractor::PARSER_ODFXML;
		else if (arg == "--odf" || arg == "--ooxml")
			parser_type = PlainTextExtractor::PARSER_ODF_OOXML;
		else if (arg == "--xls")
			parser_type = PlainTextExtractor::PARSER_XLS;
		else if (arg == "--xlsb")
			parser_type = PlainTextExtractor::PARSER_XLSB;
		else if (arg == "--iwork")
			parser_type = PlainTextExtractor::PARSER_IWORK;
		else if (arg == "--ppt")
			parser_type = PlainTextExtractor::PARSER_PPT;
		else if (arg == "--doc")
			parser_type = PlainTextExtractor::PARSER_DOC;
		else if (arg == "--html")
			parser_type = PlainTextExtractor::PARSER_HTML;
		else if (arg == "--pdf")
			parser_type = PlainTextExtractor::PARSER_PDF;
		else if (arg == "--eml")
			parser_type = PlainTextExtractor::PARSER_EML;
		else if (arg == "--txt")
			parser_type = PlainTextExtractor::PARSER_TXT;
		else if (arg == "--image")
			parser_type = PlainTextExtractor::PARSER_IMAGE;
		else if (arg.compare(0, 14, "--table-style=") == 0)
		{
			const std::string value = arg.substr(14);
			if (value == "one-row")
				options.table_style = TABLE_STYLE_ONE_ROW;
			else if (value == "one-col")
				options.table_style = TABLE_STYLE_ONE_COL;
			else if (value == "table-look")
				options.table_style = TABLE_STYLE_TABLE_LOOK;
			else
				cerr << "Warning: unknown table-style \"" << value << "\" ignored.\n";
		}
		else if (arg.compare(0, 12, "--url-style=") == 0)
		{
			const std::string value = arg.substr(12);
			if (value == "text-only")
				options.url_style = URL_STYLE_TEXT_ONLY;
			else if (value == "extended")
				options.url_style = URL_STYLE_EXTENDED;
			else if (value == "underscored")
				options.url_style = URL_STYLE_UNDERSCORED;
			else
				cerr << "Warning: unknown url-style \"" << value << "\" ignored.\n";
		}
		else if (arg.compare(0, 20, "--list-style-prefix=") == 0)
			options.list_style.setPrefix(arg.substr(20));
		else if (arg == "--fix-xml")
			mode = FIX_XML;
		else if (arg == "--strip-xml")
			mode = STRIP_XML;
		else if (arg.compare(0, 12, "--unzip-cmd=") == 0)
			DocToTextUnzip::setUnzipCommand(arg.substr(12));
		else if (arg == "--verbose")
			verbose = true;
		else if (arg == "--compact-whitespace")
			options.compact_whitespace = true;
		else if (arg == "--parse-embedded")
			options.parse_embedded_files = true;
		else if (arg == "--no-parse-embedded")
			options.parse_embedded_files = false;
		else if (arg == "--parse-doc-images")
			options.parse_document_images = true;
		else if (arg == "--no-parse-doc-images")
			options.parse_document_images = false;
		else if (arg.compare(0, 11, "--log-file=") == 0)
		{
			delete log_stream;
			log_stream = new ofstream(arg.substr(11).c_str());
		}
		else
			cerr << "Warning: unknown option \"" << arg << "\" ignored.\n";
	}


	PlainTextExtractor extractor(parser_type);
	if (verbose)
		extractor.setVerboseLogging(true);
	if (log_stream != NULL)
		extractor.setLogStream(*log_stream);
	if (mode != PARSE_XML)
		extractor.setXmlParseMode(mode);
	extractor.setFormattingStyle(options);
	if (extract_metadata)
	{
		Metadata meta;
		if (!extractor.extractMetadata(argv[argc - 1], meta))
		{
			(log_stream != NULL ? *log_stream : cerr) << "Error processing file " << argv[argc - 1] << ".\n";
			delete log_stream;
			return EXIT_FAILURE;
		}
		cout << "Author: " << meta.author() << (meta.authorType() == Metadata::ESTIMATED ? " (estimated)" : "")
			<< "\nCreation time: " << date_to_string(meta.creationDate()) << (meta.creationDateType() == Metadata::ESTIMATED ? " (estimated)" : "")
			<< "\nLast modified by: " << meta.lastModifiedBy() << (meta.lastModifiedByType() == Metadata::ESTIMATED ? " (estimated)" : "")
			<< "\nLast modification time: " << date_to_string(meta.lastModificationDate()) << (meta.lastModificationDateType() == Metadata::ESTIMATED ? " (estimated)" : "")
			<< "\nPage count: " << meta.pageCount() << (meta.pageCountType() == Metadata::ESTIMATED ? " (estimated)" : "")
			<< "\nWord count: " << meta.wordCount() << (meta.wordCountType() == Metadata::ESTIMATED ? " (estimated)" : "")
			<< "\n";
	}
	else
	{
		string text;
		if (!extractor.processFile(argv[argc - 1], text))
		{
			(log_stream != NULL ? *log_stream : cerr) << "Error processing file " << argv[argc - 1] << ".\n";
			delete log_stream;
			return EXIT_FAILURE;
		}
		printf("%s\n", text.c_str());
		std::vector<Link> links;
		extractor.getParsedLinks(links);
		if (links.size() > 0)
		{
			printf("parsed links:\n");
			for (size_t i = 0; i < links.size(); ++i)
			{
				printf("%s @ index = %d length = %d\n", links[i].getLinkUrl(), links[i].getLinkTextPosition(), strlen(links[i].getLinkText()));
			}
		}
		std::vector<Attachment> attachments;
		extractor.getAttachments(attachments);
		if (attachments.size() > 0)
		{
			printf("parsed attachments:\n");
			for (size_t i = 0; i < attachments.size(); ++i)
			{
				printf("\nname: %s\n", attachments[i].filename());
				std::map<std::string, Variant> variables = attachments[i].getFields();
				for (std::map<std::string, Variant>::iterator it = variables.begin(); it != variables.end(); ++it)
				{
					#warning TODO: If Content-ID is not present in the file, mimetic generates it... and test/Makefile always goes wrong.\
					Maybe we should skip this field?
					if (it->first != "Content-ID")
						printf("field: %s, value: %s\n", it->first.c_str(), it->second.getString());
				}
			}
		}
	}
	if (log_stream != NULL)
		delete log_stream;
	return EXIT_SUCCESS;
}
