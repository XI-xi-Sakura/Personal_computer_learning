/*
   Copyright (c) 2011, 2025, Oracle and/or its affiliates.

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

package testsuite.clusterj;

import testsuite.clusterj.model.StringTypes;
import testsuite.clusterj.model.IdBase;

/** Derived from QueryStringTypesTest.
 */
public class QueryLikeTest extends AbstractQueryTest {

    @Override
    public Class<?> getInstanceType() {
        return StringTypes.class;
    }

    @Override
    void createInstances(int number) {
        createAllStringTypesInstances(number);
    }

    static String[] strings = new String[] {
        "Alabama",
        "Arkansas",
        "Delaware",
        "New Jersey",
        "New York",
        "Pennsylvania",
        "Rhode Island",
        "Texax",
        "Virginia",
        "Wyoming"
    };

    /** Schema
    *
   drop table if exists stringtypes;
   create table stringtypes (
    id int not null primary key,

    string_null_hash varchar(20),
    string_null_btree varchar(300),
    string_null_both varchar(20),
    string_null_none varchar(300),

    string_not_null_hash varchar(300),
    string_not_null_btree varchar(20),
    string_not_null_both varchar(300),
    string_not_null_none varchar(20),
    unique key idx_string_null_hash (string_null_hash) using hash,
    key idx_string_null_btree (string_null_btree),
    unique key idx_string_null_both (string_null_both),

    unique key idx_string_not_null_hash (string_not_null_hash) using hash,
    key idx_string_not_null_btree (string_not_null_btree),
    unique key idx_string_not_null_both (string_not_null_both)

   ) ENGINE=ndbcluster DEFAULT CHARSET=latin1;

    */
    public void test() {
        btreeIndexScanString();
        hashIndexScanString();
        bothIndexScanString();
        noneIndexScanString();
        failOnError();
    }

    public void btreeIndexScanString() {
        likeQuery("string_not_null_btree", "none", "%", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
        likeQuery("string_not_null_btree", "none", "_");
        likeQuery("string_not_null_btree", "none", "_____", 7);
        likeQuery("string_not_null_btree", "none", "%York", 4);
        likeQuery("string_not_null_btree", "none", "New York", 4);
        likeQuery("string_not_null_btree", "none", "New Yor_", 4);
        likeQuery("string_not_null_btree", "none", "New%", 3, 4);
        greaterEqualAndLikeQuery("string_not_null_btree", "idx_string_not_null_btree", getString(4), "%", 4, 5, 6, 7, 8, 9);
        greaterEqualAndLikeQuery("string_not_null_btree", "idx_string_not_null_btree", getString(4), "%enns%", 5);
        greaterThanAndLikeQuery("string_not_null_btree", "idx_string_not_null_btree", getString(4), "%e%", 5, 6, 7);
    }

    public void hashIndexScanString() {
        greaterEqualAndLikeQuery("string_not_null_hash", "none", getString(4), "%enns%", 5);
        greaterThanAndLikeQuery("string_not_null_hash", "none", getString(4), "%e%", 5, 6, 7);
    }

    public void bothIndexScanString() {
        greaterEqualAndLikeQuery("string_not_null_both", "idx_string_not_null_both", getString(4), "%enns%", 5);
        greaterThanAndLikeQuery("string_not_null_both", "idx_string_not_null_both", getString(4), "%e%", 5, 6, 7);
    }

    public void noneIndexScanString() {
        greaterEqualAndLikeQuery("string_not_null_none", "none", getString(4), "%enns%", 5);
        greaterThanAndLikeQuery("string_not_null_none", "none", getString(4), "%e%", 5, 6, 7);
    }

    private void createAllStringTypesInstances(int number) {
        for (int i = 0; i < number; ++i) {
            StringTypes instance = session.newInstance(StringTypes.class);
            instance.setId(i);
            instance.setString_not_null_hash(getString(i));
            instance.setString_not_null_btree(getString(i));
            instance.setString_not_null_both(getString(i));
            instance.setString_not_null_none(getString(i));
            instance.setString_null_hash(getString(i));
            instance.setString_null_btree(getString(i));
            instance.setString_null_both(getString(i));
            instance.setString_null_none(getString(i));
            instances.add(instance);
        }
    }

    protected String getString(int number) {
        return strings[number];
    }

    /** Print the results of a query for debugging.
     *
     * @param instance the instance to print
     */
    @Override
    protected void printResultInstance(IdBase instance) {
        if (instance instanceof StringTypes) {
            @SuppressWarnings("unused")
            StringTypes stringType = (StringTypes)instance;
//            System.out.println(toString(stringType));
        }
    }

    public static String toString(IdBase idBase) {
        StringTypes instance = (StringTypes)idBase;
        StringBuffer buffer = new StringBuffer("StringTypes id: ");
        buffer.append(instance.getId());
        buffer.append("; string_not_null_both: ");
        buffer.append(instance.getString_not_null_both());
        buffer.append("; string_not_null_btree: ");
        buffer.append(instance.getString_not_null_btree());
        buffer.append("; string_not_null_hash: ");
        buffer.append(instance.getString_not_null_hash());
        buffer.append("; string_not_null_none: ");
        buffer.append(instance.getString_not_null_none());
        buffer.append("; string_null_both: ");
        buffer.append(instance.getString_null_both().toString());
        buffer.append("; string_null_btree: ");
        buffer.append(instance.getString_null_btree().toString());
        buffer.append("; string_null_hash: ");
        buffer.append(instance.getString_null_hash().toString());
        buffer.append("; string_null_none: ");
        buffer.append(instance.getString_null_none().toString());
        return buffer.toString();
    }
}
