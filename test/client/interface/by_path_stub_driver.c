// test/client/interface/by_path_stub_driver.c
//
// The libraries the by-path source's tests load, one per macro below; with none, a library that
// exports every entry point the driver interface names.

// Defines no entry point and its own probe, so a test sees which definition a loaded stub binds to.
#ifdef REDSHIFT_STUB_INTERPOSER

int redshift_stub_probe(void) {
	return 2;
}

#else

// Repeated, not taken from REDSHIFT_ODBC_ENTRY_POINTS, whose header is C++ with the real signatures;
// the test "loads a library that exports every entry point" fails when one is missing here.
#define REDSHIFT_STUB_ENTRY_POINTS(X)                                                                                  \
	X(FreeHandle)                                                                                                      \
	X(SetEnvAttr)                                                                                                      \
	X(SetConnectAttr)                                                                                                  \
	X(GetConnectAttr)                                                                                                  \
	X(DriverConnect)                                                                                                   \
	X(Disconnect)                                                                                                      \
	X(EndTran)                                                                                                         \
	X(GetInfo)                                                                                                         \
	X(Prepare)                                                                                                         \
	X(NumParams)                                                                                                       \
	X(NumResultCols)                                                                                                   \
	X(DescribeCol)                                                                                                     \
	X(ColAttribute)                                                                                                    \
	X(BindParameter)                                                                                                   \
	X(Execute)                                                                                                         \
	X(ExecDirect)                                                                                                      \
	X(RowCount)                                                                                                        \
	X(MoreResults)                                                                                                     \
	X(SetStmtAttr)                                                                                                     \
	X(BindCol)                                                                                                         \
	X(Fetch)                                                                                                           \
	X(SetPos)                                                                                                          \
	X(GetData)                                                                                                         \
	X(FreeStmt)                                                                                                        \
	X(CloseCursor)                                                                                                     \
	X(GetDiagRec)                                                                                                      \
	X(GetDiagField)

// The tests resolve these by name and never call them; each reports failure (SQL_ERROR) regardless.
#define REDSHIFT_STUB_FUNCTION(name)                                                                                   \
	int SQL##name(void) {                                                                                              \
		return -1;                                                                                                     \
	}

REDSHIFT_STUB_ENTRY_POINTS(REDSHIFT_STUB_FUNCTION)

// Left out of one stub, so a test sees a library the extension cannot use.
#ifndef REDSHIFT_STUB_WITHOUT_ALLOC_HANDLE
REDSHIFT_STUB_FUNCTION(AllocHandle)
#endif

// The stub's own probe answers 1 and the interposer's 2, so which one the call below reaches shows
// whether the loader searched the stub's own definitions before the global scope. Weak, so no
// compiler may inline or constant-fold the call: Clang assumes no interposition otherwise.
__attribute__((noinline, weak)) int redshift_stub_probe(void) {
	return 1;
}

int redshift_stub_bound_probe(void) {
	return redshift_stub_probe();
}

// A function nothing defines, so a load that binds every symbol at once fails.
#ifdef REDSHIFT_STUB_WITH_UNRESOLVED_SYMBOL
int redshift_stub_unresolved_symbol(void);

int redshift_stub_call_unresolved(void) {
	return redshift_stub_unresolved_symbol();
}
#endif

// A call into the library above, built as this one's dependency, so loading this one fails there.
#ifdef REDSHIFT_STUB_WITH_FAILING_DEPENDENCY
int redshift_stub_call_unresolved(void);

int redshift_stub_call_dependency(void) {
	return redshift_stub_call_unresolved();
}
#endif

#endif
