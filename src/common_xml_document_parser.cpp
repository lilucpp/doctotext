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

#include "common_xml_document_parser.h"

#include <cstring>
#include "doctotext_unzip.h"
#include <iostream>
#include <libxml/xmlreader.h>
#include "metadata.h"
#include "misc.h"
#include "xml_stream.h"
#include "xml_fixer.h"

struct CommonXMLDocumentParser::Implementation
{
	bool m_manage_xml_parser;
	bool m_verbose_logging;
	bool m_error;
	std::ostream* m_log_stream;
	size_t m_list_depth;
	std::map<std::string, ListStyleVector> m_list_styles;
	std::vector<Link> m_links;
	std::map<int, Comment> m_comments;
	std::vector<SharedString> m_shared_strings;
	std::map<std::string, CommandHandler> m_command_handlers;
	bool m_disabled_text;
	int m_xml_options;
	CommonXMLDocumentParser* m_parser;
	doctotext::ImageParserCallback m_image_callback;
	void* m_image_user_data;
	std::map<std::string, std::string> m_image_rels;

	void executeCommand(const std::string& command, XmlStream& xml_stream, XmlParseMode mode,
						const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
						bool& children_processed, std::string& level_suffix, bool first_on_level,
						std::vector<Link>& links)
	{
		children_processed = false;
		std::map<std::string, CommonXMLDocumentParser::CommandHandler>::iterator it = m_command_handlers.find(command);
		if (it != m_command_handlers.end())
			(*it->second)(*m_parser, xml_stream, mode, options, zipfile, text, children_processed, level_suffix, first_on_level, links);
		else
			m_parser->onUnregisteredCommand(xml_stream, mode, options, zipfile, text, children_processed, level_suffix, first_on_level, links);
	}
};

class CommonXMLDocumentParser::CommandHandlersSet
{
	public:
		static void onODFOOXMLText(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
								   const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
								   bool& children_processed, std::string& level_suffix, bool first_on_level,
								   std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "ODFOOXML_TEXT command.\n";
			char* content = xml_stream.content();
			if (content != NULL)
				text += content;
		}

		static void onODFOOXMLPara(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
								   const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
								   bool& children_processed, std::string& level_suffix, bool first_on_level,
								   std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "ODFOOXML_PARA command.\n";
			xml_stream.levelDown();
			text += parser.parseXmlData(xml_stream, mode, options, zipfile, links) + '\n';
			xml_stream.levelUp();
			children_processed = true;
		}

		static void onODFOOXMLTab(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
								  const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
								  bool& children_processed, std::string& level_suffix, bool first_on_level,
								  std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "ODFOOXML_TAB command.\n";
			text += "\t";
		}

		static void onODFOOXMLUrl(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
								  const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
								  bool& children_processed, std::string& level_suffix, bool first_on_level,
								  std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "ODFOOXML_URL command.\n";
			std::string mlink = xml_stream.attribute("href");
			xml_stream.levelDown();
			std::string text_link = parser.parseXmlData(xml_stream, mode, options, zipfile, links);
			text_link = formatUrl(mlink, text_link, options);
			xml_stream.levelUp();
			Link link(mlink, text_link, 0);
			insertSpecialLinkBlockIntoText(text, link);
			links.push_back(link);
			children_processed = true;
		}

		static void onODFOOXMLListStyle(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
										const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
										bool& children_processed, std::string& level_suffix, bool first_on_level,
										std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "ODFOOXML_LIST_STYLE command.\n";
			std::string style_code = xml_stream.attribute("name");
			if (!style_code.empty())
			{
				CommonXMLDocumentParser::ListStyleVector list_style(10, CommonXMLDocumentParser::bullet);
				xml_stream.levelDown();
				while (xml_stream)
				{
					std::string list_style_name = xml_stream.name();
					unsigned int level = strtol(xml_stream.attribute("level").c_str(), NULL, 10);
					if (level <= 10 && level > 0)
					{
						if (list_style_name == "list-level-style-number")
							list_style[level - 1] = CommonXMLDocumentParser::number;
						else
							list_style[level - 1] = CommonXMLDocumentParser::bullet;
					}
					xml_stream.next();
				}
				xml_stream.levelUp();
				children_processed = true;
				parser.getListStyles()[style_code] = list_style;
			}
		}

