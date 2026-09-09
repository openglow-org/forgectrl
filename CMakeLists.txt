cmake_minimum_required(VERSION 3.10)
project(forgectrl C)

set(CMAKE_C_STANDARD 11)

# The pages are normal static files under src/ui/: the control panel
# (index.html with theme.css, help.js, forms.js, panel.js), the
# commissioning wizard (wizard.html with wizard.css, md.js, wizard.js),
# and the login page (login.html with login.js), all on Bootstrap in
# vendor/. embed.cmake bundles each into one self-contained page, gzips
# it, and emits the compressed bytes as `<page>_html_gz[]` (see
# src/ui.h); the bundled pages land in build/ui/ for inspection.
set(UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
set(UI_OUT ${CMAKE_CURRENT_BINARY_DIR}/ui)
set(UI_COMMON ${UI_DIR}/theme.css ${UI_DIR}/vendor/bootstrap.min.css
    ${UI_DIR}/vendor/bootstrap.bundle.min.js ${UI_DIR}/embed.cmake)
add_custom_command(
  OUTPUT ${UI_OUT}/ui_blob.c ${UI_OUT}/index.html
  COMMAND ${CMAKE_COMMAND} -E make_directory ${UI_OUT}
  COMMAND ${CMAKE_COMMAND} -DUI_DIR=${UI_DIR} -DPAGE=index
          -DOUT_HTML=${UI_OUT}/index.html
          -DOUT_C=${UI_OUT}/ui_blob.c -P ${UI_DIR}/embed.cmake
  DEPENDS ${UI_DIR}/index.html ${UI_DIR}/help.js ${UI_DIR}/forms.js
          ${UI_DIR}/panel.js ${UI_DIR}/regions.js ${UI_COMMON}
  COMMENT "Bundling the control panel (src/ui -> index_html_gz[])"
  VERBATIM)
add_custom_command(
  OUTPUT ${UI_OUT}/wizard_blob.c ${UI_OUT}/wizard.html
  COMMAND ${CMAKE_COMMAND} -E make_directory ${UI_OUT}
  COMMAND ${CMAKE_COMMAND} -DUI_DIR=${UI_DIR} -DPAGE=wizard
          -DOUT_HTML=${UI_OUT}/wizard.html
          -DOUT_C=${UI_OUT}/wizard_blob.c -P ${UI_DIR}/embed.cmake
  DEPENDS ${UI_DIR}/wizard.html ${UI_DIR}/wizard.css ${UI_DIR}/md.js
          ${UI_DIR}/regions.js
          ${UI_DIR}/wizard.js ${UI_COMMON}
  COMMENT "Bundling the commissioning wizard (src/ui -> wizard_html_gz[])"
  VERBATIM)
add_custom_command(
  OUTPUT ${UI_OUT}/login_blob.c ${UI_OUT}/login.html
  COMMAND ${CMAKE_COMMAND} -E make_directory ${UI_OUT}
  COMMAND ${CMAKE_COMMAND} -DUI_DIR=${UI_DIR} -DPAGE=login
          -DOUT_HTML=${UI_OUT}/login.html
          -DOUT_C=${UI_OUT}/login_blob.c -P ${UI_DIR}/embed.cmake
  DEPENDS ${UI_DIR}/login.html ${UI_DIR}/wizard.css ${UI_DIR}/login.js
          ${UI_COMMON}
  COMMENT "Bundling the login page (src/ui -> login_html_gz[])"
  VERBATIM)

# The advisory documents (docs/advisories/*.md) ride in the binary the
# same way, uncompressed: the wizard serves them raw and hashes them.
set(DOCS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/docs/advisories)
add_custom_command(
  OUTPUT ${UI_OUT}/advisories_blob.c
  COMMAND ${CMAKE_COMMAND} -E make_directory ${UI_OUT}
  COMMAND ${CMAKE_COMMAND} -DDOCS_DIR=${DOCS_DIR}
          -DOUT_C=${UI_OUT}/advisories_blob.c -P ${UI_DIR}/embed_docs.cmake
  DEPENDS ${DOCS_DIR}/safety-and-risk.md ${DOCS_DIR}/licenses.md
          ${DOCS_DIR}/privacy.md ${DOCS_DIR}/cloud-service.md
          ${UI_DIR}/embed_docs.cmake
  COMMENT "Embedding the advisory documents (docs/advisories -> advisories_blob.c)"
  VERBATIM)

add_executable(forgectrl
  src/main.c
  src/accel.c
  src/airflow.c
  src/auth.c
  src/cam.c
  src/peer.c
  src/camhealth.c
  src/cool.c
  src/coolfmt.c
  src/debayer.c
  src/diag.c
  src/fflog.c
  src/gates.c
  src/gpu_debayer.c
  src/ipu_copy.c
  src/lenshome.c
  src/liveness.c
  src/logs.c
  src/mp4mux.c
  src/sanitize.c
  src/settings.c
  src/status.c
  src/curverec.c
  src/font_hershey.c
  src/jobstream.c
  src/sheet.c
  src/wizlive.c
  src/super.c
  src/update.c
  src/vpu_h264.c
  src/vpu_jpeg.c
  src/advisories.c
  src/button.c
  src/camkey.c
  src/commission.c
  src/recordhtml.c
  src/led.c
  src/session.c
  src/sha256.c
  src/sheetid.c
  src/tls.c
  src/users.c
  src/wiz.c
  src/wizcalc.c
  src/wizdark.c
  ${UI_OUT}/ui_blob.c
  ${UI_OUT}/wizard_blob.c
  ${UI_OUT}/login_blob.c
  ${UI_OUT}/advisories_blob.c
)
target_include_directories(forgectrl PRIVATE src)
target_compile_options(forgectrl PRIVATE -Wall -Wextra -O2)
# dl: the GPU demosaic loads libEGL/libGLESv2 at runtime, so the build
# has no GL dependency and images without Mesa fall back to NEON.
# jansson: the commissioning record. gnutls: the panel's self-signed
# certificate (the HTTPS listener itself is libmicrohttpd's). crypt: the
# account's sha512-crypt hash, shared with the system's shadow file.
target_link_libraries(forgectrl ulfius jansson gnutls crypt jpeg z pthread m
                      ${CMAKE_DL_LIBS})

install(TARGETS forgectrl RUNTIME DESTINATION bin)

# Host unit test for the machine_is_idle() fail-closed contract (X-2):
# a read failure - including fd exhaustion under a connection flood -
# must report not-idle. Built everywhere, installed nowhere; CI runs it
# after the build.
add_executable(status_idle_test tests/status_idle_test.c src/status.c)
target_compile_options(status_idle_test PRIVATE -Wall -Wextra)
target_link_libraries(status_idle_test m pthread)

# Host unit test for the dose-curve fitter. CI runs it after the build.
add_executable(curverec_fit_test tests/curverec_fit_test.c src/curverec.c src/jobstream.c
               src/settings.c src/fflog.c)
target_compile_options(curverec_fit_test PRIVATE -Wall -Wextra)
target_link_libraries(curverec_fit_test m pthread)

add_executable(status_grbl_test tests/status_grbl_test.c src/status.c)
target_compile_options(status_grbl_test PRIVATE -Wall -Wextra)
target_link_libraries(status_grbl_test m pthread)

add_executable(status_sys_test tests/status_sys_test.c src/status.c)
target_compile_options(status_sys_test PRIVATE -Wall -Wextra)
target_link_libraries(status_sys_test m pthread)

# Host unit test for the log-export sanitizer: every identifying class
# redacted, ordinary log text untouched, stable placeholders, idempotent.
# A sanitized bundle is what gets attached to public issue reports.
add_executable(sanitize_test tests/sanitize_test.c src/sanitize.c)
target_compile_options(sanitize_test PRIVATE -Wall -Wextra)

# Host unit test for the camera privacy gate's lid read: every way it
# can fail must land on "not closed", or the cameras could capture with
# the enclosure open. CI runs it after the build.
add_executable(lid_gate_test tests/lid_gate_test.c src/status.c)
target_compile_options(lid_gate_test PRIVATE -Wall -Wextra)
target_link_libraries(lid_gate_test m pthread)

# Host unit test for the raw-frame front end: the 10-bit -> 8-bit
# narrowing the 8 MP (OV8856) capture path depends on, and the Bayer
# phase surviving it. CI runs it after the build.
add_executable(debayer_test tests/debayer_test.c src/debayer.c)
target_compile_options(debayer_test PRIVATE -Wall -Wextra)

# Host unit test for the capture frame-health ladder: a frame the queue
# flagged errored must never be demosaiced, a run of them must escalate to
# a stream restart and then stop being retried, and one usable frame must
# put the ladder back at the bottom. CI runs it after the build.
add_executable(camhealth_test tests/camhealth_test.c src/camhealth.c)
target_compile_options(camhealth_test PRIVATE -Wall -Wextra)

# Host unit test for the gate-settings table the cooling gates, their
# validators, the settings reply and the panel all read: the legal
# range, the recommended band, the off end, and the two JSON shapes.
# CI runs it after the build.
add_executable(cool_gate_test tests/cool_gate_test.c src/gates.c)
target_compile_options(cool_gate_test PRIVATE -Wall -Wextra)
target_link_libraries(cool_gate_test m)

# Host unit test for the flow check's reading: the baseline as the mean
# of the settled window, the end as the mean of the last samples, the
# tube's share subtracted from the rise and bounded, and a stopped pump
# still read as stopped under a lit tube. Includes the engine source
# against a fake sysfs tree and a fake clock; CI runs it after the build.
add_executable(cool_flow_test tests/cool_flow_test.c src/gates.c src/airflow.c src/accel.c src/coolfmt.c)
target_compile_options(cool_flow_test PRIVATE -Wall -Wextra)
target_link_libraries(cool_flow_test m pthread)

# Host unit test for the per-fan airflow gate: grace, the three-tick
# debounce, the latch, off by value, and the tach conversions. CI runs
# it after the build.
add_executable(airflow_test tests/airflow_test.c src/airflow.c)
target_compile_options(airflow_test PRIVATE -Wall -Wextra)
target_link_libraries(airflow_test m)

# Host unit test for the crash-watch tier logic: the alert raised once
# and released by the quiet count, the abort latched for the session,
# the session reset, the axis names. CI runs it after the build.
add_executable(accel_test tests/accel_test.c src/accel.c)
target_compile_options(accel_test PRIVATE -Wall -Wextra)

# Host unit test for the lens hall-edge reference: the failures that
# gate a controller spawn (no head, a sensor stuck either way) cannot be
# staged on working hardware, so the head's sysfs is a scratch tree here
# and each fault is a file layout. CI runs it after the build.
add_executable(lenshome_test tests/lenshome_test.c src/lenshome.c src/fflog.c)
target_compile_options(lenshome_test PRIVATE -Wall -Wextra)

# Host unit test for the /cool/status document: the widest legal
# rendering of every fragment fits the buffer the engine uses, the
# widest document fits COOL_STATUS_JSON_MAX and parses, and an oversize
# value set is refused rather than served cut off.
add_executable(coolfmt_test tests/coolfmt_test.c src/coolfmt.c)
target_compile_options(coolfmt_test PRIVATE -Wall -Wextra)
target_link_libraries(coolfmt_test m)

# Host unit test for the loopback peer check behind the cooling report
# channel: every spelling of this host passes (127/8, ::1, the v4-mapped
# ::ffff:127/8 a dual-stack listener reports for an IPv4 client); every
# other address, another family and a truncated sockaddr fail closed.
add_executable(auth_peer_test tests/auth_peer_test.c src/peer.c)
target_compile_options(auth_peer_test PRIVATE -Wall -Wextra)

# Host unit test for the fragmented-MP4 muxer behind /cam/h264: the init
# segment's box tree and avcC must match the fed SPS/PPS, fragments must
# carry the right sizes, offsets and sample flags, and the Annex B
# reframing must drop parameter sets and length-prefix the rest. CI runs
# it after the build.
add_executable(mp4mux_test tests/mp4mux_test.c src/mp4mux.c)
target_compile_options(mp4mux_test PRIVATE -Wall -Wextra)

# Host unit test for the digest primitives behind the sheet id, the
# advisory hashes, and the session comparison: FIPS 180-4 and RFC 4231
# vectors, and the constant-time compare. CI runs it after the build.
add_executable(sha256_test tests/sha256_test.c src/sha256.c)
target_compile_options(sha256_test PRIVATE -Wall -Wextra)

# Host unit test for the sheet id: a fixed salt yields the ids an
# independent HMAC/base32 implementation yields, and the id never
# carries the serial. CI runs it after the build.
add_executable(sheetid_test tests/sheetid_test.c src/sheetid.c src/sha256.c
               src/fflog.c)
target_compile_options(sheetid_test PRIVATE -Wall -Wextra)
target_link_libraries(sheetid_test pthread)

# Host unit test for the commissioning record and the gate: consent,
# the account, the required table and its versions, engine flags, the
# override file, a damaged record, and the status document. CI runs it
# after the build.
add_executable(commission_test tests/commission_test.c src/commission.c
               src/advisories.c src/sha256.c src/sheetid.c src/fflog.c
               ${UI_OUT}/advisories_blob.c)
target_compile_options(commission_test PRIVATE -Wall -Wextra)
target_link_libraries(commission_test jansson pthread)

# The record as a printable page: every value escaped, the steps in
# catalog order with the sentence, the settings written, and the
# numbers, the flags, and nothing the record does not hold. CI runs it
# after the build.
add_executable(recordhtml_test tests/recordhtml_test.c src/recordhtml.c)
target_compile_options(recordhtml_test PRIVATE -Wall -Wextra)
target_link_libraries(recordhtml_test jansson)

# Host unit test for the account and the sessions: the name and
# password rules, create and verify with a real sha512-crypt hash, the
# reset marker, the session store, cookie parsing, and the per-address
# login throttle. CI runs it after the build.
add_executable(users_test tests/users_test.c src/users.c src/session.c
               src/sha256.c src/fflog.c)
target_compile_options(users_test PRIVATE -Wall -Wextra)
target_link_libraries(users_test crypt pthread)

# Host unit test for the camera key: made once, loaded back, constant-time
# checked, rotated. CI runs it after the build.
add_executable(camkey_test tests/camkey_test.c src/camkey.c src/sha256.c
               src/fflog.c)
target_compile_options(camkey_test PRIVATE -Wall -Wextra)
target_link_libraries(camkey_test pthread)

# Host unit test for the certificate: made once, loaded back, the
# fingerprint stable across a reload, the names on it. CI runs it after
# the build.
add_executable(wizcalc_test tests/wizcalc_test.c src/wizcalc.c)
target_compile_options(wizcalc_test PRIVATE -Wall -Wextra)
target_link_libraries(wizcalc_test m)

# The commissioning sheet: the font table, every card inside its box,
# the programs, the SVG, the curve inverse.
add_executable(sheet_test tests/sheet_test.c src/sheet.c src/font_hershey.c)
target_compile_options(sheet_test PRIVATE -Wall -Wextra)
target_link_libraries(sheet_test m)

# The daemon's own sender against a mock controller on a local port.
add_executable(jobstream_test tests/jobstream_test.c src/jobstream.c src/fflog.c)
target_compile_options(jobstream_test PRIVATE -Wall -Wextra)
target_link_libraries(jobstream_test pthread)

add_executable(tls_test tests/tls_test.c src/tls.c src/fflog.c)
target_compile_options(tls_test PRIVATE -Wall -Wextra)
target_link_libraries(tls_test gnutls pthread)

# Emitter half of the host end-to-end logging check (tests/fflog_e2e.sh
# drives it against a private rsyslogd; manual, not CI).
add_executable(fflog_e2e_test tests/fflog_e2e_test.c src/fflog.c)
target_compile_options(fflog_e2e_test PRIVATE -Wall -Wextra)
target_link_libraries(fflog_e2e_test pthread)
