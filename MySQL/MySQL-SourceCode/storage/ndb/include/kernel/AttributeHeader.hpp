/*
   Copyright (c) 2003, 2025, Oracle and/or its affiliates.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef ATTRIBUTE_HEADER
#define ATTRIBUTE_HEADER

#include <cstdio>  // FILE
#include "ndb_types.h"

#define JAM_FILE_ID 6

/**
 * @class AttributeHeader
 * @brief Header passed in front of every attribute value in AttrInfo signal
 */
class AttributeHeader {
  friend class Dbtup;
  friend class Backup;
  friend class NdbOperation;
  friend class DbUtil;
  friend class Suma;

 public:
  /**
   * Pseudo columns
   */
  static constexpr Uint32 PSEUDO = 0x8000;
  static constexpr Uint32 FRAGMENT = 0xFFFE;      // Read fragment no
  static constexpr Uint32 ROW_COUNT = 0xFFFD;     // Read row count (committed)
  static constexpr Uint32 COMMIT_COUNT = 0xFFFC;  // Read commit count
  static constexpr Uint32 RANGE_NO =
      0xFFFB;  // Read range no (when batched ranges)

  static constexpr Uint32 ROW_SIZE = 0xFFFA;
  static constexpr Uint32 FRAGMENT_FIXED_MEMORY = 0xFFF9;

  static constexpr Uint32 RECORDS_IN_RANGE = 0xFFF8;
  static constexpr Uint32 DISK_REF = 0xFFF7;
  static constexpr Uint32 ROWID = 0xFFF6;
  static constexpr Uint32 ROW_GCI = 0xFFF5;
  static constexpr Uint32 FRAGMENT_VARSIZED_MEMORY = 0xFFF4;
  static constexpr Uint32 READ_PACKED = 0xFFF3;
  static constexpr Uint32 ANY_VALUE = 0xFFF2;
  static constexpr Uint32 COPY_ROWID = 0xFFF1;
  static constexpr Uint32 READ_ALL = 0xFFF0;
  static constexpr Uint32 READ_LCP = 0xFFEF;
  static constexpr Uint32 LOCK_REF = 0xFFEE;  // Operation lock reference
  static constexpr Uint32 OP_ID = 0xFFED;     // Operation runtime identity

  // Extents * sizeof(Extent) allocated to fragment
  static constexpr Uint32 FRAGMENT_EXTENT_SPACE = 0xFFEC;

  // Free but allocated DD extent space
  static constexpr Uint32 FRAGMENT_FREE_EXTENT_SPACE = 0xFFEB;

  static constexpr Uint32 FLUSH_AI = 0xFFEA;
  static constexpr Uint32 CORR_FACTOR32 = 0xFFE9;  // excluding root-frag
  static constexpr Uint32 CORR_FACTOR64 = 0xFFE8;  // including root-frag

  /**
   * 64-bit row gci (extending lower if not sufficient bits)
   *   read-only
   */
  static constexpr Uint32 ROW_GCI64 = 0xFFE7;

  /**
   * Row author... autoset to 0, can be over written
   */
  static constexpr Uint32 ROW_AUTHOR = 0xFFE6;

  /**
   * Optimize pseudo column and optimization options
   */
  static constexpr Uint32 OPTIMIZE = 0xFFE0;  // pseudo column id to optimize
  static constexpr Uint32 OPTIMIZE_OPTIONS_MASK =
      0xFFFF;  // bitmask AND column value
  static constexpr Uint32 OPTIMIZE_MOVE_VARPART =
      0x0001;  // option to move varpart
  static constexpr Uint32 OPTIMIZE_MOVE_FIXPART =
      0x0002;  // option to move fixpart

  // index stats pseudo columns
  static constexpr Uint32 INDEX_STAT_KEY = 0xFFD0;
  static constexpr Uint32 INDEX_STAT_VALUE = 0xFFD1;

  // NOTE: in 5.1 ctors and init take size in bytes

  /** Initialize AttributeHeader at location aHeaderPtr */
  static void init(Uint32 *aHeaderPtr, Uint32 anAttributeId, Uint32 aByteSize);

  /** Returns size of AttributeHeader (usually one or two words) */
  Uint32 getHeaderSize() const;  // In 32-bit words

  /** Store AttributeHeader in location given as argument */
  void insertHeader(Uint32 *);

  /** Get next attribute header (if there is one) */
  AttributeHeader *getNext();
  const AttributeHeader *getNext() const;

  /** Get location of attribute value */
  Uint32 *getDataPtr();
  const Uint32 *getDataPtr() const;

