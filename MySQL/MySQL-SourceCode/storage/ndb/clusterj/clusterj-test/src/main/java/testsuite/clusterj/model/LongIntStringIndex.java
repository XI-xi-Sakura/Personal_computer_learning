/*
   Copyright (c) 2010, 2025, Oracle and/or its affiliates.

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

package testsuite.clusterj.model;

import com.mysql.clusterj.annotation.Column;
import com.mysql.clusterj.annotation.Index;
import com.mysql.clusterj.annotation.PersistenceCapable;
import com.mysql.clusterj.annotation.PrimaryKey;

/** Schema
 *
drop table if exists longintstringix;
create table longintstringix (
 id int primary key,
 longix bigint not null,
 stringix varchar(10) not null,
 intix int not null,
 stringvalue varchar(10)
) ENGINE=ndbcluster DEFAULT CHARSET=latin1;

create index idx_long_int_string on longintstringix(longix, intix, stringix);

 */
@PersistenceCapable(table="longintstringix")
public interface LongIntStringIndex extends IdBase{

    @Column(name="id")
    int getId();
    void setId(int id);

    @Column(name="longix")
    long getLongix();
    void setLongix(long value);

    @Column(name="intix")
    int getIntix();
    void setIntix(int value);

    @Column(name="stringix")
    String getStringix();
    void setStringix(String value);

    @Column(name="stringvalue")
    String getStringvalue();
    void setStringvalue(String value);

}
