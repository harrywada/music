#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tags.h"

[[gnu::nonnull(1, 2)]]
bool sp_field_name(const char *, enum tag_field *);
