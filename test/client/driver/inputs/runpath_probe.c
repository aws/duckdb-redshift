/* A library with a run path of its own, for the test that the search list covers the run paths of the
 * object the loader is in (a loadable extension), not only the main program's. The tests never install it. */
int redshift_test_runpath_probe(void) {
	return 0;
}