		static void onODFOOXMLList(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
								   const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
								   bool& children_processed, std::string& level_suffix, bool first_on_level,
								   std::vector<Link>& links)
		{
			std::vector<std::string> list_vector;
			if (parser.verbose())
				parser.getLogStream() << "ODFOOXML_LIST command.\n";
			++parser.getListDepth();
			std::string header;
			CommonXMLDocumentParser::ODFOOXMLListStyle list_style = CommonXMLDocumentParser::bullet;
			std::string style_name = xml_stream.attribute("style-name");
			if (parser.getListDepth() <= 10 && !style_name.empty() && parser.getListStyles().find(style_name) != parser.getListStyles().end())
				list_style = parser.getListStyles()[style_name].at(parser.getListDepth() - 1);
			xml_stream.levelDown();
			while (xml_stream)
			{
				std::string list_element_type = xml_stream.name();
				xml_stream.levelDown();
				if (list_element_type == "list-header" && xml_stream)
				{
					header = parser.parseXmlData(xml_stream, mode, options, zipfile, links);
				}
				else if (xml_stream)
				{
					list_vector.push_back(parser.parseXmlData(xml_stream, mode, options, zipfile, links));
				}
				xml_stream.levelUp();
				xml_stream.next();
			}
			xml_stream.levelUp();

			if (header.length() > 0)
			{
				text += header;
				if (list_vector.size() > 0)
					text += "\n";
			}
			--parser.getListDepth();
			if (list_style == CommonXMLDocumentParser::number)
				text += formatNumberedList(list_vector);
			else
				text += formatList(list_vector, options);
			children_processed = true;
		}

		static void onODFOOXMLTable(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
									const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
									bool& children_processed, std::string& level_suffix, bool first_on_level,
									std::vector<Link>& links)
		{
			svector cell_vector;
			std::vector<svector> row_vector;
			if (parser.verbose())
				parser.getLogStream() << "ODFOOXML_TABLE command.\n";
			xml_stream.levelDown();
			while (xml_stream)
			{
				if (xml_stream.name() == "table-row")
				{
					xml_stream.levelDown();
					cell_vector.clear();

					while (xml_stream)
					{
						if (xml_stream.name() == "table-cell")
						{
							xml_stream.levelDown();
							cell_vector.push_back(parser.parseXmlData(xml_stream, mode, options, zipfile, links));
							xml_stream.levelUp();
						}
						xml_stream.next();
					}
					row_vector.push_back(cell_vector);
					xml_stream.levelUp();
				}
				xml_stream.next();
			}
			xml_stream.levelUp();

			text += formatTable(row_vector, options);
			children_processed = true;
		}

		static void onODFOOXMLTableRow(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
									   const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
									   bool& children_processed, std::string& level_suffix, bool first_on_level,
									   std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "ODFOOXML_ROW command.\n";
		}

		static void onODFTableCell(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
								   const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
								   bool& children_processed, std::string& level_suffix, bool first_on_level,
								   std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "ODF_CELL command.\n";
		}

		static void onODFAnnotation(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
									const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
									bool& children_processed, std::string& level_suffix, bool first_on_level,
									std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "ODF_ANNOTATION command.\n";
			std::string creator;
			std::string date;
			std::string content;
			xml_stream.levelDown();
			while (xml_stream)
			{
				if (xml_stream.name() == "creator")
					creator = xml_stream.stringValue();
				if (xml_stream.name() == "date")
					date = xml_stream.stringValue();
				if (xml_stream.name() == "p")
				{
					if (!content.empty())
						content += "\n";
					xml_stream.levelDown();
					content += parser.parseXmlData(xml_stream, mode, options, zipfile, links);
					xml_stream.levelUp();
				}
				xml_stream.next();
			}
			xml_stream.levelUp();
			text += parser.formatComment(creator, date, content);
			children_processed = true;
		}

		static void onODFLineBreak(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
								   const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
								   bool& children_processed, std::string& level_suffix, bool first_on_level,
								   std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "ODF_LINE_BREAK command.\n";
			text += "\n";
		}

		static void onODFHeading(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
								 const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
								 bool& children_processed, std::string& level_suffix, bool first_on_level,
								 std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "ODF_HEADING command.\n";
			xml_stream.levelDown();
			text += parser.parseXmlData(xml_stream, mode, options, zipfile, links) + '\n';
			xml_stream.levelUp();
			children_processed = true;
		}

