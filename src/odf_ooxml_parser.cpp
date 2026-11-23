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

#include "odf_ooxml_parser.h"

#include "xml_fixer.h"
#include "doctotext_unzip.h"
#include "embedded_object_extractor.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <fstream>
#include <iostream>
#include <libxml/xmlreader.h>
#include <map>
#include "metadata.h"
#include "misc.h"
#include "oshared.h"
#include <set>
#include <stdlib.h>
#include <string.h>
#include <sstream>
#include "xml_stream.h"

using namespace std;

const int CASESENSITIVITY = 1;

static bool has_odf_encryption_manifest(const DocToTextUnzip& zipfile)
{
	if (!zipfile.exists("META-INF/manifest.xml"))
		return false;
	std::string manifest;
	if (!zipfile.read("META-INF/manifest.xml", &manifest))
		return false;
	return manifest.find("encryption-data") != std::string::npos;
}

static string locate_main_file(const DocToTextUnzip& zipfile, std::ostream& log_stream)
{
	if (zipfile.exists("content.xml"))
		return "content.xml";
	if (zipfile.exists("word/document.xml"))
		return "word/document.xml";
	if (zipfile.exists("xl/workbook.xml"))
		return "xl/workbook.xml";
	if (zipfile.exists("ppt/presentation.xml"))
		return "ppt/presentation.xml";
	log_stream << "Error - no content.xml, no word/document.xml and no ppt/presentation.xml" << endl;
	return "";
}

namespace
{
	xmlNodePtr findChildElementByLocalName(xmlNodePtr parent, const char* name)
	{
		for (xmlNodePtr cur = parent ? parent->children : NULL; cur != NULL; cur = cur->next)
		{
			if (cur->type == XML_ELEMENT_NODE && cur->name && xmlStrcmp(cur->name, BAD_CAST name) == 0)
				return cur;
		}
		return NULL;
	}

	bool isPptxPlaceholderShape(xmlNodePtr node)
	{
		if (!node || node->type != XML_ELEMENT_NODE || !node->name)
			return false;
		const char* nv_name = NULL;
		if (xmlStrcmp(node->name, BAD_CAST "sp") == 0)
			nv_name = "nvSpPr";
		else if (xmlStrcmp(node->name, BAD_CAST "pic") == 0)
			nv_name = "nvPicPr";
		else if (xmlStrcmp(node->name, BAD_CAST "graphicFrame") == 0)
			nv_name = "nvGraphicFramePr";
		else if (xmlStrcmp(node->name, BAD_CAST "grpSp") == 0)
			nv_name = "nvGrpSpPr";
		else
			return false;

		xmlNodePtr nv_sp_pr = findChildElementByLocalName(node, nv_name);
		xmlNodePtr nv_pr = findChildElementByLocalName(nv_sp_pr, "nvPr");
		return findChildElementByLocalName(nv_pr, "ph") != NULL;
	}

	void unlinkPptxPlaceholderShapes(xmlNodePtr parent)
	{
		xmlNodePtr cur = parent ? parent->children : NULL;
		while (cur != NULL)
		{
			xmlNodePtr next = cur->next;
			if (cur->type == XML_ELEMENT_NODE)
			{
				if (isPptxPlaceholderShape(cur))
				{
					xmlUnlinkNode(cur);
					xmlFreeNode(cur);
				}
				else
				{
					unlinkPptxPlaceholderShapes(cur);
				}
			}
			cur = next;
		}
	}

	bool hasExtractablePptxContent(xmlNodePtr node, bool check_images)
	{
		for (xmlNodePtr cur = node; cur != NULL; cur = cur->next)
		{
			if (cur->type == XML_ELEMENT_NODE && cur->name)
			{
				if (xmlStrcmp(cur->name, BAD_CAST "t") == 0)
				{
					for (xmlNodePtr ch = cur->children; ch != NULL; ch = ch->next)
					{
						if ((ch->type == XML_TEXT_NODE || ch->type == XML_CDATA_SECTION_NODE) && ch->content)
						{
							for (const unsigned char* p = ch->content; *p; ++p)
							{
								if (!std::isspace(*p))
									return true;
							}
						}
					}
				}
				else if (check_images && xmlStrcmp(cur->name, BAD_CAST "blip") == 0)
				{
					return true;
				}
			}
			if (cur->children && hasExtractablePptxContent(cur->children, check_images))
				return true;
		}
		return false;
	}

	bool filterPptxPlaceholderShapes(const std::string& xml_in,
	                                 XmlParseMode mode,
	                                 bool manage_xml_parser,
	                                 int xml_options,
	                                 bool check_images,
	                                 std::string& xml_out)
	{
		std::string fixed;
		const std::string* src = &xml_in;
		if (mode == FIX_XML)
		{
			DocToTextXmlFixer xml_fixer;
			fixed = xml_fixer.fix(xml_in);
			src = &fixed;
		}
		if (src->size() > static_cast<size_t>(INT_MAX))
			return false;
		XmlStream::ensureXmlParserInitialized(manage_xml_parser);
		struct XmlDocGuard
		{
			xmlDocPtr doc;
			explicit XmlDocGuard(xmlDocPtr d) : doc(d) {}
			~XmlDocGuard() { if (doc) xmlFreeDoc(doc); }
		} doc_guard(xmlReadMemory(src->data(), static_cast<int>(src->size()), "slidePart.xml", NULL,
		                           xml_options | XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING));
		if (!doc_guard.doc)
			return false;
		xmlNodePtr root = xmlDocGetRootElement(doc_guard.doc);
		if (!root)
			return false;
		unlinkPptxPlaceholderShapes(root);
		if (!hasExtractablePptxContent(root, check_images))
			return false;
		xmlChar* mem = NULL;
		int len = 0;
		xmlDocDumpMemoryEnc(doc_guard.doc, &mem, &len, "UTF-8");
		if (!mem || len <= 0)
		{
			if (mem)
				xmlFree(mem);
			return false;
		}
		xml_out.assign(reinterpret_cast<const char*>(mem), static_cast<size_t>(len));
		xmlFree(mem);
		return true;
	}

