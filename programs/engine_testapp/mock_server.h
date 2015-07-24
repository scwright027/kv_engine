#ifndef MEMCACHED_MOCK_SERVER_H
#define MEMCACHED_MOCK_SERVER_H

#include <memcached/engine.h>
#include <platform/platform.h>

#ifdef  __cplusplus
extern "C" {
#endif

struct mock_connstruct {
    uint64_t magic;
    const char *uname;
    void *engine_data;
    bool connected;
    struct mock_connstruct *next;
    int sfd;
    ENGINE_ERROR_CODE status;
    uint64_t evictions;
    int nblocks; /* number of ewouldblocks */
    bool handle_ewouldblock;
    bool handle_mutation_extras;
    bool handle_datatype_support;
    cb_mutex_t mutex;
    cb_cond_t cond;
    int references;
};

struct mock_callbacks {
    EVENT_CALLBACK cb;
    const void *cb_data;
    struct mock_callbacks *next;
};

struct mock_stats {
    uint64_t astat;
};

MEMCACHED_PUBLIC_API SERVER_HANDLE_V1 *get_mock_server_api(void);

MEMCACHED_PUBLIC_API void init_mock_server(bool log_to_stderr);

MEMCACHED_PUBLIC_API
struct mock_connstruct *mk_mock_connection(const char *user);

MEMCACHED_PUBLIC_API const void *create_mock_cookie(void);

MEMCACHED_PUBLIC_API void destroy_mock_cookie(const void *cookie);

MEMCACHED_PUBLIC_API void mock_set_ewouldblock_handling(const void *cookie, bool enable);

MEMCACHED_PUBLIC_API void mock_set_mutation_extras_handling(const void *cookie,
                                                            bool enable);

MEMCACHED_PUBLIC_API void mock_set_datatype_support(const void *cookie,
                                                    bool enable);

MEMCACHED_PUBLIC_API void lock_mock_cookie(const void *cookie);

MEMCACHED_PUBLIC_API void unlock_mock_cookie(const void *cookie);

MEMCACHED_PUBLIC_API void waitfor_mock_cookie(const void *cookie);

MEMCACHED_PUBLIC_API void mock_time_travel(int by);

MEMCACHED_PUBLIC_API void disconnect_mock_connection(struct mock_connstruct *c);

MEMCACHED_PUBLIC_API void disconnect_all_mock_connections(void);

MEMCACHED_PUBLIC_API void destroy_mock_event_callbacks_rec(struct mock_callbacks *h);

MEMCACHED_PUBLIC_API void destroy_mock_event_callbacks(void);

#ifdef  __cplusplus
}
#endif

#endif  /* MEMCACHED_MOCK_SERVER_H */
