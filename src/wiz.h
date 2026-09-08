/*
 * wiz.h - the commissioning wizards and their routes (see wiz.c)
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_WIZ_H
#define FORGECTRL_WIZ_H
#include <ulfius.h>

/* The typed acknowledgment that turns cloud mode on: the cloud step's,
 * and the one POST /settings asks for on cloud_enabled=1. */
#define WIZ_CLOUD_PHRASE "I UNDERSTAND"

/* Startup: nothing to recover for the form wizards; the button wait
 * starts idle. */
void wiz_init(void);
/* Stop anything a wizard is doing (a button wait); used before a
 * factory return and at shutdown. */
void wiz_abort_all(void);
/* The control panel was served: the green "finished" light, if it is
 * on, goes out. */
void wiz_panel_opened(void);

/* Routes. GET /wiz is the catalog and state; GET /advisories/:id serves
 * a document; the rest are the form wizards' completion calls. */
int cb_wiz_status(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_record(const struct _u_request *req, struct _u_response *res, void *ud);
/* GET /wiz/record.html: the printable summary; POST /wiz/changed
 * (`what`): a replaced part or a service, mapped to the wizards to run
 * again. */
int cb_wiz_record_html(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_changed(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_advisory_get(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_agree(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_press_start(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_press_status(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_press_cancel(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_account(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_preferences(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_machine(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_cloud(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_complete(const struct _u_request *req, struct _u_response *res, void *ud);
/* The dark wizards (wizdark.c): POST /wiz/:id/start, /answer, /abort;
 * GET /wiz/dark (the running or last run); GET /wiz/shot?cam= the
 * cameras wizard's snapshot. */
int cb_wiz_dark_start(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_dark_answer(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_dark_abort(const struct _u_request *req, struct _u_response *res, void *ud);
/* POST /wiz/:id/takeover: a second browser takes the running step over. */
int cb_wiz_dark_takeover(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_dark_status(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_shot(const struct _u_request *req, struct _u_response *res, void *ud);
/* The sheet wizards' previews (wizlive.c): GET /wiz/sheet.svg?card=<id>
 * the drawing, GET /wiz/sheet.gcode?card=<id> the program body. */
int cb_wiz_sheet_svg(const struct _u_request *req, struct _u_response *res, void *ud);
int cb_wiz_sheet_gcode(const struct _u_request *req, struct _u_response *res, void *ud);
#endif