	void collectRelTargetsBySuffix(const std::string& rels_xml,
	                               const std::string& base_dir,
	                               const std::string& type_suffix,
	                               bool manage_xml_parser,
	                               int xml_options,
	                               std::vector<std::string>& out_targets)
	{
		if (rels_xml.find(type_suffix) == std::string::npos)
			return;
		XmlStream rel_stream(rels_xml, manage_xml_parser, xml_options);
		if (!rel_stream)
			return;
		rel_stream.levelDown();
		while (rel_stream)
		{
			if (rel_stream.name() == "Relationship")
			{
				std::string rtype = rel_stream.attribute("Type");
				std::string rtarget = rel_stream.attribute("Target");
				if (!rtarget.empty() && rtype.size() >= type_suffix.size() &&
				    rtype.compare(rtype.size() - type_suffix.size(), type_suffix.size(), type_suffix) == 0)
				{
					std::string full_target = (rtarget[0] == '/') ? rtarget.substr(1) : (base_dir + rtarget);
					out_targets.push_back(normalizeZipPath(full_target));
				}
			}
			rel_stream.next();
		}
	}

	std::string makeRelsPathForPart(const std::string& part_path, std::string& out_base_dir)
	{
		size_t last_slash = part_path.find_last_of('/');
		out_base_dir = (last_slash == std::string::npos) ? "" : part_path.substr(0, last_slash + 1);
		std::string name = (last_slash == std::string::npos) ? part_path : part_path.substr(last_slash + 1);
		return out_base_dir + "_rels/" + name + ".rels";
	}

	std::string getXmlPropLocal(xmlNodePtr node, const char* local_name)
	{
		for (xmlAttrPtr attr = node ? node->properties : NULL; attr != NULL; attr = attr->next)
		{
			if (attr->name && xmlStrcmp(attr->name, BAD_CAST local_name) == 0)
			{
				xmlChar* val = xmlNodeListGetString(node->doc, attr->children, 1);
				if (!val)
					return "";
				std::string res(reinterpret_cast<const char*>(val));
				xmlFree(val);
				return res;
			}
		}
		return "";
	}

	bool hasNonSpaceText(const std::string& s)
	{
		for (size_t i = 0; i < s.size(); ++i)
		{
			unsigned char c = static_cast<unsigned char>(s[i]);
			if (c > 0x7F || !std::isspace(c))
				return true;
		}
		return false;
	}

	void collectOdfFrameText(xmlNodePtr node, std::string& cur_line, std::vector<std::string>& out_lines)
	{
		for (xmlNodePtr cur = node; cur != NULL; cur = cur->next)
		{
			if (cur->type == XML_TEXT_NODE || cur->type == XML_CDATA_SECTION_NODE)
			{
				if (cur->content)
					cur_line += reinterpret_cast<const char*>(cur->content);
				continue;
			}
			if (cur->type != XML_ELEMENT_NODE || !cur->name)
				continue;
			if (xmlStrcmp(cur->name, BAD_CAST "page-number") == 0 ||
			    xmlStrcmp(cur->name, BAD_CAST "page-count") == 0 ||
			    xmlStrcmp(cur->name, BAD_CAST "date-time") == 0 ||
			    xmlStrcmp(cur->name, BAD_CAST "header") == 0 ||
			    xmlStrcmp(cur->name, BAD_CAST "footer") == 0)
			{
				continue;
			}
			if (xmlStrcmp(cur->name, BAD_CAST "line-break") == 0)
			{
				if (hasNonSpaceText(cur_line))
					out_lines.push_back(cur_line);
				cur_line.clear();
				continue;
			}
			bool is_para = (xmlStrcmp(cur->name, BAD_CAST "p") == 0 || xmlStrcmp(cur->name, BAD_CAST "h") == 0);
			if (cur->children)
				collectOdfFrameText(cur->children, cur_line, out_lines);
			if (is_para && hasNonSpaceText(cur_line))
			{
				out_lines.push_back(cur_line);
				cur_line.clear();
			}
		}
	}

