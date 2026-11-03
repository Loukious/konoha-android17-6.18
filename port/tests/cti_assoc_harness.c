#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
struct coresight_device { int unused; };
struct cti_assoc_op {
    void (*add)(struct coresight_device *);
    void (*remove)(struct coresight_device *);
};
static _Atomic(const struct cti_assoc_op *) cti_assoc_ops;
static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned readers;
} cti_assoc_srcu = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
static int srcu_read_lock(void *unused) {
    (void)unused;
    pthread_mutex_lock(&cti_assoc_srcu.lock);
    cti_assoc_srcu.readers++;
    pthread_mutex_unlock(&cti_assoc_srcu.lock);
    return 0;
}
static void srcu_read_unlock(void *unused, int index) {
    (void)unused; (void)index;
    pthread_mutex_lock(&cti_assoc_srcu.lock);
    assert(cti_assoc_srcu.readers);
    cti_assoc_srcu.readers--;
    pthread_cond_broadcast(&cti_assoc_srcu.changed);
    pthread_mutex_unlock(&cti_assoc_srcu.lock);
}
static void synchronize_srcu(void *unused) {
    (void)unused;
    pthread_mutex_lock(&cti_assoc_srcu.lock);
    while (cti_assoc_srcu.readers)
        pthread_cond_wait(&cti_assoc_srcu.changed, &cti_assoc_srcu.lock);
    pthread_mutex_unlock(&cti_assoc_srcu.lock);
}
#define srcu_dereference(p, s) ((void)(s), atomic_load_explicit(&(p), memory_order_acquire))
#define rcu_assign_pointer(p, v) do { \
    atomic_store_explicit(&(p), (v), memory_order_release); \
    pthread_mutex_lock(&cti_assoc_srcu.lock); \
    pthread_cond_broadcast(&cti_assoc_srcu.changed); \
    pthread_mutex_unlock(&cti_assoc_srcu.lock); \
} while (0)
/* INSERT_FUNCTIONS */
static pthread_mutex_t callback_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t callback_changed = PTHREAD_COND_INITIALIZER;
static bool entered, finish_callback;
static atomic_bool removed;
static unsigned callback_count;
static void blocked_callback(struct coresight_device *dev) {
    (void)dev;
    pthread_mutex_lock(&callback_lock);
    entered = true;
    callback_count++;
    pthread_cond_broadcast(&callback_changed);
    while (!finish_callback)
        pthread_cond_wait(&callback_changed, &callback_lock);
    assert(!atomic_load(&removed));
    pthread_mutex_unlock(&callback_lock);
}
static void *add_thread(void *dev) { coresight_cti_add(dev); return NULL; }
static void *remove_thread(void *dev) { coresight_cti_remove(dev); return NULL; }
static void *unload_thread(void *unused) {
    (void)unused;
    coresight_remove_cti_ops();
    atomic_store(&removed, true);
    return NULL;
}
int main(void) {
    struct coresight_device dev = {0};
    struct cti_assoc_op ops = {.add = blocked_callback, .remove = blocked_callback};
    unsigned cases = 0;
    /* Both association directions must keep the module alive through callback completion. */
    for (unsigned direction = 0; direction < 2; direction++) {
        pthread_t reader, unloader;
        entered = finish_callback = false;
        atomic_store(&removed, false);
        coresight_set_cti_ops(&ops);
        assert(!pthread_create(&reader, NULL, direction ? remove_thread : add_thread, &dev));
        pthread_mutex_lock(&callback_lock);
        while (!entered)
            pthread_cond_wait(&callback_changed, &callback_lock);
        pthread_mutex_unlock(&callback_lock);
        assert(!pthread_create(&unloader, NULL, unload_thread, NULL));
        pthread_mutex_lock(&cti_assoc_srcu.lock);
        while (atomic_load(&cti_assoc_ops))
            pthread_cond_wait(&cti_assoc_srcu.changed, &cti_assoc_srcu.lock);
        assert(cti_assoc_srcu.readers && !atomic_load(&removed));
        pthread_mutex_unlock(&cti_assoc_srcu.lock);
        /* A fresh request sees NULL while the old callback is still draining. */
        coresight_cti_add(&dev);
        coresight_cti_remove(&dev);
        pthread_mutex_lock(&callback_lock);
        finish_callback = true;
        pthread_cond_broadcast(&callback_changed);
        pthread_mutex_unlock(&callback_lock);
        assert(!pthread_join(reader, NULL));
        assert(!pthread_join(unloader, NULL));
        assert(atomic_load(&removed) && !atomic_load(&cti_assoc_ops));
        assert(callback_count == direction + 1);
        cases++;
    }
    struct cti_assoc_op empty = {0};
    coresight_set_cti_ops(&empty);
    coresight_cti_add(&dev);
    coresight_cti_remove(&dev);
    coresight_remove_cti_ops();
    assert(!cti_assoc_srcu.readers && callback_count == 2);
    cases++;
    printf("%u\n", cases);
}
