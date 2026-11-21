/* A library that test inputs link against and the tests never install, so the platform loader
 * reports it missing, as it would a host prerequisite. */
int redshift_test_absent_symbol(void) {
	return 0;
}
