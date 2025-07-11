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

package com.mysql.cluster.crund;

import com.mysql.clusterj.annotation.Column;
import com.mysql.clusterj.annotation.PersistenceCapable;

/**
 * An Entity test interface for ClusterJ.
 */
@PersistenceCapable(table="B")
public interface IB {

    public int getId();
    public void setId(int id);

    public int getCint();
    public void setCint(int cint);

    public long getClong();
    public void setClong(long clong);

    public float getCfloat();
    public void setCfloat(float cfloat);

    public double getCdouble();
    public void setCdouble(double cdouble);

/*
   // XXX NPE despite allowsNull="true" annotation, must set to non-null
   o.setCvarbinary_def(new byte[0]);

     [java] SEVERE: Error executing getInsertOperation on table b.
     [java] caught com.mysql.clusterj.ClusterJException: Error executing getInsertOperation on table b. Caused by java.lang.NullPointerException:null
     [java] com.mysql.clusterj.ClusterJException: Error executing getInsertOperation on table b. Caused by java.lang.NullPointerException:null
     [java] at com.mysql.clusterj.core.SessionImpl.insert(SessionImpl.java:283)
*/
    // XXX @javax.persistence.Basic(fetch=javax.persistence.FetchType.LAZY)
    @Column(name="cvarbinary_def",allowsNull="true")
    public byte[] getCvarbinary_def();
    public void setCvarbinary_def(byte[] cvarbinary_def);

    // XXX @javax.persistence.Basic(fetch=javax.persistence.FetchType.LAZY)
    @Column(name="cvarchar_def")
    public String getCvarchar_def();
    public void setCvarchar_def(String cvarchar_def);

    @Column(name="a_id") // or change name of attr
    public int getAid();
    public void setAid(int aid);
}
