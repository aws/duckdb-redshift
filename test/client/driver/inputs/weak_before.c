/* The weak-definition test's library loaded before the driver (weak_host.cpp, macos-hosts.sh): it defines,
 * weak, one of the driver's weak-bound names that the driver also exports, an AWS SDK template instance that
 * neither the test's handles nor a password connect reach, which macos-hosts.sh picks from the driver's export
 * list and passes as REDSHIFT_WEAK_SYMBOL (its Mach-O name). Its definition records that it ran, and
 * redshift_weak_before_call calls it through this library's own bind: 1 when that reached this definition. */
static int g_called;

__attribute__((weak, visibility("default"))) void redshift_weak_before(void *self,
                                                                       const void *value) __asm__(REDSHIFT_WEAK_SYMBOL);
void redshift_weak_before(void *self, const void *value) {
	(void)self;
	(void)value;
	g_called = 1;
}

void (*volatile redshift_weak_before_slot)(void *, const void *) = redshift_weak_before;

int redshift_weak_before_call(void) {
	g_called = 0;
	redshift_weak_before_slot((void *)0, (const void *)0);
	return g_called;
}
