/*
**	Command & Conquer Generals Zero Hour(tm)
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "PreRTS.h"

#include "Common/StatsUploader.h"
#include "Common/AsciiString.h"
#include "Common/crc.h"
#include "Common/FileSystem.h"
#include "Common/File.h"
#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "Common/ReleaseLog.h"
#include "Common/version.h"
#include "GameClient/MapUtil.h"
#include "GameNetwork/FileTransfer.h"
#include "ZuluClientKey.h"
#include "CncStatsClientKey.h"
#include "BuildVariant.h"

#include <windows.h>
#include <wininet.h>
#include <process.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>
#include <map>
#include <vector>

#pragma comment(lib, "wininet.lib")

// VC6's bundled Platform SDK predates iphlpapi.h and the
// GetSystemFirmwareTable declaration in winbase.h, so we resolve both
// APIs at runtime via LoadLibrary/GetProcAddress and forward-declare just
// the structs we touch. These layouts have been stable across every
// Windows SDK that ships them (Win2000+ for iphlpapi, XP SP2+ for
// GetSystemFirmwareTable).

#define LOCAL_MAX_ADAPTER_NAME_LENGTH        256
#define LOCAL_MAX_ADAPTER_DESCRIPTION_LENGTH 128
#define LOCAL_MAX_ADAPTER_ADDRESS_LENGTH     8

struct LocalIpAddressString
{
	char String[16];
};

struct LocalIpAddrString
{
	LocalIpAddrString *Next;
	LocalIpAddressString IpAddress;
	LocalIpAddressString IpMask;
	DWORD Context;
};

struct LocalIpAdapterInfo
{
	LocalIpAdapterInfo *Next;
	DWORD ComboIndex;
	char AdapterName[LOCAL_MAX_ADAPTER_NAME_LENGTH + 4];
	char Description[LOCAL_MAX_ADAPTER_DESCRIPTION_LENGTH + 4];
	UINT AddressLength;
	BYTE Address[LOCAL_MAX_ADAPTER_ADDRESS_LENGTH];
	DWORD Index;
	UINT Type;
	UINT DhcpEnabled;
	LocalIpAddrString *CurrentIpAddress;
	LocalIpAddrString IpAddressList;
	LocalIpAddrString GatewayList;
	LocalIpAddrString DhcpServer;
	BOOL HaveWins;
	LocalIpAddrString PrimaryWinsServer;
	LocalIpAddrString SecondaryWinsServer;
	DWORD LeaseObtained;
	DWORD LeaseExpires;
};

typedef DWORD (WINAPI *FnGetAdaptersInfo)(LocalIpAdapterInfo *, ULONG *);
typedef UINT (WINAPI *FnGetSystemFirmwareTable)(DWORD, DWORD, PVOID, DWORD);

// Internal: open a WinINet request handle for either a GET or POST.
// Returns nullptr on any failure (logs to stdout). On success the caller
// owns hInternet/hConnect/hRequest and must close them in reverse order.
struct WinInetSession
{
	HINTERNET hInternet;
	HINTERNET hConnect;
	HINTERNET hRequest;
};

static bool openHttpRequest(const AsciiString& url,
                            const char *method,
                            const char *pathOverride,
                            const char *logTag,
                            WinInetSession *out)
{
	out->hInternet = nullptr;
	out->hConnect = nullptr;
	out->hRequest = nullptr;

	if (url.isEmpty())
		return false;

	char hostBuf[256];
	char pathBuf[1024];
	URL_COMPONENTSA uc;
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = hostBuf;
	uc.dwHostNameLength = sizeof(hostBuf);
	uc.lpszUrlPath = pathBuf;
	uc.dwUrlPathLength = sizeof(pathBuf);

	if (!InternetCrackUrlA(url.str(), 0, 0, &uc))
	{
		printf("%s: failed to parse URL \"%s\"\n", logTag, url.str());
		ReleaseLog("%s: failed to parse URL \"%s\"", logTag, url.str());
		return false;
	}

	INTERNET_PORT port = uc.nPort;
	if (port == 0)
		port = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

	DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
	if (uc.nScheme == INTERNET_SCHEME_HTTPS)
		flags |= INTERNET_FLAG_SECURE;

	out->hInternet = InternetOpenA("GeneralsStatsExporter/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (out->hInternet == nullptr)
	{
		DWORD err = GetLastError();
		printf("%s: InternetOpen failed (%lu)\n", logTag, err);
		ReleaseLog("%s: InternetOpen failed (%lu)", logTag, err);
		return false;
	}

	out->hConnect = InternetConnectA(out->hInternet, hostBuf, port, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
	if (out->hConnect == nullptr)
	{
		DWORD err = GetLastError();
		printf("%s: InternetConnect failed (%lu)\n", logTag, err);
		ReleaseLog("%s: InternetConnect failed (%lu)", logTag, err);
		InternetCloseHandle(out->hInternet);
		out->hInternet = nullptr;
		return false;
	}

	const char *requestPath = (pathOverride != nullptr) ? pathOverride : pathBuf;
	out->hRequest = HttpOpenRequestA(out->hConnect, method, requestPath, nullptr, nullptr, nullptr, flags, 0);
	if (out->hRequest == nullptr)
	{
		DWORD err = GetLastError();
		printf("%s: HttpOpenRequest failed (%lu)\n", logTag, err);
		ReleaseLog("%s: HttpOpenRequest failed (%lu)", logTag, err);
		InternetCloseHandle(out->hConnect);
		InternetCloseHandle(out->hInternet);
		out->hConnect = nullptr;
		out->hInternet = nullptr;
		return false;
	}

	// Pick the auth scheme by host: cncstats uses Authorization: Bearer
	// with its own key (CNCSTATS_ZULU_CLIENT_KEY from gcloud secret
	// cncstats_zuluclientkey, baked in by cmake/cncstatsclientkey.cmake);
	// everything else (radarvan) gets X-API-Key with ZULU_CLIENT_KEY (gcloud
	// secret zuluclientkey, baked in by cmake/zuluclientkey.cmake).
	// Configure fails if either secret is missing, so both macros are
	// guaranteed non-empty here.
	{
		const bool isCncStats = (strstr(hostBuf, "cncstats") != nullptr);
		char authHeader[512];
		int authLen;
		if (isCncStats)
			authLen = sprintf(authHeader, "Authorization: Bearer %s\r\n", CNCSTATS_ZULU_CLIENT_KEY);
		else
			authLen = sprintf(authHeader, "X-API-Key: %s\r\n", ZULU_CLIENT_KEY);
		if (authLen > 0)
			HttpAddRequestHeadersA(out->hRequest, authHeader, (DWORD)authLen,
				HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
	}

	// Tag every outbound request with the build variant ("dev-a1b2c3d" /
	// "release-a1b2c3d") so radarvan and cncstats can filter dev traffic
	// out of their dashboards. Baked in at configure time by
	// cmake/buildvariant.cmake; defaults to "dev" so unsigned local
	// builds are auto-tagged.
	{
		char variantHeader[256];
		int variantLen = sprintf(variantHeader, "X-Zulu-Build: %s\r\n", ZULU_BUILD_VARIANT_TAG);
		if (variantLen > 0)
			HttpAddRequestHeadersA(out->hRequest, variantHeader, (DWORD)variantLen,
				HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
	}

	return true;
}

static void closeHttpRequest(WinInetSession *s)
{
	if (s->hRequest)  InternetCloseHandle(s->hRequest);
	if (s->hConnect)  InternetCloseHandle(s->hConnect);
	if (s->hInternet) InternetCloseHandle(s->hInternet);
}

// Shared HTTP POST, X-Game-Seed given as an already-formatted string. Match
// telemetry passes the numeric game seed (see httpPostBytes); uploads that
// have no match to key off - the coordinator connection-failure logs - pass
// a bucket label instead. Best-effort; logs status to stdout.
static void httpPostBytesSeedStr(const AsciiString& url,
                                 const void *data,
                                 unsigned int dataLen,
                                 const char *contentType,
                                 const char *extraHeaders,
                                 const char *seedStr,
                                 const char *logTag)
{
	if (data == nullptr || dataLen == 0)
		return;

	WinInetSession s;
	if (!openHttpRequest(url, "POST", nullptr, logTag, &s))
		return;

	char headers[1024];
	int n = sprintf(headers, "Content-Type: %s\r\nX-Game-Seed: %.63s\r\n", contentType,
		(seedStr != nullptr) ? seedStr : "0");
	if (extraHeaders != nullptr && extraHeaders[0] != '\0' && n < (int)sizeof(headers))
	{
		// Caller-provided extra headers (already terminated with \r\n).
		strncat(headers, extraHeaders, sizeof(headers) - 1 - (size_t)n);
	}

	BOOL result = HttpSendRequestA(s.hRequest, headers, (DWORD)strlen(headers), const_cast<void*>(data), dataLen);

	// Outcomes also go to the ReleaseLog so they survive on disk and ride up
	// with the next match's log upload (stdout is lost on players' machines).
	// Safe from the telemetry worker thread: the log file is already open by
	// match end and ReleaseLog is a single append+flush per call.
	if (result)
	{
		DWORD statusCode = 0;
		DWORD statusSize = sizeof(statusCode);
		HttpQueryInfoA(s.hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusSize, nullptr);
		printf("%s: %s -> %lu\n", logTag, url.str(), statusCode);
		ReleaseLog("%s: %u bytes to %s -> %lu", logTag, dataLen, url.str(), statusCode);
	}
	else
	{
		DWORD err = GetLastError();
		printf("%s: HttpSendRequest failed (%lu)\n", logTag, err);
		ReleaseLog("%s: %u bytes to %s -> HttpSendRequest failed (%lu)", logTag, dataLen, url.str(), err);
	}

	closeHttpRequest(&s);
}

// Numeric-seed form: every match-telemetry channel goes through here.
static void httpPostBytes(const AsciiString& url,
                          const void *data,
                          unsigned int dataLen,
                          const char *contentType,
                          const char *extraHeaders,
                          unsigned int seed,
                          const char *logTag)
{
	char seedStr[32];
	sprintf(seedStr, "%u", seed);
	httpPostBytesSeedStr(url, data, dataLen, contentType, extraHeaders, seedStr, logTag);
}

void UploadStatsToServer(const AsciiString& url, const void *data, unsigned int dataLen, unsigned int seed)
{
	httpPostBytes(url, data, dataLen, "application/gzip", nullptr, seed, "Stats upload");
}

// Sanitize a filename for the multipart Content-Disposition header. Strips
// any path components (forward or back slashes), and any double-quote /
// CR / LF that would break the header. Output is bounded by outCap.
static void sanitizeMultipartFilename(const char *src, char *out, unsigned int outCap)
{
	if (outCap == 0)
		return;
	if (src == nullptr) src = "";
	// VC6 leaks for-loop variables into the enclosing scope, so declare
	// the iterator once and reuse it across the two scans.
	const char *p;
	// Skip to last path separator
	const char *base = src;
	for (p = src; *p != '\0'; ++p)
	{
		if (*p == '/' || *p == '\\')
			base = p + 1;
	}
	if (*base == '\0')
		base = "replay.rep";
	unsigned int n = 0;
	for (p = base; *p != '\0' && n + 1 < outCap; ++p)
	{
		char c = *p;
		if (c == '"' || c == '\r' || c == '\n')
			c = '_';
		out[n++] = c;
	}
	out[n] = '\0';
}

// One text part (form field) in a multipart/form-data body. Empty `value`
// signals "omit this field"; the caller is expected to skip it. The `value`
// bytes are emitted verbatim, so non-ASCII (e.g. UTF-8 player names) is the
// caller's responsibility.
struct MultipartTextField
{
	const char *name;
	AsciiString value;
};

// Issue an HTTP POST as multipart/form-data with one binary part plus any
// number of text parts. Skips any text field whose value is empty so we
// don't bake a sentinel into the form.
static void httpPostMultipartFileSeedStr(const AsciiString& url,
                                         const char *fieldName,
                                         const char *filename,
                                         const void *data,
                                         unsigned int dataLen,
                                         const MultipartTextField *textFields,
                                         unsigned int textFieldCount,
                                         const char *extraHeaders,
                                         const char *seedStr,
                                         const char *logTag)
{
	if (data == nullptr || dataLen == 0)
		return;

	// Boundary: long enough that random collision with binary file bytes
	// is negligible. Must not appear inside the file payload preceded by
	// "\r\n--".
	static const char boundary[] = "----GeneralsReplayBoundaryK8nQv2pXr9TfH3";

	// Build the text-field block first so we know its size up front. Each
	// part is "--<boundary>\r\nContent-Disposition: form-data; name=\"X\"\r\n\r\n<value>\r\n".
	AsciiString textBlock;
	unsigned int ti;
	for (ti = 0; ti < textFieldCount; ++ti)
	{
		const MultipartTextField &tf = textFields[ti];
		if (tf.name == nullptr || tf.name[0] == '\0' || tf.value.isEmpty())
			continue;
		char header[256];
		sprintf(header,
			"--%s\r\n"
			"Content-Disposition: form-data; name=\"%.63s\"\r\n"
			"\r\n",
			boundary, tf.name);
		textBlock.concat(header);
		textBlock.concat(tf.value);
		textBlock.concat("\r\n");
	}

	char filePrefix[512];
	int filePrefixLen = sprintf(filePrefix,
		"--%s\r\n"
		"Content-Disposition: form-data; name=\"%.63s\"; filename=\"%.255s\"\r\n"
		"Content-Type: application/octet-stream\r\n"
		"\r\n",
		boundary, fieldName, filename);

	char trailer[64];
	int trailerLen = sprintf(trailer, "\r\n--%s--\r\n", boundary);

	if (filePrefixLen <= 0 || trailerLen <= 0)
		return;

	unsigned int textLen = (unsigned int)textBlock.getLength();
	unsigned int bodyLen = textLen + (unsigned int)filePrefixLen + dataLen + (unsigned int)trailerLen;
	char *body = (char *)malloc(bodyLen);
	if (body == nullptr)
		return;
	unsigned int pos = 0;
	if (textLen > 0)
	{
		memcpy(body, textBlock.str(), (size_t)textLen);
		pos += textLen;
	}
	memcpy(body + pos, filePrefix, (size_t)filePrefixLen);
	pos += (unsigned int)filePrefixLen;
	memcpy(body + pos, data, dataLen);
	pos += dataLen;
	memcpy(body + pos, trailer, (size_t)trailerLen);

	char contentType[128];
	sprintf(contentType, "multipart/form-data; boundary=%s", boundary);

	httpPostBytesSeedStr(url, body, bodyLen, contentType, extraHeaders, seedStr, logTag);

	free(body);
}

// Numeric-seed form (match telemetry); see httpPostBytesSeedStr.
static void httpPostMultipartFile(const AsciiString& url,
                                  const char *fieldName,
                                  const char *filename,
                                  const void *data,
                                  unsigned int dataLen,
                                  const MultipartTextField *textFields,
                                  unsigned int textFieldCount,
                                  const char *extraHeaders,
                                  unsigned int seed,
                                  const char *logTag)
{
	char seedStr[32];
	sprintf(seedStr, "%u", seed);
	httpPostMultipartFileSeedStr(url, fieldName, filename, data, dataLen,
		textFields, textFieldCount, extraHeaders, seedStr, logTag);
}

// ---------------------------------------------------------------------------
// Per-machine identifiers: MAC of the LAN-IP-tied adapter, and SMBIOS UUID.
// Both are best-effort and quietly produce an empty string on failure so the
// caller can just omit the form field.
// ---------------------------------------------------------------------------

// Format the first six bytes of `mac` as 12 uppercase hex chars with no
// separators. Matches gentool's `5211058E5C33`-style display.
static void macBytesToHex(const unsigned char *mac, AsciiString &out)
{
	static const char hex[] = "0123456789ABCDEF";
	char buf[13];
	int b;
	for (b = 0; b < 6; ++b)
	{
		buf[b * 2 + 0] = hex[(mac[b] >> 4) & 0xF];
		buf[b * 2 + 1] = hex[mac[b] & 0xF];
	}
	buf[12] = '\0';
	out = buf;
}

// Format a host-byte-order IPv4 address as dotted decimal "A.B.C.D" so it
// can be compared against the dotted strings GetAdaptersInfo returns.
static void formatIpv4Dotted(UnsignedInt ipHostOrder, char *out, unsigned int outCap)
{
	if (outCap == 0)
		return;
	unsigned int a = (ipHostOrder >> 24) & 0xFF;
	unsigned int b = (ipHostOrder >> 16) & 0xFF;
	unsigned int c = (ipHostOrder >>  8) & 0xFF;
	unsigned int d = ipHostOrder & 0xFF;
	_snprintf(out, outCap, "%u.%u.%u.%u", a, b, c, d);
	out[outCap - 1] = '\0';
}

// Enumerate Windows IPv4 adapters and return the MAC of the adapter whose
// IPv4 list contains `wantIpHostOrder`. If no adapter matches (or
// `wantIpHostOrder` is 0), returns the MAC of the first enumerated adapter
// that has a 6-byte physical address. This mirrors gentool's fallback so a
// Zulu client without an explicit LAN-IP selection still emits the same
// identifier gentool would have.
static AsciiString getLocalMacIdHex(UnsignedInt wantIpHostOrder)
{
	AsciiString result;

	HMODULE hMod = LoadLibraryA("iphlpapi.dll");
	if (hMod == nullptr)
		return result;
	FnGetAdaptersInfo getAdaptersInfo = (FnGetAdaptersInfo)GetProcAddress(hMod, "GetAdaptersInfo");
	if (getAdaptersInfo == nullptr)
	{
		FreeLibrary(hMod);
		return result;
	}

	ULONG bufLen = 16 * 1024;
	LocalIpAdapterInfo *info = (LocalIpAdapterInfo *)malloc(bufLen);
	if (info == nullptr)
	{
		FreeLibrary(hMod);
		return result;
	}

	DWORD status = getAdaptersInfo(info, &bufLen);
	if (status == ERROR_BUFFER_OVERFLOW)
	{
		free(info);
		info = (LocalIpAdapterInfo *)malloc(bufLen);
		if (info == nullptr)
		{
			FreeLibrary(hMod);
			return result;
		}
		status = getAdaptersInfo(info, &bufLen);
	}
	if (status != ERROR_SUCCESS)
	{
		free(info);
		FreeLibrary(hMod);
		return result;
	}

	char wantDotted[16];
	wantDotted[0] = '\0';
	if (wantIpHostOrder != 0)
		formatIpv4Dotted(wantIpHostOrder, wantDotted, sizeof(wantDotted));

	AsciiString firstMac;
	LocalIpAdapterInfo *p;
	for (p = info; p != nullptr; p = p->Next)
	{
		if (p->AddressLength < 6)
			continue;

		if (firstMac.isEmpty())
			macBytesToHex(p->Address, firstMac);

		if (wantDotted[0] != '\0')
		{
			LocalIpAddrString *ip;
			for (ip = &p->IpAddressList; ip != nullptr; ip = ip->Next)
			{
				if (strcmp(ip->IpAddress.String, wantDotted) == 0)
				{
					macBytesToHex(p->Address, result);
					free(info);
					FreeLibrary(hMod);
					return result;
				}
			}
		}
	}

	free(info);
	FreeLibrary(hMod);
	if (result.isEmpty())
		result = firstMac;
	return result;
}

// Read the system SMBIOS table via GetSystemFirmwareTable('RSMB', ...) and
// return the type 1 (System Information) UUID as a canonical dashed string
// "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX". On SMBIOS 2.6+ the first three
// fields are stored little-endian and are byte-swapped here to match what
// `wmic csproduct get UUID` shows. Returns empty on failure or all-zero/all-
// FF UUID (the latter being SMBIOS's "not present" sentinel).
static AsciiString getBoardIdDashed()
{
	AsciiString result;

	HMODULE hMod = GetModuleHandleA("kernel32.dll");
	if (hMod == nullptr)
		return result;
	FnGetSystemFirmwareTable getSysFwTable =
		(FnGetSystemFirmwareTable)GetProcAddress(hMod, "GetSystemFirmwareTable");
	if (getSysFwTable == nullptr)
		return result; // pre-XP-SP2 host; quietly skip.

	const DWORD provider = 'RSMB'; // big-endian 'R','S','M','B'
	DWORD bufSize = getSysFwTable(provider, 0, nullptr, 0);
	if (bufSize == 0)
		return result;

	BYTE *buf = (BYTE *)malloc(bufSize);
	if (buf == nullptr)
		return result;

	DWORD got = getSysFwTable(provider, 0, buf, bufSize);
	if (got == 0 || got > bufSize)
	{
		free(buf);
		return result;
	}

	// RawSMBIOSData header: Used20CallingMethod, MajorVer, MinorVer,
	// DmiRevision, Length (DWORD), then SMBIOSTableData[Length].
	if (got < 8)
	{
		free(buf);
		return result;
	}
	BYTE majorVer = buf[1];
	BYTE minorVer = buf[2];
	DWORD tableLen = *(DWORD *)(buf + 4);
	if (tableLen + 8 > got)
		tableLen = got - 8;
	BYTE *table = buf + 8;

	const bool littleEndianFields = (majorVer > 2) || (majorVer == 2 && minorVer >= 6);

	BYTE *cur = table;
	BYTE *end = table + tableLen;
	while (cur + 4 <= end)
	{
		BYTE type = cur[0];
		BYTE structLen = cur[1];
		if (structLen < 4 || cur + structLen > end)
			break;

		if (type == 1 && structLen >= 0x18)
		{
			// UUID is 16 bytes at offset 8 of the formatted area.
			const BYTE *u = cur + 8;

			// Detect "not present" sentinels per the SMBIOS spec.
			bool allZero = true;
			bool allFF = true;
			int i;
			for (i = 0; i < 16; ++i)
			{
				if (u[i] != 0x00) allZero = false;
				if (u[i] != 0xFF) allFF = false;
			}
			if (!allZero && !allFF)
			{
				BYTE swapped[16];
				if (littleEndianFields)
				{
					swapped[0] = u[3]; swapped[1] = u[2]; swapped[2] = u[1]; swapped[3] = u[0];
					swapped[4] = u[5]; swapped[5] = u[4];
					swapped[6] = u[7]; swapped[7] = u[6];
					memcpy(swapped + 8, u + 8, 8);
				}
				else
				{
					memcpy(swapped, u, 16);
				}

				char outBuf[40];
				sprintf(outBuf,
					"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
					swapped[0], swapped[1], swapped[2], swapped[3],
					swapped[4], swapped[5],
					swapped[6], swapped[7],
					swapped[8], swapped[9],
					swapped[10], swapped[11], swapped[12], swapped[13], swapped[14], swapped[15]);
				result = outBuf;
			}
			break;
		}

		// Skip the formatted area, then walk the trailing string set:
		// strings are null-terminated; the set ends with a double null.
		BYTE *next = cur + structLen;
		if (next >= end) break;
		while (next < end - 1 && !(next[0] == 0 && next[1] == 0))
			++next;
		next += 2;
		if (next <= cur) break;
		cur = next;
	}

	free(buf);
	return result;
}

void UploadReplayToServer(const AsciiString& url, const void *data, unsigned int dataLen,
                          const AsciiString& filename, unsigned int seed,
                          const AsciiString& playerName)
{
	char nameBuf[256];
	sanitizeMultipartFilename(filename.isEmpty() ? nullptr : filename.str(), nameBuf, sizeof(nameBuf));

	// Populate the optional identifier fields. Each helper returns an empty
	// AsciiString on failure; httpPostMultipartFile drops empty fields.
	OptionPreferences prefs;
	UnsignedInt lanIp = prefs.getLANIPAddress();

	MultipartTextField fields[4];
	fields[0].name = "mac_id";
	fields[0].value = getLocalMacIdHex(lanIp);
	fields[1].name = "board_id";
	fields[1].value = getBoardIdDashed();
	fields[2].name = "player_name";
	fields[2].value = playerName;
	fields[3].name = "client_version";
	fields[3].value = (TheVersion != nullptr) ? TheVersion->getAsciiVersion() : AsciiString();

	httpPostMultipartFile(url, "file", nameBuf, data, dataLen,
		fields, 4, nullptr, seed, "Replay upload");
}

// ---------------------------------------------------------------------------
// Per-match client log upload (gzip'd multipart to cncstats /logs).
// ---------------------------------------------------------------------------

// Compress srcLen bytes of src into a genuine gzip (.gz) stream returned via
// *outBuf / *outLen (caller frees with free()). Returns false on any failure.
//
// zlib 1.1.4 (the bundled version) can't emit a gzip wrapper directly: its
// deflateInit2 only accepts windowBits 8..15 - no negative "raw" or +16 "gzip"
// modes. So we let compress2() produce a zlib stream (a 2-byte zlib header, the
// raw DEFLATE payload, then a 4-byte Adler-32) and re-wrap just the DEFLATE
// payload in the gzip container (10-byte header + payload + CRC-32 + ISIZE).
// The DEFLATE bytes are identical to what a native gzip encoder would carry.
static bool gzipBuffer(const unsigned char *src, unsigned int srcLen,
                       unsigned char **outBuf, unsigned int *outLen)
{
	*outBuf = nullptr;
	*outLen = 0;
	if (src == nullptr || srcLen == 0)
		return false;

	// zlib 1.1.4 has no compressBound(); its documented worst case for
	// compress2() is srcLen + srcLen/1000 + 12. Add slack on top.
	uLong zbufCap = (uLong)srcLen + (uLong)srcLen / 1000u + 64u;
	unsigned char *zbuf = (unsigned char *)malloc((size_t)zbufCap);
	if (zbuf == nullptr)
		return false;

	uLong zlen = zbufCap;
	if (compress2(zbuf, &zlen, src, (uLong)srcLen, Z_BEST_COMPRESSION) != Z_OK
		|| zlen < 6)
	{
		free(zbuf);
		return false;
	}

	// Strip the 2-byte zlib header and the trailing 4-byte Adler-32; compress2
	// never uses a preset dictionary, so the header is always exactly 2 bytes.
	const unsigned char *deflateData = zbuf + 2;
	unsigned int deflateLen = (unsigned int)zlen - 6u;

	static const unsigned char gzHeader[10] = {
		0x1F, 0x8B,             // magic
		0x08,                   // CM = DEFLATE
		0x00,                   // FLG (no optional fields)
		0x00, 0x00, 0x00, 0x00, // MTIME (unknown)
		0x00,                   // XFL
		0xFF                    // OS = unknown
	};

	unsigned int gzLen = (unsigned int)sizeof(gzHeader) + deflateLen + 8u;
	unsigned char *gz = (unsigned char *)malloc(gzLen);
	if (gz == nullptr)
	{
		free(zbuf);
		return false;
	}

	unsigned int pos = 0;
	memcpy(gz + pos, gzHeader, sizeof(gzHeader));
	pos += (unsigned int)sizeof(gzHeader);
	memcpy(gz + pos, deflateData, deflateLen);
	pos += deflateLen;

	// CRC-32 of the uncompressed data, little-endian.
	uLong crc = crc32(0L, src, srcLen);
	gz[pos++] = (unsigned char)(crc & 0xFF);
	gz[pos++] = (unsigned char)((crc >> 8) & 0xFF);
	gz[pos++] = (unsigned char)((crc >> 16) & 0xFF);
	gz[pos++] = (unsigned char)((crc >> 24) & 0xFF);

	// ISIZE: uncompressed size mod 2^32, little-endian.
	gz[pos++] = (unsigned char)(srcLen & 0xFF);
	gz[pos++] = (unsigned char)((srcLen >> 8) & 0xFF);
	gz[pos++] = (unsigned char)((srcLen >> 16) & 0xFF);
	gz[pos++] = (unsigned char)((srcLen >> 24) & 0xFF);

	free(zbuf);
	*outBuf = gz;
	*outLen = gzLen;
	return true;
}

// Read an entire file into a malloc'd buffer. Returns false (out params left
// zeroed) if the file can't be opened, can't be read whole, or is empty.
static bool readWholeFile(const char *path, unsigned char **outBuf, unsigned int *outLen)
{
	*outBuf = nullptr;
	*outLen = 0;
	FILE *fp = fopen(path, "rb");
	if (fp == nullptr)
		return false;
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size <= 0)
	{
		fclose(fp);
		return false;
	}
	unsigned char *buf = (unsigned char *)malloc((size_t)size);
	if (buf == nullptr)
	{
		fclose(fp);
		return false;
	}
	size_t got = fread(buf, 1, (size_t)size, fp);
	fclose(fp);
	if (got != (size_t)size)
	{
		free(buf);
		return false;
	}
	*outBuf = buf;
	*outLen = (unsigned int)size;
	return true;
}

// Cap on how much of any one client log is uploaded. A log is a diagnostic
// tail: when one runs long, the end explains the failure and the head is a
// session that already finished. Keeps a future runaway logger from turning
// end-of-match telemetry into a multi-megabyte POST, the way the per-frame
// observer checkpoints did at 15 MB a session.
static const unsigned int LOG_UPLOAD_MAX_BYTES = 1024 * 1024;

// Read at most the last maxBytes of a file. Under the cap this is exactly
// readWholeFile. Over it, the returned buffer opens on a line boundary and
// carries a marker saying how much was dropped, so a truncated log can never
// be mistaken for a complete one. maxBytes of 0 means no cap.
static bool readLogFileTail(const char *path, unsigned int maxBytes,
                            unsigned char **outBuf, unsigned int *outLen)
{
	*outBuf = nullptr;
	*outLen = 0;

	FILE *fp = fopen(path, "rb");
	if (fp == nullptr)
		return false;
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fclose(fp);
	if (size <= 0)
		return false;
	if (maxBytes == 0 || (unsigned long)size <= (unsigned long)maxBytes)
		return readWholeFile(path, outBuf, outLen);

	fp = fopen(path, "rb");
	if (fp == nullptr)
		return false;
	const long dropped = size - (long)maxBytes;
	if (fseek(fp, dropped, SEEK_SET) != 0)
	{
		fclose(fp);
		return false;
	}
	unsigned char *tail = (unsigned char *)malloc((size_t)maxBytes);
	if (tail == nullptr)
	{
		fclose(fp);
		return false;
	}
	size_t got = fread(tail, 1, (size_t)maxBytes, fp);
	fclose(fp);
	if (got == 0)
	{
		free(tail);
		return false;
	}

	// Advance to just past the first newline so the upload never opens on
	// half a line. If the whole tail holds no newline at all, keep it as-is
	// rather than throwing the only content away.
	size_t start = 0;
	while (start < got && tail[start] != '\n')
		++start;
	if (start < got)
		++start;
	else
		start = 0;

	char marker[160];
	int markerLen = sprintf(marker,
		"[log truncated for upload: %ld earlier bytes dropped, last %u kept]\n",
		dropped + (long)start, (unsigned int)(got - start));
	if (markerLen <= 0)
	{
		free(tail);
		return false;
	}

	unsigned int outSize = (unsigned int)markerLen + (unsigned int)(got - start);
	unsigned char *buf = (unsigned char *)malloc((size_t)outSize);
	if (buf == nullptr)
	{
		free(tail);
		return false;
	}
	memcpy(buf, marker, (size_t)markerLen);
	memcpy(buf + markerLen, tail + start, got - start);
	free(tail);

	*outBuf = buf;
	*outLen = outSize;
	return true;
}

// Reduce an arbitrary player identifier to a header-safe, single-segment token:
// map anything that would break an HTTP header line or a path component to '_',
// and cap the length.
static void sanitizeHeaderToken(const AsciiString& in, char *out, unsigned int outCap)
{
	if (outCap == 0)
		return;
	unsigned int n = 0;
	const char *p;
	for (p = in.str(); *p != '\0' && n + 1 < outCap; ++p)
	{
		unsigned char c = (unsigned char)*p;
		if (c < 0x20 || c == 0x7F || c == '\r' || c == '\n' ||
			c == '/' || c == '\\' || c == '"' || c == ':')
			c = '_';
		out[n++] = (char)c;
	}
	out[n] = '\0';
}

void UploadLogsToServer(const AsciiString& url, unsigned int seed,
                        const AsciiString& player,
                        const AsciiString *filePaths, unsigned int fileCount)
{
	if (url.isEmpty() || filePaths == nullptr || fileCount == 0)
		return;

	char playerToken[128];
	sanitizeHeaderToken(player, playerToken, sizeof(playerToken));
	if (playerToken[0] == '\0')
	{
		printf("Log upload: no usable X-Player id; skipping\n");
		fflush(stdout);
		return;
	}

	char playerHeader[192];
	int hdrLen = sprintf(playerHeader, "X-Player: %s\r\n", playerToken);
	if (hdrLen <= 0)
		return;

	unsigned int i;
	for (i = 0; i < fileCount; ++i)
	{
		if (filePaths[i].isEmpty())
			continue;

		unsigned char *raw = nullptr;
		unsigned int rawLen = 0;
		if (!readWholeFile(filePaths[i].str(), &raw, &rawLen))
		{
			// Missing/empty is expected for e.g. ObserverLog.txt when this
			// client wasn't an observer; skip quietly.
			continue;
		}

		unsigned char *gz = nullptr;
		unsigned int gzLen = 0;
		bool didGzip = gzipBuffer(raw, rawLen, &gz, &gzLen);
		free(raw);
		if (!didGzip)
			continue;

		// Upload filename = "<basename>.gz". httpPostMultipartFile reduces the
		// name to its basename; the .gz suffix advertises the gzip encoding.
		char baseBuf[256];
		sanitizeMultipartFilename(filePaths[i].str(), baseBuf, sizeof(baseBuf));
		char nameBuf[280];
		_snprintf(nameBuf, sizeof(nameBuf), "%.255s.gz", baseBuf);
		nameBuf[sizeof(nameBuf) - 1] = '\0';

		printf("Log upload: %s -> %u bytes gzipped (%u raw) as %s\n",
			filePaths[i].str(), gzLen, rawLen, nameBuf);
		fflush(stdout);

		httpPostMultipartFile(url, "file", nameBuf, gz, gzLen,
			nullptr, 0, playerHeader, seed, "Log upload");

		free(gz);
	}
}

void *AppendZuluUploadTag(const void *fileData, unsigned int fileLen,
                          unsigned int *outLen)
{
	static const unsigned char tag[8] = {
		'Z', 'U', 'T', 'G',
		0x01, 0x00,  // version 1, little-endian
		0x00, 0x00,  // payload length 0
	};
	if (outLen != nullptr)
		*outLen = 0;
	if (fileData == nullptr || fileLen == 0)
		return nullptr;
	unsigned int newLen = fileLen + (unsigned int)sizeof(tag);
	void *buf = malloc(newLen);
	if (buf == nullptr)
		return nullptr;
	memcpy(buf, fileData, fileLen);
	memcpy((char *)buf + fileLen, tag, sizeof(tag));
	if (outLen != nullptr)
		*outLen = newLen;
	return buf;
}

void UploadMapToServer(const AsciiString& uploadUrl, const void *data, unsigned int dataLen,
                       unsigned int mapCRC, const AsciiString& mapName,
                       const char *fileKind, unsigned int seed)
{
	if (uploadUrl.isEmpty() || data == nullptr || dataLen == 0)
		return;

	// Build extra headers: X-Map-CRC, X-Map-Name, X-Map-File. Truncate the
	// map name to keep the header bounded in size.
	char extra[512];
	const char *name = mapName.isEmpty() ? "" : mapName.str();
	const char *kind = (fileKind != nullptr && fileKind[0] != '\0') ? fileKind : "map";
	sprintf(extra, "X-Map-CRC: %u\r\nX-Map-Name: %.255s\r\nX-Map-File: %.31s\r\n",
		mapCRC, name, kind);

	httpPostBytes(uploadUrl, data, dataLen, "application/octet-stream", extra, seed, "Map upload");
}

// Read a single file via TheFileSystem and POST it to the upload endpoint
// under the given asset kind. Missing files are silently skipped (most
// sidecars are optional). Logs every step so a failed lobby upload is
// recoverable from stdout.
static void uploadOneAssetIfPresent(const AsciiString& uploadUrl,
                                    unsigned int mapCRC,
                                    const AsciiString& mapName,
                                    const AsciiString& assetPath,
                                    const char *kind,
                                    unsigned int seed)
{
	if (assetPath.isEmpty() || TheFileSystem == nullptr)
		return;

	File *assetFile = TheFileSystem->openFile(assetPath.str(), File::READ);
	if (assetFile == nullptr)
	{
		// Sidecars are best-effort; "not present" is the common case.
		return;
	}

	Int assetSize = assetFile->size();
	char *assetBytes = assetFile->readEntireAndClose(); // also closes the file
	if (assetBytes != nullptr && assetSize > 0)
	{
		printf("[map] Uploading %s \"%s\" (crc=%u, %d bytes) to %s\n",
			kind, assetPath.str(), mapCRC, assetSize, uploadUrl.str());
		fflush(stdout);
		UploadMapToServer(uploadUrl, assetBytes, static_cast<unsigned int>(assetSize),
			mapCRC, mapName, kind, seed);
	}
	else
	{
		printf("[map] ERROR: readEntireAndClose returned no data for %s \"%s\"\n",
			kind, assetPath.str());
		fflush(stdout);
	}
	delete[] assetBytes;
}

void UploadAllMapAssetsIfMissing(const AsciiString& checkUrl,
                                 const AsciiString& uploadUrl,
                                 unsigned int mapCRC,
                                 const AsciiString& mapPath,
                                 unsigned int contentsMask,
                                 unsigned int seed)
{
	if (checkUrl.isEmpty() || uploadUrl.isEmpty() || mapPath.isEmpty() || mapCRC == 0)
		return;

	if (!MapMissingFromServer(checkUrl, mapCRC))
		return;

	// Always upload the .map itself; sidecars only if the host's contents
	// mask says they're present. Mask bits mirror FileTransfer.cpp:264-275
	// so the round-trip (host upload → peer download) covers exactly the
	// same set the legacy P2P transfer would have moved.
	uploadOneAssetIfPresent(uploadUrl, mapCRC, mapPath, mapPath, "map", seed);
	if (contentsMask & 2)
		uploadOneAssetIfPresent(uploadUrl, mapCRC, mapPath, GetPreviewFromMap(mapPath), "preview", seed);
	if (contentsMask & 4)
		uploadOneAssetIfPresent(uploadUrl, mapCRC, mapPath, GetINIFromMap(mapPath), "ini", seed);
	if (contentsMask & 8)
		uploadOneAssetIfPresent(uploadUrl, mapCRC, mapPath, GetStrFileFromMap(mapPath), "str", seed);
	if (contentsMask & 16)
		uploadOneAssetIfPresent(uploadUrl, mapCRC, mapPath, GetSoloINIFromMap(mapPath), "solo", seed);
	if (contentsMask & 32)
		uploadOneAssetIfPresent(uploadUrl, mapCRC, mapPath, GetAssetUsageFromMap(mapPath), "assets", seed);
	if (contentsMask & 64)
		uploadOneAssetIfPresent(uploadUrl, mapCRC, mapPath, GetReadmeFromMap(mapPath), "readme", seed);
}

// ---------------------------------------------------------------------------
// Map existence check via HTTP GET.
// ---------------------------------------------------------------------------

// Lowercase an ASCII string in place (for case-insensitive body comparison).
static void lowerAscii(char *s)
{
	for (; *s != '\0'; ++s)
	{
		if (*s >= 'A' && *s <= 'Z')
			*s = static_cast<char>(*s + ('a' - 'A'));
	}
}

// ---------------------------------------------------------------------------
// Team balancing via HTTP GET.
// ---------------------------------------------------------------------------

// Percent-encode an ASCII byte sequence into a query-string fragment. Writes
// at most outCap bytes (always null-terminated). Reserved characters become
// %XX; the unreserved set per RFC 3986 passes through.
static void urlEncode(const char *src, char *out, unsigned int outCap)
{
	if (outCap == 0) return;
	static const char hex[] = "0123456789ABCDEF";
	unsigned int o = 0;
	for (const char *p = src; *p != '\0' && o + 4 < outCap; ++p)
	{
		unsigned char c = (unsigned char)*p;
		bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
		if (unreserved)
		{
			out[o++] = (char)c;
		}
		else
		{
			out[o++] = '%';
			out[o++] = hex[(c >> 4) & 0xF];
			out[o++] = hex[c & 0xF];
		}
	}
	out[o] = '\0';
}

// Extract the first JSON object key from a response body. Assumes keys do
// not contain backslash-escaped quotes (player names won't).
static bool extractFirstJsonKey(const char *body, AsciiString &outKey)
{
	const char *p = strchr(body, '"');
	if (p == nullptr) return false;
	++p;
	const char *end = strchr(p, '"');
	if (end == nullptr) return false;
	outKey.set(p, (int)(end - p));
	return !outKey.isEmpty();
}

// Extract every top-level key from a flat JSON object. The balance-teams
// response is a dict whose values are floats, so every quoted token at the
// object's top level is a key.
static void extractAllJsonKeys(const char *body, std::vector<AsciiString> &outKeys)
{
	const char *p = body;
	while (*p && *p != '{') ++p;
	if (!*p) return;
	++p;

	while (*p)
	{
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')
			++p;
		if (*p == '}' || !*p) break;
		if (*p != '"') break; // malformed; bail
		++p;
		const char *end = strchr(p, '"');
		if (!end) break;
		AsciiString k;
		k.set(p, (int)(end - p));
		if (!k.isEmpty())
			outKeys.push_back(k);
		p = end + 1;
		// Skip the value (which is a number or null in the balance-teams API).
		while (*p && *p != ',' && *p != '}') ++p;
	}
}

// Split a comma-separated key like "Pancake,OneThree111" into individual
// canonical names, trimming whitespace.
static void splitCommaList(const AsciiString &key, std::vector<AsciiString> &out)
{
	AsciiString current;
	for (const char *p = key.str(); *p != '\0'; ++p)
	{
		if (*p == ',')
		{
			if (!current.isEmpty())
			{
				out.push_back(current);
				current.clear();
			}
		}
		else if (*p != ' ' && *p != '\t')
		{
			current.concat(*p);
		}
	}
	if (!current.isEmpty())
		out.push_back(current);
}

BalanceTeamsResult BalanceTeamsFromServer(const AsciiString& url,
                                          const std::vector<AsciiString>& playerNames)
{
	BalanceTeamsResult result;
	result.success = false;

	if (url.isEmpty())
	{
		result.errorMessage = "Team balance URL is not set.";
		return result;
	}
	if (playerNames.empty())
	{
		result.errorMessage = "No players to balance.";
		return result;
	}

	// Parse URL once so we can rebuild the path with our query string.
	char hostBuf[256];
	char pathBuf[1024];
	URL_COMPONENTSA uc;
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = hostBuf;
	uc.dwHostNameLength = sizeof(hostBuf);
	uc.lpszUrlPath = pathBuf;
	uc.dwUrlPathLength = sizeof(pathBuf);

	if (!InternetCrackUrlA(url.str(), 0, 0, &uc))
	{
		result.errorMessage = "Failed to parse balance-teams URL.";
		return result;
	}

	// Build full path with ?players=A&players=B&... appended (preserving any
	// existing query string in the original URL path).
	char fullPath[4096];
	int fullLen = (int)strlen(pathBuf);
	if (fullLen >= (int)sizeof(fullPath) - 1)
	{
		result.errorMessage = "Balance-teams URL path too long.";
		return result;
	}
	memcpy(fullPath, pathBuf, (size_t)fullLen);
	fullPath[fullLen] = '\0';
	bool hasQuery = (strchr(fullPath, '?') != nullptr);
	for (size_t i = 0; i < playerNames.size(); ++i)
	{
		char encoded[256];
		urlEncode(playerNames[i].str(), encoded, sizeof(encoded));
		const char *sep = hasQuery ? "&" : "?";
		int remaining = (int)sizeof(fullPath) - fullLen;
		int written = _snprintf(fullPath + fullLen, (size_t)remaining,
		                        "%splayers=%s", sep, encoded);
		if (written < 0 || written >= remaining)
		{
			result.errorMessage = "Too many players for balance-teams URL.";
			return result;
		}
		fullLen += written;
		hasQuery = true;
	}

	WinInetSession s;
	if (!openHttpRequest(url, "GET", fullPath, "Balance teams", &s))
	{
		result.errorMessage = "Could not connect to balance-teams server.";
		return result;
	}

	BOOL sent = HttpSendRequestA(s.hRequest, "Accept: application/json\r\n",
	                             (DWORD)-1L, nullptr, 0);
	if (!sent)
	{
		result.errorMessage.format("Balance-teams request failed (WinINet %lu).",
		                           GetLastError());
		closeHttpRequest(&s);
		return result;
	}

	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	HttpQueryInfoA(s.hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
	               &statusCode, &statusSize, nullptr);
	if (statusCode < 200 || statusCode >= 300)
	{
		result.errorMessage.format("Balance-teams server returned HTTP %lu.",
		                           statusCode);
		closeHttpRequest(&s);
		return result;
	}

	// Read the response body up to a sensible cap (small JSON dict).
	static const DWORD bodyCap = 16 * 1024;
	char *body = (char *)malloc(bodyCap);
	if (body == nullptr)
	{
		result.errorMessage = "Out of memory reading balance-teams response.";
		closeHttpRequest(&s);
		return result;
	}
	DWORD totalRead = 0;
	for (;;)
	{
		DWORD bytesRead = 0;
		if (!InternetReadFile(s.hRequest, body + totalRead,
		                      bodyCap - 1 - totalRead, &bytesRead))
			break;
		if (bytesRead == 0) break;
		totalRead += bytesRead;
		if (totalRead >= bodyCap - 1) break;
	}
	body[totalRead] = '\0';
	closeHttpRequest(&s);

	std::vector<AsciiString> keys;
	extractAllJsonKeys(body, keys);
	free(body);
	if (keys.empty())
	{
		result.errorMessage = "Balance-teams response was empty.";
		return result;
	}

	// First key is the best-balanced split; the names there are team 1.
	splitCommaList(keys[0], result.team1);
	if (result.team1.empty())
	{
		result.errorMessage = "Balance-teams response had no team-1 players.";
		return result;
	}

	// Union every name appearing in any key. The server fuzzy-resolves the
	// names we sent (so "Pan" comes back as "Pancake") and silently drops names
	// it doesn't recognize. allKnown lets the caller tell "slot belongs to
	// team 2" apart from "the server didn't recognize this slot at all".
	size_t k;
	for (k = 0; k < keys.size(); ++k)
	{
		std::vector<AsciiString> names;
		splitCommaList(keys[k], names);
		size_t n;
		for (n = 0; n < names.size(); ++n)
		{
			Bool dup = FALSE;
			size_t a;
			for (a = 0; a < result.allKnown.size(); ++a)
			{
				if (result.allKnown[a].compareNoCase(names[n]) == 0)
				{
					dup = TRUE;
					break;
				}
			}
			if (!dup)
				result.allKnown.push_back(names[n]);
		}
	}

	result.success = true;
	return result;
}

// ---------------------------------------------------------------------------
// Map summary blurb via HTTP POST.
// ---------------------------------------------------------------------------

// Append a JSON-escaped form of `src` to `out`. Escapes the characters that
// would otherwise break out of a JSON string: backslash, double-quote, and
// control characters. UTF-8 byte sequences pass through untouched.
static void appendJsonEscaped(AsciiString &out, const char *src)
{
	for (const char *p = src; *p != '\0'; ++p)
	{
		unsigned char c = (unsigned char)*p;
		switch (c)
		{
			case '"':  out.concat("\\\""); break;
			case '\\': out.concat("\\\\"); break;
			case '\b': out.concat("\\b");  break;
			case '\f': out.concat("\\f");  break;
			case '\n': out.concat("\\n");  break;
			case '\r': out.concat("\\r");  break;
			case '\t': out.concat("\\t");  break;
			default:
				if (c < 0x20)
				{
					char esc[8];
					sprintf(esc, "\\u%04X", c);
					out.concat(esc);
				}
				else
				{
					out.concat((char)c);
				}
				break;
		}
	}
}

MapSummaryResult MapSummaryFromServer(const AsciiString& url,
                                      const AsciiString& mapName,
                                      const std::vector<MapSummaryPlayer>& players)
{
	MapSummaryResult result;
	result.success = false;

	if (url.isEmpty())
		return result;

	// Build the JSON body: { "map_name": "...", "players": [{"name":"...","general":N,"team":T}, ...] }
	AsciiString body;
	body.concat("{\"map_name\":\"");
	appendJsonEscaped(body, mapName.isEmpty() ? "" : mapName.str());
	body.concat("\",\"players\":[");
	size_t i;
	for (i = 0; i < players.size(); ++i)
	{
		if (i > 0)
			body.concat(',');
		body.concat("{\"name\":\"");
		appendJsonEscaped(body, players[i].name.isEmpty() ? "" : players[i].name.str());
		char gbuf[64];
		// players[i].team is 0-based (-1 = no team); emit it 1-based in the
		// map_summary request so teams start at 1 (0 = no team) on the wire.
		sprintf(gbuf, "\",\"general\":%d,\"team\":%d}", players[i].general, players[i].team + 1);
		body.concat(gbuf);
	}
	body.concat("]}");

	WinInetSession s;
	if (!openHttpRequest(url, "POST", nullptr, "Map summary", &s))
		return result;

	const char *headers = "Content-Type: application/json\r\nAccept: application/json\r\n";
	BOOL sent = HttpSendRequestA(s.hRequest, headers, (DWORD)strlen(headers),
	                             const_cast<char*>(body.str()), (DWORD)body.getLength());
	if (!sent)
	{
		printf("Map summary: HttpSendRequest failed (%lu)\n", GetLastError());
		closeHttpRequest(&s);
		return result;
	}

	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	HttpQueryInfoA(s.hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
	               &statusCode, &statusSize, nullptr);
	if (statusCode < 200 || statusCode >= 300)
	{
		printf("Map summary: %s -> %lu\n", url.str(), statusCode);
		closeHttpRequest(&s);
		return result;
	}

	// Read the response body. The server returns a JSON string (a single
	// quoted, possibly newline-bearing token). Cap at 16 KiB.
	static const DWORD bodyCap = 16 * 1024;
	char *resp = (char *)malloc(bodyCap);
	if (resp == nullptr)
	{
		closeHttpRequest(&s);
		return result;
	}
	DWORD totalRead = 0;
	for (;;)
	{
		DWORD bytesRead = 0;
		if (!InternetReadFile(s.hRequest, resp + totalRead,
		                      bodyCap - 1 - totalRead, &bytesRead))
			break;
		if (bytesRead == 0) break;
		totalRead += bytesRead;
		if (totalRead >= bodyCap - 1) break;
	}
	resp[totalRead] = '\0';
	closeHttpRequest(&s);

	printf("Map summary: %s -> %lu\n", url.str(), statusCode);

	// Strip a single layer of surrounding double-quotes if present (the
	// endpoint returns a bare JSON string), and unescape \n / \" / \\ so
	// the caller can split the text on real newlines.
	const char *p = resp;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		++p;
	const char *end = resp + totalRead;
	while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
		--end;
	if (end > p && *p == '"' && end[-1] == '"')
	{
		++p;
		--end;
	}

	AsciiString current;
	while (p < end)
	{
		char c = *p++;
		if (c == '\\' && p < end)
		{
			char n = *p++;
			switch (n)
			{
				case 'n':
					if (!current.isEmpty())
					{
						result.lines.push_back(current);
						current.clear();
					}
					break;
				case 'r':                          break;
				case 't':  current.concat('\t');   break;
				case '"':  current.concat('"');    break;
				case '\\': current.concat('\\');   break;
				case '/':  current.concat('/');    break;
				default:   current.concat(n);      break;
			}
		}
		else if (c == '\n')
		{
			if (!current.isEmpty())
			{
				result.lines.push_back(current);
				current.clear();
			}
		}
		else if (c != '\r')
		{
			current.concat(c);
		}
	}
	if (!current.isEmpty())
		result.lines.push_back(current);

	free(resp);
	result.success = true;
	return result;
}

// ---------------------------------------------------------------------------
// Map vote (radarvan map_vote) via HTTP POST.
// ---------------------------------------------------------------------------

// Locate the first occurrence of "<key>" as a top-level JSON object key
// inside body, and return a pointer to the first non-whitespace character
// after its colon. Returns nullptr when the key isn't present or the
// surrounding shape isn't recognized. Cheap and best-effort: assumes the
// key name itself contains no backslash-escapes and that the body is a
// well-formed flat object (no nested object whose keys collide with the
// ones we look up).
static const char *jsonValueStart(const char *body, const char *key)
{
	if (body == nullptr || key == nullptr || key[0] == '\0')
		return nullptr;
	size_t keyLen = strlen(key);
	const char *p = body;
	while ((p = strchr(p, '"')) != nullptr)
	{
		++p;
		if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '"')
		{
			const char *q = p + keyLen + 1;
			while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
				++q;
			if (*q != ':')
			{
				p = q;
				continue;
			}
			++q;
			while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
				++q;
			return q;
		}
		// Skip to the matching closing quote so we don't trip into
		// the middle of a string value.
		const char *end = strchr(p, '"');
		if (end == nullptr)
			return nullptr;
		p = end + 1;
	}
	return nullptr;
}

// Extract a JSON string value for the given top-level key. Returns true if
// found and writes the unescaped value into outValue (handles \", \\, \n,
// \r, \t, \/ ; everything else passes through). Returns false if the key
// is missing or the value isn't a string.
static bool jsonGetString(const char *body, const char *key, AsciiString &outValue)
{
	const char *p = jsonValueStart(body, key);
	if (p == nullptr || *p != '"')
		return false;
	++p;
	outValue.clear();
	while (*p != '\0' && *p != '"')
	{
		if (*p == '\\' && p[1] != '\0')
		{
			char n = p[1];
			switch (n)
			{
				case '"':  outValue.concat('"'); break;
				case '\\': outValue.concat('\\'); break;
				case '/':  outValue.concat('/'); break;
				case 'n':  outValue.concat('\n'); break;
				case 'r':  outValue.concat('\r'); break;
				case 't':  outValue.concat('\t'); break;
				default:   outValue.concat(n); break;
			}
			p += 2;
		}
		else
		{
			outValue.concat(*p++);
		}
	}
	return true;
}

// Extract an unsigned integer JSON value for the given top-level key.
// Returns true and writes the parsed number into outValue when present.
static bool jsonGetUInt(const char *body, const char *key, unsigned int &outValue)
{
	const char *p = jsonValueStart(body, key);
	if (p == nullptr)
		return false;
	if (*p < '0' || *p > '9')
		return false;
	unsigned int v = 0;
	while (*p >= '0' && *p <= '9')
	{
		v = v * 10u + (unsigned int)(*p - '0');
		++p;
	}
	outValue = v;
	return true;
}

ChooseMapResult ChooseMapFromServer(const AsciiString& baseUrl,
                                    const std::vector<AsciiString>& playerNames,
                                    unsigned int playerCount)
{
	ChooseMapResult result;
	result.success = false;
	result.mapCRC = 0;
	result.contentsMask = 0;

	if (baseUrl.isEmpty())
	{
		result.errorMessage = "Map vote URL is not set.";
		return result;
	}

	// Compose the full URL: baseUrl + "<playerCount>/choose". Append a
	// separator slash if the configured base doesn't end with one.
	AsciiString fullUrl = baseUrl;
	if (fullUrl.getLength() == 0 || fullUrl.str()[fullUrl.getLength() - 1] != '/')
		fullUrl.concat('/');
	char seg[64];
	sprintf(seg, "%u/choose", playerCount);
	fullUrl.concat(seg);

	// Build the JSON body: { "players": ["A","B",...] }
	AsciiString body;
	body.concat("{\"players\":[");
	size_t i;
	for (i = 0; i < playerNames.size(); ++i)
	{
		if (i > 0)
			body.concat(',');
		body.concat('"');
		appendJsonEscaped(body, playerNames[i].isEmpty() ? "" : playerNames[i].str());
		body.concat('"');
	}
	body.concat("]}");

	WinInetSession s;
	if (!openHttpRequest(fullUrl, "POST", nullptr, "Map vote", &s))
	{
		result.errorMessage = "Could not connect to map-vote server.";
		return result;
	}

	const char *headers = "Content-Type: application/json\r\nAccept: application/json\r\n";
	BOOL sent = HttpSendRequestA(s.hRequest, headers, (DWORD)strlen(headers),
	                             const_cast<char*>(body.str()), (DWORD)body.getLength());
	if (!sent)
	{
		result.errorMessage.format("Map vote request failed (WinINet %lu).",
		                           GetLastError());
		closeHttpRequest(&s);
		return result;
	}

	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	HttpQueryInfoA(s.hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
	               &statusCode, &statusSize, nullptr);
	if (statusCode < 200 || statusCode >= 300)
	{
		result.errorMessage.format("Map vote server returned HTTP %lu.", statusCode);
		closeHttpRequest(&s);
		return result;
	}

	static const DWORD bodyCap = 32 * 1024;
	char *resp = (char *)malloc(bodyCap);
	if (resp == nullptr)
	{
		result.errorMessage = "Out of memory reading map-vote response.";
		closeHttpRequest(&s);
		return result;
	}
	DWORD totalRead = 0;
	for (;;)
	{
		DWORD bytesRead = 0;
		if (!InternetReadFile(s.hRequest, resp + totalRead,
		                      bodyCap - 1 - totalRead, &bytesRead))
			break;
		if (bytesRead == 0) break;
		totalRead += bytesRead;
		if (totalRead >= bodyCap - 1) break;
	}
	resp[totalRead] = '\0';
	closeHttpRequest(&s);

	printf("Map vote: %s -> %lu\n", fullUrl.str(), statusCode);

	if (!jsonGetString(resp, "chosen_map", result.chosenMap) || result.chosenMap.isEmpty())
	{
		free(resp);
		result.errorMessage = "Map vote response missing chosen_map.";
		return result;
	}

	// CRC of the picked map. radarvan returns it as an uppercase hex string
	// under "chosen_map_crc" (e.g. "2778D1EA") and it equals the engine file
	// CRC, which is exactly how the local map cache and the cncstats CDN are
	// keyed. The caller uses it to match the map locally by CRC and, when
	// missing, install it CRC-verified from the CDN. Fall back to a bare
	// decimal "map_crc" for any older/alternate schema. A null value (the
	// server's "no pick" case) leaves mapCRC at 0 and disables the download.
	AsciiString crcStr;
	if (jsonGetString(resp, "chosen_map_crc", crcStr) && !crcStr.isEmpty())
		result.mapCRC = (unsigned int)strtoul(crcStr.str(), nullptr, 16);
	else
		jsonGetUInt(resp, "map_crc", result.mapCRC);

	// Optional extras (not currently sent by radarvan): a relative .map path
	// and the sidecar contents mask. When map_filename is absent the caller
	// derives an install path from chosen_map; when the mask is absent it
	// asks the CDN for every sidecar kind.
	jsonGetString(resp, "map_filename", result.mapFileName);
	jsonGetUInt(resp, "map_contents_mask", result.contentsMask);

	free(resp);
	result.success = true;
	return result;
}

bool DownloadMapAssetFromServer(const AsciiString& downloadUrl,
                                unsigned int mapCRC,
                                const char *fileKind,
                                unsigned int maxBytes,
                                void **outData,
                                unsigned int *outLen)
{
	if (outData == nullptr || outLen == nullptr)
		return false;
	*outData = nullptr;
	*outLen = 0;

	if (downloadUrl.isEmpty() || fileKind == nullptr || fileKind[0] == '\0')
		return false;

	// 16 MiB ceiling unless the caller asked for something smaller. Map files
	// are typically a few hundred KB; the cap exists to keep a misbehaving
	// server from forcing us into an unbounded allocation.
	if (maxBytes == 0)
		maxBytes = 16u * 1024u * 1024u;

	// Build "<path>?crc=<dec>&kind=<sanitized>" appended to whatever query
	// string the configured URL already carries.
	char hostBuf[256];
	char pathBuf[1024];
	URL_COMPONENTSA uc;
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = hostBuf;
	uc.dwHostNameLength = sizeof(hostBuf);
	uc.lpszUrlPath = pathBuf;
	uc.dwUrlPathLength = sizeof(pathBuf);

	if (!InternetCrackUrlA(downloadUrl.str(), 0, 0, &uc))
	{
		printf("Map download: failed to parse URL \"%s\"\n", downloadUrl.str());
		return false;
	}

	char kindEncoded[64];
	urlEncode(fileKind, kindEncoded, sizeof(kindEncoded));

	const char *separator = (strchr(pathBuf, '?') != nullptr) ? "&" : "?";
	char fullPath[1536];
	int wrote = _snprintf(fullPath, sizeof(fullPath), "%s%scrc=%u&kind=%s",
		pathBuf, separator, mapCRC, kindEncoded);
	if (wrote < 0 || wrote >= (int)sizeof(fullPath))
	{
		printf("Map download: URL too long for crc=%u kind=%s\n", mapCRC, fileKind);
		return false;
	}

	WinInetSession s;
	if (!openHttpRequest(downloadUrl, "GET", fullPath, "Map download", &s))
		return false;

	BOOL sent = HttpSendRequestA(s.hRequest, nullptr, 0, nullptr, 0);
	if (!sent)
	{
		printf("Map download: HttpSendRequest failed (%lu)\n", GetLastError());
		closeHttpRequest(&s);
		return false;
	}

	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	HttpQueryInfoA(s.hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
		&statusCode, &statusSize, nullptr);

	if (statusCode == 404)
	{
		// Server doesn't have this asset for this CRC. Quiet path; the
		// caller is walking the kinds and skipping missing ones.
		closeHttpRequest(&s);
		return false;
	}
	if (statusCode < 200 || statusCode >= 300)
	{
		printf("Map download: crc=%u kind=%s -> %lu\n", mapCRC, fileKind, statusCode);
		closeHttpRequest(&s);
		return false;
	}

	// Reject up-front if the server advertises a Content-Length that exceeds
	// our cap. Saves the allocation roundtrip on obvious overruns.
	DWORD contentLen = 0;
	DWORD contentLenSize = sizeof(contentLen);
	if (HttpQueryInfoA(s.hRequest, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
		&contentLen, &contentLenSize, nullptr))
	{
		if (contentLen > maxBytes)
		{
			printf("Map download: crc=%u kind=%s body %lu > cap %u\n",
				mapCRC, fileKind, contentLen, maxBytes);
			closeHttpRequest(&s);
			return false;
		}
	}

	// Grow a heap buffer in chunks. Most map assets fit in the first read;
	// the chunked grow handles chunked-transfer responses without a known
	// Content-Length, and keeps the upper bound honest.
	unsigned int cap = (contentLen > 0) ? (contentLen + 64u) : 65536u;
	if (cap > maxBytes) cap = maxBytes;
	unsigned char *buf = (unsigned char *)malloc(cap);
	if (buf == nullptr)
	{
		closeHttpRequest(&s);
		return false;
	}

	unsigned int total = 0;
	for (;;)
	{
		if (total >= cap)
		{
			if (cap >= maxBytes)
			{
				printf("Map download: crc=%u kind=%s body exceeds cap %u\n",
					mapCRC, fileKind, maxBytes);
				free(buf);
				closeHttpRequest(&s);
				return false;
			}
			unsigned int newCap = cap * 2u;
			if (newCap > maxBytes) newCap = maxBytes;
			unsigned char *grown = (unsigned char *)realloc(buf, newCap);
			if (grown == nullptr)
			{
				free(buf);
				closeHttpRequest(&s);
				return false;
			}
			buf = grown;
			cap = newCap;
		}

		DWORD bytesRead = 0;
		BOOL ok = InternetReadFile(s.hRequest, buf + total,
			(DWORD)(cap - total), &bytesRead);
		if (!ok)
		{
			printf("Map download: InternetReadFile failed (%lu) after %u bytes\n",
				GetLastError(), total);
			free(buf);
			closeHttpRequest(&s);
			return false;
		}
		if (bytesRead == 0)
			break;
		total += bytesRead;
	}

	closeHttpRequest(&s);

	if (total == 0)
	{
		printf("Map download: crc=%u kind=%s empty body\n", mapCRC, fileKind);
		free(buf);
		return false;
	}

	printf("Map download: crc=%u kind=%s -> %u bytes\n", mapCRC, fileKind, total);
	*outData = buf;
	*outLen = total;
	return true;
}

// Write a downloaded asset to disk, creating the containing directory if
// missing. Returns true on a successful write. Frees `data` either way.
static bool writeAssetToDisk(const AsciiString& path, void *data, unsigned int len)
{
	if (data == nullptr || len == 0)
	{
		if (data != nullptr) free(data);
		return false;
	}

	// Make sure the per-map subdirectory exists. GetBasePathFromPath strips
	// the filename and leaves the parent directory.
	AsciiString dir = GetBasePathFromPath(path);
	if (!dir.isEmpty() && TheFileSystem != nullptr)
	{
		TheFileSystem->createDirectory(dir);
	}

	File *fp = (TheFileSystem != nullptr)
		? TheFileSystem->openFile(path.str(), File::CREATE | File::BINARY | File::WRITE)
		: nullptr;
	if (fp == nullptr)
	{
		printf("[map] Cannot open \"%s\" for writing\n", path.str());
		fflush(stdout);
		free(data);
		return false;
	}

	Int wrote = fp->write(data, (Int)len);
	fp->close();
	free(data);

	if (wrote != (Int)len)
	{
		printf("[map] Short write to \"%s\": %d of %u\n", path.str(), wrote, len);
		fflush(stdout);
		return false;
	}
	return true;
}

// The sidecar kinds a map install can carry, in contentsMask bit order. Bit 1
// (value 1) is the .map itself and so has no entry here.
struct MapSidecarKind
{
	UnsignedInt mask;
	const char *kind;
};

enum { MAP_SIDECAR_COUNT = 6 };

static const MapSidecarKind s_mapSidecars[MAP_SIDECAR_COUNT] =
{
	{ 2,  "preview" },
	{ 4,  "ini"     },
	{ 8,  "str"     },
	{ 16, "solo"    },
	{ 32, "assets"  },
	{ 64, "readme"  },
};

// Where sidecar `idx` installs to, given the .map's path.
static AsciiString mapSidecarPath(Int idx, const AsciiString& localMapPath)
{
	switch (idx)
	{
		case 0: return GetPreviewFromMap(localMapPath);
		case 1: return GetINIFromMap(localMapPath);
		case 2: return GetStrFileFromMap(localMapPath);
		case 3: return GetSoloINIFromMap(localMapPath);
		case 4: return GetAssetUsageFromMap(localMapPath);
		case 5: return GetReadmeFromMap(localMapPath);
	}
	return AsciiString::TheEmptyString;
}

Bool DownloadAndInstallMap(const AsciiString& localMapPath,
                           UnsignedInt mapCRC,
                           UnsignedInt contentsMask)
{
	if (mapCRC == 0 || localMapPath.isEmpty())
		return FALSE;
	if (TheGlobalData == nullptr || TheGlobalData->m_mapDownloadUrl.isEmpty())
		return FALSE;

	const AsciiString downloadUrl = TheGlobalData->m_mapDownloadUrl;

	// Fetch the .map first; if it doesn't come down clean, no point
	// touching the rest. The .map is the only required asset.
	void *mapData = nullptr;
	unsigned int mapLen = 0;
	if (!DownloadMapAssetFromServer(downloadUrl, mapCRC, "map", 0, &mapData, &mapLen))
		return FALSE;

	// Validate the bytes match the CRC the host advertised before we
	// commit anything to disk. cncstats currently has no upload auth, so
	// a malicious actor could overwrite a popular CRC with arbitrary
	// content; this check is the only thing keeping us safe.
	CRC theCRC;
	theCRC.clear();
	theCRC.computeCRC(mapData, (Int)mapLen);
	UnsignedInt actualCRC = theCRC.get();
	if (actualCRC != mapCRC)
	{
		printf("[map] Downloaded map crc mismatch: expected %u, got %u (rejecting)\n",
			mapCRC, actualCRC);
		fflush(stdout);
		free(mapData);
		return FALSE;
	}

	if (!writeAssetToDisk(localMapPath, mapData, mapLen))
		return FALSE;
	// writeAssetToDisk freed mapData

	printf("[map] Installed downloaded map \"%s\" (crc=%u, %u bytes)\n",
		localMapPath.str(), mapCRC, mapLen);
	fflush(stdout);

	// Sidecars: best-effort. Each bit in contentsMask tells us the host
	// has that sidecar on its disk and therefore (presumably) uploaded
	// it. If the GET 404s, the host's upload didn't make it or the
	// server lost the file; either way we just skip and continue.
	Int s;
	for (s = 0; s < MAP_SIDECAR_COUNT; ++s)
	{
		if ((contentsMask & s_mapSidecars[s].mask) == 0)
			continue;
		AsciiString sidecarPath = mapSidecarPath(s, localMapPath);
		if (sidecarPath.isEmpty())
			continue;
		void *sd = nullptr;
		unsigned int sl = 0;
		if (!DownloadMapAssetFromServer(downloadUrl, mapCRC, s_mapSidecars[s].kind, 0, &sd, &sl))
			continue;
		if (writeAssetToDisk(sidecarPath, sd, sl))
		{
			printf("[map] Installed %s sidecar \"%s\" (%u bytes)\n",
				s_mapSidecars[s].kind, sidecarPath.str(), sl);
			fflush(stdout);
		}
	}

	// Refresh MapCache so findMap() and getMapPreviewImage() see the new
	// entry. Without this the lobby preview would stay blank until the
	// next full game restart.
	if (TheMapCache != nullptr)
		TheMapCache->refreshUserMaps();

	return TRUE;
}

bool MapMissingFromServer(const AsciiString& checkUrl, unsigned int mapCRC)
{
	if (checkUrl.isEmpty())
		return false;

	// Append ?crc=<hex> (or &crc=...) to the URL. We rebuild the path so
	// existing query strings are preserved.
	char hostBuf[256];
	char pathBuf[1024];
	URL_COMPONENTSA uc;
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = hostBuf;
	uc.dwHostNameLength = sizeof(hostBuf);
	uc.lpszUrlPath = pathBuf;
	uc.dwUrlPathLength = sizeof(pathBuf);

	if (!InternetCrackUrlA(checkUrl.str(), 0, 0, &uc))
	{
		printf("Map check: failed to parse URL \"%s\"\n", checkUrl.str());
		return false;
	}

	const char *separator = (strchr(pathBuf, '?') != nullptr) ? "&" : "?";
	char fullPath[1280];
	sprintf(fullPath, "%s%scrc=%u", pathBuf, separator, mapCRC);

	WinInetSession s;
	if (!openHttpRequest(checkUrl, "GET", fullPath, "Map check", &s))
		return false;

	BOOL result = HttpSendRequestA(s.hRequest, nullptr, 0, nullptr, 0);
	if (!result)
	{
		printf("Map check: HttpSendRequest failed (%lu)\n", GetLastError());
		closeHttpRequest(&s);
		return false;
	}

	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	HttpQueryInfoA(s.hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusSize, nullptr);

	if (statusCode < 200 || statusCode >= 300)
	{
		printf("Map check: %s?crc=%u -> %lu (treating as already-have)\n", checkUrl.str(), mapCRC, statusCode);
		closeHttpRequest(&s);
		return false;
	}

	// Read up to 31 bytes of body — only need enough to hold "true" / "false"
	// with some slack for whitespace.
	char body[32];
	memset(body, 0, sizeof(body));
	DWORD totalRead = 0;
	for (;;)
	{
		DWORD bytesRead = 0;
		if (!InternetReadFile(s.hRequest, body + totalRead,
				static_cast<DWORD>(sizeof(body) - 1 - totalRead), &bytesRead))
			break;
		if (bytesRead == 0)
			break;
		totalRead += bytesRead;
		if (totalRead >= sizeof(body) - 1)
			break;
	}
	body[totalRead] = '\0';

	closeHttpRequest(&s);

	// Trim leading/trailing whitespace.
	char *start = body;
	while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
		++start;
	char *end = start + strlen(start);
	while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
		--end;
	*end = '\0';

	lowerAscii(start);
	const bool missing = (strcmp(start, "false") == 0);
	printf("Map check: crc=%u -> %lu, body=\"%s\", missing=%s\n",
		mapCRC, statusCode, start, missing ? "true" : "false");
	return missing;
}

// ---------------------------------------------------------------------------
// Map match counts via HTTP GET. The endpoint returns a JSON array of
// {"map": "<path>", "matchCount": N} entries where <path> is in the form
// "maps/<folder>" for system maps and "userdata/maps/<folder>" for user maps,
// lowercased and slash-separated. We cache the parsed table per-process for
// a short window so each map-select dialog open performs at most one fetch
// (and the LAN search-filter re-populates don't refetch on every keystroke).
// ---------------------------------------------------------------------------

static std::map<AsciiString, int> s_mapMatchCounts;
static DWORD s_mapMatchCountsLastFetchTick = 0;
static bool s_mapMatchCountsEverFetched = false;

// Convert a MapCache key ("Maps\\Defcon6\\Defcon6.map" or the same with the
// user-data path prefix) into the radarvan API form ("maps/defcon6" or
// "userdata/maps/defcon6"). Empty string when the key can't be classified.
static AsciiString normalizeMapCacheKeyForApi(const AsciiString& cacheKey)
{
	if (cacheKey.isEmpty())
		return AsciiString::TheEmptyString;

	AsciiString lc = cacheKey;
	lc.toLower();

	AsciiString flat;
	{
		const char *p;
		for (p = lc.str(); *p != '\0'; ++p)
			flat.concat((*p == '\\') ? '/' : *p);
	}

	AsciiString userFlat;
	if (TheMapCache != nullptr)
	{
		AsciiString userDir = TheMapCache->getUserMapDir();
		userDir.toLower();
		const char *p;
		for (p = userDir.str(); *p != '\0'; ++p)
			userFlat.concat((*p == '\\') ? '/' : *p);
	}

	AsciiString out;
	if (!userFlat.isEmpty() && flat.startsWith(userFlat.str()))
	{
		// <userdata>/maps/<folder>/<file>.map -> userdata/maps/<folder>/<file>.map
		out = "userdata/maps";
		out.concat(flat.str() + userFlat.getLength());
	}
	else if (flat.startsWith("maps/") || flat.compare("maps") == 0)
	{
		out = flat;
	}
	else
	{
		return AsciiString::TheEmptyString;
	}

	const char *lastSlash = strrchr(out.str(), '/');
	if (lastSlash != nullptr)
	{
		AsciiString trimmed;
		trimmed.set(out.str(), (int)(lastSlash - out.str()));
		return trimmed;
	}
	return out;
}

void FetchMapMatchCountsIfStale(const AsciiString& url, unsigned int maxAgeSec)
{
	if (url.isEmpty())
		return;

	DWORD now = GetTickCount();
	if (s_mapMatchCountsEverFetched &&
	    (now - s_mapMatchCountsLastFetchTick) < maxAgeSec * 1000)
		return;

	// Bump the tick up-front so every failure path below cools down for the
	// full TTL window. The LAN search box re-runs populateMapListboxFiltered
	// on every keystroke; without this, a transient HTTP error would retry
	// (and stall the UI) on each keystroke until the server came back.
	s_mapMatchCountsLastFetchTick = now;

	WinInetSession s;
	if (!openHttpRequest(url, "GET", nullptr, "Map match counts", &s))
		return;

	BOOL sent = HttpSendRequestA(s.hRequest, "Accept: application/json\r\n",
	                             (DWORD)-1L, nullptr, 0);
	if (!sent)
	{
		printf("Map match counts: HttpSendRequest failed (%lu)\n", GetLastError());
		closeHttpRequest(&s);
		return;
	}

	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	HttpQueryInfoA(s.hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
	               &statusCode, &statusSize, nullptr);
	if (statusCode < 200 || statusCode >= 300)
	{
		printf("Map match counts: %s -> %lu\n", url.str(), statusCode);
		closeHttpRequest(&s);
		return;
	}

	// Pull the whole body. A 500-entry response is ~30 KiB; keep slack so the
	// table can grow without us bumping the cap.
	static const DWORD bodyCap = 1024 * 1024;
	char *body = (char *)malloc(bodyCap);
	if (body == nullptr)
	{
		closeHttpRequest(&s);
		return;
	}
	DWORD totalRead = 0;
	bool readOk = true;
	for (;;)
	{
		DWORD bytesRead = 0;
		if (!InternetReadFile(s.hRequest, body + totalRead,
		                      bodyCap - 1 - totalRead, &bytesRead))
		{
			// Mid-stream error: keep the prior cache. Re-using a partially
			// downloaded body would silently drop most entries.
			printf("Map match counts: InternetReadFile failed (%lu) after %lu bytes\n",
				GetLastError(), totalRead);
			readOk = false;
			break;
		}
		if (bytesRead == 0) break;
		totalRead += bytesRead;
		if (totalRead >= bodyCap - 1)
		{
			// Hit our cap before EOF. We can't tell whether the next byte
			// would have been the closing bracket, so treat this as truncated.
			printf("Map match counts: body exceeded %lu byte cap; keeping prior cache\n",
				bodyCap);
			readOk = false;
			break;
		}
	}
	body[totalRead] = '\0';
	closeHttpRequest(&s);

	if (!readOk)
	{
		free(body);
		return;
	}

	// Sanity-check: a healthy JSON-array response ends with ']' (after any
	// trailing whitespace). If we don't see one, the response is truncated
	// or a non-array error blob; in either case keep the previous cache so
	// a hiccup doesn't wipe rows that were showing correct counts.
	const char *tail = body + totalRead;
	while (tail > body && (tail[-1] == ' ' || tail[-1] == '\t' ||
	                       tail[-1] == '\r' || tail[-1] == '\n'))
		--tail;
	if (tail == body || tail[-1] != ']')
	{
		printf("Map match counts: response did not end with ']'; keeping prior cache\n");
		free(body);
		return;
	}

	// Walk the array. The response keys never contain escaped quotes (paths
	// are vanilla ASCII path characters plus brackets and spaces), so a
	// tolerant find-next-quoted-token parser is sufficient. Entries that are
	// missing one of the two expected keys, or that carry a non-positive
	// matchCount, are silently skipped; the loop keeps going so one bad
	// entry doesn't poison the rest.
	std::map<AsciiString, int> fresh;
	const char *p = body;
	while (*p != '\0')
	{
		const char *mkey = strstr(p, "\"map\"");
		if (mkey == nullptr) break;
		const char *brace = strchr(mkey, '}');
		if (brace == nullptr) break;

		const char *colon = strchr(mkey + 5, ':');
		const char *q1 = (colon != nullptr) ? strchr(colon + 1, '"') : nullptr;
		const char *q2 = (q1 != nullptr) ? strchr(q1 + 1, '"') : nullptr;
		const char *ckey = strstr(mkey, "\"matchCount\"");
		if (q1 != nullptr && q2 != nullptr && q2 < brace &&
		    ckey != nullptr && ckey < brace)
		{
			AsciiString key;
			key.set(q1 + 1, (int)(q2 - q1 - 1));
			key.toLower();
			const char *ccolon = strchr(ckey + 12, ':');
			if (ccolon != nullptr && ccolon < brace)
			{
				++ccolon;
				while (*ccolon == ' ' || *ccolon == '\t') ++ccolon;
				int count = atoi(ccolon);
				if (count > 0 && !key.isEmpty())
					fresh[key] = count;
			}
		}
		p = brace + 1;
	}
	free(body);

	s_mapMatchCounts.swap(fresh);
	s_mapMatchCountsEverFetched = true;
	printf("Map match counts: fetched %u entries\n",
		(unsigned)s_mapMatchCounts.size());
}

int GetMapMatchCount(const AsciiString& mapCacheKey)
{
	if (!s_mapMatchCountsEverFetched || mapCacheKey.isEmpty())
		return 0;
	AsciiString apiKey = normalizeMapCacheKeyForApi(mapCacheKey);
	if (apiKey.isEmpty())
		return 0;
	std::map<AsciiString, int>::const_iterator it = s_mapMatchCounts.find(apiKey);
	if (it == s_mapMatchCounts.end())
		return 0;
	return it->second;
}

// ===========================================================================
// Multiplayer loading-screen "battlefield intel".
//
// Calls radarvan's /api/predict (anchor), /api/team_stats/, and
// /api/player_ratings/synergy/ on a background thread and distills the
// results into a short, display-ready blurb. See StatsUploader.h for the
// public contract; everything below is best-effort and non-blocking.
// ===========================================================================

// ---- shared state (guarded by s_intelCS) ---------------------------------
//
// The engine's global operator new / AsciiString allocate through the
// (thread-safe, TheDmaCriticalSection-guarded) game allocator, so a worker
// thread may allocate freely. What is NOT safe is sharing a single
// AsciiString's copy-on-write buffer across threads: its refCount is bumped
// without that lock. So nothing here shares an AsciiString across the
// thread boundary - the job is a plain-C POD, and the result is handed back
// as a malloc'd C string that each side turns into its own AsciiString.
static CRITICAL_SECTION s_intelCS;
static bool s_intelCSInit = false;   // only touched from the main thread
static bool s_intelPending = false;  // worker running, no result yet
static bool s_intelReady = false;    // s_intelTextC holds a usable blurb
static char *s_intelTextC = nullptr; // malloc'd blurb (CRT heap), guarded
static unsigned long s_intelGen = 0; // bumped per game; stale workers discard
static RadarvanIntelData s_intelData; // numbers behind the blurb (POD), guarded
static bool s_intelDataValid = false;  // s_intelData is meaningful

static void ensureIntelCS(void)
{
	// Called only from the main thread (RadarvanIntelStart / poll), so this
	// lazy init needs no lock of its own.
	if (!s_intelCSInit)
	{
		InitializeCriticalSection(&s_intelCS);
		s_intelCSInit = true;
	}
}

// ---- tiny JSON helpers on top of jsonValueStart ---------------------------

// Copy the half-open byte range [b, e) into a fresh AsciiString.
static AsciiString jsonSubstr(const char *b, const char *e)
{
	AsciiString out;
	if (e <= b)
		return out;
	size_t n = (size_t)(e - b);
	char *buf = (char *)malloc(n + 1);
	if (buf != nullptr)
	{
		memcpy(buf, b, n);
		buf[n] = '\0';
		out = buf;
		free(buf);
	}
	return out;
}

// Parse a (possibly signed/decimal/exponent) JSON number for the given key.
static bool jsonGetFloat(const char *body, const char *key, double &out)
{
	const char *p = jsonValueStart(body, key);
	if (p == nullptr)
		return false;
	char *endp = nullptr;
	double v = strtod(p, &endp);
	if (endp == p)
		return false;
	out = v;
	return true;
}

// Parse a JSON array-of-strings value for the given key into out (appended).
// Returns true if the key was found and pointed at a '['.
static bool jsonGetStringArray(const char *body, const char *key,
                               std::vector<AsciiString> &out)
{
	const char *p = jsonValueStart(body, key);
	if (p == nullptr || *p != '[')
		return false;
	++p;
	for (;;)
	{
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')
			++p;
		if (*p == ']' || *p == '\0')
			break;
		if (*p != '"')
			break; // not a string array element
		++p;
		AsciiString s;
		while (*p != '\0' && *p != '"')
		{
			if (*p == '\\' && p[1] != '\0')
			{
				char n = p[1];
				switch (n)
				{
					case 'n':  s.concat('\n'); break;
					case 't':  s.concat('\t'); break;
					case 'r':  s.concat('\r'); break;
					case '"':  s.concat('"');  break;
					case '\\': s.concat('\\'); break;
					case '/':  s.concat('/');  break;
					default:   s.concat(n);    break;
				}
				p += 2;
			}
			else
			{
				s.concat(*p++);
			}
		}
		if (*p == '"')
			++p;
		out.push_back(s);
	}
	return true;
}

// Split the top-level object elements ({...}) of the JSON array that begins at
// or after `arr` (the first '[' found) into `out`. Handles nested objects,
// arrays, and strings-with-escapes; scalar (non-object) elements are ignored,
// which is all we need for the radarvan payloads.
static void jsonSplitObjects(const char *arr, std::vector<AsciiString> &out)
{
	if (arr == nullptr)
		return;
	const char *p = arr;
	while (*p != '\0' && *p != '[')
		++p;
	if (*p != '[')
		return;
	++p;
	bool inStr = false;
	int depth = 0;
	const char *start = nullptr;
	for (; *p != '\0'; ++p)
	{
		char c = *p;
		if (inStr)
		{
			if (c == '\\' && p[1] != '\0')
				++p;
			else if (c == '"')
				inStr = false;
			continue;
		}
		if (c == '"')
		{
			inStr = true;
		}
		else if (c == '{' || c == '[')
		{
			if (c == '{' && depth == 0)
				start = p;
			++depth;
		}
		else if (c == '}' || c == ']')
		{
			if (depth == 0)
				break; // closing ']' of the outer array
			--depth;
			if (depth == 0 && c == '}' && start != nullptr)
			{
				out.push_back(jsonSubstr(start, p + 1));
				start = nullptr;
			}
		}
	}
}

// Case-insensitive "is name present in the set".
static bool nameInList(const AsciiString &name, const std::vector<AsciiString> &set)
{
	size_t i;
	for (i = 0; i < set.size(); ++i)
	{
		if (set[i].compareNoCase(name) == 0)
			return true;
	}
	return false;
}

// True when the two name lists are the same set (same size, all present).
static bool sameNameSet(const std::vector<AsciiString> &a,
                        const std::vector<AsciiString> &b)
{
	if (a.size() != b.size())
		return false;
	size_t i;
	for (i = 0; i < a.size(); ++i)
	{
		if (!nameInList(a[i], b))
			return false;
	}
	return true;
}

// ---- blocking HTTP that returns the response body into a string -----------

// GET or POST `url`; on HTTP 2xx, fills outResp with the body and returns
// true. Bounds every phase with a timeout so a stalled server can't keep the
// worker thread (and its WinINet handles) alive indefinitely.
//
// url/body are plain C strings (the caller owns them) so nothing crosses the
// thread boundary as a shared AsciiString. The AsciiString built here for
// openHttpRequest is worker-local and never escapes, which is safe.
static bool intelHttpJson(const char *url, const char *method,
                          const char *body, AsciiString &outResp)
{
	outResp.clear();
	if (url == nullptr || url[0] == '\0')
		return false;

	AsciiString urlStr;
	urlStr = url; // fresh, worker-local buffer

	WinInetSession s;
	if (!openHttpRequest(urlStr, method, nullptr, "Radarvan intel", &s))
		return false;

	DWORD toConnect = 4000, toSend = 4000, toReceive = 6000;
	InternetSetOption(s.hRequest, INTERNET_OPTION_CONNECT_TIMEOUT, &toConnect, sizeof(toConnect));
	InternetSetOption(s.hRequest, INTERNET_OPTION_SEND_TIMEOUT, &toSend, sizeof(toSend));
	InternetSetOption(s.hRequest, INTERNET_OPTION_RECEIVE_TIMEOUT, &toReceive, sizeof(toReceive));

	const bool hasBody = (body != nullptr && body[0] != '\0');
	const char *headers = hasBody
		? "Content-Type: application/json\r\nAccept: application/json\r\n"
		: "Accept: application/json\r\n";
	void *bodyPtr = hasBody ? (void *)const_cast<char *>(body) : nullptr;
	DWORD bodyLen = hasBody ? (DWORD)strlen(body) : 0;

	BOOL sent = HttpSendRequestA(s.hRequest, headers, (DWORD)strlen(headers), bodyPtr, bodyLen);
	if (!sent)
	{
		closeHttpRequest(&s);
		return false;
	}

	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	HttpQueryInfoA(s.hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
	               &statusCode, &statusSize, nullptr);
	if (statusCode < 200 || statusCode >= 300)
	{
		printf("Radarvan intel: %s -> %lu\n", url, statusCode);
		closeHttpRequest(&s);
		return false;
	}

	// synergy for large formats can run to tens of KB; cap generously.
	static const DWORD bodyCap = 256 * 1024;
	char *buf = (char *)malloc(bodyCap);
	if (buf == nullptr)
	{
		closeHttpRequest(&s);
		return false;
	}
	DWORD totalRead = 0;
	for (;;)
	{
		DWORD bytesRead = 0;
		if (!InternetReadFile(s.hRequest, buf + totalRead, bodyCap - 1 - totalRead, &bytesRead))
			break;
		if (bytesRead == 0)
			break;
		totalRead += bytesRead;
		if (totalRead >= bodyCap - 1)
			break;
	}
	buf[totalRead] = '\0';
	closeHttpRequest(&s);
	outResp = buf;
	free(buf);
	return true;
}

// ---- composition ----------------------------------------------------------

// Plain-C POD handed to the worker (malloc'd on the main thread, free'd on the
// worker). No AsciiString/STL members, so nothing is shared across threads.
// The predict body and the fully-formed synergy URL (query string included)
// are built on the main thread where AsciiString use is unambiguously safe.
struct IntelJob
{
	char predictUrl[512];
	char teamStatsUrl[512];
	char synergyUrl[640];
	char body[4096];      // prebuilt predict JSON request body
	int localTeam;
	unsigned long gen;
};

struct SynPair
{
	bool valid;
	AsciiString a;
	AsciiString b;
	double synergy;
	double delta;
	SynPair() : valid(false), synergy(0.0), delta(0.0) {}
};

// Round a probability/fraction to a signed integer percent.
static int pctSigned(double frac)
{
	return (int)(frac * 100.0 + (frac >= 0.0 ? 0.5 : -0.5));
}

// Find team_stats' exact-composition record for `team`. Returns true and sets
// wins/losses when found (teams with <6 games together are simply absent).
static bool lookupTeamRecord(const char *ts, const std::vector<AsciiString> &team,
                             int &wins, int &losses)
{
	const char *groupsVal = jsonValueStart(ts, "groups");
	if (groupsVal == nullptr)
		return false;
	std::vector<AsciiString> groups;
	jsonSplitObjects(groupsVal, groups);
	size_t gi;
	for (gi = 0; gi < groups.size(); ++gi)
	{
		unsigned int size = 0;
		if (!jsonGetUInt(groups[gi].str(), "size", size) || (int)size != (int)team.size())
			continue;
		const char *teamsVal = jsonValueStart(groups[gi].str(), "teams");
		if (teamsVal == nullptr)
			continue;
		std::vector<AsciiString> teams;
		jsonSplitObjects(teamsVal, teams);
		size_t ti;
		for (ti = 0; ti < teams.size(); ++ti)
		{
			std::vector<AsciiString> players;
			jsonGetStringArray(teams[ti].str(), "players", players);
			if (sameNameSet(players, team))
			{
				unsigned int w = 0, l = 0;
				jsonGetUInt(teams[ti].str(), "wins", w);
				jsonGetUInt(teams[ti].str(), "losses", l);
				wins = (int)w;
				losses = (int)l;
				return true;
			}
		}
	}
	return false;
}

// Pick the pair with the largest-magnitude synergy where both players are on
// `team`. `syn` is the top-level synergy JSON array.
static void standoutPair(const char *syn, const std::vector<AsciiString> &team, SynPair &best)
{
	std::vector<AsciiString> items;
	jsonSplitObjects(syn, items);
	size_t i;
	for (i = 0; i < items.size(); ++i)
	{
		AsciiString pa, pb;
		if (!jsonGetString(items[i].str(), "player_a", pa))
			continue;
		if (!jsonGetString(items[i].str(), "player_b", pb))
			continue;
		if (!nameInList(pa, team) || !nameInList(pb, team))
			continue;
		double synergy = 0.0, delta = 0.0;
		jsonGetFloat(items[i].str(), "synergy", synergy);
		jsonGetFloat(items[i].str(), "win_prob_delta", delta);
		if (!best.valid || fabs(synergy) > fabs(best.synergy))
		{
			best.valid = true;
			best.a = pa;
			best.b = pb;
			best.synergy = synergy;
			best.delta = delta;
		}
	}
}

// Append one team's block ("<LABEL>  W-L" plus an optional standout-duo line).
static void appendTeamBlock(AsciiString &out, const char *label,
                            bool haveRecord, int wins, int losses, const SynPair &pair)
{
	char line[160];
	if (haveRecord)
		sprintf(line, "%s  %d-%d", label, wins, losses);
	else
		sprintf(line, "%s  untested lineup", label);
	out.concat(line);
	out.concat('\n');
	if (pair.valid)
	{
		const char *tag = (pair.synergy >= 0.0) ? "spark" : "drag";
		char l2[192];
		sprintf(l2, "  %s: %s & %s %+d%%", tag, pair.a.str(), pair.b.str(), pctSigned(pair.delta));
		out.concat(l2);
		out.concat('\n');
	}
}

// Do the calls and build the blurb. Returns "" when there's nothing worth
// showing (predict URL empty / predict failed / not two teams) so the caller
// falls back to the themed "recon down" note.
static AsciiString composeIntel(const IntelJob &job, RadarvanIntelData &data)
{
	memset(&data, 0, sizeof(data));
	data.localTeam = job.localTeam;

	if (job.predictUrl[0] == '\0' || job.body[0] == '\0')
		return AsciiString();

	// ---- 1) predict (anchor); body was prebuilt on the main thread ----
	AsciiString predictResp;
	if (!intelHttpJson(job.predictUrl, "POST", job.body, predictResp))
		return AsciiString();

	unsigned int favoredTeam = 0;
	double favoredProb = 0.0;
	AsciiString mapName;
	std::vector<AsciiString> teamA, teamB, unknown;
	jsonGetString(predictResp.str(), "map_name", mapName);
	jsonGetUInt(predictResp.str(), "favored_team", favoredTeam);
	jsonGetFloat(predictResp.str(), "favored_win_prob", favoredProb);
	jsonGetStringArray(predictResp.str(), "team_a_players", teamA);
	jsonGetStringArray(predictResp.str(), "team_b_players", teamB);
	jsonGetStringArray(predictResp.str(), "unknown_players", unknown);
	if (teamA.empty() || teamB.empty())
		return AsciiString(); // not a two-team match / unexpected shape

	// ---- 2) team_stats: full-lineup record per team ----
	int aW = 0, aL = 0, bW = 0, bL = 0;
	bool aHaveRec = false, bHaveRec = false;
	if (job.teamStatsUrl[0] != '\0')
	{
		AsciiString tsResp;
		if (intelHttpJson(job.teamStatsUrl, "GET", nullptr, tsResp))
		{
			aHaveRec = lookupTeamRecord(tsResp.str(), teamA, aW, aL);
			bHaveRec = lookupTeamRecord(tsResp.str(), teamB, bW, bL);
		}
	}

	// ---- 3) synergy: standout duo per team (URL prebuilt with query) ----
	SynPair aPair, bPair;
	if (job.synergyUrl[0] != '\0')
	{
		AsciiString synResp;
		if (intelHttpJson(job.synergyUrl, "GET", nullptr, synResp))
		{
			standoutPair(synResp.str(), teamA, aPair);
			standoutPair(synResp.str(), teamB, bPair);
		}
	}

	// ---- fill structured data for the load-screen graphic ----
	data.favoredTeam = (int)favoredTeam;
	data.favoredWinProb = (float)favoredProb;
	data.aHasRecord = aHaveRec; data.aWins = aW; data.aLosses = aL;
	data.bHasRecord = bHaveRec; data.bWins = bW; data.bLosses = bL;
	data.aHasPair = aPair.valid; data.aDelta = (float)aPair.delta;
	data.bHasPair = bPair.valid; data.bDelta = (float)bPair.delta;

	// ---- 4) compose ----
	AsciiString out;
	out.concat("BATTLEFIELD INTEL\n");

	char oddsLine[128];
	if (job.localTeam == 1 || job.localTeam == 2)
	{
		// Always report the local player's own odds (matches the gauge/needle).
		double yourProb = (favoredTeam == (unsigned int)job.localTeam)
			? favoredProb : (1.0 - favoredProb);
		sprintf(oddsLine, "Victory odds: %d%%", pctSigned(yourProb));
	}
	else
	{
		// Observer: no "your" side, so name the favored team.
		sprintf(oddsLine, "Odds favor Team %u - %d%%", favoredTeam, pctSigned(favoredProb));
	}
	out.concat(oddsLine);
	out.concat('\n');

	if (!mapName.isEmpty())
	{
		out.concat(mapName);
		out.concat('\n');
	}
	out.concat('\n');

	const char *labA;
	const char *labB;
	if (job.localTeam == 1) { labA = "YOUR SQUAD"; labB = "ENEMY"; }
	else if (job.localTeam == 2) { labA = "ENEMY"; labB = "YOUR SQUAD"; }
	else { labA = "TEAM 1"; labB = "TEAM 2"; }

	appendTeamBlock(out, labA, aHaveRec, aW, aL, aPair);
	appendTeamBlock(out, labB, bHaveRec, bW, bL, bPair);

	if (!unknown.empty())
	{
		AsciiString u = "Unranked: ";
		size_t k;
		for (k = 0; k < unknown.size(); ++k)
		{
			if (k > 0)
				u.concat(", ");
			u.concat(unknown[k]);
		}
		out.concat(u);
		out.concat('\n');
	}

	return out;
}

static unsigned __stdcall intelThreadProc(void *arg)
{
	IntelJob *job = (IntelJob *)arg;
	RadarvanIntelData data;
	AsciiString blurb = composeIntel(*job, data); // worker-local AsciiString

	// Publish a private malloc'd copy of the text so no AsciiString buffer is
	// shared with the main thread. `blurb` (worker-local) is destroyed here.
	char *copy = nullptr;
	if (!blurb.isEmpty())
	{
		size_t n = (size_t)blurb.getLength();
		copy = (char *)malloc(n + 1);
		if (copy != nullptr)
			memcpy(copy, blurb.str(), n + 1);
	}

	EnterCriticalSection(&s_intelCS);
	if (job->gen == s_intelGen) // ignore results from a superseded game
	{
		if (s_intelTextC != nullptr)
			free(s_intelTextC);
		s_intelTextC = copy;
		s_intelReady = (copy != nullptr);
		s_intelData = data;             // POD copy, no cross-thread sharing
		s_intelDataValid = (copy != nullptr);
		s_intelPending = false;
	}
	else if (copy != nullptr)
	{
		free(copy); // superseded; discard
	}
	LeaveCriticalSection(&s_intelCS);

	free(job); // POD, malloc'd on the main thread
	return 0;
}

void RadarvanIntelReset(void)
{
	ensureIntelCS();
	EnterCriticalSection(&s_intelCS);
	++s_intelGen; // any in-flight worker's result will now be discarded
	s_intelPending = false;
	s_intelReady = false;
	s_intelDataValid = false;
	if (s_intelTextC != nullptr)
	{
		free(s_intelTextC);
		s_intelTextC = nullptr;
	}
	LeaveCriticalSection(&s_intelCS);
}

// Map a player-template store index (what GameSlot::getPlayerTemplate returns,
// and what the replay stores) to radarvan's General enum. This mirrors
// radarvan's cncstats_faction_to_general() exactly, so predict/synergy see the
// same general ids as the historical data ingested from replays. Playable
// generals are template indices 2..13; anything else (observer/civilian/
// random/unknown) is UNRECOGNIZED (-1).
static int templateIndexToGeneral(int tmpl)
{
	switch (tmpl)
	{
		case 2:  return 0;  // USA
		case 3:  return 4;  // CHINA
		case 4:  return 8;  // GLA
		case 5:  return 3;  // SUPER
		case 6:  return 2;  // LASER
		case 7:  return 1;  // AIR
		case 8:  return 6;  // TANK
		case 9:  return 7;  // INFANTRY
		case 10: return 5;  // NUKE
		case 11: return 9;  // TOXIN
		case 12: return 11; // DEMO
		case 13: return 10; // STEALTH
		default: return -1; // UNRECOGNIZED
	}
}

void RadarvanIntelStart(const AsciiString& predictUrl,
                        const AsciiString& teamStatsUrl,
                        const AsciiString& synergyUrl,
                        const AsciiString& mapName,
                        int localTeam,
                        const std::vector<MapSummaryPlayer>& players)
{
	ensureIntelCS();

	// Fresh game: clear stale state and claim a new generation. If a previous
	// worker is still running its result is discarded via the gen check.
	EnterCriticalSection(&s_intelCS);
	++s_intelGen;
	unsigned long gen = s_intelGen;
	s_intelReady = false;
	s_intelPending = false;
	s_intelDataValid = false;
	if (s_intelTextC != nullptr)
	{
		free(s_intelTextC);
		s_intelTextC = nullptr;
	}
	LeaveCriticalSection(&s_intelCS);

	// Nothing to fetch without the anchor call or a roster.
	if (predictUrl.isEmpty() || players.empty())
		return;

	// Build everything the worker needs into a plain-C POD here on the main
	// thread, where AsciiString use is unambiguously safe. The worker only
	// ever reads these char buffers (no shared AsciiString crosses threads).
	IntelJob *job = (IntelJob *)malloc(sizeof(IntelJob));
	if (job == nullptr)
		return;
	memset(job, 0, sizeof(IntelJob));
	job->localTeam = localTeam;
	job->gen = gen;

	// Prebuild the predict request body: {"map_name":"...","players":[...]}.
	AsciiString body;
	body.concat("{\"map_name\":\"");
	appendJsonEscaped(body, mapName.isEmpty() ? "" : mapName.str());
	body.concat("\",\"players\":[");
	size_t i;
	int teamSizes[2];
	int teamNums[2];
	int distinctTeams = 0;
	for (i = 0; i < players.size(); ++i)
	{
		if (i > 0)
			body.concat(',');
		body.concat("{\"name\":\"");
		appendJsonEscaped(body, players[i].name.isEmpty() ? "" : players[i].name.str());
		char gb[64];
		// players[i].general is the raw template index; predict wants the
		// General enum, so convert (mirrors radarvan's replay ingestion).
		sprintf(gb, "\",\"general\":%d,\"team\":%d}", templateIndexToGeneral(players[i].general), players[i].team);
		body.concat(gb);
		// track team sizes so we can derive the synergy game_format (NvN)
		int t = players[i].team;
		int d;
		bool found = false;
		for (d = 0; d < distinctTeams; ++d)
			if (teamNums[d] == t) { ++teamSizes[d]; found = true; break; }
		if (!found && distinctTeams < 2)
		{
			teamNums[distinctTeams] = t;
			teamSizes[distinctTeams] = 1;
			++distinctTeams;
		}
	}
	body.concat("]}");
	strncpy(job->body, body.str(), sizeof(job->body) - 1);

	strncpy(job->predictUrl, predictUrl.str(), sizeof(job->predictUrl) - 1);
	if (!teamStatsUrl.isEmpty())
		strncpy(job->teamStatsUrl, teamStatsUrl.str(), sizeof(job->teamStatsUrl) - 1);

	// synergy only applies to a symmetric NvN; prebuild its URL (with query)
	// only then. Otherwise leave it empty so the worker skips synergy.
	if (!synergyUrl.isEmpty() && distinctTeams == 2 && teamSizes[0] == teamSizes[1])
	{
		const char *sep = (strchr(synergyUrl.str(), '?') != nullptr) ? "&" : "?";
		char full[640];
		_snprintf(full, sizeof(full) - 1,
		          "%s%sgame_format=%dv%d&min_games_together=1",
		          synergyUrl.str(), sep, teamSizes[0], teamSizes[0]);
		full[sizeof(full) - 1] = '\0';
		strncpy(job->synergyUrl, full, sizeof(job->synergyUrl) - 1);
	}

	EnterCriticalSection(&s_intelCS);
	s_intelPending = true;
	LeaveCriticalSection(&s_intelCS);

	// _beginthreadex returns the handle as an integer type; VC6 has no
	// uintptr_t, so capture it straight into a HANDLE.
	unsigned threadId = 0;
	HANDLE hThread = (HANDLE)_beginthreadex(nullptr, 0, intelThreadProc, job, 0, &threadId);
	if (hThread == NULL)
	{
		// Couldn't spawn; roll back so the caller shows the fallback.
		EnterCriticalSection(&s_intelCS);
		s_intelPending = false;
		LeaveCriticalSection(&s_intelCS);
		free(job);
		return;
	}
	CloseHandle(hThread); // detached; it writes into the guarded statics
}

bool RadarvanIntelReady(AsciiString& outText)
{
	if (!s_intelCSInit)
		return false;
	bool ready = false;
	EnterCriticalSection(&s_intelCS);
	if (s_intelReady && s_intelTextC != nullptr)
	{
		outText = s_intelTextC; // main-thread AsciiString from a C string copy
		ready = true;
	}
	LeaveCriticalSection(&s_intelCS);
	return ready;
}

bool RadarvanIntelPending(void)
{
	if (!s_intelCSInit)
		return false;
	bool pending;
	EnterCriticalSection(&s_intelCS);
	pending = s_intelPending;
	LeaveCriticalSection(&s_intelCS);
	return pending;
}

bool RadarvanIntelReadyData(RadarvanIntelData& out)
{
	if (!s_intelCSInit)
		return false;
	bool ready = false;
	EnterCriticalSection(&s_intelCS);
	if (s_intelReady && s_intelDataValid)
	{
		out = s_intelData; // POD copy
		ready = true;
	}
	LeaveCriticalSection(&s_intelCS);
	return ready;
}

// ---------------------------------------------------------------------------
// Background match-telemetry upload.
//
// stopRecording used to run the stats/replay/log/map uploads inline on the main
// thread, so a slow or unreachable server stalled the whole game at end of
// match (each channel is a blocking WinINet round-trip under default timeouts).
// We now snapshot the volatile inputs on the main thread and hand a plain-C job
// to a detached worker that does all the HTTP. Mirrors the RadarvanIntel worker:
// no AsciiStringData is shared across threads (the worker rebuilds AsciiStrings
// from char buffers), and every buffer handed over is malloc'd and freed by the
// worker. Best-effort - if the game exits mid-upload the worker is simply torn
// down with the process.
// ---------------------------------------------------------------------------

struct TelemetryLogBuf
{
	unsigned char *bytes;    // raw (ungzipped) log contents; the worker gzips it
	unsigned int   len;
	char           name[64]; // basename for the multipart filename (".gz" appended)
};

struct TelemetryJob
{
	unsigned int seed;
	// Non-empty overrides `seed` for the X-Game-Seed header on the log
	// upload. Diagnostic uploads that happen outside a match (a failed
	// online host/join) have no seed, so they group under a label instead.
	char seedLabel[64];

	bool haveStats;
	char statsUrl[512];
	unsigned char *statsBytes;
	unsigned int statsLen;

	bool haveReplay;
	char replayUrl[512];
	unsigned char *replayBytes;   // already carries the ZUTG upload trailer
	unsigned int replayLen;
	char replayFileName[128];
	char playerNameUtf8[192];

	bool haveLogs;
	char logsUrl[512];
	char playerId[128];
	TelemetryLogBuf logs[4];
	unsigned int logCount;

	bool haveMap;
	char mapCheckUrl[512];
	char mapUploadUrl[512];
	unsigned int mapCRC;
	char mapFilePath[512];
	unsigned int mapContentsMask;
};

static void freeTelemetryJob(TelemetryJob *job)
{
	if (job == nullptr)
		return;
	if (job->statsBytes != nullptr)
		free(job->statsBytes);
	if (job->replayBytes != nullptr)
		free(job->replayBytes);
	unsigned int i;
	for (i = 0; i < job->logCount; ++i)
		if (job->logs[i].bytes != nullptr)
			free(job->logs[i].bytes);
	free(job);
}

static void copyCStr(char *dst, unsigned int cap, const char *src)
{
	if (cap == 0)
		return;
	if (src == nullptr)
	{
		dst[0] = '\0';
		return;
	}
	strncpy(dst, src, cap - 1);
	dst[cap - 1] = '\0';
}

static unsigned __stdcall telemetryThreadProc(void *arg)
{
	TelemetryJob *job = (TelemetryJob *)arg;

	// Stats: the gzipped JSON was snapshotted on the main thread.
	if (job->haveStats && job->statsBytes != nullptr)
	{
		printf("[stats] Uploading %u bytes to %s\n", job->statsLen, job->statsUrl);
		fflush(stdout);
		UploadStatsToServer(AsciiString(job->statsUrl), job->statsBytes, job->statsLen, job->seed);
	}

	// Replay: bytes already include the ZUTG trailer (applied on the main
	// thread before the on-disk file could be overwritten by the next game).
	if (job->haveReplay && job->replayBytes != nullptr)
	{
		printf("[replay] Uploading %u bytes to %s\n", job->replayLen, job->replayUrl);
		fflush(stdout);
		UploadReplayToServer(AsciiString(job->replayUrl), job->replayBytes, job->replayLen,
			AsciiString(job->replayFileName), job->seed, AsciiString(job->playerNameUtf8));
	}

	// Logs: gzip each raw snapshot here (CPU work off the main thread) and POST
	// it as its own multipart file part, grouped under X-Player on the server.
	if (job->haveLogs && job->logCount > 0)
	{
		char playerToken[128];
		sanitizeHeaderToken(AsciiString(job->playerId), playerToken, sizeof(playerToken));
		if (playerToken[0] != '\0')
		{
			char playerHeader[192];
			sprintf(playerHeader, "X-Player: %s\r\n", playerToken);

			// One X-Game-Seed for every file in this job: the numeric match
			// seed, or the diagnostic bucket label when there is no match.
			char seedStr[64];
			if (job->seedLabel[0] != '\0')
				copyCStr(seedStr, sizeof(seedStr), job->seedLabel);
			else
				sprintf(seedStr, "%u", job->seed);

			unsigned int i;
			for (i = 0; i < job->logCount; ++i)
			{
				if (job->logs[i].bytes == nullptr || job->logs[i].len == 0)
					continue;

				unsigned char *gz = nullptr;
				unsigned int gzLen = 0;
				if (!gzipBuffer(job->logs[i].bytes, job->logs[i].len, &gz, &gzLen))
					continue;

				char nameBuf[80];
				_snprintf(nameBuf, sizeof(nameBuf), "%.63s.gz", job->logs[i].name);
				nameBuf[sizeof(nameBuf) - 1] = '\0';

				printf("Log upload: %s -> %u bytes gzipped (%u raw)\n",
					nameBuf, gzLen, job->logs[i].len);
				fflush(stdout);

				httpPostMultipartFileSeedStr(AsciiString(job->logsUrl), "file", nameBuf, gz, gzLen,
					nullptr, 0, playerHeader, seedStr, "Log upload");

				free(gz);
			}
		}
		else
		{
			printf("Log upload: no usable X-Player id; skipping\n");
			fflush(stdout);
		}
	}

	// Map: static assets, read from disk here (they aren't overwritten between
	// games). Does its own missing-from-server check first.
	if (job->haveMap)
	{
		UploadAllMapAssetsIfMissing(AsciiString(job->mapCheckUrl), AsciiString(job->mapUploadUrl),
			job->mapCRC, AsciiString(job->mapFilePath), job->mapContentsMask, job->seed);
	}

	freeTelemetryJob(job);
	return 0;
}

void StartMatchTelemetryUpload(const MatchTelemetryUpload& p)
{
	TelemetryJob *job = (TelemetryJob *)calloc(1, sizeof(TelemetryJob));
	if (job == nullptr)
		return;
	job->seed = p.seed;

	// Stats: snapshot the gzipped JSON file into memory.
	if (!p.statsUrl.isEmpty() && !p.statsFilePath.isEmpty())
	{
		unsigned char *b = nullptr;
		unsigned int n = 0;
		if (readWholeFile(p.statsFilePath.str(), &b, &n))
		{
			copyCStr(job->statsUrl, sizeof(job->statsUrl), p.statsUrl.str());
			job->statsBytes = b;
			job->statsLen = n;
			job->haveStats = true;
		}
	}

	// Replay: snapshot the .rep and append the ZUTG upload trailer now, so the
	// on-disk file is free to be overwritten by the next game immediately.
	if (!p.replayUrl.isEmpty() && !p.replayFilePath.isEmpty())
	{
		unsigned char *raw = nullptr;
		unsigned int rawLen = 0;
		if (readWholeFile(p.replayFilePath.str(), &raw, &rawLen))
		{
			unsigned int taggedLen = 0;
			void *tagged = AppendZuluUploadTag(raw, rawLen, &taggedLen);
			free(raw);
			if (tagged != nullptr)
			{
				copyCStr(job->replayUrl, sizeof(job->replayUrl), p.replayUrl.str());
				copyCStr(job->replayFileName, sizeof(job->replayFileName), p.replayFileName.str());
				copyCStr(job->playerNameUtf8, sizeof(job->playerNameUtf8), p.playerNameUtf8.str());
				job->replayBytes = (unsigned char *)tagged;
				job->replayLen = taggedLen;
				job->haveReplay = true;
			}
		}
	}

	// Logs: snapshot each present file's raw bytes (the worker gzips them). An
	// absent/empty log (e.g. ObserverLog.txt when this client wasn't observing)
	// is simply skipped.
	if (!p.logsUrl.isEmpty() && !p.logFilePaths.empty())
	{
		copyCStr(job->logsUrl, sizeof(job->logsUrl), p.logsUrl.str());
		copyCStr(job->playerId, sizeof(job->playerId), p.playerId.str());

		size_t i;
		const size_t maxLogs = sizeof(job->logs) / sizeof(job->logs[0]);
		for (i = 0; i < p.logFilePaths.size() && job->logCount < maxLogs; ++i)
		{
			if (p.logFilePaths[i].isEmpty())
				continue;

			unsigned char *b = nullptr;
			unsigned int n = 0;
			if (!readLogFileTail(p.logFilePaths[i].str(), LOG_UPLOAD_MAX_BYTES, &b, &n))
				continue;

			TelemetryLogBuf *slot = &job->logs[job->logCount];
			slot->bytes = b;
			slot->len = n;
			sanitizeMultipartFilename(p.logFilePaths[i].str(), slot->name, sizeof(slot->name));
			++job->logCount;
		}
		job->haveLogs = (job->logCount > 0);
	}

	// Map: static assets read lazily by the worker; just carry the identifiers.
	if (!p.mapCheckUrl.isEmpty() && !p.mapFilePath.isEmpty() && p.mapCRC != 0)
	{
		copyCStr(job->mapCheckUrl, sizeof(job->mapCheckUrl), p.mapCheckUrl.str());
		copyCStr(job->mapUploadUrl, sizeof(job->mapUploadUrl), p.mapUploadUrl.str());
		copyCStr(job->mapFilePath, sizeof(job->mapFilePath), p.mapFilePath.str());
		job->mapCRC = p.mapCRC;
		job->mapContentsMask = p.mapContentsMask;
		job->haveMap = true;
	}

	// Nothing survived the snapshot step? Don't spawn a worker.
	if (!job->haveStats && !job->haveReplay && !job->haveLogs && !job->haveMap)
	{
		freeTelemetryJob(job);
		return;
	}

	// _beginthreadex returns the handle as an integer type; VC6 has no
	// uintptr_t, so capture it straight into a HANDLE.
	unsigned threadId = 0;
	HANDLE hThread = (HANDLE)_beginthreadex(nullptr, 0, telemetryThreadProc, job, 0, &threadId);
	if (hThread == NULL)
	{
		// Couldn't spawn; telemetry is best-effort, so just drop it.
		freeTelemetryJob(job);
		return;
	}
	CloseHandle(hThread); // detached; the worker frees the job when it finishes
}

void StartDiagnosticLogUpload(const AsciiString& logsUrl,
                              const AsciiString& seedLabel,
                              const AsciiString& playerId,
                              const std::vector<AsciiString>& logFilePaths)
{
	if (logsUrl.isEmpty() || seedLabel.isEmpty() || logFilePaths.empty())
		return;

	TelemetryJob *job = (TelemetryJob *)calloc(1, sizeof(TelemetryJob));
	if (job == nullptr)
		return;

	copyCStr(job->logsUrl, sizeof(job->logsUrl), logsUrl.str());
	copyCStr(job->playerId, sizeof(job->playerId), playerId.str());
	// The label lands in an HTTP header and, server-side, in a directory
	// name; sanitize it exactly like the player id.
	sanitizeHeaderToken(seedLabel, job->seedLabel, sizeof(job->seedLabel));
	if (job->seedLabel[0] == '\0')
	{
		freeTelemetryJob(job);
		return;
	}

	size_t i;
	const size_t maxLogs = sizeof(job->logs) / sizeof(job->logs[0]);
	for (i = 0; i < logFilePaths.size() && job->logCount < maxLogs; ++i)
	{
		if (logFilePaths[i].isEmpty())
			continue;

		unsigned char *b = nullptr;
		unsigned int n = 0;
		if (!readLogFileTail(logFilePaths[i].str(), LOG_UPLOAD_MAX_BYTES, &b, &n))
			continue;

		TelemetryLogBuf *slot = &job->logs[job->logCount];
		slot->bytes = b;
		slot->len = n;
		sanitizeMultipartFilename(logFilePaths[i].str(), slot->name, sizeof(slot->name));
		++job->logCount;
	}
	job->haveLogs = (job->logCount > 0);
	if (!job->haveLogs)
	{
		freeTelemetryJob(job);
		return;
	}

	unsigned threadId = 0;
	HANDLE hThread = (HANDLE)_beginthreadex(nullptr, 0, telemetryThreadProc, job, 0, &threadId);
	if (hThread == NULL)
	{
		freeTelemetryJob(job);
		return;
	}
	CloseHandle(hThread); // detached; the worker frees the job when it finishes
}

// ---------------------------------------------------------------------------
// Background (non-blocking) map download.
//
// The same fetch DownloadAndInstallMap does, split across a thread boundary:
// the worker only does the WinINet round-trips (into malloc'd buffers), and
// the main thread's MapDownloadPoll() CRC-verifies, writes to disk and
// refreshes MapCache. TheFileSystem and TheMapCache are therefore still only
// ever touched on the main thread.
//
// The lobby needs this: DownloadAndInstallMap blocks for as long as the CDN
// takes, and the lobby callsites run on the thread that also services the LAN
// heartbeat, so a slow download freezes the UI and can get the peer dropped
// from the game.
//
// Mirrors the telemetry worker: the job is a plain-C POD, so no AsciiString
// buffer is ever shared across the thread boundary.
// ---------------------------------------------------------------------------

struct MapDownloadJob
{
	char        downloadUrl[512];
	char        localMapPath[512];
	UnsignedInt mapCRC;
	UnsignedInt contentsMask;

	// Written by the worker; owned by the main thread once it takes the job.
	bool         fetched;    // the .map came down (CRC not checked yet)
	void        *mapData;
	unsigned int mapLen;
	void        *sidecarData[MAP_SIDECAR_COUNT];
	unsigned int sidecarLen[MAP_SIDECAR_COUNT];
};

static CRITICAL_SECTION s_mapDlCS;
static bool s_mapDlCSInit = false;             // only touched from the main thread
static bool s_mapDlPending = false;            // worker running (guarded)
static MapDownloadJob *s_mapDlDone = nullptr;  // finished job awaiting a poll (guarded)

static void ensureMapDlCS(void)
{
	// Only ever called from the main thread (MapDownloadStart / MapDownloadPoll,
	// both of which run before any worker can touch the section), so this lazy
	// init needs no lock of its own.
	if (!s_mapDlCSInit)
	{
		InitializeCriticalSection(&s_mapDlCS);
		s_mapDlCSInit = true;
	}
}

static void freeMapDownloadJob(MapDownloadJob *job)
{
	if (job == nullptr)
		return;
	if (job->mapData != nullptr)
		free(job->mapData);
	Int i;
	for (i = 0; i < MAP_SIDECAR_COUNT; ++i)
	{
		if (job->sidecarData[i] != nullptr)
			free(job->sidecarData[i]);
	}
	free(job);
}

static unsigned __stdcall mapDownloadThreadProc(void *arg)
{
	MapDownloadJob *job = (MapDownloadJob *)arg;
	AsciiString url(job->downloadUrl); // worker-local; never crosses back

	void *data = nullptr;
	unsigned int len = 0;
	if (DownloadMapAssetFromServer(url, job->mapCRC, "map", 0, &data, &len))
	{
		job->mapData = data;
		job->mapLen = len;
		job->fetched = true;

		// Sidecars are best-effort, exactly as in the blocking path: a 404
		// just means the server never got that one.
		Int i;
		for (i = 0; i < MAP_SIDECAR_COUNT; ++i)
		{
			if ((job->contentsMask & s_mapSidecars[i].mask) == 0)
				continue;
			void *sd = nullptr;
			unsigned int sl = 0;
			if (DownloadMapAssetFromServer(url, job->mapCRC, s_mapSidecars[i].kind, 0, &sd, &sl))
			{
				job->sidecarData[i] = sd;
				job->sidecarLen[i] = sl;
			}
		}
	}

	EnterCriticalSection(&s_mapDlCS);
	s_mapDlDone = job; // the main thread owns the buffers from here on
	s_mapDlPending = false;
	LeaveCriticalSection(&s_mapDlCS);
	return 0;
}

Bool MapDownloadStart(const AsciiString& localMapPath,
                      UnsignedInt mapCRC,
                      UnsignedInt contentsMask)
{
	if (mapCRC == 0 || localMapPath.isEmpty())
		return FALSE;
	if (TheGlobalData == nullptr || TheGlobalData->m_mapDownloadUrl.isEmpty())
		return FALSE;

	ensureMapDlCS();

	// Claim the slot. A result nobody polled for (the user left the lobby
	// mid-download) is dropped here rather than leaked.
	EnterCriticalSection(&s_mapDlCS);
	bool busy = s_mapDlPending;
	MapDownloadJob *stale = nullptr;
	if (!busy)
	{
		stale = s_mapDlDone;
		s_mapDlDone = nullptr;
		s_mapDlPending = true;
	}
	LeaveCriticalSection(&s_mapDlCS);

	freeMapDownloadJob(stale);
	if (busy)
		return FALSE;

	MapDownloadJob *job = (MapDownloadJob *)calloc(1, sizeof(MapDownloadJob));
	if (job != nullptr)
	{
		copyCStr(job->downloadUrl, sizeof(job->downloadUrl), TheGlobalData->m_mapDownloadUrl.str());
		copyCStr(job->localMapPath, sizeof(job->localMapPath), localMapPath.str());
		job->mapCRC = mapCRC;
		job->contentsMask = contentsMask;

		// _beginthreadex returns the handle as an integer type; VC6 has no
		// uintptr_t, so capture it straight into a HANDLE.
		unsigned threadId = 0;
		HANDLE hThread = (HANDLE)_beginthreadex(nullptr, 0, mapDownloadThreadProc, job, 0, &threadId);
		if (hThread != NULL)
		{
			CloseHandle(hThread); // detached; it hands the job back through s_mapDlDone
			printf("[map] Downloading map crc=%u -> \"%s\" (mask=%u)\n",
				mapCRC, localMapPath.str(), contentsMask);
			fflush(stdout);
			return TRUE;
		}
		freeMapDownloadJob(job);
	}

	// Couldn't allocate or spawn: release the slot again.
	EnterCriticalSection(&s_mapDlCS);
	s_mapDlPending = false;
	LeaveCriticalSection(&s_mapDlCS);
	return FALSE;
}

MapDownloadStatus MapDownloadPoll(void)
{
	ensureMapDlCS();

	EnterCriticalSection(&s_mapDlCS);
	bool pending = s_mapDlPending;
	MapDownloadJob *job = s_mapDlDone;
	s_mapDlDone = nullptr;
	LeaveCriticalSection(&s_mapDlCS);

	if (job == nullptr)
		return pending ? MAPDOWNLOAD_PENDING : MAPDOWNLOAD_IDLE;

	MapDownloadStatus status = MAPDOWNLOAD_FAILED;
	if (job->fetched && job->mapData != nullptr && job->mapLen > 0)
	{
		// Validate the bytes against the CRC the server advertised before we
		// commit anything to disk. cncstats has no upload auth, so a malicious
		// actor could overwrite a popular CRC with arbitrary content; this
		// check is the only thing keeping us safe.
		CRC theCRC;
		theCRC.clear();
		theCRC.computeCRC(job->mapData, (Int)job->mapLen);
		UnsignedInt actualCRC = theCRC.get();
		if (actualCRC != job->mapCRC)
		{
			printf("[map] Downloaded map crc mismatch: expected %u, got %u (rejecting)\n",
				job->mapCRC, actualCRC);
			fflush(stdout);
		}
		else
		{
			AsciiString localPath(job->localMapPath);
			void *mapData = job->mapData;
			unsigned int mapLen = job->mapLen;
			job->mapData = nullptr; // writeAssetToDisk frees it, pass or fail
			job->mapLen = 0;

			if (writeAssetToDisk(localPath, mapData, mapLen))
			{
				printf("[map] Installed downloaded map \"%s\" (crc=%u, %u bytes)\n",
					localPath.str(), job->mapCRC, mapLen);
				fflush(stdout);

				Int i;
				for (i = 0; i < MAP_SIDECAR_COUNT; ++i)
				{
					if (job->sidecarData[i] == nullptr)
						continue;
					void *sd = job->sidecarData[i];
					unsigned int sl = job->sidecarLen[i];
					job->sidecarData[i] = nullptr; // ownership moves to writeAssetToDisk
					AsciiString sidecarPath = mapSidecarPath(i, localPath);
					if (sidecarPath.isEmpty())
					{
						free(sd);
						continue;
					}
					if (writeAssetToDisk(sidecarPath, sd, sl))
					{
						printf("[map] Installed %s sidecar \"%s\" (%u bytes)\n",
							s_mapSidecars[i].kind, sidecarPath.str(), sl);
						fflush(stdout);
					}
				}

				// Refresh MapCache so findMap() and getMapPreviewImage() see the
				// new entry without a game restart.
				if (TheMapCache != nullptr)
					TheMapCache->refreshUserMaps();

				status = MAPDOWNLOAD_INSTALLED;
			}
		}
	}

	freeMapDownloadJob(job);
	return status;
}
