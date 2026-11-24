#ifndef DOCTOTEXT_UNZIP_H
#define DOCTOTEXT_UNZIP_H

#include <string>
#include <vector>

class DocToTextUnzip
{
	private:
		struct Implementation;
		Implementation* Impl;

	public:
		DocToTextUnzip();
		DocToTextUnzip(const std::string& archive_file_name);
		DocToTextUnzip(const char* buffer, size_t size);
		void setArchiveFile(const std::string& archive_file_name);
		void setBuffer(const char* buffer, size_t size);
		~DocToTextUnzip();
		void setLogStream(std::ostream& log_stream);
		static void setUnzipCommand(const std::string& command);
		bool open();
		void close();
		bool exists(const std::string& file_name) const;
		bool read(const std::string& file_name, std::string* contents, int num_of_chars = 0) const;
		bool getFileSize(const std::string& file_name, unsigned long& file_size) const;
		bool readChunk(const std::string& file_name, std::string* contents, int chunk_size) const;
		bool readChunk(const std::string& file_name, char* contents, int chunk_size, int& readed) const;
		void closeReadingFileForChunks() const;
		/**
			Load and cache zip file directory. Speed up locating files dramatically. Use before multiple read() calls.
		**/
		bool loadDirectory();
		/// Populates `files` with all entry paths inside the opened zip archive.
		bool getFileList(std::vector<std::string>& files);
		/// Returns true if the given archive entry uses ZIP/iWork encryption flags or headers.
		bool isEncryptedEntry(const std::string& file_name) const;
		/// Returns true if any entry in the opened archive appears encrypted.
		bool hasEncryptedEntries() const;
};

#endif
