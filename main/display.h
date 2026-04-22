#pragma once

#include <stdbool.h>

#include "stock.h"

void display_init(void);
void display_draw_boot_screen(void);
void display_boot_msg(const char *msg, bool error);

void display_draw_main_screen(void);
void display_show_status(const char *msg);
void display_clear_status(void);

void display_draw_stock_list(const stock_t *stocks, int count, int active_idx);
void display_scroll_ticker(const stock_t *stocks, int count);