		static void onODFObject(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
								const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
								bool& children_processed, std::string& level_suffix, bool first_on_level,
								std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "ODF_OBJECT command.\n";
			std::string href = xml_stream.attribute("href");
			std::string content_fn = (href.substr(0, 2) == "./" ? href.substr(2) : href) + "/content.xml";
			std::string content;
			if (!zipfile->read(content_fn, &content))
			{
				parser.getLogStream() << "Error reading " << content_fn << std::endl;
				return;
			}
			std::string object_text;
			if (!parser.extractText(content, mode, options, zipfile, object_text, links))
			{
				parser.getLogStream()  << "Error parsing " << content_fn << std::endl;
				return;
			}
			text += object_text;
		}

		static void onODFOOXMLDrawing(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
									   const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
									   bool& children_processed, std::string& level_suffix, bool first_on_level,
									   std::vector<Link>& links)
		{
			if (parser.getImageParserCallback() == NULL)
			{
				children_processed = false;
				return;
			}

			if (xml_stream.fullName() == "office:drawing")
			{
				// In ODF, office:drawing is the body container; do not intercept it.
				children_processed = false;
				return;
			}

			if (parser.verbose())
				parser.getLogStream() << "ODFOOXML_DRAWING command.\n";

			xmlNodePtr root_node = xml_stream.expand();
			if (root_node != NULL)
			{
				parser.extractContentFromDrawingNode(root_node, zipfile, text);
			}
			children_processed = true;
		}
};

static std::string normalizeZipPath(const std::string& path)
{
	std::vector<std::string> parts;
	std::string cur;
	for (size_t i = 0; i <= path.size(); ++i)
	{
		if (i == path.size() || path[i] == '/')
		{
			if (cur == "." || cur.empty())
			{
				// skip
			}
			else if (cur == "..")
			{
				if (!parts.empty())
					parts.pop_back();
			}
			else
			{
				parts.push_back(cur);
			}
			cur.clear();
		}
		else
		{
			cur += path[i];
		}
	}
	std::string res;
	for (size_t i = 0; i < parts.size(); ++i)
	{
		if (i > 0)
			res += "/";
		res += parts[i];
	}
	return res;
}

void CommonXMLDocumentParser::extractContentFromDrawingNode(xmlNodePtr node, const DocToTextUnzip* zipfile, std::string& out_text)
{
	if (!node)
		return;

	for (xmlNodePtr cur = node; cur; cur = cur->next)
	{
		if (cur->type == XML_ELEMENT_NODE)
		{
			const char* tag_name = (const char*)cur->name;
			if (tag_name == NULL)
				continue;

			if (strcmp(tag_name, "blip") == 0)
			{
				xmlChar* embed = xmlGetProp(cur, (xmlChar*)"embed");
				if (!embed)
					embed = xmlGetProp(cur, (xmlChar*)"id");
				if (embed)
				{
					std::string rId = (char*)embed;
					xmlFree(embed);
					const std::map<std::string, std::string>& rels = getImageRelationships();
					std::map<std::string, std::string>::const_iterator it = rels.find(rId);
					if (it != rels.end() && zipfile != NULL && impl->m_image_callback != NULL)
					{
						std::string img_data;
						if (zipfile->read(it->second, &img_data))
						{
							std::string parsed = impl->m_image_callback(it->second, img_data.data(), img_data.size(), impl->m_image_user_data);
							if (!parsed.empty())
							{
								if (!out_text.empty() && out_text[out_text.size() - 1] != '\n')
									out_text += '\n';
								out_text += parsed;
								if (out_text[out_text.size() - 1] != '\n')
									out_text += '\n';
							}
						}
					}
				}
			}
			else if (strcmp(tag_name, "imagedata") == 0)
			{
				xmlChar* id = xmlGetProp(cur, (xmlChar*)"id");
				if (!id)
					id = xmlGetProp(cur, (xmlChar*)"embed");
				if (id)
				{
					std::string rId = (char*)id;
					xmlFree(id);
					const std::map<std::string, std::string>& rels = getImageRelationships();
					std::map<std::string, std::string>::const_iterator it = rels.find(rId);
					if (it != rels.end() && zipfile != NULL && impl->m_image_callback != NULL)
					{
						std::string img_data;
						if (zipfile->read(it->second, &img_data))
						{
							std::string parsed = impl->m_image_callback(it->second, img_data.data(), img_data.size(), impl->m_image_user_data);
							if (!parsed.empty())
							{
								if (!out_text.empty() && out_text[out_text.size() - 1] != '\n')
									out_text += '\n';
								out_text += parsed;
								if (out_text[out_text.size() - 1] != '\n')
									out_text += '\n';
							}
						}
					}
				}
			}
			else if (strcmp(tag_name, "image") == 0)
			{
				// ODF <draw:image xlink:href="Pictures/..." />
				xmlChar* href = xmlGetProp(cur, (xmlChar*)"href");
				if (href)
				{
					std::string path = (char*)href;
					xmlFree(href);
					if (path.length() >= 2 && path[0] == '.' && path[1] == '/')
						path = path.substr(2);
					if (zipfile != NULL && impl->m_image_callback != NULL)
					{
						std::string img_data;
						if (zipfile->read(path, &img_data))
						{
							std::string parsed = impl->m_image_callback(path, img_data.data(), img_data.size(), impl->m_image_user_data);
							if (!parsed.empty())
							{
								if (!out_text.empty() && out_text[out_text.size() - 1] != '\n')
									out_text += '\n';
								out_text += parsed;
								if (out_text[out_text.size() - 1] != '\n')
									out_text += '\n';
							}
						}
					}
				}
			}
			else if (strcmp(tag_name, "t") == 0)
			{
				xmlChar* val = xmlNodeListGetString(cur->doc, cur->children, 1);
				if (val != NULL)
				{
					out_text += (char*)val;
					xmlFree(val);
				}
			}
			else if (strcmp(tag_name, "p") == 0)
			{
				if (!out_text.empty() && out_text[out_text.size() - 1] != '\n')
					out_text += '\n';
			}
			else if (strcmp(tag_name, "br") == 0)
			{
				out_text += '\n';
			}
			else if (strcmp(tag_name, "tab") == 0)
			{
				out_text += '\t';
			}

			// Traverse children
			extractContentFromDrawingNode(cur->children, zipfile, out_text);
		}
	}
}

