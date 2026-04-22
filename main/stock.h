#pragma once

#include <stdbool.h>

typedef struct {
    char symbol[10];
    char short_name[32];
    float price;
    float change;
    float change_pct;
    bool valid;
} stock_t;