	void extractReferencedOdfMasterFooters(const std::string& content_xml,
	                                       const std::string& styles_xml,
	                                       XmlParseMode mode,
	                                       bool manage_xml_parser,
	                                       int xml_options,
	                                       std::string& out_footer_text)
	{
		out_footer_text.clear();
		if (styles_xml.find("master-styles") == std::string::npos)
			return;
		XmlStream::ensureXmlParserInitialized(manage_xml_parser);

		struct XmlDocGuard
		{
			xmlDocPtr doc;
			explicit XmlDocGuard(xmlDocPtr d) : doc(d) {}
			~XmlDocGuard() { if (doc) xmlFreeDoc(doc); }
		};

		auto parseXmlString = [&](const std::string& raw, const char* name) -> xmlDocPtr
		{
			std::string fixed;
			const std::string* src = &raw;
			if (mode == FIX_XML)
			{
				DocToTextXmlFixer xml_fixer;
				fixed = xml_fixer.fix(raw);
				src = &fixed;
			}
			if (src->size() > static_cast<size_t>(INT_MAX))
				return NULL;
			return xmlReadMemory(src->data(), static_cast<int>(src->size()), name, NULL,
			                     xml_options | XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
		};

		XmlDocGuard content_doc(parseXmlString(content_xml, "content.xml"));
		XmlDocGuard styles_doc(parseXmlString(styles_xml, "styles.xml"));
		if (!content_doc.doc || !styles_doc.doc)
			return;

		// 1. Gather display-header / display-footer visibility per drawing-page style in content.xml.
		struct PageVis { bool show_header; bool show_footer; };
		std::map<std::string, PageVis> style_vis;
		std::map<std::string, PageVis> active_master_vis;

		auto scanContent = [&](auto& self, xmlNodePtr node) -> void
		{
			for (xmlNodePtr cur = node; cur != NULL; cur = cur->next)
			{
				if (cur->type != XML_ELEMENT_NODE || !cur->name)
					continue;
				if (xmlStrcmp(cur->name, BAD_CAST "style") == 0)
				{
					std::string sname = getXmlPropLocal(cur, "name");
					xmlNodePtr dpp = findChildElementByLocalName(cur, "drawing-page-properties");
					if (!sname.empty() && dpp != NULL)
					{
						PageVis pv;
						pv.show_header = (getXmlPropLocal(dpp, "display-header") == "true");
						pv.show_footer = (getXmlPropLocal(dpp, "display-footer") != "false");
						style_vis[sname] = pv;
					}
				}
				else if (xmlStrcmp(cur->name, BAD_CAST "page") == 0)
				{
					std::string mp = getXmlPropLocal(cur, "master-page-name");
					std::string st = getXmlPropLocal(cur, "style-name");
					if (!mp.empty())
					{
						PageVis pv = {false, true};
						std::map<std::string, PageVis>::const_iterator it = style_vis.find(st);
						if (it != style_vis.end())
							pv = it->second;
						PageVis& dst = active_master_vis[mp];
						dst.show_header = dst.show_header || pv.show_header;
						dst.show_footer = dst.show_footer || pv.show_footer;
					}
				}
				if (cur->children)
					self(self, cur->children);
			}
		};
		scanContent(scanContent, xmlDocGetRootElement(content_doc.doc));
		if (active_master_vis.empty())
			return;

		// 2. Inspect referenced <style:master-page> nodes in styles.xml.
		xmlNodePtr styles_root = xmlDocGetRootElement(styles_doc.doc);
		xmlNodePtr master_styles = findChildElementByLocalName(styles_root, "master-styles");
		if (!master_styles)
			return;

		std::set<std::string> seen_lines;
		for (xmlNodePtr mp = master_styles->children; mp != NULL; mp = mp->next)
		{
			if (mp->type != XML_ELEMENT_NODE || !mp->name || xmlStrcmp(mp->name, BAD_CAST "master-page") != 0)
				continue;
			std::string mp_name = getXmlPropLocal(mp, "name");
			std::map<std::string, PageVis>::const_iterator vit = active_master_vis.find(mp_name);
			if (vit == active_master_vis.end())
				continue;
			for (xmlNodePtr ch = mp->children; ch != NULL; ch = ch->next)
			{
				if (ch->type != XML_ELEMENT_NODE || !ch->name || xmlStrcmp(ch->name, BAD_CAST "frame") != 0)
					continue;
				if (getXmlPropLocal(ch, "placeholder") == "true")
					continue;
				std::string cls = getXmlPropLocal(ch, "class");
				bool allow = (cls == "footer" && vit->second.show_footer) ||
				             (cls == "header" && vit->second.show_header);
				if (!allow)
					continue;
				std::string cur_line;
				std::vector<std::string> lines;
				collectOdfFrameText(ch->children, cur_line, lines);
				if (hasNonSpaceText(cur_line))
					lines.push_back(cur_line);
				for (size_t i = 0; i < lines.size(); ++i)
				{
					if (seen_lines.insert(lines[i]).second)
					{
						if (!out_footer_text.empty() && out_footer_text[out_footer_text.size() - 1] != '\n')
							out_footer_text.push_back('\n');
						out_footer_text += lines[i];
					}
				}
			}
		}
		if (!out_footer_text.empty() && out_footer_text[out_footer_text.size() - 1] != '\n')
			out_footer_text.push_back('\n');
	}
}

class ODFOOXMLParser::CommandHandlersSet
{
	public:
		static void onOOXMLAttribute(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
									 const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
									 bool& children_processed, std::string& level_suffix, bool first_on_level,
									 std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "OOXML_ATTR command.\n";
			children_processed = true;
		}

		static void onOOXMLCell(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
								const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
								bool& children_processed, std::string& level_suffix, bool first_on_level,
								std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "OOXML_CELL command.\n";
			if (!first_on_level)
				text += "\t";
			if (xml_stream.attribute("t") == "s")
			{
				xml_stream.levelDown();
				int shared_string_index = str_to_int(parser.parseXmlData(xml_stream, mode, options, zipfile, links));
				xml_stream.levelUp();
				if (shared_string_index < parser.getSharedStrings().size())
				{
					text += parser.getSharedStrings()[shared_string_index].m_text;
					links.insert(links.begin(), parser.getSharedStrings()[shared_string_index].m_links.begin(), parser.getSharedStrings()[shared_string_index].m_links.end());
				}
			}
			else
			{
				xml_stream.levelDown();
				text += parser.parseXmlData(xml_stream, mode, options, zipfile, links);
				xml_stream.levelUp();
			}
			level_suffix = "\n";
			children_processed = true;
		}

		static void onOOXMLHeaderFooter(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
										const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
										bool& children_processed, std::string& level_suffix, bool first_on_level,
										std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "OOXML_HEADERFOOTER command.\n";
			// Ignore headers and footers. They can contain some commands like font settings that can mess up output.
			#warning TODO: Better headers and footers support
			children_processed = true;
		}