void CommonXMLDocumentParser::loadRelationshipsFromXml(const std::string& rels_xml, const std::string& base_dir)
{
	XmlStream xml_stream(rels_xml, manageXmlParser(), getXmlOptions());
	if (!xml_stream)
		return;
	xml_stream.levelDown();
	while (xml_stream)
	{
		if (xml_stream.name() == "Relationship")
		{
			std::string id = xml_stream.attribute("Id");
			std::string target = xml_stream.attribute("Target");
			if (!id.empty() && !target.empty())
			{
				std::string full_path;
				if (target[0] == '/')
					full_path = target.substr(1);
				else
					full_path = base_dir + target;
				addImageRelationship(id, normalizeZipPath(full_path));
			}
		}
		xml_stream.next();
	}
}

void CommonXMLDocumentParser::registerODFOOXMLCommandHandler(const std::string& xml_tag, CommandHandler handler)
{
	impl->m_command_handlers[xml_tag] = handler;
}

void CommonXMLDocumentParser::onUnregisteredCommand(XmlStream& xml_stream, XmlParseMode mode,
													const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
													bool& children_processed, std::string& level_suffix, bool first_on_level,
													std::vector<Link>& links)
{
	if (impl->m_disabled_text == false && xml_stream.name() == "#text")
	{
		char* content = xml_stream.content();
		if (content != NULL)
			text += content;
		children_processed = true;
	}
}

std::string CommonXMLDocumentParser::parseXmlData(XmlStream& xml_stream, XmlParseMode mode, const FormattingStyle& options, const DocToTextUnzip* zipfile, std::vector<Link>& links)
{
	std::string text;
	std::string level_suffix;
	bool first_on_level = true;

	while (xml_stream)
	{
		bool children_processed;
		impl->executeCommand(xml_stream.name(), xml_stream, mode, options, zipfile, text, children_processed, level_suffix, first_on_level, links);
		if (xml_stream && (!children_processed))
		{
			xml_stream.levelDown();
			if (xml_stream)
				text += parseXmlData(xml_stream, mode, options, zipfile, links);
			xml_stream.levelUp();
		}
		xml_stream.next();
		first_on_level = false;
	}
	if (!level_suffix.empty())
		text += level_suffix;
	return text;
}

