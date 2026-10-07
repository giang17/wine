/*
 * Subject interface package for Authenticode-signed PowerShell scripts
 *
 * Copyright 2026 Giang Nguyen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* A signed script carries its PKCS#7 signature as base64 text in a trailing
 * comment block:
 *
 *   # SIG # Begin signature block
 *   # MIIxxxx...
 *   # SIG # End signature block
 *
 * The signature's indirect data hashes the script text that precedes the
 * block, minus the one line break separating the two, encoded as UTF-16LE
 * regardless of the file's own encoding.
 */

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "wincrypt.h"
#include "mssip.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(pwrshsip);

static GUID subject_guid = { 0x603bcc1f, 0x4b59, 0x4e08, { 0xb7,0x24,0xd2,0xc6,0x29,0x7e,0xf3,0x51 }};

static const WCHAR begin_marker[] = L"# SIG # Begin signature block";
static const WCHAR end_marker[] = L"# SIG # End signature block";

/***********************************************************************
 *              DllRegisterServer (PWRSHSIP.@)
 */
HRESULT WINAPI DllRegisterServer(void)
{
    static WCHAR dll[] = L"pwrshsip.dll";
    static WCHAR get_signature[] = L"PsGetSignature";
    static WCHAR put_signature[] = L"PsPutSignature";
    static WCHAR create_hash[] = L"PsCreateHash";
    static WCHAR verify_hash[] = L"PsVerifyHash";
    static WCHAR remove_signature[] = L"PsRemoveSignature";
    static WCHAR is_my_file_type[] = L"PsIsMyFileType";
    SIP_ADD_NEWPROVIDER prov;

    memset(&prov, 0, sizeof(prov));
    prov.cbStruct = sizeof(prov);
    prov.pwszDLLFileName = dll;
    prov.pgSubject = &subject_guid;
    prov.pwszGetFuncName = get_signature;
    prov.pwszPutFuncName = put_signature;
    prov.pwszCreateFuncName = create_hash;
    prov.pwszVerifyFuncName = verify_hash;
    prov.pwszRemoveFuncName = remove_signature;
    prov.pwszIsFunctionNameFmt2 = is_my_file_type;
    return CryptSIPAddProvider(&prov) ? S_OK : S_FALSE;
}

/***********************************************************************
 *              DllUnregisterServer (PWRSHSIP.@)
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    CryptSIPRemoveProvider(&subject_guid);
    return S_OK;
}

/***********************************************************************
 *              PsIsMyFileType (PWRSHSIP.@)
 */
BOOL WINAPI PsIsMyFileType(WCHAR *name, GUID *subject)
{
    static const WCHAR * const extensions[] = { L".ps1", L".psm1", L".psd1" };
    const WCHAR *ext;
    unsigned int i;

    TRACE("(%s, %p)\n", debugstr_w(name), subject);

    if (!name || !subject)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(ext = wcsrchr(name, '.')) || wcspbrk(ext, L"\\/")) return FALSE;
    for (i = 0; i < ARRAY_SIZE(extensions); i++)
    {
        if (!wcsicmp(ext, extensions[i]))
        {
            *subject = subject_guid;
            return TRUE;
        }
    }
    return FALSE;
}

/* Reads the whole script as UTF-16 text, which is what the signature covers.
 * A UTF-8 or UTF-16LE byte order mark selects the file's encoding; anything
 * else is read as ANSI, like Windows PowerShell does. */
