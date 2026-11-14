#ifndef LUNA_CURL_H
#define LUNA_CURL_H
/* libcurl-compatible surface for LunaOS guest programs.
 *
 * This is not libcurl. It is the subset the upstream Git HTTP transport
 * (http.c, remote-curl.c, http-walker.c) needs, implemented on top of the
 * LunaOS guest stream/TLS backend. The declared version number selects the
 * same upstream feature set that Git 2.56 supports with curl 7.66.0, so
 * git-curl-compat.h enables exactly the code paths we implement.
 *
 * TLS is terminated and verified by the LunaOS kernel (BearSSL chain,
 * hostname and validity checks, no insecure bypass), so the options libcurl
 * would use to configure TLS are accepted and ignored here.
 */
#include <stddef.h>
#include <sys/select.h>
#include <sys/types.h>
#include <time.h>

#define LIBCURL_VERSION_NUM 0x074200
#define LIBCURL_VERSION "7.66.0"

typedef void CURL;
typedef void CURLM;
typedef void CURLU;
typedef int CURLcode;
typedef int CURLMcode;
typedef int CURLUcode;
typedef long long curl_off_t;

#define CURL_ERROR_SIZE 256

/* Result codes. Values match libcurl so error strings stay comparable. */
#define CURLE_OK 0
#define CURLE_UNSUPPORTED_PROTOCOL 1
#define CURLE_FAILED_INIT 2
#define CURLE_URL_MALFORMAT 3
#define CURLE_NOT_BUILT_IN 4
#define CURLE_COULDNT_RESOLVE_PROXY 5
#define CURLE_COULDNT_RESOLVE_HOST 6
#define CURLE_COULDNT_CONNECT 7
#define CURLE_WEIRD_SERVER_REPLY 8
#define CURLE_HTTP_RETURNED_ERROR 22
#define CURLE_PARTIAL_FILE 18
#define CURLE_WRITE_ERROR 23
#define CURLE_READ_ERROR 26
#define CURLE_OUT_OF_MEMORY 27
#define CURLE_OPERATION_TIMEDOUT 28
#define CURLE_SSL_CONNECT_ERROR 35
#define CURLE_FILE_COULDNT_READ_FILE 37
#define CURLE_FTP_COULDNT_RETR_FILE 46
#define CURLE_TOO_MANY_REDIRECTS 47
#define CURLE_UNKNOWN_OPTION 48
#define CURLE_SETOPT_OPTION_SYNTAX 49
#define CURLE_GOT_NOTHING 52
#define CURLE_SEND_ERROR 55
#define CURLE_RECV_ERROR 56
#define CURLE_SSL_CERTPROBLEM 58
#define CURLE_LOGIN_DENIED 67
#define CURLE_SSL_CACERT_BADFILE 77
#define CURLE_SSL_PINNEDPUBKEYNOTMATCH 90
#define CURLE_BAD_FUNCTION_ARGUMENT 43
#define CURLE_ABORTED_BY_CALLBACK 42

/* Multi interface codes. */
#define CURLM_CALL_MULTI_PERFORM (-1)
#define CURLM_OK 0
#define CURLM_BAD_HANDLE 2
#define CURLM_BAD_EASY_HANDLE 3
#define CURLM_OUT_OF_MEMORY 5
#define CURLM_INTERNAL_ERROR 6
#define CURLM_BAD_SOCKET 7
#define CURLM_UNKNOWN_OPTION 8
#define CURLM_ADDED_ALREADY 9
#define CURLM_LAST 100

typedef enum {
    CURLMSG_NONE,
    CURLMSG_DONE,
    CURLMSG_LAST
} CURLMSG;

typedef struct CURLMsg {
    CURLMSG msg;
    CURL *easy_handle;
    union {
        void *whatever;
        CURLcode result;
    } data;
} CURLMsg;

struct curl_slist {
    char *data;
    struct curl_slist *next;
};

