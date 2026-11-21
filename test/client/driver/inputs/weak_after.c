/* The weak-definition test's library loaded after the driver (weak_host.cpp, macos-hosts.sh): it weak-binds
 * one of the driver's exported weak definitions, as another DuckDB extension that bundles the AWS SDK could,
 * by defining it weak itself and taking its address through its own bind. macos-hosts.sh picks the name, an AWS
 * SDK template instance, from the driver's export list and passes it as REDSHIFT_WEAK_SYMBOL (its Mach-O name)
 * and REDSHIFT_WEAK_NAME (dlsym's). redshift_weak_after_target is where that bind went. */
__attribute__((weak, visibility("default"))) void redshift_weak_after(void *self,
                                                                      const void *value) __asm__(REDSHIFT_WEAK_SYMBOL);
void redshift_weak_after(void *self, const void *value) {
	(void)self;
	(void)value;
}

void (*volatile redshift_weak_after_slot)(void *, const void *) = redshift_weak_after;

void *redshift_weak_after_target(void) {
	return (void *)redshift_weak_after_slot;
}

const char *redshift_weak_after_name(void) {
	return REDSHIFT_WEAK_NAME;
}
