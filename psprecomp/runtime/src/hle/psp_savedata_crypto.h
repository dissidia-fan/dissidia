// PSP savedata crypto (sceChnnlsv + the KIRK CMD4/CMD7 subset it needs).
//
// Generic and host-testable: no dependency on the runtime. Ported from the
// public PPSSPP sources (Core/HLE/sceChnnlsv.cpp, Core/Dialog/SavedataParam.cpp,
// ext/libkirk/kirk_engine.c) so that save folders written by PPSSPP load here
// and vice versa. AES-128 is implemented locally.
//
// On-disk layout of a secure data file (SavedataParam::EncryptData):
//   [16-byte header][ciphertext, dataSize bytes]       (file size = dataSize+16)
// The header is the encrypted per-file IV (12 random bytes + 4 zero bytes, run
// through KIRK); the body is a CTR-like keystream XOR. The MAC over
// header + zero-padded-to-16 ciphertext is stored per file in PARAM.SFO's
// SAVEDATA_FILE_LIST (32-byte entries: char name[13], u8 hash[16], u8 pad[3]),
// and PARAM.SFO itself is signed by the three hashes in SAVEDATA_PARAMS.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psp_sdcrypto {

// ---- AES-128 (ECB block + CBC with zero IV, as KIRK CMD4/CMD7 use) ----
void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16],
                          uint8_t out[16]);
void aes128_decrypt_block(const uint8_t key[16], const uint8_t in[16],
                          uint8_t out[16]);
// len must be a multiple of 16; in/out may be the same buffer. IV = 0.
void aes128_cbc_encrypt(const uint8_t key[16], const uint8_t* in,
                        uint8_t* out, size_t len);
void aes128_cbc_decrypt(const uint8_t key[16], const uint8_t* in,
                        uint8_t* out, size_t len);

// ---- KIRK key vault (CMD4/CMD7 key seeds 0..0x7F) ----
// Returns nullptr when seed is out of range.
const uint8_t* kirk_keyvault(int seed);

// Fuse ID used for the per-console key of KIRK CMD5/CMD8 (modes 2/4/6 only,
// i.e. the SAVEDATA_PARAMS hash PPSSPP never verifies). Default = PPSSPP's.
void set_fuse_id(uint32_t fuse90, uint32_t fuse94);

// Random source used for the per-file IV (KIRK CMD14 in the original).
// Default: /dev/urandom. Tests install a deterministic one.
using RandomFn = void (*)(uint8_t* out, size_t len);
void set_random_source(RandomFn fn);

// ---- sceChnnlsv primitives (names follow PPSSPP sceChnnlsv.h) ----
struct MacCtx {            // pspChnnlsvContext1
    int32_t mode;
    uint8_t result[16];
    uint8_t key[16];
    int32_t keyLength;
};
struct CipherCtx {         // pspChnnlsvContext2
    int32_t mode;
    int32_t unkn;
    uint8_t cryptedData[0x92];
};

int sd_mac_init(MacCtx& ctx, int mode);
int sd_mac_update(MacCtx& ctx, const uint8_t* data, int length);
// cryptkey may be null. Returns <0 on failure.
int sd_mac_final(MacCtx& ctx, uint8_t hash_out[16], const uint8_t* cryptkey);
// uknw: 1 = encrypt side (generates IV into data[16]), 2 = decrypt side
// (data[16] holds the file header).
int sd_cipher_init(CipherCtx& ctx, int mode, int uknw, uint8_t* data,
                   const uint8_t* cryptkey);
int sd_cipher_update(CipherCtx& ctx, uint8_t* data, int alignedLen);
int sd_cipher_final(CipherCtx& ctx);

// ---- SavedataParam-level operations ----
// Crypt mode: 1 = no key (secureVersion 1 / old structs), 3 = key (old hash),
// 5 = key (new hash, SDK >= 2.7). Mirrors SavedataParam::DetermineCryptMode.
// paramCommonSize = SceUtilitySavedataParam.common.size.
int determine_crypt_mode(uint32_t paramCommonSize, uint32_t secureVersion,
                         uint32_t compiledSdkVersion);
// SAVEDATA_PARAMS[0] -> crypt mode (0 = not encrypted). SavedataParam::
// GetSaveCryptMode.
int crypt_mode_from_params_flag(uint8_t flag);
inline bool crypt_mode_has_key(int mode) { return mode > 1; }

// Encrypts `plain` (size n) into the on-disk file image (n+16 bytes) and
// returns the per-file MAC (store it in SAVEDATA_FILE_LIST). 0 on success.
int encrypt_file(int mode, const uint8_t* plain, size_t n,
                 const uint8_t key[16] /*null if mode==1*/,
                 std::vector<uint8_t>& file_out, uint8_t hash_out[16]);

// Decrypts an on-disk file image; reproduces PPSSPP's LoadCryptedSave logic
// (mode fallbacks, tolerance of a bad hash). `expectedHash` may be null.
// `prevMode` is the mode recorded in PARAM.SFO, `detectedMode` the one from
// determine_crypt_mode. Returns 0 on success (plain_out = size-16 bytes);
// nonzero if the data cannot be a secure file (caller should read it plain).
// *hash_ok (optional) reports whether the MAC matched.
int decrypt_file(int prevMode, int detectedMode, const uint8_t* file,
                 size_t n, const uint8_t key[16], const uint8_t* expectedHash,
                 std::vector<uint8_t>& plain_out, bool* hash_ok);

// Signs a PARAM.SFO image in place (SavedataParam::UpdateHash). `sfo` must
// contain a 128-byte SAVEDATA_PARAMS value at paramsOffset and be zero-padded
// to a multiple of 16 (sfo.size() % 16 == 0).
int update_sfo_hash(std::vector<uint8_t>& sfo, size_t paramsOffset,
                    int cryptMode);

// SavedataParam::BuildHash (exposed for tests). `len` must be 16-aligned.
int build_hash(uint8_t out[16], const uint8_t* data, unsigned len, int mode,
               const uint8_t* cryptkey);

}  // namespace psp_sdcrypto