		static void onOOXMLCommentReference(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
											const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
											bool& children_processed, std::string& level_suffix, bool first_on_level,
											std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "OOXML_COMMENTREFERENCE command.\n";
			int comment_id = str_to_int(xml_stream.attribute("id"));
			if (parser.getComments().count(comment_id))
			{
				const Comment& c = parser.getComments()[comment_id];
				text += parser.formatComment(c.m_author, c.m_time, c.m_text);
				links.insert(links.end(), c.m_links.begin(), c.m_links.end());
			}
			else
				parser.getLogStream() << "Comment with id " << comment_id << " not found, skipping.\n";
		}

		static void onOOXMLBreak(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
								 const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
								 bool& children_processed, std::string& level_suffix, bool first_on_level,
								 std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "OOXML_BREAK command.\n";
			text += "\n";
		}

		static void onOOXMLInstrtext(CommonXMLDocumentParser& parser, XmlStream& xml_stream, XmlParseMode mode,
									 const FormattingStyle& options, const DocToTextUnzip* zipfile, std::string& text,
									 bool& children_processed, std::string& level_suffix, bool first_on_level,
									 std::vector<Link>& links)
		{
			if (parser.verbose())
				parser.getLogStream() << "OOXML_INSTRTEXT command.\n";
			children_processed = true;
		}
};

struct ODFOOXMLParser::ExtendedImplementation
{
	const char* m_buffer;
	size_t m_buffer_size;
	std::string m_file_name;
	std::string m_decrypted_zip;
	ODFOOXMLParser* m_interf;
	bool m_needs_password;

	ExtendedImplementation()
		: m_buffer(NULL), m_buffer_size(0), m_interf(NULL), m_needs_password(false)
	{
	}

	void clearDecryptedZip()
	{
		std::string().swap(m_decrypted_zip);
	}

	struct DecryptedZipGuard
	{
		ExtendedImplementation* impl;
		explicit DecryptedZipGuard(ExtendedImplementation* p) : impl(p) {}
		~DecryptedZipGuard()
		{
			if (impl)
				impl->clearDecryptedZip();
		}
		DecryptedZipGuard(const DecryptedZipGuard&) = delete;
		DecryptedZipGuard& operator=(const DecryptedZipGuard&) = delete;
	};

	void bindZipSource(DocToTextUnzip& zipfile) const
	{
		if (!m_decrypted_zip.empty())
			zipfile.setBuffer(m_decrypted_zip.data(), m_decrypted_zip.size());
		else if (m_buffer)
			zipfile.setBuffer(m_buffer, m_buffer_size);
		else
			zipfile.setArchiveFile(m_file_name);
	}

	bool hasZipMagicHeader(bool& out_io_error) const
	{
		out_io_error = false;
		if (!m_buffer)
		{
			FILE* f = fopen(m_file_name.c_str(), "rb");
			if (!f)
			{
				out_io_error = true;
				return false;
			}
			char magic[2];
			bool is_pk = (fread(magic, 1, 2, f) == 2 && magic[0] == 'P' && magic[1] == 'K');
			fclose(f);
			return is_pk;
		}
		if (m_buffer_size == 0)
		{
			out_io_error = true;
			return false;
		}
		return m_buffer_size >= 2 && m_buffer[0] == 'P' && m_buffer[1] == 'K';
	}

	bool openZipOrDecrypt(DocToTextUnzip& zipfile, bool check_header_first, bool& out_encrypted)
	{
		out_encrypted = false;
		std::ostream* decrypt_log = m_interf->verbose() ? &m_interf->getLogStream() : NULL;
		if (&m_interf->getLogStream() != &std::cerr)
			zipfile.setLogStream(m_interf->getLogStream());

		if (check_header_first && m_decrypted_zip.empty())
		{
			bool io_err = false;
			if (!hasZipMagicHeader(io_err))
			{
				if (io_err)
				{
					m_interf->getLogStream() << (m_buffer ? "Memory buffer is empty.\n" : ("Error opening file " + m_file_name + ".\n"));
					m_interf->setError(true);
					return false;
				}
				OffCryptoResult dec_res = try_decrypt_offcrypto_source(
					m_file_name, m_buffer, m_buffer_size, m_decrypted_zip, decrypt_log);
				if (dec_res == OFFCRYPTO_NEEDS_PASSWORD)
				{
					out_encrypted = true;
					return false;
				}
				if (dec_res != OFFCRYPTO_DECRYPTED)
					return false;
			}
		}

		bindZipSource(zipfile);
		if (zipfile.open())
			return true;

		if (!check_header_first && m_decrypted_zip.empty())
		{
			OffCryptoResult dec_res = try_decrypt_offcrypto_source(
				m_file_name, m_buffer, m_buffer_size, m_decrypted_zip, decrypt_log);
			if (dec_res == OFFCRYPTO_DECRYPTED)
			{
				bindZipSource(zipfile);
				if (zipfile.open())
					return true;
				clearDecryptedZip();
			}
			else if (dec_res == OFFCRYPTO_NEEDS_PASSWORD)
			{
				out_encrypted = true;
				return false;
			}
		}
		return false;
	}

	bool readOOXMLComments(const DocToTextUnzip& zipfile, XmlParseMode mode, FormattingStyle& options)
	{
		std::string content;
		if (!zipfile.read("word/comments.xml", &content))
		{
			m_interf->getLogStream() << "Error reading word/comments.xml" << endl;
			return false;
		}
		std::string xml;
		if (mode == FIX_XML)
		{
			DocToTextXmlFixer xml_fixer;
			xml = xml_fixer.fix(content);
		}
		else
			xml = content;
		XmlStream xml_stream(xml, m_interf->manageXmlParser(), m_interf->getXmlOptions());
		if (!xml_stream)
		{
			m_interf->getLogStream() << "Error parsing word/comments.xml" << endl;
			return false;
		}
		xml_stream.levelDown();
		while (xml_stream)
		{
			if (xml_stream.name() == "comment")
			{
				std::vector<Link> links;
				int id = str_to_int(xml_stream.attribute("id"));
				std::string author = xml_stream.attribute("author");
				std::string date = xml_stream.attribute("date");
				xml_stream.levelDown();
				std::string text = m_interf->parseXmlData(xml_stream, mode, options, &zipfile, links);
				xml_stream.levelUp();
				CommonXMLDocumentParser::Comment c(author, date, text, links);
				m_interf->getComments()[id] = c;
			}
			xml_stream.next();
		}
		return true;
	}

