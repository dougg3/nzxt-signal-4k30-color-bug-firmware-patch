/*
 * mcu_flash - command line utility to flash the MCU (MM32L073) firmware of an
 * NZXT Signal 4K30 capture card by calling CTITSDKDeviceTool.dll directly,
 * bypassing the updater EXE's "already up to date" version check, with an
 * optional patch to fix the DVI input color format bug in the IT68051 driver.
 *
 * Put mcu_flash.exe in the extracted NZXT updater folder, next to
 * CTITSDKDeviceTool.dll and setting.cfg, and run one of:
 *
 *     mcu_flash.exe NZXT4K30_MM32_APP_v0E.07.80.0202_20220615.bin
 *         flash the stock image unchanged
 *
 *     mcu_flash.exe --patch-dvi-rgb NZXT4K30_MM32_APP_v0E.07.80.0202_20220615.bin
 *         patch a copy in memory so DVI-mode sources are treated as RGB, then
 *         flash that copy
 *
 *     mcu_flash.exe --check <image.bin>
 *         verify without touching the card
 *
 * The input must be byte-identical to NZXT's stock 0E.07.80.0202 image; it is
 * checked by SHA-256 before anything else happens, and the patched result is
 * checked against its known SHA-256 as well.
 *
 * DLL signatures and structs were recovered by reverse engineering.
 *
 * Disclosure: AI (Claude Opus 5) wrote this code.
 */
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The size of NZXT4K30_MM32_APP_v0E.07.80.0202_20220615.bin */
#define IMAGE_SIZE 111772

/* SHA-256 of NZXT4K30_MM32_APP_v0E.07.80.0202_20220615.bin as shipped in
 * NZXT's updater package, and of the same image with the DVI/RGB patch. */
static const char STOCK_SHA256[] =
	"052913ba20d04a912b1e8696e28e6f0e16a271ec42c60da5f1adf0a7c4393e30";
static const char PATCHED_SHA256[] =
	"9967752df27d19596d974d54decbf130a19408d39e52e6450f90b13b4fc5e684";

/*
 * The DVI-mode branch of the IT68051 color setup (firmware 0x0800F8D4,
 * function iTE6805_Enable_Video_Output in the ITE driver) writes 0x10 into
 * register 0x6B bits 5:4, forcing the input color format to YUV422. DVI
 * carries only RGB, so the value should really be 0x00.
 *
 * Instruction bytes: "10 22" (movs r2, #16) -> "00 22" (movs r2, #0).
 */
#define PATCH_OFFSET 0xB8D4
static const uint8_t PATCH_OLD[2] = { 0x10, 0x22 };
static const uint8_t PATCH_NEW[2] = { 0x00, 0x22 };

/* One Scan_DeviceList entry: 0x20 bytes, four C strings. Select_Device
 * requires fields 1..3 to be non-NULL. Meanings are not fully known. */
typedef struct {
	char *field0;
	char *field1;
	char *field2;
	char *field3;
} ctit_device;

/* Struct passed to the firmware-update callback; freed by the DLL right after
 * the callback returns, so copy anything you want to keep. Any string may be
 * NULL. progress runs 0..100. */
typedef struct {
	char *field0;
	char *field1;
	char *state;
	char *message;
	float progress;
} ctit_fw_info;

typedef unsigned char (*cb_fw_t)(void *info);

typedef unsigned char (*init_t)(const wchar_t *cfg_path);
typedef unsigned char (*scan_t)(ctit_device **list, uint16_t *count);
typedef unsigned char (*free_list_t)(ctit_device **list, uint16_t *count);
typedef unsigned char (*select_t)(ctit_device *dev);
typedef char *(*get_str_t)(void);
typedef unsigned char (*free_str_t)(char **s);
typedef unsigned char (*set_bin_t)(const wchar_t *path);
typedef unsigned char (*void_bool_t)(void);
typedef unsigned char (*open_cb_t)(cb_fw_t cb);

static struct {
	init_t Init;
	scan_t Scan_DeviceList;
	free_list_t Free_DeviceList;
	select_t Select_Device;
	get_str_t Get_MCU_FW_Ver;
	get_str_t Get_MCU_BinFile_Ver;
	free_str_t Free_PeCHAR;
	set_bin_t Set_MCU_BinFile;
	void_bool_t Start_MCU_FwUpdate;
	void_bool_t Check_IsFirmwareUpdating;
	void_bool_t Check_Is_MCU_OnBoard;
	open_cb_t Open_CB_DevFwUpdate_StateInfo;
	void_bool_t Close_CB_DevFwUpdate_StateInfo;
} sdk;

