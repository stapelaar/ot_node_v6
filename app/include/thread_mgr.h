#pragma once
#include <stdbool.h>

enum thread_mgr_role {
    THREAD_MGR_DETACHED = 0,
    THREAD_MGR_CHILD,
    THREAD_MGR_ROUTER,
    THREAD_MGR_LEADER
};

int thread_mgr_init(void);
void thread_mgr_poll(void);

bool thread_mgr_is_attached(void);
enum thread_mgr_role thread_mgr_get_role(void);