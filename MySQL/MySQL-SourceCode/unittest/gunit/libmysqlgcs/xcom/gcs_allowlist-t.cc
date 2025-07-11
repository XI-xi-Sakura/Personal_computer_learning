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

#include <string>
#include <vector>

#include "gcs_base_test.h"

using std::vector;

namespace gcs_allowlist_unittest {

class GcsAllowlist : public GcsBaseTest {
 protected:
  GcsAllowlist() = default;
};

TEST_F(GcsAllowlist, ValidIPs) {
  Gcs_ip_allowlist wl;
  ASSERT_TRUE(wl.is_valid("192.168.1.1"));
  ASSERT_TRUE(wl.is_valid("192.168.1.2"));
  ASSERT_TRUE(wl.is_valid("192.168.1.254"));

  ASSERT_TRUE(wl.is_valid("::1"));
  ASSERT_TRUE(wl.is_valid("2606:b400:8b0:40:4308:1306:ad4a:6e51"));
  ASSERT_TRUE(wl.is_valid("::1/2"));
  ASSERT_TRUE(wl.is_valid("::1/64,192.168.1.2/24"));
  ASSERT_TRUE(wl.is_valid("::1/64,192.168.1.2/24,192.168.1.1"));
  ASSERT_TRUE(wl.is_valid("::1/64,192.168.1.2/24,192.168.1.1,10.1.1.1"));
}

TEST_F(GcsAllowlist, InvalidConfiguration) {
  Gcs_ip_allowlist wl;
  ASSERT_FALSE(wl.is_valid("192.168.1"));
  ASSERT_FALSE(wl.is_valid("192.168.1/24"));
  ASSERT_FALSE(wl.is_valid("192.168.1.0/33"));
  ASSERT_FALSE(wl.is_valid("192.168.1.0/24,192.168.2.0/33"));
}

TEST_F(GcsAllowlist, ValidListIPv6) {
  Gcs_ip_allowlist wl;
  std::string const list =
      "::1/128,192.168.1.1/24,fe80::2ab2:bdff:fe16:8d07/67, "
      "2606:b400:8b0:40:4308:1306:ad4a:6e51";
  wl.configure(list);

  ASSERT_FALSE(wl.shall_block("::1"));
  ASSERT_FALSE(wl.shall_block("fe80::2ab2:bdff:fe16:8d07"));
  ASSERT_FALSE(wl.shall_block("2606:b400:8b0:40:4308:1306:ad4a:6e51"));
  ASSERT_FALSE(wl.shall_block("::ffff:192.168.1.10"));
  ASSERT_FALSE(wl.shall_block("192.168.1.10"));
}

TEST_F(GcsAllowlist, ValidListIPv4) {
  Gcs_ip_allowlist wl;
  wl.configure("192.168.1.0/31,localhost/32");

  ASSERT_FALSE(wl.shall_block("192.168.1.1"));
  ASSERT_TRUE(wl.shall_block("192.168.2.1"));
  ASSERT_TRUE(wl.shall_block("192.168.1.2"));

  wl.configure("192.168.1.0/32");
  ASSERT_TRUE(wl.shall_block("192.168.1.1"));

  wl.configure("192.168.1.1/32");
  ASSERT_FALSE(wl.shall_block("192.168.1.1"));

  // never block localhost
  ASSERT_FALSE(wl.shall_block("127.0.0.1"));

  wl.configure("192.168.1.0/24,192.168.2.0/24");
  ASSERT_FALSE(wl.shall_block("127.0.0.1"));
  ASSERT_FALSE(wl.shall_block("192.168.1.2"));
  ASSERT_FALSE(wl.shall_block("192.168.1.254"));
  ASSERT_FALSE(wl.shall_block("192.168.2.2"));
  ASSERT_FALSE(wl.shall_block("192.168.2.254"));
}

TEST_F(GcsAllowlist, DefaultList) {
  Gcs_ip_allowlist wl;

  wl.configure(Gcs_ip_allowlist::DEFAULT_ALLOWLIST);
  ASSERT_FALSE(wl.shall_block("127.0.0.1"));
  ASSERT_FALSE(wl.shall_block("::1"));
  ASSERT_FALSE(wl.shall_block("fe80::da2:aab6:88aa:5061"));
  ASSERT_FALSE(wl.shall_block("192.168.1.2"));
  ASSERT_FALSE(wl.shall_block("192.168.2.2"));
  ASSERT_FALSE(wl.shall_block("10.0.0.1"));
  ASSERT_TRUE(wl.shall_block("172.15.0.1"));
  ASSERT_FALSE(wl.shall_block("172.16.0.1"));
  ASSERT_FALSE(wl.shall_block("172.24.0.1"));
  ASSERT_FALSE(wl.shall_block("172.31.0.1"));
  ASSERT_TRUE(wl.shall_block("172.38.0.1"));
}

TEST_F(GcsAllowlist, ListAsText) {
  Gcs_ip_allowlist wl;

  wl.configure(Gcs_ip_allowlist::DEFAULT_ALLOWLIST);
  ASSERT_STRCASEEQ(Gcs_ip_allowlist::DEFAULT_ALLOWLIST.c_str(),
                   wl.get_configured_ip_allowlist().c_str());
}

TEST_F(GcsAllowlist, AbsentList) {
  Gcs_interface_parameters params;
  params.add_parameter("group_name", "ola");
  params.add_parameter("peer_nodes", "127.0.0.1:24844");
  params.add_parameter("local_node", "127.0.0.1:24844");
  params.add_parameter("bootstrap_group", "true");
  params.add_parameter("poll_spin_loops", "100");

  Gcs_interface *gcs = Gcs_xcom_interface::get_interface();
  enum_gcs_error err = gcs->initialize(params);
  ASSERT_EQ(err, GCS_OK);

  // verify that a allowlist was provided by default
  auto *xcs = static_cast<Gcs_xcom_interface *>(gcs);
  MYSQL_GCS_LOG_INFO("Allowlist as string with collected IP addresses: "
                     << xcs->get_ip_allowlist().to_string());
  ASSERT_FALSE(xcs->get_ip_allowlist().get_configured_ip_allowlist().empty());
  ASSERT_FALSE(xcs->get_ip_allowlist().to_string().empty());

  // this finalizes the m_logger, so be careful to not add a call to
  // MYSQL_GCS_LOG after this line
  err = gcs->finalize();

  // claim interface memory back
  Gcs_xcom_interface::cleanup();

  // initialization failed, and thus so will finalization
  ASSERT_EQ(err, GCS_OK);
}

TEST_F(GcsAllowlist, ListWithHostname) {
  Gcs_interface_parameters params;
  params.add_parameter("group_name", "ola");
  params.add_parameter("peer_nodes", "127.0.0.1:24844");
  params.add_parameter("local_node", "127.0.0.1:24844");
  params.add_parameter("bootstrap_group", "true");
  params.add_parameter("poll_spin_loops", "100");

  vector<char> machine_hostname;
  machine_hostname.resize(MAXHOSTNAMELEN);
  gethostname(machine_hostname.data(), MAXHOSTNAMELEN);

  std::ostringstream assembled_allowlist;
  assembled_allowlist << machine_hostname.data();
  assembled_allowlist << "/16,";
  assembled_allowlist << "localhost/32";
  params.add_parameter("ip_allowlist", assembled_allowlist.str());

  Gcs_interface *gcs = Gcs_xcom_interface::get_interface();
  enum_gcs_error err = gcs->initialize(params);
  ASSERT_EQ(err, GCS_OK);

  // verify that a allowlist was provided by default
  auto *xcs = static_cast<Gcs_xcom_interface *>(gcs);
  MYSQL_GCS_LOG_INFO("Allowlist as string with collected IP addresses: "
                     << xcs->get_ip_allowlist().to_string());
  ASSERT_FALSE(xcs->get_ip_allowlist().get_configured_ip_allowlist().empty());
  ASSERT_FALSE(xcs->get_ip_allowlist().to_string().empty());

  std::vector<std::pair<sa_family_t, std::string>> ips;
  resolve_all_ip_addr_from_hostname("localhost", ips);

  auto has_v4_addresses_it =
      std::find_if(ips.begin(), ips.end(),
                   [](std::pair<sa_family_t, std::string> const &ip_entry) {
                     return ip_entry.first == AF_INET;
                   });
  bool const has_v4_addresses = has_v4_addresses_it != ips.end();

  // This should not block to whatever address localhost resolves
  for (auto &ip : ips) {
    if (has_v4_addresses && ip.first == AF_INET6)
      EXPECT_TRUE(xcs->get_ip_allowlist().shall_block(ip.second));
    else
      EXPECT_FALSE(xcs->get_ip_allowlist().shall_block(ip.second));
  }

  // this finalizes the m_logger, so be careful to not add a call to
  // MYSQL_GCS_LOG after this line
  err = gcs->finalize();

  // claim interface memory back
  Gcs_xcom_interface::cleanup();

  ASSERT_EQ(err, GCS_OK);
}

TEST_F(GcsAllowlist, ListWithUnresolvableHostname) {
  Gcs_interface_parameters params;
  params.add_parameter("group_name", "ola");
  params.add_parameter("peer_nodes", "127.0.0.1:24844");
  params.add_parameter("local_node", "127.0.0.1:24844");
  params.add_parameter("bootstrap_group", "true");
  params.add_parameter("poll_spin_loops", "100");

  vector<char> machine_hostname;
  machine_hostname.resize(MAXHOSTNAMELEN);
  gethostname(machine_hostname.data(), MAXHOSTNAMELEN);

  std::ostringstream assembled_allowlist;
  assembled_allowlist << machine_hostname.data();
  assembled_allowlist << "/16,";
  assembled_allowlist << "unresolvablehostname/32,";
  assembled_allowlist << "localhost/32";
  params.add_parameter("ip_allowlist", assembled_allowlist.str());

  Gcs_interface *gcs = Gcs_xcom_interface::get_interface();
  enum_gcs_error err = gcs->initialize(params);
  ASSERT_EQ(err, GCS_OK);

  // verify that a allowlist was provided by default
  auto *xcs = static_cast<Gcs_xcom_interface *>(gcs);
  MYSQL_GCS_LOG_INFO("Allowlist as string with collected IP addresses: "
                     << xcs->get_ip_allowlist().to_string());
  ASSERT_FALSE(xcs->get_ip_allowlist().get_configured_ip_allowlist().empty());
  ASSERT_FALSE(xcs->get_ip_allowlist().to_string().empty());

  // This will force a allowlist validation and a failure on name resolution
  // code
  ASSERT_TRUE(xcs->get_ip_allowlist().shall_block("192.12.13.14"));

  // this finalizes the m_logger, so be careful to not add a call to
  // MYSQL_GCS_LOG after this line
  err = gcs->finalize();

  // claim interface memory back
  Gcs_xcom_interface::cleanup();

  ASSERT_EQ(err, GCS_OK);
}

TEST_F(GcsAllowlist, XComMembers) {
  Gcs_ip_allowlist wl;
  char const *members[] = {"8.8.8.8:12435", "8.8.4.4:1234", "localhost:12346"};
  const char **xcom_addrs{members};
  node_address *xcom_names = new_node_address(3, xcom_addrs);
  site_def *xcom_config = new_site_def();
  init_site_def(3, xcom_names, xcom_config);
  wl.configure(Gcs_ip_allowlist::DEFAULT_ALLOWLIST);

  ASSERT_FALSE(wl.shall_block("127.0.0.1", xcom_config));
  ASSERT_FALSE(wl.shall_block("::1", xcom_config));
  ASSERT_FALSE(wl.shall_block("192.168.1.2", xcom_config));
  ASSERT_FALSE(wl.shall_block("192.168.2.2", xcom_config));
  ASSERT_FALSE(wl.shall_block("10.0.0.1", xcom_config));
  ASSERT_TRUE(wl.shall_block("172.15.0.1", xcom_config));
  ASSERT_FALSE(wl.shall_block("172.16.0.1", xcom_config));
  ASSERT_FALSE(wl.shall_block("172.24.0.1", xcom_config));
  ASSERT_FALSE(wl.shall_block("172.31.0.1", xcom_config));
  ASSERT_TRUE(wl.shall_block("172.38.0.1", xcom_config));

  ASSERT_FALSE(wl.shall_block("8.8.8.8", xcom_config));
  ASSERT_FALSE(wl.shall_block("8.8.4.4", xcom_config));
  ASSERT_TRUE(wl.shall_block("8.8.8.9", xcom_config));
  ASSERT_TRUE(wl.shall_block("8.8.9.8", xcom_config));
  ASSERT_TRUE(wl.shall_block("8.9.8.8", xcom_config));
  ASSERT_TRUE(wl.shall_block("9.8.8.8", xcom_config));

  delete_node_address(3, xcom_names);
  free_site_def(xcom_config);
}

TEST_F(GcsAllowlist, ComparisonBetweenIPv4AndIPv6) {
  Gcs_ip_allowlist wl;
  wl.configure("ac0f:0001::/32");

  ASSERT_TRUE(wl.shall_block("172.15.0.1"));
}

}  // namespace gcs_allowlist_unittest