static CRITICAL_SECTION result_lock;
static int saw_success;
static int saw_failure;

static const char *s(const char *p)
{
	return p ? p : "";
}

static unsigned char on_fw_update(void *p)
{
	ctit_fw_info *info = p;

	if (!info)
		return 1;
	printf("[%5.1f%%] %s %s\n", info->progress, s(info->state),
	       s(info->message));
	fflush(stdout);

	EnterCriticalSection(&result_lock);
	if (info->message && strstr(info->message, "has been updated"))
		saw_success = 1;
	if ((info->message && (strstr(info->message, "Error") ||
			       strstr(info->message, "failed"))) ||
	    (info->state && strstr(info->state, "Failed")))
		saw_failure = 1;
	LeaveCriticalSection(&result_lock);
	return 1;
}

static void *need(HMODULE dll, const char *name)
{
	FARPROC f = GetProcAddress(dll, name);

	if (!f) {
		fprintf(stderr, "DLL is missing export %s\n", name);
		ExitProcess(1);
	}
	return (void *)f;
}

static void print_and_free(const char *label, char *str)
{
	printf("%s%s\n", label, s(str));
	if (str)
		sdk.Free_PeCHAR(&str);
}

/* Major field of a version string like "0e.07.80.0202ff" (the product ID). */
static long major_of(const char *ver)
{
	return ver ? strtol(ver, NULL, 16) : -1;
}

/* Hex SHA-256 of buf into out (65 bytes). Returns 0 on success. */
static int sha256_hex(const uint8_t *buf, DWORD len, char *out)
{
	BCRYPT_ALG_HANDLE alg = NULL;
	uint8_t digest[32];
	int ret = -1;

	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL,
					0) != 0)
		return -1;
	if (BCryptHash(alg, NULL, 0, (PUCHAR)buf, len, digest,
		       sizeof(digest)) == 0) {
		for (int i = 0; i < 32; i++)
			sprintf(out + 2 * i, "%02x", digest[i]);
		ret = 0;
	}
	BCryptCloseAlgorithmProvider(alg, 0);
	return ret;
}

/* Read the whole file at path into a new buffer. Returns NULL on failure. */
static uint8_t *read_file(const wchar_t *path, DWORD *len)
{
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
			       OPEN_EXISTING, 0, NULL);
	LARGE_INTEGER size;
	uint8_t *buf = NULL;
	DWORD got;

	if (h == INVALID_HANDLE_VALUE)
		return NULL;
	if (!GetFileSizeEx(h, &size) || size.QuadPart > 16 * 1024 * 1024)
		goto out;
	buf = malloc(size.QuadPart ? size.QuadPart : 1);
	if (!buf)
		goto out;
	if (!ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) ||
	    got != size.QuadPart) {
		free(buf);
		buf = NULL;
		goto out;
	}
	*len = got;
out:
	CloseHandle(h);
	return buf;
}

static int write_file(const wchar_t *path, const uint8_t *buf, DWORD len)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			       FILE_ATTRIBUTE_TEMPORARY, NULL);
	DWORD put;
	int ok;

	if (h == INVALID_HANDLE_VALUE)
		return -1;
	ok = WriteFile(h, buf, len, &put, NULL) && put == len;
	ok = CloseHandle(h) && ok;
	return ok ? 0 : -1;
}

/*
 * Check that image is NZXT's stock 0E.07.80.0202 image and, if patch is set,
 * apply the DVI/RGB patch in place and check the result. Returns 0 if the
 * buffer is ready to flash.
 */
static int prepare_image(uint8_t *image, DWORD len, int patch)
{
	char hash[65];

	if (sha256_hex(image, len, hash)) {
		fprintf(stderr, "cannot compute SHA-256\n");
		return -1;
	}
	printf("input SHA-256:   %s\n", hash);
	if (strcmp(hash, STOCK_SHA256) != 0) {
		if (strcmp(hash, PATCHED_SHA256) == 0)
			fprintf(stderr, "this image already has the DVI/RGB "
					"patch applied; pass NZXT's original "
					"image instead\n");
		else
			fprintf(stderr, "this is not NZXT's stock "
					"NZXT4K30_MM32_APP_v0E.07.80.0202_"
					"20220615.bin (expected SHA-256 %s); "
					"refusing\n", STOCK_SHA256);
		return -1;
	}
	printf("input matches the stock 4K30 MCU image 0E.07.80.0202\n");
	if (!patch)
		return 0;

	/* Unreachable with a matching hash, but never write blind. */
	if (len != IMAGE_SIZE ||
	    memcmp(image + PATCH_OFFSET, PATCH_OLD, sizeof(PATCH_OLD)) != 0) {
		fprintf(stderr, "unexpected bytes at patch offset; refusing\n");
		return -1;
	}
	memcpy(image + PATCH_OFFSET, PATCH_NEW, sizeof(PATCH_NEW));
	if (sha256_hex(image, len, hash) || strcmp(hash, PATCHED_SHA256) != 0) {
		fprintf(stderr, "patched image has an unexpected SHA-256; "
				"refusing\n");
		return -1;
	}
	printf("patched SHA-256: %s\n", hash);
	printf("applied DVI/RGB patch at file offset 0x%X\n", PATCH_OFFSET);
	return 0;
}

