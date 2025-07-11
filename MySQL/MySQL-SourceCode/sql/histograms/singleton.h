#ifndef HISTOGRAMS_SINGLETON_INCLUDED
#define HISTOGRAMS_SINGLETON_INCLUDED

/* Copyright (c) 2016, 2025, Oracle and/or its affiliates.

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
  @file sql/histograms/singleton.h
  Singleton histogram.

  This file defines the Singleton histogram. A Singleton histogram is a
  histogram where only a value and it's frequency is stored. It allows us to
  use less storage space, as well as estimating selectivity a bit more
  efficient.

  A singleton histogram converted to a JSON object, follows the following
  "schema":

  {
    // Last time the histogram was updated. As of now, this means "when the
    // histogram was created" (incremental updates are not supported). Date/time
    // is given in UTC.
    // -- J_DATETIME
    "last-updated": "2015-11-04 15:19:51.000000",

    // Histogram type. Always "singleton" for singleton histograms.
    // -- J_STRING
    "histogram-type": "singleton",

    // Fraction of NULL values. This is the total fraction of NULL values in the
    // original data set.
    // -- J_DOUBLE
    "null-values": 0.1,

    // Histogram buckets. May be an empty array, if for instance the source
    // only contains NULL values.
    // -- J_ARRAY
    "buckets":
    [
      [
        // Value
        // -- Data type depends on the source column.
        42,

        // Cumulative frequency
        // -- J_DOUBLE
        0.001978728666831561
      ]
    ]
  }
*/

#include <stddef.h>
#include <string>  // std::string

#include "my_inttypes.h"
#include "mysql_time.h"
#include "sql-common/my_decimal.h"
#include "sql/histograms/histogram.h"  // Histogram, Histogram_comparator,
#include "sql/histograms/value_map_type.h"
#include "sql/mem_root_allocator.h"
#include "sql/mem_root_array.h"
#include "sql_string.h"

class Json_array;
class Json_object;
struct MEM_ROOT;

namespace histograms {

/**
  Singleton histogram.

  Singleton histograms do not have a public constructor, but are instead created
  through the factory method Singleton<T>::create() and returned by pointer.
  This is done to ensure that we can return nullptr in case memory allocations
  carried out during construction fail.

  Likewise, the Singleton class does not have a public copy constructor, but
  instead implements a clone() method that returns nullptr in case of failure.
*/
struct Histogram_comparator;
template <class T>
class Value_map;

template <class T>
struct SingletonBucket {
  T value;
  double cumulative_frequency;
  SingletonBucket(T value, double cumulative_frequency)
      : value(value), cumulative_frequency(cumulative_frequency) {}
};

template <class T>
class Singleton : public Histogram {
 public:
  /**
    Singleton histogram factory method.

    Attempts to allocate and initialize a singleton histogram on the supplied
    mem_root. This will not build the histogram, but only set its properties.
    If the attempt to allocate the histogram fails or if an error occurs during
    construction we return nullptr.

    @param mem_root the mem_root where the histogram contents will be allocated
    @param db_name  name of the database this histogram represents
    @param tbl_name name of the table this histogram represents
    @param col_name name of the column this histogram represents
    @param data_type the type of data that this histogram contains

    @return A pointer to a Singleton histogram on success. Returns nullptr on
    error.
  */
  static Singleton<T> *create(MEM_ROOT *mem_root, const std::string &db_name,
                              const std::string &tbl_name,
                              const std::string &col_name,
                              Value_map_type data_type);

  /**
    Make a clone of this histogram on a MEM_ROOT.

    @param mem_root the MEM_ROOT to allocate the new histogram contents on.

    @return a copy of the histogram allocated on the provided MEM_ROOT.
  */
  Histogram *clone(MEM_ROOT *mem_root) const override;

  Singleton(const Singleton<T> &other) = delete;

  /**
    Build the Singleton histogram.

    @param   value_map   values to create the histogram for
    @param   num_buckets the number of buckets specified/requested by the user

    @return  true on error, false otherwise
  */
  bool build_histogram(const Value_map<T> &value_map, size_t num_buckets);

