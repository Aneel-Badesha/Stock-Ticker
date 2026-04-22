#pragma once

#include <stdbool.h>

#include "stock.h"

bool yahoo_fetch(const char *symbol, stock_t *out);
