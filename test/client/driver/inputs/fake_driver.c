/* A test-input driver for the loader's tests: every entry point the interface names, each
 * returning SQL_ERROR, unless built with OMIT_ENTRY_POINT, which leaves that one out. Its
 * constructor appends a line to the file REDSHIFT_TEST_CONSTRUCTOR_LOG names, so a test can count
 * how many times it ran. Built with RECORD_CALLS it is the isolation tests' decoy: it
 * also exports SQLFreeConnect, the one other SQL* function the locked driver calls through its own
 * PLT (with SQLDisconnect), and each function appends its name to the file REDSHIFT_TEST_DECOY_LOG
 * names, so a test can show the driver never calls into it. */
#if defined(_WIN64)
/* The interface header's Windows x64 route, copied since C cannot include it: windows.h first, then
 * the LLP64 sizes, so this fake's ABI is the loader's. No POSIX header. */
#include <windows.h>
#define SIZEOF_LONG_INT 4
#define HAVE_LONG_LONG  1
#define SQL_WCHART_CONVERT
/* sql.h declares each entry point without dllexport, so a dllexport definition is a different
 * linkage (C2375); the linker exports each by name instead (x64 has no stdcall decoration). */
#define EXPORT_NAME(name) __pragma(comment(linker, "/EXPORT:SQL" #name))
#else
#define SIZEOF_LONG_INT 8
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define EXPORT_NAME(name)
#endif

#include "sql.h"
#include "sqlext.h"

#if defined(_WIN64)
typedef char windows_x64_odbc_abi[(sizeof(SQLLEN) == 8 && sizeof(SQLBIGINT) == 8) ? 1 : -1];

/* DllMain at process attach does what the constructor does elsewhere, reading the environment block. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	char log[MAX_PATH];
	DWORD length = GetEnvironmentVariableA("REDSHIFT_TEST_CONSTRUCTOR_LOG", log, MAX_PATH);
	(void)instance;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH && length > 0 && length < MAX_PATH) {
		HANDLE file = CreateFileA(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
		                          FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD wrote = 0;
		if (file != INVALID_HANDLE_VALUE) {
			WriteFile(file, "run\n", 4, &wrote, NULL);
			CloseHandle(file);
		}
	}
	return TRUE;
}
#else
__attribute__((constructor)) static void count_constructor_run(void) {
	const char *log = getenv("REDSHIFT_TEST_CONSTRUCTOR_LOG");
	if (log != NULL) {
		int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (fd >= 0) {
			(void)!write(fd, "run\n", 4);
			close(fd);
		}
	}
}
#endif

#ifdef RECORD_CALLS
static void record_call(const char *name) {
	const char *log = getenv("REDSHIFT_TEST_DECOY_LOG");
	int fd = log != NULL ? open(log, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
	if (fd >= 0) {
		(void)!write(fd, name, strlen(name));
		(void)!write(fd, "\n", 1);
		close(fd);
	}
}
#define RECORD(name) record_call("SQL" #name)
#else
#define RECORD(name) ((void)0)
#endif

#define STUB(name, ...)                                                                                                \
	EXPORT_NAME(name) SQLRETURN SQL_API SQL##name(__VA_ARGS__) {                                                       \
		RECORD(name);                                                                                                  \
		return SQL_ERROR;                                                                                              \
	}

STUB(AllocHandle, SQLSMALLINT a, SQLHANDLE b, SQLHANDLE *c)
STUB(FreeHandle, SQLSMALLINT a, SQLHANDLE b)
STUB(SetEnvAttr, SQLHENV a, SQLINTEGER b, SQLPOINTER c, SQLINTEGER d)
STUB(SetConnectAttr, SQLHDBC a, SQLINTEGER b, SQLPOINTER c, SQLINTEGER d)
STUB(GetConnectAttr, SQLHDBC a, SQLINTEGER b, SQLPOINTER c, SQLINTEGER d, SQLINTEGER *e)
STUB(DriverConnect, SQLHDBC a, SQLHWND b, SQLCHAR *c, SQLSMALLINT d, SQLCHAR *e, SQLSMALLINT f, SQLSMALLINT *g,
     SQLUSMALLINT h)
STUB(Disconnect, SQLHDBC a)
STUB(EndTran, SQLSMALLINT a, SQLHANDLE b, SQLSMALLINT c)
STUB(GetInfo, SQLHDBC a, SQLUSMALLINT b, SQLPOINTER c, SQLSMALLINT d, SQLSMALLINT *e)
STUB(Prepare, SQLHSTMT a, SQLCHAR *b, SQLINTEGER c)
STUB(NumParams, SQLHSTMT a, SQLSMALLINT *b)
STUB(NumResultCols, SQLHSTMT a, SQLSMALLINT *b)
STUB(DescribeCol, SQLHSTMT a, SQLUSMALLINT b, SQLCHAR *c, SQLSMALLINT d, SQLSMALLINT *e, SQLSMALLINT *f, SQLULEN *g,
     SQLSMALLINT *h, SQLSMALLINT *i)
STUB(ColAttribute, SQLHSTMT a, SQLUSMALLINT b, SQLUSMALLINT c, SQLPOINTER d, SQLSMALLINT e, SQLSMALLINT *f, SQLLEN *g)
STUB(BindParameter, SQLHSTMT a, SQLUSMALLINT b, SQLSMALLINT c, SQLSMALLINT d, SQLSMALLINT e, SQLULEN f, SQLSMALLINT g,
     SQLPOINTER h, SQLLEN i, SQLLEN *j)
STUB(Execute, SQLHSTMT a)
STUB(ExecDirect, SQLHSTMT a, SQLCHAR *b, SQLINTEGER c)
STUB(RowCount, SQLHSTMT a, SQLLEN *b)
STUB(MoreResults, SQLHSTMT a)
STUB(SetStmtAttr, SQLHSTMT a, SQLINTEGER b, SQLPOINTER c, SQLINTEGER d)
STUB(BindCol, SQLHSTMT a, SQLUSMALLINT b, SQLSMALLINT c, SQLPOINTER d, SQLLEN e, SQLLEN *f)
STUB(Fetch, SQLHSTMT a)
STUB(SetPos, SQLHSTMT a, SQLSETPOSIROW b, SQLUSMALLINT c, SQLUSMALLINT d)
STUB(GetData, SQLHSTMT a, SQLUSMALLINT b, SQLSMALLINT c, SQLPOINTER d, SQLLEN e, SQLLEN *f)
STUB(FreeStmt, SQLHSTMT a, SQLUSMALLINT b)
STUB(CloseCursor, SQLHSTMT a)
STUB(GetDiagRec, SQLSMALLINT a, SQLHANDLE b, SQLSMALLINT c, SQLCHAR *d, SQLINTEGER *e, SQLCHAR *f, SQLSMALLINT g,
     SQLSMALLINT *h)
#ifndef OMIT_GET_DIAG_FIELD
STUB(GetDiagField, SQLSMALLINT a, SQLHANDLE b, SQLSMALLINT c, SQLSMALLINT d, SQLPOINTER e, SQLSMALLINT f,
     SQLSMALLINT *g)
#endif

#ifdef RECORD_CALLS
STUB(FreeConnect, SQLHDBC a)
#endif

#ifdef NEEDS_ABSENT_LIBRARY
/* Referenced so the link records the absent library as needed. */
extern int redshift_test_absent_symbol(void);
int redshift_test_use_absent(void) {
	return redshift_test_absent_symbol();
}
#endif