static WCHAR *read_script(SIP_SUBJECTINFO *info, DWORD *len)
{
    LARGE_INTEGER zero = {{0}}, saved, size;
    HANDLE file = info->hFile;
    BOOL close_file = FALSE;
    WCHAR *text = NULL;
    BYTE *data = NULL;
    DWORD read, count, cp = CP_ACP, skip = 0;

    if (!file || file == INVALID_HANDLE_VALUE)
    {
        if (!info->pwsFileName)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }
        file = CreateFileW(info->pwsFileName, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return NULL;
        close_file = TRUE;
    }
    else if (!SetFilePointerEx(file, zero, &saved, FILE_CURRENT)) return NULL;

    if (!GetFileSizeEx(file, &size) || size.HighPart || !size.LowPart ||
        !SetFilePointerEx(file, zero, NULL, FILE_BEGIN))
        goto done;
    if (!(data = malloc(size.LowPart))) goto done;
    if (!ReadFile(file, data, size.LowPart, &read, NULL) || read != size.LowPart) goto done;

    if (read >= 2 && data[0] == 0xff && data[1] == 0xfe)
    {
        count = (read - 2) / sizeof(WCHAR);
        if (!(text = malloc((count + 1) * sizeof(WCHAR)))) goto done;
        memcpy(text, data + 2, count * sizeof(WCHAR));
    }
    else
    {
        if (read >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
        {
            cp = CP_UTF8;
            skip = 3;
        }
        if (!(count = MultiByteToWideChar(cp, 0, (char *)data + skip, read - skip, NULL, 0))) goto done;
        if (!(text = malloc((count + 1) * sizeof(WCHAR)))) goto done;
        MultiByteToWideChar(cp, 0, (char *)data + skip, read - skip, text, count);
    }
    text[count] = 0;
    *len = count;

done:
    free(data);
    if (close_file) CloseHandle(file);
    else SetFilePointerEx(file, saved, NULL, FILE_BEGIN);
    return text;
}

/* Locates the signature block. On success *text_len is the length of the
 * script text the signature covers, and the base64 lines between the two
 * markers are returned as a single string without their comment prefixes. */
static WCHAR *find_signature(const WCHAR *text, DWORD len, DWORD *text_len)
{
    const WCHAR *begin = text, *end, *line;
    WCHAR *base64, *dst;

    for (;;)
    {
        if (!(begin = wcsstr(begin, begin_marker))) return NULL;
        if (begin == text || begin[-1] == '\n') break;
        begin += ARRAY_SIZE(begin_marker) - 1;
    }
    if (!(end = wcsstr(begin, end_marker))) return NULL;

    /* The block is separated from the script by one line break, which the
     * signature does not cover. */
    *text_len = begin - text;
    if (*text_len >= 2 && text[*text_len - 2] == '\r' && text[*text_len - 1] == '\n') *text_len -= 2;
    else if (*text_len >= 1 && text[*text_len - 1] == '\n') *text_len -= 1;

    if (!(base64 = malloc((end - begin + 1) * sizeof(WCHAR)))) return NULL;
    dst = base64;
    line = begin + ARRAY_SIZE(begin_marker) - 1;
    while (line < end)
    {
        while (line < end && (*line == '\r' || *line == '\n')) line++;
        if (line < end && *line == '#') line++;
        if (line < end && *line == ' ') line++;
        while (line < end && *line != '\r' && *line != '\n') *dst++ = *line++;
    }
    *dst = 0;
    return base64;
}

/***********************************************************************
 *              PsGetSignature (PWRSHSIP.@)
 */
BOOL WINAPI PsGetSignature(SIP_SUBJECTINFO *info, DWORD *encoding, DWORD index,
                           DWORD *size, BYTE *data)
{
    WCHAR *text, *base64 = NULL;
    DWORD len, text_len, needed;
    BOOL ret = FALSE;

    TRACE("(%p %p %lu %p %p)\n", info, encoding, index, size, data);

    if (!info || !size || index)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(text = read_script(info, &len))) return FALSE;
    if (!(base64 = find_signature(text, len, &text_len)) ||
        !CryptStringToBinaryW(base64, 0, CRYPT_STRING_BASE64, NULL, &needed, NULL, NULL))
    {
        TRACE("no signature block in %s\n", debugstr_w(info->pwsFileName));
        SetLastError(TRUST_E_NOSIGNATURE);
        goto done;
    }
    if (!data)
    {
        *size = needed;
        ret = TRUE;
    }
    else if (*size < needed)
    {
        *size = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
    }
    else
        ret = CryptStringToBinaryW(base64, 0, CRYPT_STRING_BASE64, data, size, NULL, NULL);

    if (ret && encoding) *encoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

done:
    free(base64);
    free(text);
    return ret;
}

/***********************************************************************
 *              PsVerifyHash (PWRSHSIP.@)
 */
BOOL WINAPI PsVerifyHash(SIP_SUBJECTINFO *info, SIP_INDIRECT_DATA *indirect)
{
    HCRYPTPROV prov;
    HCRYPTHASH hash = 0;
    BOOL release_prov = FALSE, ret = FALSE;
    WCHAR *text = NULL, *base64;
    DWORD len, text_len, digest_len;
    BYTE digest[64];
    ALG_ID alg;

    TRACE("(%p %p)\n", info, indirect);

    if (!info || !indirect)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(alg = CertOIDToAlgId(indirect->DigestAlgorithm.pszObjId)))
    {
        WARN("unsupported digest algorithm %s\n", debugstr_a(indirect->DigestAlgorithm.pszObjId));
        SetLastError(NTE_BAD_ALGID);
        return FALSE;
    }
    if (!(text = read_script(info, &len))) return FALSE;
    if (!(base64 = find_signature(text, len, &text_len)))
    {
        SetLastError(TRUST_E_NOSIGNATURE);
        goto done;
    }
    free(base64);

    if (!(prov = info->hProv))
    {
        if (!CryptAcquireContextW(&prov, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
            goto done;
        release_prov = TRUE;
    }
    if (!CryptCreateHash(prov, alg, 0, 0, &hash)) goto done;
    if (!CryptHashData(hash, (const BYTE *)text, text_len * sizeof(WCHAR), 0)) goto done;
    digest_len = sizeof(digest);
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_len, 0)) goto done;

    if (digest_len != indirect->Digest.cbData || memcmp(digest, indirect->Digest.pbData, digest_len))
    {
        WARN("digest mismatch for %s\n", debugstr_w(info->pwsFileName));
        SetLastError(TRUST_E_BAD_DIGEST);
        goto done;
    }
    ret = TRUE;

done:
    if (hash) CryptDestroyHash(hash);
    if (release_prov) CryptReleaseContext(prov, 0);
    free(text);
    return ret;
}

/***********************************************************************
 *              PsCreateHash (PWRSHSIP.@)
 */
BOOL WINAPI PsCreateHash(SIP_SUBJECTINFO *info, DWORD *size, SIP_INDIRECT_DATA *data)
{
    FIXME("(%p %p %p): stub\n", info, size, data);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/***********************************************************************
 *              PsPutSignature (PWRSHSIP.@)
 */
BOOL WINAPI PsPutSignature(SIP_SUBJECTINFO *info, DWORD encoding, DWORD *index,
                           DWORD size, BYTE *data)
{
    FIXME("(%p %lu %p %lu %p): stub\n", info, encoding, index, size, data);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/***********************************************************************
 *              PsRemoveSignature (PWRSHSIP.@)
 */
BOOL WINAPI PsRemoveSignature(SIP_SUBJECTINFO *info, DWORD index)
{
    FIXME("(%p %lu): stub\n", info, index);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
