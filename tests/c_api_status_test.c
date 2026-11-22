#include "../build/doctotext_c_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_entire_file(const char* path, char** out_buf, size_t* out_size)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fseek(f, 0, SEEK_END) != 0)
	{
		fclose(f);
		return 0;
	}
	long sz = ftell(f);
	if (sz < 0)
	{
		fclose(f);
		return 0;
	}
	rewind(f);
	char* buf = (char*)malloc((size_t)sz + 1);
	if (!buf)
	{
		fclose(f);
		return 0;
	}
	size_t n = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	if (n != (size_t)sz)
	{
		free(buf);
		return 0;
	}
	buf[sz] = '\0';
	*out_buf = buf;
	*out_size = (size_t)sz;
	return 1;
}

static int assert_ok_text(const char* label,
                          DocToTextExtractedData* data,
                          doctotext_status status,
                          DocToTextException* exception,
                          const char* expected_substr)
{
	if (status != DOCTOTEXT_STATUS_OK || !data || exception != NULL)
	{
		fprintf(stderr, "FAIL [%s]: expected OK with data (got status=%d, data=%p, exc=%p)\n",
		        label, status, (void*)data, (void*)exception);
		if (exception)
			doctotext_free_exception(exception);
		if (data)
			doctotext_free_extracted_data(data);
		return 0;
	}
	const char* text = doctotext_extracted_data_get_text(data);
	if (!text || strstr(text, expected_substr) == NULL)
	{
		fprintf(stderr, "FAIL [%s]: expected substring '%s', got '%s'\n",
		        label, expected_substr, text ? text : "(null)");
		doctotext_free_extracted_data(data);
		return 0;
	}
	doctotext_free_extracted_data(data);
	return 1;
}

static int assert_failure_status(const char* label,
                                 DocToTextExtractedData* data,
                                 doctotext_status status,
                                 doctotext_status expected_status,
                                 DocToTextException* exception,
                                 int require_exception)
{
	if (data != NULL || status != expected_status || (require_exception && exception == NULL))
	{
		fprintf(stderr, "FAIL [%s]: expected NULL with status=%d (got status=%d, data=%p, exc=%p)\n",
		        label, expected_status, status, (void*)data, (void*)exception);
		if (data)
			doctotext_free_extracted_data(data);
		if (exception)
			doctotext_free_exception(exception);
		return 0;
	}
	if (exception)
	{
		size_t count = doctotext_exception_error_messages_count(exception);
		if (count == 0)
		{
			fprintf(stderr, "FAIL [%s]: exception had 0 error messages\n", label);
			doctotext_free_exception(exception);
			return 0;
		}
		doctotext_free_exception(exception);
	}
	return 1;
}

int main(void)
{
	DocToTextExtractorParams* params = doctotext_create_extractor_params();
	DocToTextException* exc = NULL;
	doctotext_status status = DOCTOTEXT_STATUS_ERROR;
	DocToTextExtractedData* data = NULL;
	char* buf = NULL;
	size_t buf_size = 0;

	/* 1. Empty user password PDF (file & buffer) -> OK */
	data = doctotext_process_file_ex("testPDF_no_extract_no_accessibility_owner_empty.pdf", params, &status, &exc);
	if (!assert_ok_text("empty_pw_file", data, status, exc, "Hello World"))
		return 1;

	if (!read_entire_file("testPDF_no_extract_no_accessibility_owner_empty.pdf", &buf, &buf_size))
	{
		fprintf(stderr, "FAIL: could not read empty_pw buffer fixture\n");
		return 1;
	}
	exc = NULL;
	status = DOCTOTEXT_STATUS_ERROR;
	data = doctotext_process_file_from_buffer_ex(buf, buf_size, params, &status, &exc);
	free(buf);
	buf = NULL;
	if (!assert_ok_text("empty_pw_buffer", data, status, exc, "Hello World"))
		return 1;

	/* 2. Empty user password 41-page RC4 PDF -> OK */
	exc = NULL;
	status = DOCTOTEXT_STATUS_ERROR;
	data = doctotext_process_file_ex("testPDF_protected.pdf", params, &status, &exc);
	if (!assert_ok_text("protected_empty_pw_file", data, status, exc, "RETHINKING THE FINANCIAL NETWORK"))
		return 1;

	/* 3. Non-empty user password PDF (file, buffer, legacy API) -> DOCTOTEXT_STATUS_ENCRYPTED */
	exc = NULL;
	status = DOCTOTEXT_STATUS_OK;
	data = doctotext_process_file_ex("encrypted/testPDF_no_extract_no_accessibility_owner_user.pdf", params, &status, &exc);
	if (!assert_failure_status("user_pw_file", data, status, DOCTOTEXT_STATUS_ENCRYPTED, exc, 1))
		return 1;

	if (!read_entire_file("encrypted/testPDF_no_extract_no_accessibility_owner_user.pdf", &buf, &buf_size))
	{
		fprintf(stderr, "FAIL: could not read user_pw buffer fixture\n");
		return 1;
	}
	exc = NULL;
	status = DOCTOTEXT_STATUS_OK;
	data = doctotext_process_file_from_buffer_ex(buf, buf_size, params, &status, &exc);
	if (!assert_failure_status("user_pw_buffer", data, status, DOCTOTEXT_STATUS_ENCRYPTED, exc, 1))
	{
		free(buf);
		return 1;
	}

	exc = NULL;
	data = doctotext_process_file_from_buffer(buf, buf_size, params, &exc);
	free(buf);
	buf = NULL;
	if (data != NULL || exc == NULL)
	{
		fprintf(stderr, "FAIL [user_pw_legacy_buffer]: expected NULL + exception\n");
		return 1;
	}
	doctotext_free_exception(exc);

	/* 4. Invalid / nonexistent inputs -> DOCTOTEXT_STATUS_ERROR */
	exc = NULL;
	status = DOCTOTEXT_STATUS_OK;
	data = doctotext_process_file_ex("nonexistent_missing_file_for_status_test.pdf", params, &status, &exc);
	if (!assert_failure_status("missing_file_error", data, status, DOCTOTEXT_STATUS_ERROR, exc, 0))
		return 1;

	{
		const char bad_pdf[] = "%PDF-1.4\nnot valid objects\n";
		exc = NULL;
		status = DOCTOTEXT_STATUS_OK;
		data = doctotext_process_file_from_buffer_ex(bad_pdf, sizeof(bad_pdf) - 1, params, &status, &exc);
		if (!assert_failure_status("corrupt_pdf_buffer_error", data, status, DOCTOTEXT_STATUS_ERROR, exc, 0))
			return 1;
	}

	doctotext_free_extractor_params(params);
	printf("c_api_status_test PASSED.\n");
	return 0;
}
