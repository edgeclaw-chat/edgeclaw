#ifndef __FAKE_RELEASE_H__
#define __FAKE_RELEASE_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* A one-shot local release server, for tests that need the update check to actually FIND
 * something.
 *
 * Why a helper rather than a struct per test: the chain from `edc_app_new` to the banner runs
 * through the HTTPS client, so exercising it needs a real TLS handshake. Both the C tests
 * (test_app_boundary) and the C++ ones (test_tui_dispatch) need that, hence a plain C surface
 * they can link against -- `tls_stub.h` is header-only C and does not survive a C++ include.
 *
 * The certificate is generated per call and its path is written to |cert_out|; the caller
 * copies it wherever the code under test expects a CA bundle (the app reads
 * `<workspace>/ssl/cacert.pem`). The host is `localhost`, which is what the generated
 * certificate names.
 *
 * Answers exactly one connection -- enough for one check -- with a 200 and the JSON given.
 * Not restartable: call stop() then start() again for a second case.
 */
int fake_release_start(const char *json, char *url_out, size_t url_cap, char *cert_out, size_t cert_cap);

/* Same, answering one release named |tag| that carries this platform's asset name. Exists so
 * a C++ caller does not have to build the JSON: including sds.h from C++ does not compile
 * (see the note in edc_app.h), and the asset name has to match edc_update_target() or the
 * picker correctly ignores the release. */
int fake_release_start_newer(const char *tag, char *url_out, size_t url_cap, char *cert_out, size_t cert_cap);

/* Release the port and remove the generated files. Safe to call after a failed start. */
void fake_release_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __FAKE_RELEASE_H__ */
