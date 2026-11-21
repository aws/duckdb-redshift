# The fake driver and its self-tests, built for make test_client. Add files here only.
list(APPEND REDSHIFT_CLIENT_TEST_SOURCES
     test/client/fake_driver/fake_odbc_driver.cpp
     test/client/fake_driver/fake_odbc_driver_test.cpp)
