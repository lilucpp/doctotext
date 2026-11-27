#include "../build/doctotext_c_api.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	int call_count;
	char last_tag[128];
} TestImageCtx;

static const char* DOCTOTEXT_CALL test_image_callback(
	const char* image_name, const char* image_buffer, size_t image_size, void* user_data)
{
	static __thread char tls_buf[256];
	TestImageCtx* ctx = (TestImageCtx*)user_data;
	if (!image_buffer || image_size == 0)
		return "";
	if (ctx)
		ctx->call_count++;
	snprintf(tls_buf, sizeof(tls_buf), "[OCR_EMBED:%s:%zu]",
	         image_name ? image_name : "noname", image_size);
	if (ctx)
		snprintf(ctx->last_tag, sizeof(ctx->last_tag), "%s", tls_buf);
	return tls_buf;
}

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

static int check_contains_all(const char* label, const char* text, const char* const* expected, size_t count)
{
	if (!text)
	{
		fprintf(stderr, "FAIL [%s]: text is NULL\n", label);
		return 0;
	}
	for (size_t i = 0; i < count; ++i)
	{
		if (strstr(text, expected[i]) == NULL)
		{
			fprintf(stderr, "FAIL [%s]: missing expected substring '%s'\nActual output:\n%s\n",
			        label, expected[i], text);
			return 0;
		}
	}
	return 1;
}

static int check_excludes_all(const char* label, const char* text, const char* const* forbidden, size_t count)
{
	if (!text)
	{
		fprintf(stderr, "FAIL [%s]: text is NULL\n", label);
		return 0;
	}
	for (size_t i = 0; i < count; ++i)
	{
		if (strstr(text, forbidden[i]) != NULL)
		{
			fprintf(stderr, "FAIL [%s]: unexpected substring '%s'\nActual output:\n%s\n",
			        label, forbidden[i], text);
			return 0;
		}
	}
	return 1;
}

static int run_file_and_buffer_case(
	const char* rel_path,
	const char* const* expected_on, size_t expected_on_count,
	const char* const* forbidden_on, size_t forbidden_on_count,
	const char* const* expected_off, size_t expected_off_count,
	const char* const* forbidden_off, size_t forbidden_off_count,
	int use_image_callback)
{
	char* buf = NULL;
	size_t buf_size = 0;
	if (!read_entire_file(rel_path, &buf, &buf_size))
	{
		fprintf(stderr, "FAIL [%s]: cannot read file\n", rel_path);
		return 0;
	}

	/* 1. Default ON via file & buffer */
	for (int mode = 0; mode < 2; ++mode)
	{
		DocToTextExtractorParams* params = doctotext_create_extractor_params();
		TestImageCtx img_ctx;
		img_ctx.call_count = 0;
		img_ctx.last_tag[0] = '\0';
		if (use_image_callback)
		{
			doctotext_extractor_params_set_image_parser_callback(params, test_image_callback, &img_ctx);
			doctotext_extractor_params_set_parse_document_images(params, 1);
		}

		doctotext_status status = DOCTOTEXT_STATUS_ERROR;
		DocToTextException* exc = NULL;
		DocToTextExtractedData* data = (mode == 0)
			? doctotext_process_file_ex(rel_path, params, &status, &exc)
			: doctotext_process_file_from_buffer_ex(buf, buf_size, params, &status, &exc);

		doctotext_free_extractor_params(params);
		if (status != DOCTOTEXT_STATUS_OK || !data || exc != NULL)
		{
			fprintf(stderr, "FAIL [%s ON mode=%d]: status=%d data=%p exc=%p\n",
			        rel_path, mode, status, (void*)data, (void*)exc);
			if (exc) doctotext_free_exception(exc);
			if (data) doctotext_free_extracted_data(data);
			free(buf);
			return 0;
		}
		const char* text = doctotext_extracted_data_get_text(data);
		int ok = check_contains_all(rel_path, text, expected_on, expected_on_count);
		if (ok && forbidden_on_count > 0)
			ok = check_excludes_all(rel_path, text, forbidden_on, forbidden_on_count);
		if (ok && use_image_callback && img_ctx.call_count == 0)
		{
			fprintf(stderr, "FAIL [%s ON mode=%d]: expected image callback invocation\n", rel_path, mode);
			ok = 0;
		}
		doctotext_free_extracted_data(data);
		if (!ok)
		{
			free(buf);
			return 0;
		}
	}

	/* 2. Disabled OFF (calling set_parse_embedded_files(0) BEFORE set_formatting_style) via file & buffer */
	if (expected_off_count > 0 || forbidden_off_count > 0)
	{
		for (int mode = 0; mode < 2; ++mode)
		{
			DocToTextExtractorParams* params = doctotext_create_extractor_params();
			DocToTextFormattingStyle* fmt = doctotext_create_formatting_style();
			doctotext_extractor_params_set_parse_embedded_files(params, 0);
			doctotext_extractor_params_set_formatting_style(params, fmt);

			doctotext_status status = DOCTOTEXT_STATUS_ERROR;
			DocToTextException* exc = NULL;
			DocToTextExtractedData* data = (mode == 0)
				? doctotext_process_file_ex(rel_path, params, &status, &exc)
				: doctotext_process_file_from_buffer_ex(buf, buf_size, params, &status, &exc);

			doctotext_free_formatting_style(fmt);
			doctotext_free_extractor_params(params);
			if (status != DOCTOTEXT_STATUS_OK || !data || exc != NULL)
			{
				fprintf(stderr, "FAIL [%s OFF mode=%d]: status=%d data=%p exc=%p\n",
				        rel_path, mode, status, (void*)data, (void*)exc);
				if (exc) doctotext_free_exception(exc);
				if (data) doctotext_free_extracted_data(data);
				free(buf);
				return 0;
			}
			const char* text = doctotext_extracted_data_get_text(data);
			int ok = check_contains_all(rel_path, text, expected_off, expected_off_count);
			if (ok)
				ok = check_excludes_all(rel_path, text, forbidden_off, forbidden_off_count);
			doctotext_free_extracted_data(data);
			if (!ok)
			{
				free(buf);
				return 0;
			}
		}
	}

	free(buf);
	return 1;
}