  /**
    Convert this histogram to a JSON object.

    This function will take the contents of the current histogram and put
    it in the output parameter "json_object".

    @param[in,out] json_object output where the histogram is to be stored. The
                   caller is responsible for allocating/deallocating the JSON
                   object

    @return        true on error, false otherwise
  */
  bool histogram_to_json(Json_object *json_object) const override;

  /**
    @return number of values/buckets in this histogram
  */
  size_t get_num_buckets() const override { return m_buckets.size(); }

  /**
    Get the estimated number of distinct non-NULL values.
    @return number of distinct non-NULL values

    TODO(christiani): If the histogram is based on sampling, then this estimate
    is potentially off by a factor 1/sampling_rate. It should be adjusted to an
    actual estimate if we are going to use it.
  */
  size_t get_num_distinct_values() const override { return get_num_buckets(); }

  /**
    Returns the histogram type as a readable string.

    @return a readable string representation of the histogram type
  */
  std::string histogram_type_to_str() const override;

  /**
    Find the number of values equal to 'value'.

    This function will estimate the number of values that is equal to the
    provided value.

    @param value The value to estimate the selectivity for.

    @return the selectivity between 0.0 and 1.0 inclusive.
  */
  double get_equal_to_selectivity(const T &value) const;

  /**
    Find the number of values less than 'value'.

    This function will estimate the number of values that is less than the
    provided value.

    @param value The value to estimate the selectivity for.

    @return the selectivity between 0.0 and 1.0 inclusive.
  */
  double get_less_than_selectivity(const T &value) const;

  /**
    Find the number of values greater than 'value'.

    This function will estimate the number of values that is greater than the
    provided value.

    @param value The value to estimate the selectivity for.

    @return the selectivity between 0.0 and 1.0 inclusive.
  */
  double get_greater_than_selectivity(const T &value) const;

 protected:
  /**
    Populate this histogram with contents from a JSON object.

    @param json_object  a JSON object that represents an Singleton histogram
    @param context      error context for validation

    @return true on error, false otherwise.
   */
  bool json_to_histogram(const Json_object &json_object,
                         Error_context *context) override;

 private:
  /// String representation of the histogram type SINGLETON.
  static constexpr const char *singleton_str() { return "singleton"; }

  /**
    Singleton constructor.

    This will not build the histogram, but only set its properties.

    @param mem_root  the mem_root where the histogram contents will be allocated
    @param db_name   name of the database this histogram represents
    @param tbl_name  name of the table this histogram represents
    @param col_name  name of the column this histogram represents
    @param data_type the type of data that this histogram contains
    @param[out] error is set to true if an error occurs
  */
  Singleton(MEM_ROOT *mem_root, const std::string &db_name,
            const std::string &tbl_name, const std::string &col_name,
            Value_map_type data_type, bool *error);

  /**
    Singleton copy-constructor

    This will take a copy of the histogram and all of its contents on the
    provided MEM_ROOT.

    @param mem_root   the MEM_ROOT to allocate the new histogram on.
    @param other      the histogram to take a copy of
    @param[out] error is set to true if an error occurs
  */
  Singleton(MEM_ROOT *mem_root, const Singleton<T> &other, bool *error);

  /**
    Add value to a JSON bucket

    This function adds the value to the supplied JSON array.

    @param      value       the value to add
    @param[out] json_bucket a JSON array where the bucket data is to be stored

    @return     true on error, false otherwise
  */
  static bool add_value_json_bucket(const T &value, Json_array *json_bucket);

  /**
    Convert one bucket to a JSON object.

    @param      bucket      the histogram bucket to convert
    @param[out] json_bucket a JSON array where the bucket data is to be stored

    @return     true on error, false otherwise
  */
  static bool create_json_bucket(const SingletonBucket<T> &bucket,
                                 Json_array *json_bucket);

  /// The buckets for this histogram [value, cumulative frequency].
  Mem_root_array<SingletonBucket<T>> m_buckets;
};

}  // namespace histograms

#endif