/* Authentication methods and masks. */
#define CURLAUTH_BASIC (1 << 0)
#define CURLAUTH_DIGEST (1 << 1)
#define CURLAUTH_NEGOTIATE (1 << 2)
#define CURLAUTH_GSSNEGOTIATE (1 << 2)
#define CURLAUTH_NTLM (1 << 3)
#define CURLAUTH_DIGEST_IE (1 << 4)
#define CURLAUTH_ANY (~CURLAUTH_DIGEST)
#define CURLAUTH_ANYSAFE (CURLAUTH_BASIC | CURLAUTH_NEGOTIATE | CURLAUTH_NTLM)

#define CURLPROTO_HTTP (1 << 0)
#define CURLPROTO_HTTPS (1 << 1)
#define CURLPROTO_FTP (1 << 2)
#define CURLPROTO_FTPS (1 << 3)
#define CURLPROTO_SOCKS (1 << 4)

#define CURL_NETRC_OPTIONAL 1
#define CURL_NETRC_IGNORED 2
#define CURL_NETRC_REQUIRED 3

#define CURL_GLOBAL_SSL (1 << 0)
#define CURL_GLOBAL_WIN32 (1 << 1)
#define CURL_GLOBAL_ALL (CURL_GLOBAL_SSL | CURL_GLOBAL_WIN32)
#define CURL_GLOBAL_NOTHING 0
#define CURL_GLOBAL_DEFAULT CURL_GLOBAL_ALL

#define CURL_HTTP_VERSION_NONE 0
#define CURL_HTTP_VERSION_1_0 1
#define CURL_HTTP_VERSION_1_1 2
#define CURL_HTTP_VERSION_2_0 3
#define CURL_HTTP_VERSION_2 CURL_HTTP_VERSION_2_0
#define CURL_HTTP_VERSION_2TLS 4
#define CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE 5

#define CURLUSESSL_NONE 0
#define CURLUSESSL_TRY 1
#define CURLUSESSL_CONTROL 2
#define CURLUSESSL_ALL 3

#define CURL_SSLVERSION_DEFAULT 0
#define CURL_SSLVERSION_TLSv1 1
#define CURL_SSLVERSION_SSLv2 2
#define CURL_SSLVERSION_SSLv3 3
#define CURL_SSLVERSION_TLSv1_0 4
#define CURL_SSLVERSION_TLSv1_1 5
#define CURL_SSLVERSION_TLSv1_2 6
#define CURL_SSLVERSION_TLSv1_3 7
#define CURL_SSLVERSION_SSL_LAST (1 << 16)
#define CURL_SSLVERSION_TLS_LAST (1 << 17)
#define CURL_SSLVERSION_MAX_NONE (1 << 16)
#define CURL_SSLVERSION_MAX_DEFAULT (1 << 17)
#define CURL_SSLVERSION_MAX_TLSv1_0 (1 << 18)
#define CURL_SSLVERSION_MAX_TLSv1_1 (1 << 19)
#define CURL_SSLVERSION_MAX_TLSv1_2 (1 << 20)
#define CURL_SSLVERSION_MAX_TLSv1_3 (1 << 21)

#define CURLSSLOPT_ALLOW_BEAST 1
#define CURLSSLOPT_NO_REVOKE 2
#define CURLSSLOPT_NO_PARTIALCHAIN 4
#define CURLSSLOPT_REVOKE_BEST_EFFORT 8
#define CURLSSLOPT_NO_AUTO_V4 16
#define CURLSSLOPT_NO_AUTO_V6 32
#define CURLSSLOPT_NO_HOSTNAME_VERIFY 256

#define CURL_IPRESOLVE_WHATEVER 0
#define CURL_IPRESOLVE_V4 1
#define CURL_IPRESOLVE_V6 2

#define CURLPROXY_HTTP 0
#define CURLPROXY_HTTPS 2
#define CURLPROXY_SOCKS4 4
#define CURLPROXY_SOCKS5 5
#define CURLPROXY_SOCKS5_HOSTNAME 5
#define CURLPROXY_SOCKS4A 6