	void readStyles(const DocToTextUnzip& zipfile, XmlParseMode mode, FormattingStyle options)
	{
		std::string content;
		if (!zipfile.read("styles.xml", &content))
		{

			m_interf->getLogStream() << "Error reading styles.xml" << endl;
			return;
		}
		std::string xml;
		if (mode == FIX_XML)
		{
			DocToTextXmlFixer xml_fixer;
			xml = xml_fixer.fix(content);
		}
		else
			xml = content;
		XmlStream xml_stream(xml, m_interf->manageXmlParser(), m_interf->getXmlOptions());
		if (!xml_stream)
		{
			m_interf->getLogStream() << "Error parsing styles.xml" << endl;
			return;
		}
		//we dont need those links (we dont expect them in "styles". But parseXmlData require this
		std::vector<Link> links;
		m_interf->parseXmlData(xml_stream, mode, options, &zipfile, links);
	}
};

ODFOOXMLParser::ODFOOXMLParser(const string& file_name)
{
	extended_impl = NULL;
	try
	{
		extended_impl = new ExtendedImplementation();
		extended_impl->m_file_name = file_name;
		extended_impl->m_buffer = NULL;
		extended_impl->m_buffer_size = 0;
		extended_impl->m_interf = this;
		registerODFOOXMLCommandHandler("attrName", &CommandHandlersSet::onOOXMLAttribute);
		registerODFOOXMLCommandHandler("c", &CommandHandlersSet::onOOXMLCell);
		registerODFOOXMLCommandHandler("headerFooter", &CommandHandlersSet::onOOXMLHeaderFooter);
		registerODFOOXMLCommandHandler("commentReference", &CommandHandlersSet::onOOXMLCommentReference);
		registerODFOOXMLCommandHandler("br", &CommandHandlersSet::onOOXMLBreak);
		registerODFOOXMLCommandHandler("instrText", &CommandHandlersSet::onOOXMLInstrtext);
	}
	catch (std::bad_alloc& ba)
	{
		if (extended_impl)
			delete extended_impl;
		cleanUp();
		throw;
	}
}

ODFOOXMLParser::ODFOOXMLParser(const char *buffer, size_t size)
{
	extended_impl = NULL;
	try
	{
		extended_impl = new ExtendedImplementation();
		extended_impl->m_file_name = "Memory buffer";
		extended_impl->m_buffer = buffer;
		extended_impl->m_buffer_size = size;
		extended_impl->m_interf = this;
		registerODFOOXMLCommandHandler("attrName", &CommandHandlersSet::onOOXMLAttribute);
		registerODFOOXMLCommandHandler("c", &CommandHandlersSet::onOOXMLCell);
		registerODFOOXMLCommandHandler("headerFooter", &CommandHandlersSet::onOOXMLHeaderFooter);
		registerODFOOXMLCommandHandler("commentReference", &CommandHandlersSet::onOOXMLCommentReference);
		registerODFOOXMLCommandHandler("br", &CommandHandlersSet::onOOXMLBreak);
		registerODFOOXMLCommandHandler("instrText", &CommandHandlersSet::onOOXMLInstrtext);
	}
	catch (std::bad_alloc& ba)
	{
		if (extended_impl)
			delete extended_impl;
		cleanUp();
		throw;
	}
}

ODFOOXMLParser::~ODFOOXMLParser()
{
	if (extended_impl)
		delete extended_impl;
}

bool ODFOOXMLParser::isODFOOXML()
{
	setError(false);
	setEncrypted(false);
	extended_impl->m_needs_password = false;
	extended_impl->clearDecryptedZip();

	DocToTextUnzip zipfile;
	bool is_enc = false;
	if (!extended_impl->openZipOrDecrypt(zipfile, true, is_enc))
	{
		if (is_enc)
		{
			extended_impl->m_needs_password = true;
			setEncrypted(true);
			return true;
		}
		extended_impl->clearDecryptedZip();
		return false;
	}
	if (has_odf_encryption_manifest(zipfile))
	{
		extended_impl->m_needs_password = true;
		setEncrypted(true);
		zipfile.close();
		extended_impl->clearDecryptedZip();
		return true;
	}
	string main_file_name = locate_main_file(zipfile, getLogStream());
	if (main_file_name == "")
	{
		zipfile.close();
		extended_impl->clearDecryptedZip();
		return false;
	}
	string contents;
	if (!zipfile.read(main_file_name, &contents, 1))
	{
		zipfile.close();
		extended_impl->clearDecryptedZip();
		return false;
	}
	zipfile.close();
	return true;
}

string ODFOOXMLParser::plainText(XmlParseMode mode, FormattingStyle& options)
{
	setError(false);
	setEncrypted(false);
	ExtendedImplementation::DecryptedZipGuard decrypted_guard(extended_impl);
	if (extended_impl->m_needs_password)
	{
		getLogStream() << "Office file is encrypted and requires a password.\n";
		setEncrypted(true);
		setError(true);
		return "";
	}
	DocToTextUnzip zipfile;
	bool is_enc = false;
	if (!extended_impl->openZipOrDecrypt(zipfile, false, is_enc))
	{
		if (is_enc)
			getLogStream() << "Office file is encrypted and requires a password.\n";
		else
			getLogStream() << "Error opening file " << extended_impl->m_file_name.c_str() << endl;
		setEncrypted(is_enc);
		setError(true);
		return "";
	}
	if (has_odf_encryption_manifest(zipfile))
	{
		getLogStream() << "ODF file is encrypted and requires a password.\n";
		setEncrypted(true);
		setError(true);
		zipfile.close();
		return "";
	}
	string main_file_name = locate_main_file(zipfile, getLogStream());
	if (main_file_name == "")
	{
		getLogStream() << "Could not locate main file.\n";
		setError(true);
		zipfile.close();
		return "";
	}
	//according to the ODF specification, we must skip blank nodes. Otherwise output may be messed up.
	if (main_file_name == "content.xml")
		setXmlOptions(XML_PARSE_NOBLANKS);
	if (zipfile.exists("word/comments.xml"))
		if (!extended_impl->readOOXMLComments(zipfile, mode, options))
		{
			getLogStream() << "Error parsing comments.\n";
			setError(true);
			zipfile.close();
			return "";
		}
	if (zipfile.exists("styles.xml"))
		extended_impl->readStyles(zipfile, mode, options);
	string content;
	string text;
	std::set<std::string> visited_diagrams;
	auto appendBlockText = [](std::string& dst, const std::string& block)
	{
		if (block.empty())
			return;
		if (!dst.empty() && dst[dst.size() - 1] != '\n')
			dst += "\n";
		dst += block;
	};
	auto extractDiagramData = [&](const std::string& diag_path, std::string& dst)
	{
		if (diag_path.empty() || !visited_diagrams.insert(diag_path).second)
			return;
		std::string diag_content;
		if (!zipfile.read(diag_path, &diag_content) || diag_content.empty())
			return;
		std::string diag_text;
		if (extractText(diag_content, mode, options, &zipfile, diag_text, getInnerLinks()))
			appendBlockText(dst, diag_text);
	};

	if (main_file_name == "ppt/presentation.xml")
	{
		if (!zipfile.loadDirectory())
		{
			getLogStream() << "Error loading zip directory of file " << extended_impl->m_file_name.c_str() << endl;
			zipfile.close();
			setError(true);
			return "";
		}
		std::set<std::string> visited_slide_parts;
		std::vector<std::string> slide_layouts;
		std::vector<std::string> slide_masters;
		for (int i = 1; zipfile.read("ppt/slides/slide" + int_to_str(i) + ".xml", &content) && i < 2500; i++)
		{
			string slide_rels;
			clearImageRelationships();
			if (zipfile.read("ppt/slides/_rels/slide" + int_to_str(i) + ".xml.rels", &slide_rels))
			{
				loadRelationshipsFromXml(slide_rels, "ppt/slides/");
				collectRelTargetsBySuffix(slide_rels, "ppt/slides/", "/slideLayout",
				                          manageXmlParser(), getXmlOptions(), slide_layouts);
			}
			string slide_text;
			if (!extractText(content, mode, options, &zipfile, slide_text, getInnerLinks()))
			{
				getLogStream() << "Parser error." << endl;
				setError(true);
				zipfile.close();
				return "";
			}
			text += slide_text;
		}
		string pres_rels;
		if (zipfile.read("ppt/_rels/presentation.xml.rels", &pres_rels))
		{
			collectRelTargetsBySuffix(pres_rels, "ppt/", "/slideMaster",
			                          manageXmlParser(), getXmlOptions(), slide_masters);
		}
		auto processMasterOrLayoutPart = [&](const std::string& part_path, bool collect_masters)
		{
			if (part_path.empty() || !visited_slide_parts.insert(part_path).second)
				return;
			std::string part_base_dir;
			std::string rels_path = makeRelsPathForPart(part_path, part_base_dir);
			clearImageRelationships();
			std::string part_rels;
			if (zipfile.read(rels_path, &part_rels))
			{
				loadRelationshipsFromXml(part_rels, part_base_dir);
				if (collect_masters)
				{
					collectRelTargetsBySuffix(part_rels, part_base_dir, "/slideMaster",
					                          manageXmlParser(), getXmlOptions(), slide_masters);
				}
			}
			std::string part_xml;
			if (!zipfile.read(part_path, &part_xml) || part_xml.empty())
				return;
			std::string filtered_xml;
			if (!filterPptxPlaceholderShapes(part_xml, mode, manageXmlParser(), getXmlOptions(), getImageParserCallback() != NULL, filtered_xml))
				return;
			std::string part_text;
			XmlParseMode part_mode = (mode == STRIP_XML) ? STRIP_XML : PARSE_XML;
			if (extractText(filtered_xml, part_mode, options, &zipfile, part_text, getInnerLinks()))
				appendBlockText(text, part_text);
		};
		for (size_t i = 0; i < slide_layouts.size(); ++i)
			processMasterOrLayoutPart(slide_layouts[i], true);
		for (size_t i = 0; i < slide_masters.size(); ++i)
			processMasterOrLayoutPart(slide_masters[i], false);
	}
	else if (main_file_name == "xl/workbook.xml")
	{
		if (!zipfile.read("xl/sharedStrings.xml", &content))
		{
			//file may not exist, but this is not reason to report an error.
			if (verbose())
				getLogStream() << "xl/sharedStrings.xml does not exist" << endl;
		}
		else
		{
			std::string xml;
			if (mode == FIX_XML)
			{
				DocToTextXmlFixer xml_fixer;
				xml = xml_fixer.fix(content);
			}
			else if (mode == PARSE_XML)
				xml = content;
			else
			{
				getLogStream() << "XML stripping not possible for xlsx format" << endl;
				setError(true);
				zipfile.close();
				return "";
			}
			XmlStream xml_stream(xml, manageXmlParser(), getXmlOptions());
			if (!xml_stream)
			{
				getLogStream() << "Error parsing xl/sharedStrings.xml" << endl;
				setError(true);
				zipfile.close();
				return "";
			}
			xml_stream.levelDown();
			while (xml_stream)
			{
				if (xml_stream.name() == "si")
				{
					xml_stream.levelDown();
					SharedString shared_string;
					shared_string.m_text = parseXmlData(xml_stream, mode, options, &zipfile, shared_string.m_links);
					getSharedStrings().push_back(shared_string);
					xml_stream.levelUp();
				}
				xml_stream.next();
			}
		}
		for (int i = 1; zipfile.read("xl/worksheets/sheet" + int_to_str(i) + ".xml", &content); i++)
		{
			string sheet_rels;
			clearImageRelationships();
			std::vector<string> drawing_files;
			if (zipfile.read("xl/worksheets/_rels/sheet" + int_to_str(i) + ".xml.rels", &sheet_rels))
			{
				loadRelationshipsFromXml(sheet_rels, "xl/worksheets/");
				XmlStream rel_stream(sheet_rels, manageXmlParser(), getXmlOptions());
				if (rel_stream)
				{
					rel_stream.levelDown();
					while (rel_stream)
					{
						if (rel_stream.name() == "Relationship")
						{
							string rtype = rel_stream.attribute("Type");
							string rtarget = rel_stream.attribute("Target");
							if (rtype.find("/drawing") != string::npos && !rtarget.empty())
							{
								string full_target = (rtarget[0] == '/') ? rtarget.substr(1) : ("xl/worksheets/" + rtarget);
								drawing_files.push_back(normalizeZipPath(full_target));
							}
						}
						rel_stream.next();
					}
				}
			}
			string sheet_text;
			if (!extractText(content, mode, options, &zipfile, sheet_text, getInnerLinks()))
			{
				getLogStream() << "parser error" << endl;
				setError(true);
				zipfile.close();
				return "";
			}
			for (size_t d = 0; d < drawing_files.size(); ++d)
			{
				string drawing_content;
				if (zipfile.read(drawing_files[d], &drawing_content))
				{
					size_t last_slash = drawing_files[d].find_last_of('/');
					string d_base = (last_slash == string::npos) ? "" : drawing_files[d].substr(0, last_slash + 1);
					string d_name = (last_slash == string::npos) ? drawing_files[d] : drawing_files[d].substr(last_slash + 1);
					string d_rels_path = d_base + "_rels/" + d_name + ".rels";
					string d_rels_content;
					clearImageRelationships();
					if (zipfile.read(d_rels_path, &d_rels_content))
					{
						loadRelationshipsFromXml(d_rels_content, d_base);
						XmlStream d_rel_stream(d_rels_content, manageXmlParser(), getXmlOptions());
						if (d_rel_stream)
						{
							d_rel_stream.levelDown();
							while (d_rel_stream)
							{
								if (d_rel_stream.name() == "Relationship")
								{
									string rtype = d_rel_stream.attribute("Type");
									string rtarget = d_rel_stream.attribute("Target");
									if (rtype.find("/diagramData") != string::npos && !rtarget.empty())
									{
										string diag_path = normalizeZipPath(
											(rtarget[0] == '/') ? rtarget.substr(1) : (d_base + rtarget));
										extractDiagramData(diag_path, sheet_text);
									}
								}
								d_rel_stream.next();
							}
						}
					}
					string drawing_text;
					XmlStream::ensureXmlParserInitialized(manageXmlParser());
					xmlDocPtr ddoc = xmlReadMemory(drawing_content.c_str(), drawing_content.size(), "drawing.xml", NULL, getXmlOptions());
					if (ddoc != NULL)
					{
						extractContentFromDrawingNode(ddoc->children, &zipfile, drawing_text);
						xmlFreeDoc(ddoc);
						if (!drawing_text.empty())
						{
							if (!sheet_text.empty() && sheet_text[sheet_text.size() - 1] != '\n')
								sheet_text += "\n";
							sheet_text += drawing_text;
						}
					}
				}
			}
			text += sheet_text;
		}
	}
	else
	{
		clearImageRelationships();
		size_t last_slash = main_file_name.find_last_of('/');
		string base_dir = (last_slash == string::npos) ? "" : main_file_name.substr(0, last_slash + 1);
		string rels_file = (last_slash == string::npos) ? ("_rels/" + main_file_name + ".rels")
		                                                : (base_dir + "_rels/" + main_file_name.substr(last_slash + 1) + ".rels");
		string rels_content;
		if (zipfile.read(rels_file, &rels_content))
		{
			loadRelationshipsFromXml(rels_content, base_dir);
		}
		if (!zipfile.read(main_file_name, &content))
		{
			getLogStream() << "Error reading " << main_file_name << endl;
			setError(true);
			zipfile.close();
			return "";
		}
		if (!extractText(content, mode, options, &zipfile, text, getInnerLinks()))
		{
			getLogStream() << "Parser error." << endl;
			setError(true);
			zipfile.close();
			return "";
		}
		if (main_file_name == "content.xml" &&
		    content.find("<office:presentation") != std::string::npos &&
		    mode != STRIP_XML &&
		    zipfile.exists("styles.xml"))
		{
			std::string styles_content;
			if (zipfile.read("styles.xml", &styles_content))
			{
				std::string master_footer_text;
				extractReferencedOdfMasterFooters(content, styles_content, mode, manageXmlParser(), getXmlOptions(), master_footer_text);
				appendBlockText(text, master_footer_text);
			}
		}
	}
	// Extract SmartArt diagram data XML not already visited via drawing relationships (DOCX/PPTX/fallback XLSX)
	if (main_file_name != "content.xml")
	{
		std::vector<std::string> diag_files;
		if (zipfile.getFileList(diag_files))
		{
			std::sort(diag_files.begin(), diag_files.end());
			for (size_t i = 0; i < diag_files.size(); ++i)
			{
				const std::string& p = diag_files[i];
				bool is_diag_data =
					(p.compare(0, 16, "xl/diagrams/data") == 0 ||
					 p.compare(0, 18, "word/diagrams/data") == 0 ||
					 p.compare(0, 17, "ppt/diagrams/data") == 0) &&
					p.size() >= 4 && p.compare(p.size() - 4, 4, ".xml") == 0;
				if (is_diag_data)
					extractDiagramData(p, text);
			}
		}
	}
	decodeSpecialLinkBlocks(text, getInnerLinks(), getLogStream());
	if (options.parse_embedded_files)
	{
		doctotext::EmbeddedExtractionContext root_ctx;
		root_ctx.formatting = options;
		root_ctx.xml_parse_mode = mode;
		root_ctx.manage_xml_parser = manageXmlParser();
		root_ctx.verbose_logging = verbose();
		root_ctx.log_stream = &getLogStream();
		root_ctx.image_callback = getImageParserCallback();
		root_ctx.image_user_data = getImageParserUserData();
		doctotext::EmbeddedExtractionContext::ScopedContext tls_scope(root_ctx);
		bool is_ooxml = (main_file_name != "content.xml");
		doctotext::extractZipEmbeddings(zipfile, is_ooxml, tls_scope.context(), text);
	}
	zipfile.close();
	return text;
}

Metadata ODFOOXMLParser::metaData()
{
	setError(false);
	setEncrypted(false);
	Metadata meta;
	ExtendedImplementation::DecryptedZipGuard decrypted_guard(extended_impl);
	if (extended_impl->m_needs_password)
	{
		setEncrypted(true);
		setError(true);
		return meta;
	}
	DocToTextUnzip zipfile;
	bool is_enc = false;
	if (!extended_impl->openZipOrDecrypt(zipfile, false, is_enc))
	{
		if (!is_enc)
			getLogStream() << "Error opening file " << extended_impl->m_file_name.c_str() << endl;
		setEncrypted(is_enc);
		setError(true);
		return meta;
	}
	if (has_odf_encryption_manifest(zipfile))
	{
		setEncrypted(true);
		setError(true);
		zipfile.close();
		return meta;
	}
	if (zipfile.exists("meta.xml"))
	{
		std::string meta_xml;
		if (!zipfile.read("meta.xml", &meta_xml))
		{
			getLogStream() << "Error reading meta.xml" << endl;
			zipfile.close();
			setError(true);
			return meta;
		}
		if (!parseODFMetadata(meta_xml, meta))
		{
			getLogStream() << "Error reading meta.xml" << endl;
			zipfile.close();
			setError(true);
			return meta;
		}
	}
	else if (zipfile.exists("docProps/core.xml"))
	{
		std::string core_xml;
		if (!zipfile.read("docProps/core.xml", &core_xml))
		{
			getLogStream() << "Error reading docProps/core.xml" << endl;
			zipfile.close();
			setError(true);
			return meta;
		}
		XmlStream xml_stream(core_xml, manageXmlParser(), getXmlOptions());
		if (!xml_stream)
		{
			zipfile.close();
			setError(true);
			return meta;
		}
		xml_stream.levelDown();
		while (xml_stream)
		{
			if (xml_stream.name() == "creator")
				meta.setAuthor(xml_stream.stringValue());
			if (xml_stream.name() == "created")
			{
				tm creation_date;
				string_to_date(xml_stream.stringValue(), creation_date);
				meta.setCreationDate(creation_date);
			}
			if (xml_stream.name() == "lastModifiedBy")
				meta.setLastModifiedBy(xml_stream.stringValue());
			if (xml_stream.name() == "modified")
			{
				tm last_modification_date;
				string_to_date(xml_stream.stringValue(), last_modification_date);
				meta.setLastModificationDate(last_modification_date);
			}
			xml_stream.next();
		}

		std::string app_xml;
		if (!zipfile.read("docProps/app.xml", &app_xml))
		{
			getLogStream() << "Error reading docProps/app.xml" << endl;
			zipfile.close();
			setError(true);
			return meta;
		}
		XmlStream app_stream(app_xml, manageXmlParser(), getXmlOptions());
		if (!app_stream)
		{
			zipfile.close();
			setError(true);
			return meta;
		}
		app_stream.levelDown();
		while (app_stream)
		{
			if (app_stream.name() == "Pages")
				meta.setPageCount(str_to_int(app_stream.stringValue()));
			if (app_stream.name() == "Words")
				meta.setWordCount(str_to_int(app_stream.stringValue()));
			app_stream.next();
		}
	}
	if (meta.pageCount() == -1)
	{
		// If we are processing PPT use slide count as page count
		if (zipfile.exists("ppt/presentation.xml"))
		{
			int page_count = 0;
			for (int i = 1; zipfile.exists("ppt/slides/slide" + int_to_str(i) + ".xml"); i++)
				page_count++;
			meta.setPageCount(page_count);
		}
		else if (zipfile.exists("content.xml"))
		{
			// If we are processing ODP use slide count as page count
			// If we are processing ODG extract page count the same way
			std::string content;
			if (zipfile.read("content.xml", &content) &&
				(content.find("<office:presentation") != std::string::npos ||
				content.find("<office:drawing") != std::string::npos))
			{
				int page_count = 0;
				std::string page_str = "<draw:page ";
				for (size_t pos = content.find(page_str); pos != std::string::npos;
						pos = content.find(page_str, pos + page_str.length()))
					page_count++;
				meta.setPageCount(page_count);
			}
		}
	}
	zipfile.close();
	return meta;
}
