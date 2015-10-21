# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected (IGNORE_EXIT_CODES => 1, [<<'EOF']);
(exec-sharing) begin
(exec-sharing) parent ending...
EOF
pass;