#define CURL_REDIR_POST_301 CURL_REDIR_POST_ALL
#define CURL_REDIR_POST_302 CURL_REDIR_POST_ALL
#define CURL_REDIR_POST_303 0
#define CURL_REDIR_POST_ALL (1 << 0)
#define CURL_REDIR_POST_307 (1 << 1)
#define CURL_REDIR_POST_308 (1 << 2)

#define CURL_SEEKFUNC_OK 0
#define CURL_SEEKFUNC_CANTSEEK 1
#define CURL_SEEKFUNC_FAIL 2
#define CURL_SEEKFUNC_SAVE 0x10000

typedef enum {
    CURLINFO_TEXT = 0,
    CURLINFO_HEADER_IN,
    CURLINFO_HEADER_OUT,
    CURLINFO_DATA_IN,
    CURLINFO_DATA_OUT,
    CURLINFO_SSL_DATA_IN,
    CURLINFO_SSL_DATA_OUT,
    CURLINFO_END
} curl_infotype;

/* Info values. The grouping mirrors libcurl: the type lives in the high
 * bits and the selector in the low ones. */
#define CURLINFO_STRING 0x100000
#define CURLINFO_LONG 0x200000
#define CURLINFO_DOUBLE 0x300000
#define CURLINFO_MASK 0x0fffff
#define CURLINFO_TYPEMASK 0xf00000

typedef enum {
    CURLINFO_NONE = 0,
    CURLINFO_EFFECTIVE_URL = CURLINFO_STRING + 1,
    CURLINFO_RESPONSE_CODE = CURLINFO_LONG + 2,
    CURLINFO_TOTAL_TIME = CURLINFO_DOUBLE + 3,
    CURLINFO_CONNECT_TIME = CURLINFO_DOUBLE + 5,
    CURLINFO_PRETRANSFER_TIME = CURLINFO_DOUBLE + 6,
    CURLINFO_SIZE_UPLOAD = CURLINFO_DOUBLE + 7,
    CURLINFO_SIZE_DOWNLOAD = CURLINFO_DOUBLE + 8,
    CURLINFO_HEADER_SIZE = CURLINFO_LONG + 11,
    CURLINFO_REQUEST_SIZE = CURLINFO_LONG + 12,
    CURLINFO_SSL_VERIFYRESULT = CURLINFO_LONG + 13,
    CURLINFO_CONTENT_TYPE = CURLINFO_STRING + 18,
    CURLINFO_HTTP_CONNECTCODE = CURLINFO_LONG + 22,
    CURLINFO_HTTPAUTH_AVAIL = CURLINFO_LONG + 23,
    CURLINFO_RETRY_AFTER = CURLINFO_LONG + 24
} CURLINFO;

#define CURLINFO_HTTP_CODE CURLINFO_RESPONSE_CODE

/* Option numbering mirrors libcurl's type ranges so that Git's compile
 * time checks stay meaningful, but the values are private to this library. */
#define CURLOPTTYPE_LONGPOINT 10000
#define CURLOPTTYPE_OBJECTPOINT 20000
#define CURLOPTTYPE_FUNCTIONPOINT 30000
#define CURLOPTTYPE_OFF_T_POINT 40000
#define CURLOPTTYPE_BLOB 40000
#define CURLOPTTYPE_STRINGPOINT 10000
#define CURLOPTTYPE_SLISTPOINT 10000
#define CURLOPTTYPE_CBPOINT 20000

