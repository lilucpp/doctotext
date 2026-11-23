#include "iwork_iwa_parser.h"

#include "doctotext_unzip.h"
#include "snappy.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

namespace doctotext
{
namespace iwa
{

namespace
{

const size_t kMaxIndexZipBytes = 64u * 1024u * 1024u;
const size_t kMaxIwaDecompressedBytes = 64u * 1024u * 1024u;
const int kMaxWireGroupDepth = 16;
const int kMaxSlideTreeDepth = 64;

const uint32_t kMsgDocumentArchive = 1;
const uint32_t kMsgShowArchive = 2;
const uint32_t kMsgSlideTreeArchive = 3;
const uint32_t kMsgSlideNodeArchive = 4;
const uint32_t kMsgStorageArchive = 2001;
const uint32_t kMsgTableModelArchive = 6001;
const uint32_t kMsgTableDataList = 6005;

bool readVarint(const unsigned char* data, size_t len, size_t& pos, uint64_t& out)
{
	out = 0;
	unsigned int shift = 0;
	while (pos < len)
	{
		unsigned char b = data[pos++];
		if (shift >= 63 && b > 1)
			return false;
		out |= static_cast<uint64_t>(b & 0x7Fu) << shift;
		if ((b & 0x80u) == 0)
			return true;
		shift += 7;
	}
	return false;
}

bool skipWireValue(const unsigned char* data, size_t len, size_t& pos, uint32_t wire_type, int depth)
{
	if (depth > kMaxWireGroupDepth)
		return false;
	switch (wire_type)
	{
		case 0:
		{
			uint64_t dummy = 0;
			return readVarint(data, len, pos, dummy);
		}
		case 1:
			if (len - pos < 8)
				return false;
			pos += 8;
			return true;
		case 2:
		{
			uint64_t blen = 0;
			if (!readVarint(data, len, pos, blen) || blen > len - pos)
				return false;
			pos += static_cast<size_t>(blen);
			return true;
		}
		case 3:
		{
			while (pos < len)
			{
				uint64_t tag = 0;
				if (!readVarint(data, len, pos, tag))
					return false;
				uint32_t wt = static_cast<uint32_t>(tag & 0x07u);
				if (wt == 4)
					return true;
				if (!skipWireValue(data, len, pos, wt, depth + 1))
					return false;
			}
			return false;
		}
		case 4:
			return false;
		case 5:
			if (len - pos < 4)
				return false;
			pos += 4;
			return true;
		default:
			return false;
	}
}

bool readLengthDelimited(const unsigned char* data, size_t len, size_t& pos, size_t& slice_off, size_t& slice_len)
{
	uint64_t blen = 0;
	if (!readVarint(data, len, pos, blen) || blen > len - pos)
		return false;
	slice_off = pos;
	slice_len = static_cast<size_t>(blen);
	pos += slice_len;
	return true;
}

bool parseTspReferenceId(const unsigned char* data, size_t len, uint64_t& out_id)
{
	size_t pos = 0;
	bool found = false;
	while (pos < len)
	{
		uint64_t tag = 0;
		if (!readVarint(data, len, pos, tag))
			return false;
		uint32_t fn = static_cast<uint32_t>(tag >> 3);
		uint32_t wt = static_cast<uint32_t>(tag & 0x07u);
		if (fn == 1 && wt == 0)
		{
			if (!readVarint(data, len, pos, out_id))
				return false;
			found = true;
		}
		else if (!skipWireValue(data, len, pos, wt, 0))
		{
			return false;
		}
	}
	return found;
}

struct MessageInfoMeta
{
	uint32_t type;
	uint64_t length;
};

bool parseMessageInfo(const unsigned char* data, size_t len, MessageInfoMeta& out)
{
	out.type = 0;
	out.length = 0;
	size_t pos = 0;
	while (pos < len)
	{
		uint64_t tag = 0;
		if (!readVarint(data, len, pos, tag))
			return false;
		uint32_t fn = static_cast<uint32_t>(tag >> 3);
		uint32_t wt = static_cast<uint32_t>(tag & 0x07u);
		if (fn == 1 && wt == 0)
		{
			uint64_t v = 0;
			if (!readVarint(data, len, pos, v))
				return false;
			out.type = static_cast<uint32_t>(v);
		}
		else if (fn == 3 && wt == 0)
		{
			if (!readVarint(data, len, pos, out.length))
				return false;
		}
		else if (!skipWireValue(data, len, pos, wt, 0))
		{
			return false;
		}
	}
	return true;
}

bool parseArchiveInfo(const unsigned char* data, size_t len, uint64_t& out_aid, std::vector<MessageInfoMeta>& out_msgs)
{
	out_aid = 0;
	out_msgs.clear();
	size_t pos = 0;
	while (pos < len)
	{
		uint64_t tag = 0;
		if (!readVarint(data, len, pos, tag))
			return false;
		uint32_t fn = static_cast<uint32_t>(tag >> 3);
		uint32_t wt = static_cast<uint32_t>(tag & 0x07u);
		if (fn == 1 && wt == 0)
		{
			if (!readVarint(data, len, pos, out_aid))
				return false;
		}
		else if (fn == 2 && wt == 2)
		{
			size_t sub_off = 0, sub_len = 0;
			if (!readLengthDelimited(data, len, pos, sub_off, sub_len))
				return false;
			MessageInfoMeta meta;
			if (!parseMessageInfo(data + sub_off, sub_len, meta))
				return false;
			out_msgs.push_back(meta);
		}
		else if (!skipWireValue(data, len, pos, wt, 0))
		{
			return false;
		}
	}
	return true;
}

std::string normalizeIwaText(const std::string& raw)
{
	// Strip U+FFFC object-replacement placeholders (\xEF\xBF\xBC) inserted for shapes/tables/images.
	std::string out;
	out.reserve(raw.size());
	for (size_t i = 0; i < raw.size();)
	{
		if (i + 2 < raw.size() &&
		    static_cast<unsigned char>(raw[i]) == 0xEF &&
		    static_cast<unsigned char>(raw[i + 1]) == 0xBF &&
		    static_cast<unsigned char>(raw[i + 2]) == 0xBC)
		{
			i += 3;
			continue;
		}
		out.push_back(raw[i++]);
	}
	return out;
}

bool hasNonWhitespace(const std::string& s)
{
	for (size_t i = 0; i < s.size(); ++i)
	{
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c > 0x7F || !std::isspace(c))
			return true;
	}
	return false;
}

void appendBlock(std::string& dst, const std::string& block)
{
	if (!hasNonWhitespace(block))
		return;
	if (!dst.empty() && dst[dst.size() - 1] != '\n')
		dst.push_back('\n');
	dst += block;
	if (!dst.empty() && dst[dst.size() - 1] != '\n')
		dst.push_back('\n');
}

void extractTswpStrings(const unsigned char* data, size_t len, std::vector<std::string>& out_texts)
{
	size_t pos = 0;
	while (pos < len)
	{
		uint64_t tag = 0;
		if (!readVarint(data, len, pos, tag))
			break;
		uint32_t fn = static_cast<uint32_t>(tag >> 3);
		uint32_t wt = static_cast<uint32_t>(tag & 0x07u);
		if (fn == 3 && wt == 2)
		{
			size_t off = 0, slen = 0;
			if (!readLengthDelimited(data, len, pos, off, slen))
				break;
			std::string s = normalizeIwaText(std::string(reinterpret_cast<const char*>(data + off), slen));
			if (hasNonWhitespace(s))
				out_texts.push_back(s);
		}
		else if (!skipWireValue(data, len, pos, wt, 0))
		{
			break;
		}
	}
}

struct DataListEntryItem
{
	uint64_t key;
	size_t order;
	std::string value;
};

void extractTableDataListStrings(const unsigned char* data, size_t len, std::vector<std::string>& out_strings)
{
	std::vector<DataListEntryItem> items;
	size_t pos = 0;
	while (pos < len)
	{
		uint64_t tag = 0;
		if (!readVarint(data, len, pos, tag))
			break;
		uint32_t fn = static_cast<uint32_t>(tag >> 3);
		uint32_t wt = static_cast<uint32_t>(tag & 0x07u);
		if (fn == 3 && wt == 2)
		{
			size_t entry_off = 0, entry_len = 0;
			if (!readLengthDelimited(data, len, pos, entry_off, entry_len))
				break;
			size_t ep = entry_off;
			size_t end = entry_off + entry_len;
			uint64_t entry_key = 0;
			std::string entry_val;
			bool has_val = false;
			while (ep < end)
			{
				uint64_t etag = 0;
				if (!readVarint(data, end, ep, etag))
					break;
				uint32_t efn = static_cast<uint32_t>(etag >> 3);
				uint32_t ewt = static_cast<uint32_t>(etag & 0x07u);
				if (efn == 1 && ewt == 0)
				{
					if (!readVarint(data, end, ep, entry_key))
						break;
				}
				else if (efn == 3 && ewt == 2)
				{
					size_t soff = 0, slen = 0;
					if (!readLengthDelimited(data, end, ep, soff, slen))
						break;
					entry_val = normalizeIwaText(std::string(reinterpret_cast<const char*>(data + soff), slen));
					has_val = hasNonWhitespace(entry_val);
				}
				else if (!skipWireValue(data, end, ep, ewt, 0))
				{
					break;
				}
			}
			if (has_val)
			{
				DataListEntryItem item;
				item.key = entry_key;
				item.order = items.size();
				item.value = std::move(entry_val);
				items.push_back(std::move(item));
			}
		}
		else if (!skipWireValue(data, len, pos, wt, 0))
		{
			break;
		}
	}
	std::sort(items.begin(), items.end(), [](const DataListEntryItem& a, const DataListEntryItem& b) {
		if (a.key != b.key)
			return a.key < b.key;
		return a.order < b.order;
	});
	for (size_t i = 0; i < items.size(); ++i)
		out_strings.push_back(std::move(items[i].value));
}

struct TableModelMeta
{
	std::string name;
	uint64_t string_table_aid;
	bool has_string_table;
};

void parseTableModelArchive(const unsigned char* data, size_t len, TableModelMeta& out)
{
	out.name.clear();
	out.string_table_aid = 0;
	out.has_string_table = false;
	size_t pos = 0;
	while (pos < len)
	{
		uint64_t tag = 0;
		if (!readVarint(data, len, pos, tag))
			break;
		uint32_t fn = static_cast<uint32_t>(tag >> 3);
		uint32_t wt = static_cast<uint32_t>(tag & 0x07u);
		if (fn == 8 && wt == 2)
		{
			size_t off = 0, slen = 0;
			if (!readLengthDelimited(data, len, pos, off, slen))
				break;
			out.name.assign(reinterpret_cast<const char*>(data + off), slen);
		}
		else if (fn == 4 && wt == 2)
		{
			// TST.DataStore: field 4 = TSP.Reference stringTable
			size_t ds_off = 0, ds_len = 0;
			if (!readLengthDelimited(data, len, pos, ds_off, ds_len))
				break;
			size_t dp = ds_off;
			size_t dend = ds_off + ds_len;
			while (dp < dend)
			{
				uint64_t dtag = 0;
				if (!readVarint(data, dend, dp, dtag))
					break;
				uint32_t dfn = static_cast<uint32_t>(dtag >> 3);
				uint32_t dwt = static_cast<uint32_t>(dtag & 0x07u);
				if (dfn == 4 && dwt == 2)
				{
					size_t roff = 0, rlen = 0;
					if (!readLengthDelimited(data, dend, dp, roff, rlen))
						break;
					uint64_t ref_id = 0;
					if (parseTspReferenceId(data + roff, rlen, ref_id))
					{
						out.string_table_aid = ref_id;
						out.has_string_table = true;
					}
				}
				else if (!skipWireValue(data, dend, dp, dwt, 0))
				{
					break;
				}
			}
		}
		else if (!skipWireValue(data, len, pos, wt, 0))
		{
			break;
		}
	}
}

struct SlideNodeMeta
{
	uint64_t slide_aid;
	bool has_slide;
	std::vector<uint64_t> children;
};

void parseShowArchiveSlideTree(const unsigned char* data, size_t len, std::vector<uint64_t>& out_root_nodes)
{
	size_t pos = 0;
	while (pos < len)
	{
		uint64_t tag = 0;
		if (!readVarint(data, len, pos, tag))
			break;
		uint32_t fn = static_cast<uint32_t>(tag >> 3);
		uint32_t wt = static_cast<uint32_t>(tag & 0x07u);
		if (fn == 3 && wt == 2)
		{
			size_t toff = 0, tlen = 0;
			if (!readLengthDelimited(data, len, pos, toff, tlen))
				break;
			size_t tp = toff;
			size_t tend = toff + tlen;
			while (tp < tend)
			{
				uint64_t ttag = 0;
				if (!readVarint(data, tend, tp, ttag))
					break;
				uint32_t tfn = static_cast<uint32_t>(ttag >> 3);
				uint32_t twt = static_cast<uint32_t>(ttag & 0x07u);
				if (tfn == 2 && twt == 2)
				{
					size_t roff = 0, rlen = 0;
					if (!readLengthDelimited(data, tend, tp, roff, rlen))
						break;
					uint64_t ref_id = 0;
					if (parseTspReferenceId(data + roff, rlen, ref_id))
						out_root_nodes.push_back(ref_id);
				}
				else if (!skipWireValue(data, tend, tp, twt, 0))
				{
					break;
				}
			}
		}
		else if (!skipWireValue(data, len, pos, wt, 0))
		{
			break;
		}
	}
}

void parseSlideNodeArchive(const unsigned char* data, size_t len, SlideNodeMeta& out)
{
	out.slide_aid = 0;
	out.has_slide = false;
	out.children.clear();
	size_t pos = 0;
	while (pos < len)
	{
		uint64_t tag = 0;
		if (!readVarint(data, len, pos, tag))
			break;
		uint32_t fn = static_cast<uint32_t>(tag >> 3);
		uint32_t wt = static_cast<uint32_t>(tag & 0x07u);
		if (fn == 2 && wt == 2)
		{
			size_t roff = 0, rlen = 0;
			if (!readLengthDelimited(data, len, pos, roff, rlen))
				break;
			uint64_t ref_id = 0;
			if (parseTspReferenceId(data + roff, rlen, ref_id))
			{
				out.slide_aid = ref_id;
				out.has_slide = true;
			}
		}
		else if (fn == 3 && wt == 2)
		{
			size_t roff = 0, rlen = 0;
			if (!readLengthDelimited(data, len, pos, roff, rlen))
				break;
			uint64_t ref_id = 0;
			if (parseTspReferenceId(data + roff, rlen, ref_id))
				out.children.push_back(ref_id);
		}
		else if (!skipWireValue(data, len, pos, wt, 0))
		{
			break;
		}
	}
}

void collectOrderedSlideAids(uint64_t node_aid,
                             const std::map<uint64_t, SlideNodeMeta>& nodes,
                             std::set<uint64_t>& visited_nodes,
                             std::vector<uint64_t>& out_slide_aids,
                             int depth)
{
	if (depth > kMaxSlideTreeDepth || !visited_nodes.insert(node_aid).second)
		return;
	std::map<uint64_t, SlideNodeMeta>::const_iterator it = nodes.find(node_aid);
	if (it == nodes.end())
		return;
	if (it->second.has_slide)
		out_slide_aids.push_back(it->second.slide_aid);
	for (size_t i = 0; i < it->second.children.size(); ++i)
		collectOrderedSlideAids(it->second.children[i], nodes, visited_nodes, out_slide_aids, depth + 1);
}

bool endsWith(const std::string& s, const std::string& suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string baseName(const std::string& path)
{
	size_t slash = path.find_last_of('/');
	return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

bool startsWith(const std::string& s, const std::string& prefix)
{
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool findNestedIndexZipEntry(const std::vector<std::string>& entries, std::string& out_entry)
{
	for (size_t i = 0; i < entries.size(); ++i)
	{
		const std::string& e = entries[i];
		if (e == "Index.zip" || endsWith(e, "/Index.zip"))
		{
			out_entry = e;
			return true;
		}
	}
	return false;
}

bool hasDirectIwaEntry(const std::vector<std::string>& entries)
{
	for (size_t i = 0; i < entries.size(); ++i)
	{
		if (startsWith(entries[i], "Index/") && endsWith(entries[i], ".iwa"))
			return true;
	}
	return false;
}

struct ParsedIwaEntry
{
	std::string entry_name;
	std::string stream;
	std::vector<IwaMessageSlice> slices;
	uint64_t primary_aid;
	bool has_primary_aid;
};

bool extractFromIndexZip(DocToTextUnzip& index_zip,
                         bool verbose,
                         std::ostream& log_stream,
                         std::string& out_text,
                         bool& out_error)
{
	if (!index_zip.loadDirectory())
	{
		log_stream << "Error while loading iWork zip directory.\n";
		out_error = true;
		return false;
	}
	std::vector<std::string> entries;
	if (!index_zip.getFileList(entries))
	{
		out_error = true;
		return false;
	}
	std::sort(entries.begin(), entries.end());

	std::vector<ParsedIwaEntry> parsed_entries;
	std::map<uint64_t, size_t> aid_to_entry_idx;
	size_t doc_entry_idx = static_cast<size_t>(-1);
	size_t iwa_attempts = 0;

	for (size_t i = 0; i < entries.size(); ++i)
	{
		const std::string& path = entries[i];
		if (!startsWith(path, "Index/") || !endsWith(path, ".iwa"))
			continue;
		const std::string bname = baseName(path);
		if (startsWith(bname, "MasterSlide") || startsWith(bname, "DocumentStylesheet"))
			continue;
		++iwa_attempts;

		unsigned long raw_size = 0;
		if (!index_zip.getFileSize(path, raw_size) || raw_size > kMaxIwaDecompressedBytes)
		{
			if (verbose)
				log_stream << "Skipping oversized or unreadable iWork entry: " << path << "\n";
			continue;
		}

		std::string raw_iwa;
		if (!index_zip.read(path, &raw_iwa, static_cast<int>(kMaxIwaDecompressedBytes + 1)) ||
		    raw_iwa.size() > kMaxIwaDecompressedBytes)
		{
			if (verbose)
				log_stream << "Skipping oversized or unreadable iWork entry: " << path << "\n";
			continue;
		}

		ParsedIwaEntry item;
		item.entry_name = path;
		item.primary_aid = 0;
		item.has_primary_aid = false;

		std::string err;
		if (!decompressIwaStream(raw_iwa, item.stream, &err))
		{
			if (verbose)
				log_stream << "Failed decompressing iWork entry " << path << ": " << err << "\n";
			continue;
		}
		if (!parseIwaObjectStream(item.stream, item.slices))
		{
			if (verbose)
				log_stream << "Failed parsing iWork object stream " << path << ".\n";
			continue;
		}
		if (!item.slices.empty())
		{
			item.primary_aid = item.slices[0].archive_id;
			item.has_primary_aid = true;
			if (aid_to_entry_idx.find(item.primary_aid) == aid_to_entry_idx.end())
				aid_to_entry_idx[item.primary_aid] = parsed_entries.size();
		}
		if (path == "Index/Document.iwa")
			doc_entry_idx = parsed_entries.size();
		parsed_entries.push_back(std::move(item));
	}

	if (iwa_attempts > 0 && parsed_entries.empty())
	{
		log_stream << "All iWork IWA entries failed decompression or parsing.\n";
		out_error = true;
		return false;
	}

	// Build slide ordering from Document.iwa ShowArchive/SlideNodeArchive when present.
	std::vector<uint64_t> ordered_slide_aids;
	if (doc_entry_idx != static_cast<size_t>(-1))
	{
		const ParsedIwaEntry& doc = parsed_entries[doc_entry_idx];
		const unsigned char* dptr = reinterpret_cast<const unsigned char*>(doc.stream.data());
		std::vector<uint64_t> root_nodes;
		std::map<uint64_t, SlideNodeMeta> node_map;
		for (size_t i = 0; i < doc.slices.size(); ++i)
		{
			const IwaMessageSlice& s = doc.slices[i];
			if (s.message_type == kMsgShowArchive)
				parseShowArchiveSlideTree(dptr + s.offset, s.length, root_nodes);
			else if (s.message_type == kMsgSlideNodeArchive)
			{
				SlideNodeMeta node;
				parseSlideNodeArchive(dptr + s.offset, s.length, node);
				node_map[s.archive_id] = node;
			}
		}
		std::set<uint64_t> visited_nodes;
		for (size_t i = 0; i < root_nodes.size(); ++i)
			collectOrderedSlideAids(root_nodes[i], node_map, visited_nodes, ordered_slide_aids, 0);
	}

	std::vector<size_t> visit_order;
	std::set<size_t> scheduled;
	if (doc_entry_idx != static_cast<size_t>(-1))
	{
		visit_order.push_back(doc_entry_idx);
		scheduled.insert(doc_entry_idx);
	}
	for (size_t i = 0; i < ordered_slide_aids.size(); ++i)
	{
		std::map<uint64_t, size_t>::const_iterator it = aid_to_entry_idx.find(ordered_slide_aids[i]);
		if (it != aid_to_entry_idx.end() && scheduled.insert(it->second).second)
			visit_order.push_back(it->second);
	}
	for (size_t i = 0; i < parsed_entries.size(); ++i)
	{
		if (scheduled.insert(i).second)
			visit_order.push_back(i);
	}

	std::vector<TableModelMeta> tables;
	std::map<uint64_t, std::vector<std::string> > data_lists_by_aid;
	std::vector<uint64_t> data_list_order;

	for (size_t oi = 0; oi < visit_order.size(); ++oi)
	{
		const ParsedIwaEntry& entry = parsed_entries[visit_order[oi]];
		const unsigned char* base = reinterpret_cast<const unsigned char*>(entry.stream.data());
		for (size_t si = 0; si < entry.slices.size(); ++si)
		{
			const IwaMessageSlice& slice = entry.slices[si];
			if (slice.message_type == kMsgStorageArchive)
			{
				std::vector<std::string> strs;
				extractTswpStrings(base + slice.offset, slice.length, strs);
				for (size_t k = 0; k < strs.size(); ++k)
					appendBlock(out_text, strs[k]);
			}
			else if (slice.message_type == kMsgTableModelArchive)
			{
				TableModelMeta tm;
				parseTableModelArchive(base + slice.offset, slice.length, tm);
				tables.push_back(tm);
			}
			else if (slice.message_type == kMsgTableDataList)
			{
				std::vector<std::string> strs;
				extractTableDataListStrings(base + slice.offset, slice.length, strs);
				if (!strs.empty())
				{
					if (data_lists_by_aid.find(slice.archive_id) == data_lists_by_aid.end())
						data_list_order.push_back(slice.archive_id);
					std::vector<std::string>& dst = data_lists_by_aid[slice.archive_id];
					dst.insert(dst.end(), strs.begin(), strs.end());
				}
			}
		}
	}

	std::set<uint64_t> emitted_data_lists;
	for (size_t i = 0; i < tables.size(); ++i)
	{
		const TableModelMeta& tm = tables[i];
		if (!tm.has_string_table)
			continue;
		std::map<uint64_t, std::vector<std::string> >::const_iterator dit = data_lists_by_aid.find(tm.string_table_aid);
		if (dit == data_lists_by_aid.end() || dit->second.empty())
			continue;
		emitted_data_lists.insert(tm.string_table_aid);
		if (hasNonWhitespace(tm.name))
			appendBlock(out_text, tm.name);
		for (size_t k = 0; k < dit->second.size(); ++k)
			appendBlock(out_text, dit->second[k]);
	}
	for (size_t i = 0; i < data_list_order.size(); ++i)
	{
		uint64_t aid = data_list_order[i];
		if (emitted_data_lists.find(aid) != emitted_data_lists.end())
			continue;
		const std::vector<std::string>& strs = data_lists_by_aid[aid];
		for (size_t k = 0; k < strs.size(); ++k)
			appendBlock(out_text, strs[k]);
	}

	out_error = false;
	return true;
}

} // namespace

bool decompressIwaStream(const std::string& raw, std::string& out_decompressed, std::string* err_msg)
{
	out_decompressed.clear();
	size_t pos = 0;
	while (pos < raw.size())
	{
		if (raw.size() - pos < 4)
		{
			if (err_msg)
				*err_msg = "Truncated IWA chunk header";
			return false;
		}
		unsigned char chunk_type = static_cast<unsigned char>(raw[pos]);
		size_t chunk_len = static_cast<size_t>(static_cast<unsigned char>(raw[pos + 1])) |
		                   (static_cast<size_t>(static_cast<unsigned char>(raw[pos + 2])) << 8) |
		                   (static_cast<size_t>(static_cast<unsigned char>(raw[pos + 3])) << 16);
		pos += 4;
		if (chunk_len > raw.size() - pos)
		{
			if (err_msg)
				*err_msg = "IWA chunk length exceeds input size";
			return false;
		}
		if (chunk_type != 0x00)
		{
			if (err_msg)
				*err_msg = "Unsupported IWA chunk type";
			return false;
		}
		size_t uncompressed_len = 0;
		const char* chunk_ptr = raw.data() + pos;
		if (!snappy::GetUncompressedLength(chunk_ptr, chunk_len, &uncompressed_len))
		{
			if (err_msg)
				*err_msg = "Invalid Snappy chunk header";
			return false;
		}
		if (uncompressed_len > kMaxIwaDecompressedBytes - out_decompressed.size())
		{
			if (err_msg)
				*err_msg = "Decompressed IWA stream exceeds safety cap";
			return false;
		}
		size_t prev_size = out_decompressed.size();
		out_decompressed.resize(prev_size + uncompressed_len);
		if (uncompressed_len > 0 && !snappy::RawUncompress(chunk_ptr, chunk_len, &out_decompressed[prev_size]))
		{
			out_decompressed.clear();
			if (err_msg)
				*err_msg = "Corrupt Snappy payload in IWA chunk";
			return false;
		}
		pos += chunk_len;
	}
	return true;
}

bool parseIwaObjectStream(const std::string& stream, std::vector<IwaMessageSlice>& out_slices)
{
	out_slices.clear();
	const unsigned char* data = reinterpret_cast<const unsigned char*>(stream.data());
	size_t len = stream.size();
	size_t pos = 0;
	while (pos < len)
	{
		uint64_t hdr_len = 0;
		if (!readVarint(data, len, pos, hdr_len))
			return false;
		if (hdr_len == 0 || hdr_len > len - pos)
			return false;
		uint64_t archive_id = 0;
		std::vector<MessageInfoMeta> metas;
		if (!parseArchiveInfo(data + pos, static_cast<size_t>(hdr_len), archive_id, metas))
			return false;
		pos += static_cast<size_t>(hdr_len);
		for (size_t i = 0; i < metas.size(); ++i)
		{
			if (metas[i].length > len - pos)
				return false;
			IwaMessageSlice slice;
			slice.archive_id = archive_id;
			slice.message_type = metas[i].type;
			slice.offset = pos;
			slice.length = static_cast<size_t>(metas[i].length);
			out_slices.push_back(slice);
			pos += slice.length;
		}
	}
	return true;
}

std::string extractTextFromIwaBufferForTest(const std::string& raw_iwa, bool* ok)
{
	if (ok)
		*ok = false;
	std::string decompressed;
	if (!decompressIwaStream(raw_iwa, decompressed))
		return "";
	std::vector<IwaMessageSlice> slices;
	if (!parseIwaObjectStream(decompressed, slices))
		return "";
	std::string out;
	const unsigned char* base = reinterpret_cast<const unsigned char*>(decompressed.data());
	for (size_t i = 0; i < slices.size(); ++i)
	{
		if (slices[i].message_type == kMsgStorageArchive)
		{
			std::vector<std::string> strs;
			extractTswpStrings(base + slices[i].offset, slices[i].length, strs);
			for (size_t k = 0; k < strs.size(); ++k)
				appendBlock(out, strs[k]);
		}
	}
	if (ok)
		*ok = true;
	return out;
}

bool isModernIWorkArchive(DocToTextUnzip& unzip)
{
	if (!unzip.loadDirectory())
		return false;
	std::vector<std::string> entries;
	if (!unzip.getFileList(entries))
		return false;
	std::string nested_index_zip;
	return hasDirectIwaEntry(entries) || findNestedIndexZipEntry(entries, nested_index_zip);
}

bool extractModernIWorkText(DocToTextUnzip& unzip,
                            bool verbose,
                            std::ostream& log_stream,
                            std::string& out_text,
                            bool& out_error)
{
	out_text.clear();
	out_error = false;
	if (!unzip.loadDirectory())
	{
		out_error = true;
		return false;
	}
	std::vector<std::string> entries;
	if (!unzip.getFileList(entries))
	{
		out_error = true;
		return false;
	}
	if (hasDirectIwaEntry(entries))
		return extractFromIndexZip(unzip, verbose, log_stream, out_text, out_error);

	std::string nested_index_zip;
	if (!findNestedIndexZipEntry(entries, nested_index_zip))
	{
		out_error = true;
		return false;
	}
	unsigned long zip_size = 0;
	if (!unzip.getFileSize(nested_index_zip, zip_size) || zip_size == 0 || zip_size > kMaxIndexZipBytes)
	{
		log_stream << "Nested iWork Index.zip is missing, empty, or exceeds size cap.\n";
		out_error = true;
		return false;
	}
	std::string index_zip_buf;
	if (!unzip.read(nested_index_zip, &index_zip_buf, static_cast<int>(kMaxIndexZipBytes + 1)) ||
	    index_zip_buf.size() > kMaxIndexZipBytes)
	{
		log_stream << "Failed reading nested iWork Index.zip or size exceeds cap.\n";
		out_error = true;
		return false;
	}
	DocToTextUnzip nested_unzip(index_zip_buf.data(), index_zip_buf.size());
	if (!nested_unzip.open())
	{
		log_stream << "Failed opening nested iWork Index.zip.\n";
		out_error = true;
		return false;
	}
	bool ok = extractFromIndexZip(nested_unzip, verbose, log_stream, out_text, out_error);
	nested_unzip.close();
	return ok;
}

} // namespace iwa
} // namespace doctotext