typedef struct
{
	const char* path;
	const char* expected_sub;
	const char* forbidden_sub;
	int use_image_cb;
	int ok;
} ThreadJob;

static void* thread_worker(void* arg)
{
	ThreadJob* job = (ThreadJob*)arg;
	char* buf = NULL;
	size_t sz = 0;
	if (!read_entire_file(job->path, &buf, &sz))
	{
		job->ok = 0;
		return NULL;
	}
	TestImageCtx img_ctx;
	img_ctx.call_count = 0;
	img_ctx.last_tag[0] = '\0';

	DocToTextExtractorParams* params = doctotext_create_extractor_params();
	if (job->use_image_cb)
	{
		doctotext_extractor_params_set_image_parser_callback(params, test_image_callback, &img_ctx);
		doctotext_extractor_params_set_parse_document_images(params, 1);
	}

	doctotext_status status = DOCTOTEXT_STATUS_ERROR;
	DocToTextException* exc = NULL;
	DocToTextExtractedData* data = doctotext_process_file_from_buffer_ex(buf, sz, params, &status, &exc);
	doctotext_free_extractor_params(params);
	free(buf);

	if (status != DOCTOTEXT_STATUS_OK || !data || exc != NULL)
	{
		if (exc) doctotext_free_exception(exc);
		if (data) doctotext_free_extracted_data(data);
		job->ok = 0;
		return NULL;
	}
	const char* text = doctotext_extracted_data_get_text(data);
	int ok = (text && strstr(text, job->expected_sub) != NULL);
	if (ok && job->forbidden_sub && strstr(text, job->forbidden_sub) != NULL)
		ok = 0;
	if (ok && job->use_image_cb && img_ctx.call_count == 0)
		ok = 0;
	doctotext_free_extracted_data(data);
	job->ok = ok;
	return NULL;
}