bool CommonXMLDocumentParser::extractText(const std::string& xml_contents, XmlParseMode mode, const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text, std::vector<Link>& links)
{
	if (mode == STRIP_XML)
	{
		text = "";
		bool in_tag = false;
		for (int i = 0; i < xml_contents.length(); i++)
		{
			char ch = xml_contents[i];
			if (ch == '<')
				in_tag = true;
			else if (ch == '>')
				in_tag = false;
			else if (!in_tag)
				text += ch;
		}
		return true;
	}
	std::string xml;
	if (mode == FIX_XML)
	{
		DocToTextXmlFixer xml_fixer;
		xml = xml_fixer.fix(xml_contents);
	}
	else
		 xml = xml_contents;
	XmlStream xml_stream(xml, impl->m_manage_xml_parser, getXmlOptions());
	if (!xml_stream)
	{
		*impl->m_log_stream << "Error parsing XML contents.\n";
		return false;
	}
	text = parseXmlData(xml_stream, mode, options, zipfile, links);
	return true;
}

bool CommonXMLDocumentParser::parseODFMetadata(const std::string &xml_content, Metadata &metadata)
{
	XmlStream xml_stream(xml_content, impl->m_manage_xml_parser, XML_PARSE_NOBLANKS);
	if (!xml_stream)
	{
		impl->m_error = true;
		return false;
	}
	xml_stream.levelDown();
	while (xml_stream)
	{
		if (xml_stream.name() == "meta")
		{
			xml_stream.levelDown();
			while (xml_stream)
			{
				if (xml_stream.name() == "initial-creator")
					metadata.setAuthor(xml_stream.stringValue());
				if (xml_stream.name() == "creation-date")
				{
					tm creation_date;
					string_to_date(xml_stream.stringValue(), creation_date);
					metadata.setCreationDate(creation_date);
				}
				if (xml_stream.name() == "creator")
					metadata.setLastModifiedBy(xml_stream.stringValue());
				if (xml_stream.name() == "date")
				{
					tm last_modification_date;
					string_to_date(xml_stream.stringValue(), last_modification_date);
					metadata.setLastModificationDate(last_modification_date);
				}
				if (xml_stream.name() == "document-statistic")
				{
					std::string attr = xml_stream.attribute("meta::page-count"); // LibreOffice 3.5
					if (attr.empty())
						attr = xml_stream.attribute("page-count"); // older OpenOffice.org
					if (!attr.empty())
						metadata.setPageCount(str_to_int(attr));
					attr = xml_stream.attribute("meta:word-count"); // LibreOffice 3.5
					if (attr.empty())
						attr = xml_stream.attribute("word-count"); // older OpenOffice.org
					if (!attr.empty())
						metadata.setWordCount(str_to_int(attr));
				}
				xml_stream.next();
			}
			xml_stream.levelUp();
		}
		xml_stream.next();
	}
	return true;
}

const std::string CommonXMLDocumentParser::formatComment(const std::string& author, const std::string& time, const std::string& text)
{
	std::string comment = "\n[[[COMMENT BY " + author + " (" + time + ")]]]\n" + text;
	if (text.empty() || *text.rbegin() != '\n')
		comment += "\n";
	comment += "[[[---]]]\n";
	return comment;
}

size_t& CommonXMLDocumentParser::getListDepth() const
{
	return impl->m_list_depth;
}

std::vector<Link>& CommonXMLDocumentParser::getInnerLinks() const
{
	return impl->m_links;
}

std::map<std::string, CommonXMLDocumentParser::ListStyleVector>& CommonXMLDocumentParser::getListStyles() const
{
	return impl->m_list_styles;
}

std::map<int, CommonXMLDocumentParser::Comment>& CommonXMLDocumentParser::getComments() const
{
	return impl->m_comments;
}

std::vector<CommonXMLDocumentParser::SharedString>& CommonXMLDocumentParser::getSharedStrings() const
{
	return impl->m_shared_strings;
}

bool CommonXMLDocumentParser::disabledText() const
{
	return impl->m_disabled_text;
}

bool CommonXMLDocumentParser::verbose() const
{
	return impl->m_verbose_logging;
}

std::ostream& CommonXMLDocumentParser::getLogStream() const
{
	return *impl->m_log_stream;
}

bool CommonXMLDocumentParser::manageXmlParser() const
{
	return impl->m_manage_xml_parser;
}

void CommonXMLDocumentParser::setError(bool error)
{
	impl->m_error = error;
}

void CommonXMLDocumentParser::disableText(bool disable)
{
	impl->m_disabled_text = disable;
}

void CommonXMLDocumentParser::setXmlOptions(int options)
{
	impl->m_xml_options = options;
}