static void usage(const wchar_t *argv0)
{
	fwprintf(stderr,
		 L"usage: %ls [--check] [--patch-dvi-rgb] <stock-image.bin>\n"
		 L"\n"
		 L"  <stock-image.bin>  NZXT4K30_MM32_APP_v0E.07.80.0202_20220615.bin\n"
		 L"                     from NZXT's updater package (checked by SHA-256)\n"
		 L"  --patch-dvi-rgb    treat DVI-mode sources as RGB instead of YUV422\n"
		 L"                     (fixes pink/green capture from such sources)\n"
		 L"  --check            verify the image and exit without flashing\n",
		 argv0);
}

int wmain(int argc, wchar_t **argv)
{
	wchar_t dir[MAX_PATH], cfg[MAX_PATH], bin[MAX_PATH], flash_path[MAX_PATH];
	wchar_t tmpdir[MAX_PATH], *slash;
	const wchar_t *input = NULL;
	ctit_device *list = NULL;
	uint16_t count = 0;
	char *dev_ver, *bin_ver;
	long dev_major, bin_major;
	int idx = 0, ok = 0, patch = 0, check_only = 0, temp_written = 0;
	char answer[16];
	uint8_t *image;
	DWORD image_len = 0;
	HMODULE dll;

	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--patch-dvi-rgb")) {
			patch = 1;
		} else if (!wcscmp(argv[i], L"--check")) {
			check_only = 1;
		} else if (argv[i][0] == L'-' || input) {
			usage(argv[0]);
			return 2;
		} else {
			input = argv[i];
		}
	}
	if (!input) {
		usage(argv[0]);
		return 2;
	}
	if (!GetFullPathNameW(input, MAX_PATH, bin, NULL)) {
		fwprintf(stderr, L"bad path %ls\n", input);
		return 2;
	}
	image = read_file(bin, &image_len);
	if (!image) {
		fwprintf(stderr, L"cannot read %ls\n", bin);
		return 2;
	}
	fwprintf(stdout, L"image: %ls\n", bin);
	if (prepare_image(image, image_len, patch))
		return 1;
	if (check_only) {
		printf("check only; not flashing\n");
		return 0;
	}

	/*
	 * The DLL takes a path, not a buffer, and reopens the file when the
	 * update starts, so flash from a private copy of the verified bytes.
	 * That also means a file swapped on disk after the check can't be
	 * flashed.
	 */
	if (!GetTempPathW(MAX_PATH, tmpdir) ||
	    !GetTempFileNameW(tmpdir, L"mcu", 0, flash_path) ||
	    write_file(flash_path, image, image_len)) {
		fprintf(stderr, "cannot write temporary image file\n");
		return 1;
	}
	temp_written = 1;

	/* The DLL, setting.cfg and this program live in the same folder. */
	GetModuleFileNameW(NULL, dir, MAX_PATH);
	slash = wcsrchr(dir, L'\\');
	if (slash)
		slash[1] = 0;
	swprintf(cfg, MAX_PATH, L"%lssetting.cfg", dir);
	SetDllDirectoryW(dir);

	dll = LoadLibraryW(L"CTITSDKDeviceTool.dll");
	if (!dll) {
		fprintf(stderr, "cannot load CTITSDKDeviceTool.dll (error %lu); "
				"put mcu_flash.exe in the NZXT updater "
				"folder\n", GetLastError());
		goto out;
	}
	sdk.Init = need(dll, "CTITSDK_Init");
	sdk.Scan_DeviceList = need(dll, "CTITSDK_Scan_DeviceList");
	sdk.Free_DeviceList = need(dll, "CTITSDK_Free_DeviceList");
	sdk.Select_Device = need(dll, "CTITSDK_Select_Device");
	sdk.Get_MCU_FW_Ver = need(dll, "CTITSDK_Get_MCU_FW_Ver");
	sdk.Get_MCU_BinFile_Ver = need(dll, "CTITSDK_Get_MCU_BinFile_Ver");
	sdk.Free_PeCHAR = need(dll, "CTITSDK_Free_PeCHAR");
	sdk.Set_MCU_BinFile = need(dll, "CTITSDK_Set_MCU_BinFile");
	sdk.Start_MCU_FwUpdate = need(dll, "CTITSDK_Start_MCU_FwUpdate");
	sdk.Check_IsFirmwareUpdating =
		need(dll, "CTITSDK_Check_IsFirmwareUpdating");
	sdk.Check_Is_MCU_OnBoard = need(dll, "CTITSDK_Check_Is_MCU_OnBoard");
	sdk.Open_CB_DevFwUpdate_StateInfo =
		need(dll, "CTITSDK_Open_CB_DevFwUpdate_StateInfo");
	sdk.Close_CB_DevFwUpdate_StateInfo =
		need(dll, "CTITSDK_Close_CB_DevFwUpdate_StateInfo");

	InitializeCriticalSection(&result_lock);

	if (GetFileAttributesW(cfg) == INVALID_FILE_ATTRIBUTES)
		fwprintf(stderr, L"warning: %ls not found; FW_Overwrite will "
				 L"default and a same-version flash may be "
				 L"refused\n", cfg);
	if (!sdk.Init(cfg)) {
		fprintf(stderr, "CTITSDK_Init failed\n");
		goto out;
	}
	sdk.Open_CB_DevFwUpdate_StateInfo(on_fw_update);

	if (!sdk.Scan_DeviceList(&list, &count) || count == 0) {
		fprintf(stderr, "no capture card found\n");
		goto out;
	}
	for (int i = 0; i < count; i++)
		printf("device %d: %s | %s | %s | %s\n", i, s(list[i].field0),
		       s(list[i].field1), s(list[i].field2), s(list[i].field3));
	if (count > 1) {
		printf("select device index: ");
		fflush(stdout);
		if (!fgets(answer, sizeof(answer), stdin))
			goto out;
		idx = atoi(answer);
		if (idx < 0 || idx >= count) {
			fprintf(stderr, "bad index\n");
			goto out;
		}
	}
	if (!sdk.Select_Device(&list[idx])) {
		fprintf(stderr, "CTITSDK_Select_Device failed\n");
		goto out;
	}
	if (!sdk.Check_Is_MCU_OnBoard()) {
		fprintf(stderr, "the SDK reports no MCU on this device\n");
		goto out;
	}

	if (!sdk.Set_MCU_BinFile(flash_path)) {
		fprintf(stderr, "CTITSDK_Set_MCU_BinFile rejected the image "
				"(wrong product, too big, or refused by "
				"FW_Overwrite)\n");
		goto out;
	}

	dev_ver = sdk.Get_MCU_FW_Ver();
	bin_ver = sdk.Get_MCU_BinFile_Ver();
	dev_major = major_of(dev_ver);
	bin_major = major_of(bin_ver);
	print_and_free("card MCU version:   ", dev_ver);
	print_and_free("image MCU version:  ", bin_ver);
	printf("flashing:           %s image\n",
	       patch ? "PATCHED (DVI/RGB)" : "stock");

	/* Just to be sure: never flash the 4K30 image onto another product,
	 * e.g. an HD60. */
	if (dev_major < 0 || dev_major != bin_major || dev_major == 0xff) {
		fprintf(stderr, "product ID mismatch or unknown card version "
				"(card %lx, image %lx); refusing\n",
			dev_major, bin_major);
		goto out;
	}

	printf("\nDo not unplug the card while flashing.\n"
	       "Type YES to flash: ");
	fflush(stdout);
	if (!fgets(answer, sizeof(answer), stdin) ||
	    strncmp(answer, "YES", 3) != 0) {
		printf("aborted\n");
		goto out;
	}

	if (!sdk.Start_MCU_FwUpdate()) {
		fprintf(stderr, "CTITSDK_Start_MCU_FwUpdate failed to start\n");
		goto out;
	}
	/* The update runs on a DLL worker thread; progress arrives through
	 * on_fw_update. */
	while (sdk.Check_IsFirmwareUpdating())
		Sleep(200);
	Sleep(500);

	sdk.Close_CB_DevFwUpdate_StateInfo();
	sdk.Free_DeviceList(&list, &count);

	EnterCriticalSection(&result_lock);
	ok = saw_success && !saw_failure;
	LeaveCriticalSection(&result_lock);
	if (ok)
		printf("\nDone. Unplug and re-plug the card.\n");
	else
		printf("\nThe SDK did not report success; see the messages "
		       "above. Do not power-cycle the card until you have read "
		       "them.\n");
out:
	if (temp_written)
		DeleteFileW(flash_path);
	free(image);
	return ok ? 0 : 1;
}