int main(void)
{
	/* 1. DOCX: UTF-8 + UTF-16LE + UTF-16BE + GB18030 + PDF + XLSX + XLS + PPT + PNG OCR */
	{
		const char* on[] = {
			"DOCX_HOST_MAIN_BODY_中文主文档_Zażółć",
			"DOCX_OLE10_UTF8_中文_日本語_Тест",
			"DOCX_OLE10_UTF16LE_中文测试",
			"DOCX_OLE10_UTF16BE_日本語",
			"DOCX_OLE10_GB18030_这是一段简体中文字符集自动识别测试内容用于覆盖非Unicode编码文件",
			"DOCX_CONTENTS_PDF_OK_中文PDF",
			"DOCX_PACKAGE_XLSX_OK_嵌入表格中文",
			"DOCX_DIRECT_XLS_OK_工作簿中文",
			"DOCX_DIRECT_PPT_OK_幻灯片中文",
			"[OCR_EMBED:word/embeddings/obj9_img.png:"
		};
		const char* off_keep[] = {"DOCX_HOST_MAIN_BODY_中文主文档_Zażółć"};
		const char* off_drop[] = {"DOCX_OLE10_UTF8", "DOCX_CONTENTS_PDF_OK", "DOCX_PACKAGE_XLSX_OK", "DOCX_DIRECT_PPT_OK"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/host_docx_multi.docx",
		        on, sizeof(on) / sizeof(on[0]), NULL, 0,
		        off_keep, 1, off_drop, sizeof(off_drop) / sizeof(off_drop[0]), 1))
			return 1;
		printf("PASS: C API DOCX multi-encoding & multi-container\n");
	}

	/* 2. XLSX: Unicode + RTF + DOCX Japanese + PDF */
	{
		const char* on[] = {
			"XLSX_HOST_MAIN_CELL_主表格中文",
			"XLSX_OLE10_RTF_OK_中文Unicode",
			"XLSX_DIRECT_DOCX_OK_内嵌文档日本語",
			"XLSX_CONTENTS_PDF_OK_中文"
		};
		const char* off_keep[] = {"XLSX_HOST_MAIN_CELL_主表格中文"};
		const char* off_drop[] = {"XLSX_OLE10_RTF_OK", "XLSX_DIRECT_DOCX_OK", "XLSX_CONTENTS_PDF_OK"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/host_xlsx_multi.xlsx",
		        on, sizeof(on) / sizeof(on[0]), NULL, 0,
		        off_keep, 1, off_drop, sizeof(off_drop) / sizeof(off_drop[0]), 0))
			return 1;
		printf("PASS: C API XLSX multi-encoding\n");
	}

	/* 3. PPTX: Korean/CJK slide + XLSX + RTF */
	{
		const char* on[] = {
			"PPTX_HOST_MAIN_SLIDE_幻灯片中文_한국어",
			"PPTX_DIRECT_XLSX_OK_嵌入表格中文",
			"PPTX_OLE10_RTF_OK_中文RTF"
		};
		const char* off_keep[] = {"PPTX_HOST_MAIN_SLIDE_幻灯片中文_한국어"};
		const char* off_drop[] = {"PPTX_DIRECT_XLSX_OK", "PPTX_OLE10_RTF_OK"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/host_pptx_multi.pptx",
		        on, sizeof(on) / sizeof(on[0]), NULL, 0,
		        off_keep, 1, off_drop, sizeof(off_drop) / sizeof(off_drop[0]), 0))
			return 1;
		printf("PASS: C API PPTX multi-encoding\n");
	}

	/* 4. XLSB: main workbook + embedded PDF CJK */
	{
		const char* on[] = {"hyperlink test", "XLSB_EMBED_PDF_OK_二进制表中文"};
		const char* off_keep[] = {"hyperlink test"};
		const char* off_drop[] = {"XLSB_EMBED_PDF_OK"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/host_xlsb_multi.xlsb",
		        on, sizeof(on) / sizeof(on[0]), NULL, 0,
		        off_keep, 1, off_drop, sizeof(off_drop) / sizeof(off_drop[0]), 0))
			return 1;
		printf("PASS: C API XLSB embedded PDF\n");
	}

	/* 5. ODF: ODT / ODS / ODP */
	{
		const char* odf_cases[3][4] = {
			{"embedded_fixtures/host_odt_multi.odt", "ODT_HOST_MAIN_BODY_中文主文档", "ODT_OBJ1_XML_OK_中文公式文本", "ODT_REPLACEMENT_MUST_SKIP"},
			{"embedded_fixtures/host_ods_multi.ods", "ODS_HOST_MAIN_CELL_中文表格", "ODS_OBJ1_XML_OK_中文图表", "ODS_REPLACEMENT_MUST_SKIP"},
			{"embedded_fixtures/host_odp_multi.odp", "ODP_HOST_MAIN_SLIDE_中文演示", "ODP_OBJ1_XML_OK_中文备注", "ODP_REPLACEMENT_MUST_SKIP"},
		};
		for (int i = 0; i < 3; ++i)
		{
			const char* on[] = {odf_cases[i][1], odf_cases[i][2], "ODF_OBJ2_OLE_RTF_OK_中文对象"};
			const char* forb[] = {odf_cases[i][3]};
			const char* off_keep[] = {odf_cases[i][1]};
			const char* off_drop[] = {odf_cases[i][2], "ODF_OBJ2_OLE_RTF_OK", odf_cases[i][3]};
			if (!run_file_and_buffer_case(
			        odf_cases[i][0],
			        on, 3, forb, 1,
			        off_keep, 1, off_drop, 3, 0))
				return 1;
		}
		printf("PASS: C API ODT/ODS/ODP multi-encoding & ObjectReplacements filter\n");
	}

	/* 6. DOC: Real LibreOffice DOC + ObjectPool (XLS, PPT, PDF, RTF, DOCX) */
	{
		const char* on[] = {
			"DOC_HOST_MAIN_BODY_中文测试_Zażółć",
			"DOC_POOL_XLS_OK_工作簿中文",
			"DOC_POOL_PPT_OK_演示中文",
			"DOC_POOL_PDF_OK_嵌入PDF中文",
			"DOC_POOL_RTF_OK_中文RTF",
			"DOC_POOL_DOCX_OK_中文DOCX"
		};
		const char* off_keep[] = {"DOC_HOST_MAIN_BODY_中文测试_Zażółć"};
		const char* off_drop[] = {"DOC_POOL_XLS_OK", "DOC_POOL_PPT_OK", "DOC_POOL_PDF_OK", "DOC_POOL_RTF_OK", "DOC_POOL_DOCX_OK"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/host_doc_multi.doc",
		        on, sizeof(on) / sizeof(on[0]), NULL, 0,
		        off_keep, 1, off_drop, sizeof(off_drop) / sizeof(off_drop[0]), 0))
			return 1;
		printf("PASS: C API DOC ObjectPool multi-encoding\n");
	}

	/* 7. XLS: BIFF8 Unicode + MBD* (PDF, DOCX, PPT, DOC) + ObjectPool RTF */
	{
		const char* on[] = {
			"XLS_HOST_MAIN_CELL_主表格中文",
			"XLS_MBD_PDF_OK_中文",
			"XLS_MBD_DOCX_OK_中文",
			"XLS_MBD_PPT_OK_中文",
			"XLS_MBD_DIRECT_DOC_OK_内嵌中文文档",
			"XLS_POOL_RTF_OK_中文"
		};
		const char* off_keep[] = {"XLS_HOST_MAIN_CELL_主表格中文"};
		const char* off_drop[] = {"XLS_MBD_PDF_OK", "XLS_MBD_DOCX_OK", "XLS_MBD_PPT_OK", "XLS_MBD_DIRECT_DOC_OK", "XLS_POOL_RTF_OK"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/host_xls_multi.xls",
		        on, sizeof(on) / sizeof(on[0]), NULL, 0,
		        off_keep, 1, off_drop, sizeof(off_drop) / sizeof(off_drop[0]), 0))
			return 1;
		printf("PASS: C API XLS MBD*/ObjectPool (including direct DOC & PPT) multi-encoding\n");
	}

	/* 8. PPT: UTF-16 slide + raw/zlib ExOleObjStg + bomb skip + trailing slide */
	{
		const char* on[] = {
			"PPT_HOST_MAIN_SLIDE_幻灯片中文",
			"PPT_EXOLE_RAW_PDF_OK_中文",
			"PPT_EXOLE_ZLIB_DOCX_OK_中文",
			"PPT_AFTER_EXOLE_OK_尾页中文"
		};
		const char* off_keep[] = {"PPT_HOST_MAIN_SLIDE_幻灯片中文", "PPT_AFTER_EXOLE_OK_尾页中文"};
		const char* off_drop[] = {"PPT_EXOLE_RAW_PDF_OK", "PPT_EXOLE_ZLIB_DOCX_OK"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/host_ppt_multi.ppt",
		        on, sizeof(on) / sizeof(on[0]), NULL, 0,
		        off_keep, 2, off_drop, sizeof(off_drop) / sizeof(off_drop[0]), 0))
			return 1;
		printf("PASS: C API PPT ExOleObjStg multi-encoding\n");
	}

	/* 9. PDF: EmbeddedFiles (CJK filenames, DOCX, RTF, GB18030 TXT, PNG OCR) + FileAttachment annot */
	{
		const char* on[] = {
			"PDF_HOST_MAIN_TEXT_中文主文档",
			"PDF_EF_DOCX_OK_中文附件",
			"PDF_EF_RTF_OK_中文RTF",
			"PDF_EF_GB18030_这是一段简体中文PDF内嵌纯文本用于字符集检测验证",
			"PDF_ANNOT_XLSX_OK_中文批注",
			"[OCR_EMBED:att_pic.png:"
		};
		const char* off_keep[] = {"PDF_HOST_MAIN_TEXT_中文主文档"};
		const char* off_drop[] = {"PDF_EF_DOCX_OK", "PDF_EF_RTF_OK", "PDF_EF_GB18030", "PDF_ANNOT_XLSX_OK"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/host_pdf_multi.pdf",
		        on, sizeof(on) / sizeof(on[0]), NULL, 0,
		        off_keep, 1, off_drop, sizeof(off_drop) / sizeof(off_drop[0]), 1))
			return 1;
		printf("PASS: C API PDF EmbeddedFiles + FileAttachment + GB18030 + OCR callback\n");
	}

	/* 10. Boundaries: depth=4 mixed chain, count=20 with skipped zips/images, 65 MiB zip bomb, fault isolation */
	{
		const char* depth_on[] = {
			"DEPTH_L0_MARKER_零层中文",
			"DEPTH_L1_MARKER_第一层中文",
			"DEPTH_L2_MARKER_第二层中文",
			"DEPTH_L3_MARKER_第三层中文",
			"DEPTH_L4_MARKER_第四层中文"
		};
		const char* depth_forb[] = {"DEPTH_L5_SHOULD_STOP"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/boundary_depth_mixed.pdf",
		        depth_on, 5, depth_forb, 1,
		        NULL, 0, NULL, 0, 0))
			return 1;

		const char* count_on[] = {"COUNT_HOST_MAIN_计数测试", "COUNT_ITEM_01_序号", "COUNT_ITEM_20_序号"};
		const char* count_forb[] = {"SECRET_IN_ZIP", "COUNT_ITEM_21_序号", "COUNT_ITEM_25_序号"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/boundary_count_and_skip.pdf",
		        count_on, 3, count_forb, 3,
		        NULL, 0, NULL, 0, 0))
			return 1;

		const char* bomb_on[] = {"ZIP_BOMB_HOST_OK_防炸弹中文", "SURVIVED_AFTER_BOMB_OK_存活中文"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/boundary_zip_bomb.docx",
		        bomb_on, 2, NULL, 0,
		        NULL, 0, NULL, 0, 0))
			return 1;

		const char* fault_on[] = {"FAULT_HOST_OK_隔离测试", "VALID_AFTER_CORRUPT_OK_中文恢复"};
		const char* fault_forb[] = {"GARBAGE_NO_XREF", "SECRET_IN_ZIP"};
		if (!run_file_and_buffer_case(
		        "embedded_fixtures/boundary_fault_isolation.docx",
		        fault_on, 2, fault_forb, 2,
		        NULL, 0, NULL, 0, 0))
			return 1;
		printf("PASS: C API boundary guards (depth=4 mixed, count=20+skip, 65MiB zip bomb, fault isolation)\n");
	}

	/* 11. Encrypted documents keep DOCTOTEXT_STATUS_ENCRYPTED, while default-key VelvetSweatshop docs decrypt */
	{
		const char* velvet_std_on[] = { "VELVET_STD_XLSX_OK_标准AES128中文" };
		if (!run_file_and_buffer_case(
				"embedded_fixtures/velvet_standard.xlsx",
				velvet_std_on, 1, NULL, 0, NULL, 0, NULL, 0, 0))
			return 1;
		const char* velvet_agile_on[] = { "VELVET_AGILE_DOCX_OK_多加密器中文" };
		if (!run_file_and_buffer_case(
				"embedded_fixtures/velvet_agile_multi.docx",
				velvet_agile_on, 1, NULL, 0, NULL, 0, NULL, 0, 0))
			return 1;
		printf("PASS: C API MS-OFFCRYPTO VelvetSweatshop (Standard AES-128 & multi-encryptor Agile AES-256) file & buffer\n");

		const char* enc_files[] = {
			"encrypted/testWORD_protected_passtika.doc",
			"encrypted/testODTEncrypted.odt",
			"embedded_fixtures/locked_agile_userpw.xlsx"
		};
		for (size_t i = 0; i < sizeof(enc_files) / sizeof(enc_files[0]); ++i)
		{
			DocToTextExtractorParams* params = doctotext_create_extractor_params();
			doctotext_status status = DOCTOTEXT_STATUS_OK;
			DocToTextException* exc = NULL;
			DocToTextExtractedData* data = doctotext_process_file_ex(enc_files[i], params, &status, &exc);
			doctotext_free_extractor_params(params);
			if (data || status != DOCTOTEXT_STATUS_ENCRYPTED)
			{
				fprintf(stderr, "FAIL [%s]: expected DOCTOTEXT_STATUS_ENCRYPTED, got status=%d data=%p\n",
				        enc_files[i], status, (void*)data);
				if (data) doctotext_free_extracted_data(data);
				if (exc) doctotext_free_exception(exc);
				return 1;
			}
			if (exc) doctotext_free_exception(exc);
		}
		printf("PASS: C API encrypted status preserved\n");
	}

	/* 12. Concurrent C API across pthreads (mixed hosts + depth + OCR callbacks) */
	{
		ThreadJob jobs[8] = {
			{"embedded_fixtures/boundary_depth_mixed.pdf", "DEPTH_L4_MARKER_第四层中文", "DEPTH_L5_SHOULD_STOP", 0, 0},
			{"embedded_fixtures/host_docx_multi.docx", "DOCX_OLE10_GB18030_", NULL, 1, 0},
			{"embedded_fixtures/host_doc_multi.doc", "DOC_POOL_DOCX_OK_中文DOCX", NULL, 0, 0},
			{"embedded_fixtures/host_pdf_multi.pdf", "PDF_ANNOT_XLSX_OK_中文批注", NULL, 1, 0},
			{"embedded_fixtures/boundary_depth_mixed.pdf", "DEPTH_L4_MARKER_第四层中文", "DEPTH_L5_SHOULD_STOP", 0, 0},
			{"embedded_fixtures/host_xls_multi.xls", "XLS_MBD_DIRECT_DOC_OK_内嵌中文文档", NULL, 0, 0},
			{"embedded_fixtures/host_ppt_multi.ppt", "PPT_EXOLE_ZLIB_DOCX_OK_中文", NULL, 0, 0},
			{"embedded_fixtures/boundary_zip_bomb.docx", "SURVIVED_AFTER_BOMB_OK_存活中文", NULL, 0, 0},
		};
		pthread_t tids[8];
		for (int i = 0; i < 8; ++i)
			pthread_create(&tids[i], NULL, thread_worker, &jobs[i]);
		for (int i = 0; i < 8; ++i)
			pthread_join(tids[i], NULL);
		for (int i = 0; i < 8; ++i)
		{
			if (!jobs[i].ok)
			{
				fprintf(stderr, "FAIL: C API multithreaded job %d (%s)\n", i, jobs[i].path);
				return 1;
			}
		}
		printf("PASS: C API multithreaded TLS isolation (8 pthreads)\n");
	}

	/* 13. Independent switches: parse_document_images defaults OFF, parse_embedded_files defaults ON, standalone image only needs callback */
	{
		const char* doc_cases[] = {
			"embedded_fixtures/host_docx_multi.docx",
			"embedded_fixtures/host_pdf_multi.pdf"
		};
		for (size_t i = 0; i < sizeof(doc_cases) / sizeof(doc_cases[0]); ++i)
		{
			TestImageCtx img_ctx;
			img_ctx.call_count = 0;
			img_ctx.last_tag[0] = '\0';
			DocToTextExtractorParams* params = doctotext_create_extractor_params();
			doctotext_extractor_params_set_image_parser_callback(params, test_image_callback, &img_ctx);
			/* parse_document_images left at default (0), parse_embedded_files left at default (1) */
			doctotext_status status = DOCTOTEXT_STATUS_ERROR;
			DocToTextException* exc = NULL;
			DocToTextExtractedData* data = doctotext_process_file_ex(doc_cases[i], params, &status, &exc);
			doctotext_free_extractor_params(params);
			if (status != DOCTOTEXT_STATUS_OK || !data || exc != NULL || img_ctx.call_count != 0)
			{
				fprintf(stderr, "FAIL [default_doc_images_off %s]: status=%d calls=%d\n",
				        doc_cases[i], status, img_ctx.call_count);
				if (data) doctotext_free_extracted_data(data);
				if (exc) doctotext_free_exception(exc);
				return 1;
			}
			const char* text = doctotext_extracted_data_get_text(data);
			if (!text || strstr(text, "[OCR_EMBED:") != NULL)
			{
				fprintf(stderr, "FAIL [default_doc_images_off %s]: unexpected OCR output\n", doc_cases[i]);
				doctotext_free_extracted_data(data);
				return 1;
			}
			/* Embedded non-image documents still extracted because parse_embedded_files defaults ON */
			if (i == 0 && strstr(text, "DOCX_CONTENTS_PDF_OK") == NULL)
			{
				fprintf(stderr, "FAIL [default_embedded_files_on]: embedded PDF missing when doc images OFF\n");
				doctotext_free_extracted_data(data);
				return 1;
			}
			doctotext_free_extracted_data(data);
		}

		/* Standalone image succeeds when callback is set even with parse_document_images=0 */
		{
			const char png_magic[] = { (char)0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00 };
			TestImageCtx img_ctx;
			img_ctx.call_count = 0;
			img_ctx.last_tag[0] = '\0';
			DocToTextExtractorParams* params = doctotext_create_extractor_params();
			doctotext_extractor_params_set_image_parser_callback(params, test_image_callback, &img_ctx);
			doctotext_status status = DOCTOTEXT_STATUS_ERROR;
			DocToTextException* exc = NULL;
			DocToTextExtractedData* data = doctotext_process_file_from_buffer_ex(
				png_magic, sizeof(png_magic), params, &status, &exc);
			doctotext_free_extractor_params(params);
			if (status != DOCTOTEXT_STATUS_OK || !data || exc != NULL || img_ctx.call_count != 1)
			{
				fprintf(stderr, "FAIL [standalone_image_with_cb]: status=%d calls=%d\n", status, img_ctx.call_count);
				if (data) doctotext_free_extracted_data(data);
				if (exc) doctotext_free_exception(exc);
				return 1;
			}
			doctotext_free_extracted_data(data);
		}

		/* Override preservation: set_parse_document_images(1) before set_formatting_style() */
		{
			TestImageCtx img_ctx;
			img_ctx.call_count = 0;
			img_ctx.last_tag[0] = '\0';
			DocToTextExtractorParams* params = doctotext_create_extractor_params();
			DocToTextFormattingStyle* fmt = doctotext_create_formatting_style();
			doctotext_extractor_params_set_image_parser_callback(params, test_image_callback, &img_ctx);
			doctotext_extractor_params_set_parse_document_images(params, 1);
			doctotext_extractor_params_set_formatting_style(params, fmt);
			doctotext_status status = DOCTOTEXT_STATUS_ERROR;
			DocToTextException* exc = NULL;
			DocToTextExtractedData* data = doctotext_process_file_ex(
				"embedded_fixtures/host_docx_multi.docx", params, &status, &exc);
			doctotext_free_formatting_style(fmt);
			doctotext_free_extractor_params(params);
			if (status != DOCTOTEXT_STATUS_OK || !data || exc != NULL || img_ctx.call_count == 0)
			{
				fprintf(stderr, "FAIL [doc_images_override_before_fmt]: status=%d calls=%d\n",
				        status, img_ctx.call_count);
				if (data) doctotext_free_extracted_data(data);
				if (exc) doctotext_free_exception(exc);
				return 1;
			}
			doctotext_free_extracted_data(data);
		}
		printf("PASS: C API independent parse_document_images (default OFF) & standalone image callback\n");
	}

	printf("c_api_embedded_test PASSED.\n");
	return 0;
}
