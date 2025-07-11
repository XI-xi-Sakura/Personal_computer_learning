#ifndef MY_AES_INCLUDED
#define MY_AES_INCLUDED

/* Copyright (c) 2000, 2025, Oracle and/or its affiliates.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License, version 2.0,
 as published by the Free Software Foundation.

 This program is designed to work with certain software (including
 but not limited to OpenSSL) that is licensed under separate terms,
 as designated in a particular file or component or in included license
 documentation.  The authors of MySQL hereby grant you an additional
 permission to link the program and your derivative works with the
 separately licensed software that they have either included with
 the program or referenced in the documentation.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License, version 2.0, for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file include/my_aes.h
  Wrapper to give simple interface for MySQL to AES standard encryption.
*/

#include "my_inttypes.h"
#include "my_macros.h"

#include <string>
#include <vector>

#include <openssl/evp.h>

using std::string;
using std::vector;

/** AES IV size is 16 bytes for all supported ciphers except ECB */
#define MY_AES_IV_SIZE 16

/** AES block size is fixed to be 128 bits for CBC and ECB */
#define MY_AES_BLOCK_SIZE 16

/** Supported AES cipher/block mode combos */
enum my_aes_opmode {
  my_aes_128_ecb,
  my_aes_192_ecb,
  my_aes_256_ecb,
  my_aes_128_cbc,
  my_aes_192_cbc,
  my_aes_256_cbc,
  my_aes_128_cfb1,
  my_aes_192_cfb1,
  my_aes_256_cfb1,
  my_aes_128_cfb8,
  my_aes_192_cfb8,
  my_aes_256_cfb8,
  my_aes_128_cfb128,
  my_aes_192_cfb128,
  my_aes_256_cfb128,
  my_aes_128_ofb,
  my_aes_192_ofb,
  my_aes_256_ofb
};

#define MY_AES_BEGIN my_aes_128_ecb
#define MY_AES_END my_aes_256_ofb

/* If bad data discovered during decoding */
#define MY_AES_BAD_DATA -1

/** String representations of the supported AES modes. Keep in sync with
 * my_aes_opmode */
extern const char *my_aes_opmode_names[];

/**
  Encrypt a buffer using AES

  @param [in] source           Pointer to data for encryption
  @param [in] source_length    Size of encryption data
  @param [out] dest            Buffer to place encrypted data (must be large
  enough and not overlap with source)
  @param [in] key              Key to be used for encryption
  @param [in] key_length       Length of the key. Will handle keys of any length
  @param [in] mode             encryption mode
  @param [in] iv               16 bytes initialization vector if needed.
  Otherwise NULL
  @param [in] padding          if padding needed.
  @param kdf_options           KDF options
  @return              size of encrypted data, or negative in case of error
*/

int my_aes_encrypt(const unsigned char *source, uint32 source_length,
                   unsigned char *dest, const unsigned char *key,
                   uint32 key_length, enum my_aes_opmode mode,
                   const unsigned char *iv, bool padding = true,
                   vector<string> *kdf_options = nullptr);

/**
  Encrypt a buffer using AES.
  This version accepts operation context as parameter,
  for possible performance improvement.

  @param [in] ctx              Pointer to OpenSSL operation context
  @param [in] source           Pointer to data for encryption
  @param [in] source_length    Size of encryption data
  @param [out] dest            Buffer to place encrypted data (must be large
  enough and not overlap with source)
  @param [in] key              Key to be used for encryption
  @param [in] key_length       Length of the key. Will handle keys of any length
  @param [in] mode             encryption mode
  @param [in] iv               16 bytes initialization vector if needed.
  Otherwise NULL
  @param [in] padding          if padding needed.
  @param kdf_options           KDF options
  @return              size of encrypted data, or negative in case of error
*/

int my_aes_encrypt(EVP_CIPHER_CTX *ctx, const unsigned char *source,
                   uint32 source_length, unsigned char *dest,
                   const unsigned char *key, uint32 key_length,
                   enum my_aes_opmode mode, const unsigned char *iv,
                   bool padding = true, vector<string> *kdf_options = nullptr);

/**
  Decrypt an AES encrypted buffer

  @param source         Pointer to data for decryption
  @param source_length  size of encrypted data
  @param dest           buffer to place decrypted data (must be large enough)
  @param key            Key to be used for decryption
  @param key_length     Length of the key. Will handle keys of any length
  @param mode           encryption mode
  @param iv             16 bytes initialization vector if needed. Otherwise NULL
  @param padding        if padding needed.
  @param kdf_options    KDF options
  @return size of original data.
*/

int my_aes_decrypt(const unsigned char *source, uint32 source_length,
                   unsigned char *dest, const unsigned char *key,
                   uint32 key_length, enum my_aes_opmode mode,
                   const unsigned char *iv, bool padding = true,
                   vector<string> *kdf_options = nullptr);

/**
  Decrypt an AES encrypted buffer.
  This version accepts operation context as parameter,
  for possible performance improvement.

  @param ctx            Pointer to OpenSSL operation context
  @param source         Pointer to data for decryption
  @param source_length  size of encrypted data
  @param dest           buffer to place decrypted data (must be large enough)
  @param key            Key to be used for decryption
  @param key_length     Length of the key. Will handle keys of any length
  @param mode           encryption mode
  @param iv             16 bytes initialization vector if needed. Otherwise NULL
  @param padding        if padding needed.
  @param kdf_options    KDF options
  @return size of original data.
*/

int my_aes_decrypt(EVP_CIPHER_CTX *ctx, const unsigned char *source,
                   uint32 source_length, unsigned char *dest,
                   const unsigned char *key, uint32 key_length,
                   enum my_aes_opmode mode, const unsigned char *iv,
                   bool padding = true, vector<string> *kdf_options = nullptr);

/**
  Calculate the size of a buffer large enough for encrypted data.

  @param source_length  length of data to be encrypted
  @param opmode         encryption mode
  @return               size of buffer required to store encrypted data
*/

longlong my_aes_get_size(uint32 source_length, enum my_aes_opmode opmode);

/**
  Return true if the AES cipher and block mode requires an IV.

  @param opmode           encryption mode

  @retval true   IV needed
  @retval false  IV not needed
*/

bool my_aes_needs_iv(my_aes_opmode opmode);

#endif /* MY_AES_INCLUDED */
