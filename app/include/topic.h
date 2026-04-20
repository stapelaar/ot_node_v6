#pragma once
#include <stddef.h>

int topic_root(char *out, size_t out_len, const char *node_id);

int topic_build(char *out, size_t out_len,
                const char *root,
                const char *chan,
                const char *device,
                const char *field);