  /** Getters and Setters */
  Uint32 getAttributeId() const;
  void setAttributeId(Uint32);
  Uint32 getByteSize() const;
  void setByteSize(Uint32);
  Uint32 getDataSize() const;  // In 32-bit words, rounded up
  void setDataSize(Uint32);    // Set size to multiple of word size
  bool isNULL() const;
  void setNULL();

  /** Print **/
  // void    print(NdbOut&);
  void print(FILE *);

  static Uint32 getByteSize(Uint32);
  static Uint32 getDataSize(Uint32);
  static Uint32 getAttributeId(Uint32 id);

 public:
  AttributeHeader(Uint32 = 0);
  AttributeHeader(Uint32 anAttributeId, Uint32 aByteSize);

  Uint32 m_value;
};

/**
 *           1111111111222222222233
 * 01234567890123456789012345678901
 * ssssssssssssssssiiiiiiiiiiiiiiii
 *
 * i = Attribute Id
 * s = Size of current "chunk" in bytes - 16 bits.
 *     To allow round up to word, max value is 0xFFFC (not checked).
 * e - [ obsolete future ]
 *     Element data/Blob, read element of array
 *     If == 0 next data word contains attribute value.
 *     If == 1 next data word contains:
 *       For Array of Fixed size Elements
 *         Start Index (16 bit), Stop Index(16 bit)
 *       For Blob
 *         Start offset (32 bit) (length is defined in previous word)
 *
 * An attribute value equal to "null" is represented by setting s == 0.
 */

inline void AttributeHeader::init(Uint32 *aHeaderPtr, Uint32 anAttributeId,
                                  Uint32 aByteSize) {
  AttributeHeader ah(anAttributeId, aByteSize);
  *aHeaderPtr = ah.m_value;
}

inline AttributeHeader::AttributeHeader(Uint32 aHeader) { m_value = aHeader; }

inline AttributeHeader::AttributeHeader(Uint32 anAttributeId,
                                        Uint32 aByteSize) {
  m_value = 0;
  this->setAttributeId(anAttributeId);
  this->setByteSize(aByteSize);
}

inline Uint32 AttributeHeader::getHeaderSize() const {
  // Should check 'e' bit here
  return 1;
}

inline Uint32 AttributeHeader::getAttributeId() const {
  return (m_value & 0xFFFF0000) >> 16;
}

inline void AttributeHeader::setAttributeId(Uint32 anAttributeId) {
  m_value &= 0x0000FFFF;  // Clear attribute id
  m_value |= (anAttributeId << 16);
}

inline Uint32 AttributeHeader::getByteSize() const {
  return (m_value & 0xFFFF);
}

inline void AttributeHeader::setByteSize(Uint32 aByteSize) {
  m_value &= (~0xFFFF);
  m_value |= aByteSize;
}

inline Uint32 AttributeHeader::getDataSize() const {
  return (((m_value & 0xFFFF) + 3) >> 2);
}

inline void AttributeHeader::setDataSize(Uint32 aDataSize) {
  m_value &= (~0xFFFF);
  m_value |= (aDataSize << 2);
}

inline bool AttributeHeader::isNULL() const { return (getDataSize() == 0); }

inline void AttributeHeader::setNULL() { setDataSize(0); }

inline Uint32 *AttributeHeader::getDataPtr() {
  return &m_value + getHeaderSize();
}

inline const Uint32 *AttributeHeader::getDataPtr() const {
  return &m_value + getHeaderSize();
}

inline void AttributeHeader::insertHeader(Uint32 *target) { *target = m_value; }

inline AttributeHeader *AttributeHeader::getNext() {
  return (AttributeHeader *)(getDataPtr() + getDataSize());
}

inline const AttributeHeader *AttributeHeader::getNext() const {
  return (const AttributeHeader *)(getDataPtr() + getDataSize());
}

inline void
// AttributeHeader::print(NdbOut& output) {
AttributeHeader::print(FILE *output) {
  fprintf(output,
          "AttributeId: H\'%.8x (D\'%d), DataSize: H\'%.8x (D\'%d), "
          "isNULL: %d\n",
          getAttributeId(), getAttributeId(), getDataSize(), getDataSize(),
          isNULL());
}

inline Uint32 AttributeHeader::getByteSize(Uint32 m_value) {
  return (m_value & 0xFFFF);
}

inline Uint32 AttributeHeader::getDataSize(Uint32 m_value) {
  return (((m_value & 0xFFFF) + 3) >> 2);
}

inline Uint32 AttributeHeader::getAttributeId(Uint32 m_value) {
  return m_value >> 16;
}

#undef JAM_FILE_ID

#endif
