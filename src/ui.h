/*
 * ui.h - the embedded web pages
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Three self-contained pages (HTML/CSS/JS, no external assets),
 * gzip-compressed at build time from src/ui/ by src/ui/embed.cmake: the
 * control panel (index.html), the commissioning wizard (wizard.html),
 * and the login page (login.html). For each: the compressed bytes, how
 * many there are, and the size they inflate to. main.c inflates each
 * once and substitutes the token placeholder before serving.
 */
#ifndef FORGECTRL_UI_H
#define FORGECTRL_UI_H
extern const unsigned char index_html_gz[];
extern const unsigned int index_html_gz_len;
extern const unsigned int index_html_len;
extern const unsigned char wizard_html_gz[];
extern const unsigned int wizard_html_gz_len;
extern const unsigned int wizard_html_len;
extern const unsigned char login_html_gz[];
extern const unsigned int login_html_gz_len;
extern const unsigned int login_html_len;
#endif