enum {
    CURLOPT_URL = CURLOPTTYPE_STRINGPOINT + 2,
    CURLOPT_PROXY,
    CURLOPT_PROXYPORT,
    CURLOPT_PROXYUSERPWD,
    CURLOPT_PROXYAUTH,
    CURLOPT_PROXYTYPE,
    CURLOPT_PROXYHEADER,
    CURLOPT_PROXY_CAINFO,
    CURLOPT_PROXY_SSLCERT,
    CURLOPT_PROXY_SSLKEY,
    CURLOPT_PROXY_KEYPASSWD,
    CURLOPT_PROXYUSERNAME,
    CURLOPT_PROXYPASSWORD,
    CURLOPT_NOPROXY,
    CURLOPT_USERPWD,
    CURLOPT_USERNAME,
    CURLOPT_PASSWORD,
    CURLOPT_HTTPAUTH,
    CURLOPT_POST,
    CURLOPT_POSTFIELDS,
    CURLOPT_POSTFIELDSIZE,
    CURLOPT_POSTFIELDSIZE_LARGE,
    CURLOPT_HTTPGET,
    CURLOPT_NOBODY,
    CURLOPT_CUSTOMREQUEST,
    CURLOPT_UPLOAD,
    CURLOPT_INFILE,
    CURLOPT_INFILESIZE,
    CURLOPT_INFILESIZE_LARGE,
    CURLOPT_RANGE,
    CURLOPT_USERAGENT,
    CURLOPT_REFERER,
    CURLOPT_COOKIEFILE,
    CURLOPT_COOKIEJAR,
    CURLOPT_NETRC,
    CURLOPT_FOLLOWLOCATION,
    CURLOPT_MAXREDIRS,
    CURLOPT_POSTREDIR,
    CURLOPT_FAILONERROR,
    CURLOPT_ERRORBUFFER,
    CURLOPT_ENCODING,
    CURLOPT_HTTPHEADER,
    CURLOPT_HEADERFUNCTION,
    CURLOPT_WRITEFUNCTION,
    CURLOPT_WRITEDATA,
    CURLOPT_HEADERDATA,
    CURLOPT_READFUNCTION,
    CURLOPT_READDATA,
    CURLOPT_SEEKFUNCTION,
    CURLOPT_SEEKDATA,
    CURLOPT_CAINFO,
    CURLOPT_CAPATH,
    CURLOPT_SSLCERT,
    CURLOPT_SSLCERTTYPE,
    CURLOPT_SSLKEY,
    CURLOPT_SSLKEYTYPE,
    CURLOPT_KEYPASSWD,
    CURLOPT_SSLVERSION,
    CURLOPT_SSL_CIPHER_LIST,
    CURLOPT_SSL_OPTIONS,
    CURLOPT_SSL_VERIFYPEER,
    CURLOPT_SSL_VERIFYHOST,
    CURLOPT_USE_SSL,
    CURLOPT_PINNEDPUBLICKEY,
    CURLOPT_IPRESOLVE,
    CURLOPT_RESOLVE,
    CURLOPT_PROTOCOLS,
    CURLOPT_REDIR_PROTOCOLS,
    CURLOPT_HTTP_VERSION,
    CURLOPT_CONNECTTIMEOUT,
    CURLOPT_TCP_KEEPALIVE,
    CURLOPT_TCP_KEEPIDLE,
    CURLOPT_TCP_KEEPINTVL,
    CURLOPT_LOW_SPEED_LIMIT,
    CURLOPT_LOW_SPEED_TIME,
    CURLOPT_VERBOSE,
    CURLOPT_DEBUGFUNCTION,
    CURLOPT_DEBUGDATA,
    CURLOPT_FTP_USE_EPSV,
    CURLOPT_GSSAPI_DELEGATION,
    CURLOPT_MAIL_FROM,
    CURLOPT_MAIL_RCPT,
    CURLOPT_LASTENTRY
};

typedef size_t (*curl_write_callback)(char *buffer, size_t size,
                                      size_t nitems, void *outstream);
typedef size_t (*curl_read_callback)(char *buffer, size_t size,
                                     size_t nitems, void *instream);
typedef int (*curl_seek_callback)(void *userdata, curl_off_t offset, int origin);
typedef int (*curl_progress_callback)(void *clientp, double dltotal, double dlnow,
                                       double ultotal, double ulnow);
typedef int (*curl_debug_callback)(CURL *handle, curl_infotype type,
                                    char *data, size_t size, void *clientp);

typedef struct {
    const char *name;
    const char *version;
    unsigned int version_num;
    const char *host;
    int features;
    const char *ssl_version;
    long ssl_version_num;
    const char *libz_version;
    const char *protocols;
    const char *ares;
    int ares_num;
    const char *libidn;
    int iconv_ver_num;
    const char *libssh_version;
} curl_version_info_data;

