# ==== Purpose ====
#
# Wait until the grep for a pattern in a file produces some non-empty
# result.
#
# NOTE: to better support the more strict usage with MySQL error log files
# and MTR, the search output is reset whenever the MTR beginning-of-test
# tag is found (CURRENT_TEST: ...).
#
# ==== Usage ====
#
# --let $grep_pattern = PERL_REGEX
# --let $grep_file = FILENAME
# --source ../include/patter_not_in_file.inc
#
# Parameters:
#
#   $grep_pattern
#     The pattern to search for. This can be a perl regex.
#
#   $grep_file
#     The file to search in.
#
#   $wait_timeout
#     If instantiated, the wait period in seconds to wait for the pattern
#     to appear in the file. If not instantiated, it will wait
#     indefinitely.
#
--let _WFPIF_GREP_PATTERN = $grep_pattern
--let _WFPIF_GREP_FILE = $grep_file
--let _WFPIF_WAIT_TIMEOUT = $wait_timeout

--echo include/wait_for_pattern_in_file.inc [$grep_pattern]

--perl
  use strict;
  my $file = $ENV{'_WFPIF_GREP_FILE'} or die "grep_file is not set";
  my $pattern = $ENV{'_WFPIF_GREP_PATTERN'} or die "grep_pattern is not set";
  my $wait_timeout = $ENV{'_WFPIF_WAIT_TIMEOUT'};
  my $found = 0;
  my $timeout = $wait_timeout;
  my $line = '';

  open(my $fd, "<", $file) or die("Unable to open $file: $!\n");
  
  while (<$fd>) {
    $line = $line . $_;
    if (substr($line, length($line) - 1, 1) eq "\n" || substr($line, length($line) - 2, 2) eq "\r\f") {
      if (($line =~ /^CURRENT_TEST: /)) {
        $found = 0;
      }
      if ($found == 0 && $line =~ /$pattern/) {
        $found = 1;
      }
      $line = '';
    }
  }
  
  close($fd) or die "Error closing $file: $!";
  
  if ($found == 1) {
    die "Pattern found in file.";
  }
EOF
