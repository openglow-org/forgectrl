/*
 * advisories.c - the advisory documents embedded in the daemon
 * Copyright 2026 514 LLC d/b/a OpenGlow
 * Written by Scott Wiederhold
 * SPDX-License-Identifier: MIT
 */
#include "advisories.h"
#include "sha256.h"

#include <string.h>

extern const unsigned char advisory_safety_and_risk[];
extern const size_t advisory_safety_and_risk_len;
extern const unsigned char advisory_licenses[];
extern const size_t advisory_licenses_len;
extern const unsigned char advisory_privacy[];
extern const size_t advisory_privacy_len;
extern const unsigned char advisory_cloud_service[];
extern const size_t advisory_cloud_service_len;

/* The order is the order the wizard shows them. The typed phrase is the
 * consent for the safety document; the others take a checkbox. The
 * cloud document is typed again when cloud mode is turned on, in the
 * cloud wizard. */
static advisory_t docs[] = {
    { "safety-and-risk", "Safety and risk", "typed", "I UNDERSTAND", NULL, 0, "" },
    { "licenses", "Licenses and notices", "check", NULL, NULL, 0, "" },
    { "privacy", "Privacy", "check", NULL, NULL, 0, "" },
    { "cloud-service", "The Glowforge cloud service", "check", NULL, NULL, 0, "" },
};
#define NDOCS (sizeof(docs) / sizeof(*docs))

void advisories_init(void)
{
    docs[0].text = advisory_safety_and_risk;
    docs[0].len = advisory_safety_and_risk_len;
    docs[1].text = advisory_licenses;
    docs[1].len = advisory_licenses_len;
    docs[2].text = advisory_privacy;
    docs[2].len = advisory_privacy_len;
    docs[3].text = advisory_cloud_service;
    docs[3].len = advisory_cloud_service_len;
    for (size_t i = 0; i < NDOCS; i++) {
        unsigned char d[SHA256_LEN];
        sha256(docs[i].text, docs[i].len, d);
        sha256_hex(d, SHA256_LEN, docs[i].hash);
    }
}

const advisory_t *advisories_list(size_t *n)
{
    *n = NDOCS;
    return docs;
}

const advisory_t *advisories_find(const char *id)
{
    if (!id)
        return NULL;
    for (size_t i = 0; i < NDOCS; i++)
        if (!strcmp(docs[i].id, id))
            return &docs[i];
    return NULL;
}