void CommonXMLDocumentParser::cleanUp()
{
	if (impl)
		delete impl;
}

CommonXMLDocumentParser::CommonXMLDocumentParser()
{
	impl = NULL;
	try
	{
		impl = new Implementation;
		impl->m_error = false;
		impl->m_list_depth = 0;
		impl->m_log_stream = &std::cerr;
		impl->m_manage_xml_parser = true;
		impl->m_verbose_logging = false;
		impl->m_disabled_text = false;
		impl->m_xml_options = 0;
		impl->m_image_callback = NULL;
		impl->m_image_user_data = NULL;
		impl->m_parser = this;

		registerODFOOXMLCommandHandler("text", &CommandHandlersSet::onODFOOXMLText);
		registerODFOOXMLCommandHandler("p", &CommandHandlersSet::onODFOOXMLPara);
		registerODFOOXMLCommandHandler("tab", &CommandHandlersSet::onODFOOXMLTab);
		registerODFOOXMLCommandHandler("a", &CommandHandlersSet::onODFOOXMLUrl);
		registerODFOOXMLCommandHandler("list-style", &CommandHandlersSet::onODFOOXMLListStyle);
		registerODFOOXMLCommandHandler("list", &CommandHandlersSet::onODFOOXMLList);
		registerODFOOXMLCommandHandler("table", &CommandHandlersSet::onODFOOXMLTable);
		registerODFOOXMLCommandHandler("table-row", &CommandHandlersSet::onODFOOXMLTableRow);
		registerODFOOXMLCommandHandler("table-cell", &CommandHandlersSet::onODFTableCell);
		registerODFOOXMLCommandHandler("annotation", &CommandHandlersSet::onODFAnnotation);
		registerODFOOXMLCommandHandler("line-break", &CommandHandlersSet::onODFLineBreak);
		registerODFOOXMLCommandHandler("h", &CommandHandlersSet::onODFHeading);
		registerODFOOXMLCommandHandler("object", &CommandHandlersSet::onODFObject);
		registerODFOOXMLCommandHandler("drawing", &CommandHandlersSet::onODFOOXMLDrawing);
		registerODFOOXMLCommandHandler("pict", &CommandHandlersSet::onODFOOXMLDrawing);
		registerODFOOXMLCommandHandler("pic", &CommandHandlersSet::onODFOOXMLDrawing);
		registerODFOOXMLCommandHandler("image", &CommandHandlersSet::onODFOOXMLDrawing);
		registerODFOOXMLCommandHandler("blip", &CommandHandlersSet::onODFOOXMLDrawing);
		registerODFOOXMLCommandHandler("imagedata", &CommandHandlersSet::onODFOOXMLDrawing);
	}
	catch (std::bad_alloc& ba)
	{
		if (impl)
			delete impl;
		impl = NULL;
		throw;
	}
}

CommonXMLDocumentParser::~CommonXMLDocumentParser()
{
	cleanUp();
}

void CommonXMLDocumentParser::setLogStream(std::ostream &log_stream)
{
	impl->m_log_stream = &log_stream;
}

void CommonXMLDocumentParser::setManageXmlParser(bool manage)
{
	impl->m_manage_xml_parser = manage;
}

void CommonXMLDocumentParser::setVerboseLogging(bool verbose)
{
	impl->m_verbose_logging = verbose;
}

int CommonXMLDocumentParser::getXmlOptions() const
{
	return impl->m_xml_options;
}

void CommonXMLDocumentParser::getLinks(std::vector<Link>& links)
{
	links = impl->m_links;
}

bool CommonXMLDocumentParser::error()
{
	return impl->m_error;
}

void CommonXMLDocumentParser::setImageParserCallback(doctotext::ImageParserCallback callback, void* user_data)
{
	impl->m_image_callback = callback;
	impl->m_image_user_data = user_data;
}

doctotext::ImageParserCallback CommonXMLDocumentParser::getImageParserCallback() const
{
	return impl->m_image_callback;
}

void* CommonXMLDocumentParser::getImageParserUserData() const
{
	return impl->m_image_user_data;
}

void CommonXMLDocumentParser::clearImageRelationships()
{
	impl->m_image_rels.clear();
}

void CommonXMLDocumentParser::addImageRelationship(const std::string& id, const std::string& target)
{
	impl->m_image_rels[id] = target;
}

const std::map<std::string, std::string>& CommonXMLDocumentParser::getImageRelationships() const
{
	return impl->m_image_rels;
}
