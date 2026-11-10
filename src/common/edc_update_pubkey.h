#ifndef __EDC_UPDATE_PUBKEY_H__
#define __EDC_UPDATE_PUBKEY_H__

/* The release signing public key, compiled in (see tools/gen_pubkey_c.py).
 *
 * 65 bytes: 0x04 || X || Y, the uncompressed point on P-256 that mbedtls reads directly.
 *
 * It lives in the binary on purpose. The public key is the trust anchor for self-update, and
 * a trust anchor that sits in a file beside the binary is one an attacker can replace along
 * with the binary -- at which point every signature check passes against their key.
 */
#define EDC_UPDATE_PUBKEY_LEN 65

extern const unsigned char edc_update_pubkey_point[EDC_UPDATE_PUBKEY_LEN];

#endif /* __EDC_UPDATE_PUBKEY_H__ */
