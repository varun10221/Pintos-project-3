# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(bad-syscall) begin
bad-syscall: exit(-1)
EOF
pass;
