/* gthread stubs, weak: libmocha.a (wiiu-env) references __gthread_mutex_* which this newlib does not ship.
 * Single-threaded Wii U homebrew: the mutexes are never contended, so these are no-ops. Weak, so a toolchain
 * that does provide them keeps its own. */
__attribute__((weak)) int __gthread_active(void) { return 0; }
__attribute__((weak)) int __gthread_mutex_lock(void *m) { (void)m; return 0; }
__attribute__((weak)) int __gthread_mutex_trylock(void *m) { (void)m; return 0; }
__attribute__((weak)) int __gthread_mutex_unlock(void *m) { (void)m; return 0; }
__attribute__((weak)) int __gthread_mutex_init_function(void *m) { (void)m; return 0; }
__attribute__((weak)) int __gthread_mutex_destroy(void *m) { (void)m; return 0; }
__attribute__((weak)) int __gthread_recursive_mutex_lock(void *m) { (void)m; return 0; }
__attribute__((weak)) int __gthread_recursive_mutex_trylock(void *m) { (void)m; return 0; }
__attribute__((weak)) int __gthread_recursive_mutex_unlock(void *m) { (void)m; return 0; }
__attribute__((weak)) int __gthread_recursive_mutex_init_function(void *m) { (void)m; return 0; }
__attribute__((weak)) int __gthread_recursive_mutex_destroy(void *m) { (void)m; return 0; }
__attribute__((weak)) int __gthread_key_create(void *k, void (*d)(void *)) { (void)k; (void)d; return 0; }
__attribute__((weak)) int __gthread_key_delete(void *k) { (void)k; return 0; }
__attribute__((weak)) int __gthread_key_set(void *k, void *v) { (void)k; (void)v; return 0; }
__attribute__((weak)) void *__gthread_key_get(void *k) { (void)k; return 0; }
__attribute__((weak)) int __gthread_once(void *o, void (*f)(void)) { (void)o; if (f) f(); return 0; }