/* Feature bits reported by curl_version_info(). */
#define CURL_VERSION_ASYNCHDNS (1 << 0)
#define CURL_VERSION_IPV6 (1 << 1)
#define CURL_VERSION_SSL (1 << 2)
#define CURL_VERSION_LIBZ (1 << 3)
#define CURL_VERSION_DEBUG (1 << 4)

#define CURLVERSION_NOW 0
#define CURLVERSION_IPV6 1
#define CURLVERSION_SSL 2
#define CURLVERSION_LIBZ 3
#define CURLVERSION_ASYNCHDNS 4
#define CURLVERSION_SPNEGO 5
#define CURLVERSION_KERBEROS4 6
#define CURLVERSION_DEBUG 7
#define CURLVERSION_GSSNEGOTIATE 8
#define CURLVERSION_SSL_SESSION_ID_CACHE 9
#define CURLVERSION_HTTP2 16

struct curl_ssl_backend {
    const char *name;
    const char *version;
    unsigned long version_num;
};
typedef struct curl_ssl_backend curl_ssl_backend;

#define CURLSSLSET_UNKNOWN_BACKEND 0
#define CURLSSLSET_OK 1
#define CURLSSLSET_TOO_LATE 2
#define CURLSSLSET_NO_BACKENDS 3

#define CURLUPART_URL 0
#define CURLUPART_USER 1
#define CURLUPART_PASSWORD 2
#define CURLUPART_OPTIONS 3
#define CURLUPART_HOST 4
#define CURLUPART_PORT 5
#define CURLUPART_PATH 6
#define CURLUPART_QUERY 7
#define CURLUPART_FRAGMENT 8
#define CURLU_NO_AUTHORITY 1
#define CURLU_NON_SUPPORT_SCHEME 2
#define CURLU_URLENCODE 3
#define CURLU_URLDECODE 4
#define CURLU_APPENDQUERY 5
#define CURLUE_OK 0
#define CURLUE_BAD_HANDLE 1
#define CURLUE_BAD_PARTPOINTER 2
#define CURLUE_MALFORMED_INPUT 3
#define CURLUE_BAD_SCHEME 4
#define CURLUE_USER_NOT_ALLOWED 5
#define CURLUE_UNSUPPORTED_SCHEME 6
#define CURLUE_URLDECODE 7
#define CURLUE_OUT_OF_MEMORY 8

CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *handle, int option, ...);
CURLcode curl_easy_perform(CURL *handle);
CURLcode curl_easy_getinfo(CURL *handle, int info, ...);
void curl_easy_cleanup(CURL *handle);
CURL *curl_easy_duphandle(CURL *handle);
const char *curl_easy_strerror(CURLcode code);
CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
CURLMcode curl_global_sslset(long options, const char *name,
                             const curl_ssl_backend ***available);
void curl_free(void *p);
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data);
void curl_slist_free_all(struct curl_slist *list);

CURLM *curl_multi_init(void);
CURLMcode curl_multi_add_handle(CURLM *multi, CURL *handle);
CURLMcode curl_multi_remove_handle(CURLM *multi, CURL *handle);
CURLMcode curl_multi_cleanup(CURLM *multi);
CURLMcode curl_multi_perform(CURLM *multi, int *running_handles);
CURLMcode curl_multi_timeout(CURLM *multi, long *milliseconds);
CURLMcode curl_multi_fdset(CURLM *multi, fd_set *read_fd, fd_set *write_fd,
                            fd_set *exc_fd, int *max_fd);
CURLMsg *curl_multi_info_read(CURLM *multi, int *msgs_in_queue);
const char *curl_multi_strerror(CURLMcode code);

CURLU *curl_url(void);
CURLUcode curl_url_set(CURLU *handle, unsigned int part, const char *content,
                        unsigned int flags);
CURLUcode curl_url_get(CURLU *handle, unsigned int part, char **content,
                        unsigned int flags);
void curl_url_cleanup(CURLU *handle);
curl_version_info_data *curl_version_info(int age);

#endif
